#include "net/cache.h"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "base/strings.h"
#include "net/http.h"

namespace kite {

namespace {

const char kMagic[] = "KITECACHE1";
const long long kMaxEntry = 8 * 1024 * 1024;

#ifdef _WIN32
std::wstring Wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), 0, 0);
  std::wstring w(n, 0);
  if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}
FILE* OpenFile(const std::string& path, const char* mode) {
  std::wstring m(mode, mode + strlen(mode));
  return _wfopen(Wide(path).c_str(), m.c_str());
}
void RemoveFile(const std::string& path) { DeleteFileW(Wide(path).c_str()); }
bool RenameFile(const std::string& from, const std::string& to) {
  DeleteFileW(Wide(to).c_str());
  return MoveFileW(Wide(from).c_str(), Wide(to).c_str()) != 0;
}
void MakeDir(const std::string& path) { CreateDirectoryW(Wide(path).c_str(), 0); }
void ListDir(const std::string& dir, std::vector<std::pair<std::string, long long> >& out) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(Wide(dir + "\\*.kc").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    std::string name;
    for (const wchar_t* p = fd.cFileName; *p; ++p) name += (char)*p;  // names are ASCII
    out.push_back(std::make_pair(name, ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}
const char kSep[] = "\\";
#else
FILE* OpenFile(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }
void RemoveFile(const std::string& path) { unlink(path.c_str()); }
bool RenameFile(const std::string& from, const std::string& to) { return rename(from.c_str(), to.c_str()) == 0; }
void MakeDir(const std::string& path) { mkdir(path.c_str(), 0700); }
void ListDir(const std::string& dir, std::vector<std::pair<std::string, long long> >& out) {
  DIR* d = opendir(dir.c_str());
  if (!d) return;
  while (struct dirent* e = readdir(d)) {
    std::string name = e->d_name;
    if (!EndsWith(name, ".kc")) continue;
    struct stat st;
    if (stat((dir + "/" + name).c_str(), &st) == 0) out.push_back(std::make_pair(name, (long long)st.st_size));
  }
  closedir(d);
}
const char kSep[] = "/";
#endif

std::string FileNameFor(const std::string& url) {
  // FNV-1a 64 over the URL without fragment.
  std::string key = url.substr(0, url.find('#'));
  unsigned long long h = 1469598103934665603ULL;
  for (size_t i = 0; i < key.size(); ++i) {
    h ^= (unsigned char)key[i];
    h *= 1099511628211ULL;
  }
  char buf[32];
  snprintf(buf, sizeof buf, "%016llx.kc", h);
  return buf;
}

bool ReadAll(const std::string& path, std::string& out) {
  FILE* f = OpenFile(path, "rb");
  if (!f) return false;
  char buf[65536];
  size_t n;
  out.clear();
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return true;
}

std::string HeaderValue(const std::vector<std::pair<std::string, std::string> >& h, const char* name) {
  for (size_t i = 0; i < h.size(); ++i)
    if (AsciiLower(h[i].first) == name) return h[i].second;
  return std::string();
}

// max-age=N in a Cache-Control value (-1 if absent).
long long Directive(const std::string& cc, const char* name) {
  std::vector<std::string> parts = Split(AsciiLower(cc), ',');
  for (size_t i = 0; i < parts.size(); ++i) {
    std::string p = Trim(parts[i]);
    std::string n = name;
    if (StartsWith(p, n + "=")) {
      long long v;
      if (ParseInt(Trim(p.substr(n.size() + 1)), v)) return v;
    }
  }
  return -1;
}

bool HasDirective(const std::string& cc, const char* name) {
  std::vector<std::string> parts = Split(AsciiLower(cc), ',');
  for (size_t i = 0; i < parts.size(); ++i)
    if (Trim(parts[i]) == name || StartsWith(Trim(parts[i]), std::string(name) + "=")) return true;
  return false;
}

}  // namespace

HttpCache::HttpCache(const std::string& dir, long long maxBytes) : dir_(dir), maxBytes_(maxBytes) {
  MakeDir(dir_);
  std::vector<std::pair<std::string, long long> > list;
  ListDir(dir_, list);
  MutexLock l(mu_);
  for (size_t i = 0; i < list.size(); ++i) {
    files_[list[i].first] = std::make_pair(list[i].second, 0LL);
    total_ += list[i].second;
  }
  Evict();
}

std::string HttpCache::PathFor(const std::string& url) const { return dir_ + kSep + FileNameFor(url); }

void HttpCache::Freshness(const std::vector<std::pair<std::string, std::string> >& headers, long long now,
                          Entry& e, bool& storable) {
  std::string cc = HeaderValue(headers, "cache-control");
  storable = !HasDirective(cc, "no-store") && HeaderValue(headers, "set-cookie").empty();
  std::string vary = AsciiLower(HeaderValue(headers, "vary"));
  if (!vary.empty() && Trim(vary) != "accept-encoding") storable = false;
  e.etag = HeaderValue(headers, "etag");
  e.lastModified = HeaderValue(headers, "last-modified");
  e.mustRevalidate = HasDirective(cc, "no-cache");
  long long date = HttpParseDate(HeaderValue(headers, "date"));
  if (date <= 0 || date > now) date = now;
  long long age = 0;
  ParseInt(Trim(HeaderValue(headers, "age")), age);
  long long maxAge = Directive(cc, "max-age");
  long long lifetime = 0;
  if (maxAge >= 0) {
    lifetime = maxAge;
  } else if (!HeaderValue(headers, "expires").empty()) {
    long long exp = HttpParseDate(HeaderValue(headers, "expires"));
    lifetime = exp > date ? exp - date : 0;
  } else if (!e.lastModified.empty()) {
    // Heuristic: a tenth of the time since the last change, at most a day.
    long long lm = HttpParseDate(e.lastModified);
    if (lm > 0 && lm < date) lifetime = std::min<long long>((date - lm) / 10, 24 * 3600);
  }
  e.freshUntil = now + std::max<long long>(0, lifetime - std::max<long long>(0, age));
  if (!storable) return;
  // Nothing to gain from entries that are never fresh and cannot be revalidated.
  if (e.freshUntil <= now && e.etag.empty() && e.lastModified.empty()) storable = false;
}

bool HttpCache::Fresh(const Entry& e) const { return !e.mustRevalidate && HttpNowUnix() < e.freshUntil; }

bool HttpCache::Lookup(const std::string& url, Entry& out) {
  std::string name = FileNameFor(url);
  {
    MutexLock l(mu_);
    if (maxBytes_ <= 0 || !files_.count(name)) return false;
    files_[name].second = ++clock_;
  }
  std::string data;
  if (!ReadAll(dir_ + kSep + name, data)) return false;
  // Header lines, then the body.
  size_t pos = 0;
  auto line = [&](std::string& v) -> bool {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) return false;
    v = data.substr(pos, nl - pos);
    pos = nl + 1;
    return true;
  };
  std::string magic, num;
  long long v;
  if (!line(magic) || magic != kMagic || !line(out.url) || out.url != url.substr(0, url.find('#'))) return false;
  if (!line(num) || !ParseInt(num, v)) return false;
  out.status = (int)v;
  if (!line(num) || !ParseInt(num, out.storedAt)) return false;
  if (!line(num) || !ParseInt(num, out.freshUntil)) return false;
  if (!line(num)) return false;
  out.mustRevalidate = num == "1";
  if (!line(out.etag) || !line(out.lastModified) || !line(num)) return false;
  out.secure = num == "1";
  if (!line(out.tlsVersion) || !line(num) || !ParseInt(num, v)) return false;
  out.headers.clear();
  for (long long i = 0; i < v; ++i) {
    std::string h;
    if (!line(h)) return false;
    size_t tab = h.find('\t');
    if (tab == std::string::npos) return false;
    out.headers.push_back(std::make_pair(h.substr(0, tab), h.substr(tab + 1)));
  }
  if (!line(num) || !ParseInt(num, v) || v < 0 || pos + (size_t)v != data.size()) return false;
  out.body = data.substr(pos);
  return true;
}

bool HttpCache::Write(const Entry& e) {
  std::string out = std::string(kMagic) + "\n" + e.url + "\n" + IntToString(e.status) + "\n" +
                    IntToString(e.storedAt) + "\n" + IntToString(e.freshUntil) + "\n" +
                    (e.mustRevalidate ? "1" : "0") + "\n" + e.etag + "\n" + e.lastModified + "\n" +
                    (e.secure ? "1" : "0") + "\n" + e.tlsVersion + "\n" +
                    IntToString((long long)e.headers.size()) + "\n";
  for (size_t i = 0; i < e.headers.size(); ++i) out += e.headers[i].first + "\t" + e.headers[i].second + "\n";
  out += IntToString((long long)e.body.size()) + "\n";
  out += e.body;
  std::string name = FileNameFor(e.url);
  std::string path = dir_ + kSep + name, tmp = path + ".tmp";
  FILE* f = OpenFile(tmp, "wb");
  if (!f) return false;
  bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  ok = fclose(f) == 0 && ok;
  if (!ok || !RenameFile(tmp, path)) {
    RemoveFile(tmp);
    return false;
  }
  MutexLock l(mu_);
  std::map<std::string, std::pair<long long, long long> >::iterator it = files_.find(name);
  if (it != files_.end()) total_ -= it->second.first;
  files_[name] = std::make_pair((long long)out.size(), ++clock_);
  total_ += (long long)out.size();
  Evict();
  return true;
}

void HttpCache::Store(const std::string& url, const FetchResponse& resp) {
  {
    MutexLock l(mu_);
    if (maxBytes_ <= 0) return;
  }
  if (!resp.ok || resp.status != 200 || (long long)resp.body.size() > kMaxEntry) return;
  if (!StartsWith(url, "http:") && !StartsWith(url, "https:")) return;
  Entry e;
  e.url = url.substr(0, url.find('#'));
  e.storedAt = HttpNowUnix();
  bool storable = false;
  Freshness(resp.headers, e.storedAt, e, storable);
  if (!storable) return;
  e.status = resp.status;
  e.secure = resp.secure;
  e.tlsVersion = resp.tlsVersion;
  for (size_t i = 0; i < resp.headers.size(); ++i) {
    std::string n = AsciiLower(resp.headers[i].first);
    // The stored body is already decoded.
    if (n == "content-encoding" || n == "content-length" || n == "transfer-encoding" || n == "set-cookie" ||
        n == "connection" || n == "keep-alive")
      continue;
    if (resp.headers[i].second.find('\n') != std::string::npos) continue;
    e.headers.push_back(resp.headers[i]);
  }
  e.body = resp.body;
  Write(e);
}

void HttpCache::Refresh(const std::string& url, const FetchResponse& notModified, Entry& entry) {
  long long now = HttpNowUnix();
  // Headers of the 304 replace the stored ones (freshness, validators).
  std::vector<std::pair<std::string, std::string> > merged = entry.headers;
  for (size_t i = 0; i < notModified.headers.size(); ++i) {
    std::string n = AsciiLower(notModified.headers[i].first);
    if (n == "content-length" || n == "content-encoding" || n == "transfer-encoding" || n == "set-cookie" ||
        n == "connection" || n == "keep-alive")
      continue;
    bool replaced = false;
    for (size_t k = 0; k < merged.size(); ++k)
      if (AsciiLower(merged[k].first) == n) {
        merged[k].second = notModified.headers[i].second;
        replaced = true;
      }
    if (!replaced) merged.push_back(notModified.headers[i]);
  }
  bool storable = true;
  Entry e = entry;
  e.headers = merged;
  Freshness(merged, now, e, storable);
  if (e.etag.empty()) e.etag = entry.etag;
  if (e.lastModified.empty()) e.lastModified = entry.lastModified;
  e.storedAt = now;
  entry = e;
  if (storable) Write(e);
  (void)url;
}

void HttpCache::ToResponse(const Entry& e, FetchResponse& out) {
  out.ok = true;
  out.status = e.status;
  out.finalUrl = e.url;
  out.headers = e.headers;
  out.body = e.body;
  out.secure = e.secure;
  out.tlsVersion = e.tlsVersion;
  out.protocol = "cache";
}

void HttpCache::Clear() {
  MutexLock l(mu_);
  for (std::map<std::string, std::pair<long long, long long> >::iterator it = files_.begin(); it != files_.end(); ++it)
    RemoveFile(dir_ + kSep + it->first);
  files_.clear();
  total_ = 0;
}

void HttpCache::SetLimit(long long maxBytes) {
  MutexLock l(mu_);
  maxBytes_ = maxBytes;
  Evict();
}

long long HttpCache::size() {
  MutexLock l(mu_);
  return total_;
}

void HttpCache::Evict() {
  long long limit = std::max<long long>(0, maxBytes_);
  while (total_ > limit && !files_.empty()) {
    std::map<std::string, std::pair<long long, long long> >::iterator oldest = files_.begin();
    for (std::map<std::string, std::pair<long long, long long> >::iterator it = files_.begin(); it != files_.end(); ++it)
      if (it->second.second < oldest->second.second) oldest = it;
    RemoveFile(dir_ + kSep + oldest->first);
    total_ -= oldest->second.first;
    files_.erase(oldest);
  }
}

}  // namespace kite
