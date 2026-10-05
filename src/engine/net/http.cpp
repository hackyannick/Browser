#include "net/http.h"
#include "net/http2.h"

#include <algorithm>
#include <cstdio>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include <brotli/decode.h>

#include "base/strings.h"

extern "C" {
char* stbi_zlib_decode_malloc_guesssize_headerflag(const char* buffer, int len, int initial_size,
                                                    int* outlen, int parse_header);
}

namespace kite {

namespace {

long long NowUnix() {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (long long)(t / 10000000ULL) - 11644473600LL;
#else
  return (long long)time(0);
#endif
}

long long DaysFromCivil(long long y, unsigned m, unsigned d) {
  y -= m <= 2;
  long long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}

// Parses HTTP dates like "Wed, 21 Oct 2015 07:28:00 GMT" (and the cookie
// variant with dashes). Returns 0 on failure.
long long ParseHttpDate(const std::string& s) {
  static const char* months[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                 "jul", "aug", "sep", "oct", "nov", "dec"};
  std::string t = AsciiLower(s);
  for (size_t i = 0; i < t.size(); ++i)
    if (t[i] == '-' || t[i] == ',') t[i] = ' ';
  std::vector<std::string> parts = SplitWhitespace(t);
  int day = -1, month = -1, year = -1, hh = 0, mm = 0, ss = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string& p = parts[i];
    if (p.find(':') != std::string::npos) {
      std::vector<std::string> hms = Split(p, ':');
      long long v;
      if (hms.size() >= 2) {
        if (ParseInt(hms[0], v)) hh = (int)v;
        if (ParseInt(hms[1], v)) mm = (int)v;
        if (hms.size() > 2 && ParseInt(hms[2], v)) ss = (int)v;
      }
      continue;
    }
    bool isMonth = false;
    for (int m = 0; m < 12; ++m)
      if (StartsWith(p, months[m])) {
        month = m + 1;
        isMonth = true;
      }
    if (isMonth) continue;
    long long v;
    if (ParseInt(p, v)) {
      if (day < 0 && v >= 1 && v <= 31 && p.size() <= 2) day = (int)v;
      else if (year < 0) year = (int)(v < 70 ? v + 2000 : v < 100 ? v + 1900 : v);
    }
  }
  if (day < 0 || month < 0 || year < 0) return 0;
  return DaysFromCivil(year, month, day) * 86400 + hh * 3600 + mm * 60 + ss;
}

bool DomainMatch(const std::string& host, const std::string& domain) {
  if (host == domain) return true;
  return host.size() > domain.size() && EndsWith(host, domain) &&
         host[host.size() - domain.size() - 1] == '.';
}

std::string StatusText(int code) {
  switch (code) {
    case 404: return "Seite nicht gefunden";
    case 403: return "Zugriff verweigert";
    case 500: return "Interner Serverfehler";
    case 502: return "Bad Gateway";
    case 503: return "Dienst nicht verfügbar";
    default: return "HTTP-Fehler";
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// FetchResponse

std::string FetchResponse::Header(const std::string& name) const {
  for (size_t i = 0; i < headers.size(); ++i)
    if (EqualsIgnoreCase(headers[i].first, name)) return headers[i].second;
  return std::string();
}

std::string FetchResponse::MimeType() const {
  std::string ct = Header("content-type");
  std::string mime = AsciiLower(Trim(ct.substr(0, ct.find(';'))));
  if (mime.empty() || mime == "application/octet-stream" || mime == "unknown/unknown") {
    std::string sniffed = SniffMimeType(body, finalUrl);
    if (!sniffed.empty()) return sniffed;
  }
  return mime;
}

std::string FetchResponse::Charset() const {
  std::string ct = AsciiLower(Header("content-type"));
  size_t p = ct.find("charset=");
  if (p == std::string::npos) return std::string();
  std::string cs = ct.substr(p + 8);
  cs = cs.substr(0, cs.find(';'));
  cs = Trim(cs);
  if (cs.size() >= 2 && (cs[0] == '"' || cs[0] == '\'')) cs = cs.substr(1, cs.size() - 2);
  return cs;
}

std::string FetchResponse::FileName() const {
  std::string cd = Header("content-disposition");
  size_t p = AsciiLower(cd).find("filename=");
  if (p != std::string::npos) {
    std::string f = Trim(cd.substr(p + 9));
    f = f.substr(0, f.find(';'));
    if (f.size() >= 2 && f[0] == '"') f = f.substr(1, f.find('"', 1) - 1);
    if (!f.empty()) return f;
  }
  Url u = Url::Parse(finalUrl);
  std::string path = PercentDecode(u.path(), false);
  std::string name = path.substr(path.rfind('/') + 1);
  return name.empty() ? "download" : name;
}

std::string SniffMimeType(const std::string& body, const std::string& url) {
  if (body.size() >= 8 && body.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return "image/png";
  if (body.size() >= 3 && (unsigned char)body[0] == 0xFF && (unsigned char)body[1] == 0xD8)
    return "image/jpeg";
  if (body.size() >= 6 && (body.compare(0, 6, "GIF87a") == 0 || body.compare(0, 6, "GIF89a") == 0))
    return "image/gif";
  if (body.size() >= 2 && body[0] == 'B' && body[1] == 'M') return "image/bmp";
  std::string lower = AsciiLower(url);
  size_t q = lower.find_first_of("?#");
  if (q != std::string::npos) lower = lower.substr(0, q);
  if (EndsWith(lower, ".html") || EndsWith(lower, ".htm")) return "text/html";
  if (EndsWith(lower, ".txt") || EndsWith(lower, ".log") || EndsWith(lower, ".ini") ||
      EndsWith(lower, ".c") || EndsWith(lower, ".cpp") || EndsWith(lower, ".h") ||
      EndsWith(lower, ".md"))
    return "text/plain";
  if (EndsWith(lower, ".css")) return "text/css";
  if (EndsWith(lower, ".png")) return "image/png";
  if (EndsWith(lower, ".jpg") || EndsWith(lower, ".jpeg")) return "image/jpeg";
  if (EndsWith(lower, ".gif")) return "image/gif";
  if (EndsWith(lower, ".bmp")) return "image/bmp";
  std::string head = AsciiLower(Trim(body.substr(0, 512)));
  if (EndsWith(lower, ".svg") || (head.find("<svg") != std::string::npos &&
                                  head.find("<html") == std::string::npos))
    return "image/svg+xml";
  if (StartsWith(head, "<!doctype html") || StartsWith(head, "<html") ||
      head.find("<head") != std::string::npos || head.find("<body") != std::string::npos)
    return "text/html";
  bool binary = false;
  for (size_t i = 0; i < body.size() && i < 512; ++i) {
    unsigned char c = body[i];
    if (c < 9 || (c > 13 && c < 32)) binary = true;
  }
  if (!body.empty() && !binary) return "text/plain";
  return std::string();
}

bool Inflate(const std::string& in, const std::string& enc, std::string& out) {
  std::string e = AsciiLower(Trim(enc));
  if (e.empty() || e == "identity") {
    out = in;
    return true;
  }
  const char* data = in.data();
  int len = (int)in.size();
  int parseHeader = 0;
  if (e == "gzip" || e == "x-gzip") {
    if (len < 18 || (unsigned char)data[0] != 0x1f || (unsigned char)data[1] != 0x8b) return false;
    unsigned char flags = data[3];
    int p = 10;
    if (flags & 4) {
      if (p + 2 > len) return false;
      p += 2 + ((unsigned char)data[p] | ((unsigned char)data[p + 1] << 8));
    }
    if (flags & 8) while (p < len && data[p++]) {}
    if (flags & 16) while (p < len && data[p++]) {}
    if (flags & 2) p += 2;
    if (p >= len) return false;
    data += p;
    len -= p;
  } else if (e == "deflate") {
    // Usually zlib-wrapped; some servers send raw deflate.
    parseHeader = len >= 2 && (((unsigned char)data[0] << 8) | (unsigned char)data[1]) % 31 == 0 &&
                          ((unsigned char)data[0] & 0x0f) == 8;
  } else if (e == "br") {
    BrotliDecoderState* st = BrotliDecoderCreateInstance(0, 0, 0);
    if (!st) return false;
    const uint8_t* next = (const uint8_t*)in.data();
    size_t avail = in.size();
    out.clear();
    BrotliDecoderResult res;
    do {
      uint8_t buf[65536];
      uint8_t* o = buf;
      size_t room = sizeof buf;
      res = BrotliDecoderDecompressStream(st, &avail, &next, &room, &o, 0);
      out.append((const char*)buf, sizeof buf - room);
      if (out.size() > 256u * 1024 * 1024) res = BROTLI_DECODER_RESULT_ERROR;
    } while (res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);
    BrotliDecoderDestroyInstance(st);
    return res == BROTLI_DECODER_RESULT_SUCCESS;
  } else {
    return false;  // zstd ... not supported (we never ask for it)
  }
  int outLen = 0;
  char* r = stbi_zlib_decode_malloc_guesssize_headerflag(data, len, len * 4 + 1024, &outLen,
                                                          parseHeader);
  if (!r) return false;
  out.assign(r, outLen);
  free(r);
  return true;
}

// ---------------------------------------------------------------------------
// Cookies

void CookieJar::SetFromHeader(const Url& url, const std::string& header) {
  std::vector<std::string> parts = Split(header, ';');
  if (parts.empty()) return;
  size_t eq = parts[0].find('=');
  if (eq == std::string::npos) return;
  Cookie c;
  c.name = Trim(parts[0].substr(0, eq));
  c.value = Trim(parts[0].substr(eq + 1));
  if (c.name.empty()) return;
  c.domain = url.host();
  c.hostOnly = true;
  std::string path = url.path();
  size_t slash = path.rfind('/');
  c.path = slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
  c.secure = false;
  c.expires = 0;
  long long now = NowUnix();
  for (size_t i = 1; i < parts.size(); ++i) {
    std::string a = Trim(parts[i]);
    size_t e = a.find('=');
    std::string key = AsciiLower(Trim(a.substr(0, e)));
    std::string val = e == std::string::npos ? std::string() : Trim(a.substr(e + 1));
    if (key == "domain" && !val.empty()) {
      if (val[0] == '.') val = val.substr(1);
      val = AsciiLower(val);
      if (!DomainMatch(url.host(), val)) return;  // foreign domain: reject
      if (val.find('.') == std::string::npos && val != "localhost") return;
      c.domain = val;
      c.hostOnly = false;
    } else if (key == "path" && !val.empty() && val[0] == '/') {
      c.path = val;
    } else if (key == "secure") {
      c.secure = true;
    } else if (key == "max-age") {
      long long v;
      if (ParseInt(val, v)) c.expires = v <= 0 ? 1 : now + v;
    } else if (key == "expires" && c.expires == 0) {
      long long t = ParseHttpDate(val);
      if (t > 0) c.expires = t;
    }
  }
  MutexLock l(mu_);
  for (size_t i = 0; i < cookies_.size(); ++i) {
    if (cookies_[i].name == c.name && cookies_[i].domain == c.domain && cookies_[i].path == c.path) {
      cookies_.erase(cookies_.begin() + i);
      break;
    }
  }
  if (c.expires != 0 && c.expires <= now) return;  // deletion
  if (cookies_.size() > 3000) cookies_.erase(cookies_.begin());
  cookies_.push_back(c);
}

std::string CookieJar::CookieHeader(const Url& url) {
  MutexLock l(mu_);
  long long now = NowUnix();
  std::string out;
  const std::string& host = url.host();
  std::string path = url.path().empty() ? "/" : url.path();
  for (size_t i = 0; i < cookies_.size(); ++i) {
    const Cookie& c = cookies_[i];
    if (c.expires != 0 && c.expires <= now) continue;
    if (c.hostOnly ? host != c.domain : !DomainMatch(host, c.domain)) continue;
    if (!(path == c.path || (StartsWith(path, c.path) &&
                             (c.path[c.path.size() - 1] == '/' || path[c.path.size()] == '/'))))
      continue;
    if (c.secure && url.scheme() != "https") continue;
    if (!out.empty()) out += "; ";
    out += c.name + "=" + c.value;
  }
  return out;
}

void CookieJar::Clear() {
  MutexLock l(mu_);
  cookies_.clear();
}

size_t CookieJar::Count() {
  MutexLock l(mu_);
  return cookies_.size();
}

std::string CookieJar::Serialize() {
  MutexLock l(mu_);
  std::string out;
  long long now = NowUnix();
  for (size_t i = 0; i < cookies_.size(); ++i) {
    const Cookie& c = cookies_[i];
    if (c.expires == 0 || c.expires <= now) continue;  // session cookies are not saved
    out += c.domain + "\t" + (c.hostOnly ? "0" : "1") + "\t" + c.path + "\t" +
           (c.secure ? "1" : "0") + "\t" + IntToString(c.expires) + "\t" + c.name + "\t" +
           c.value + "\n";
  }
  return out;
}

void CookieJar::Deserialize(const std::string& data) {
  MutexLock l(mu_);
  std::vector<std::string> lines = Split(data, '\n');
  for (size_t i = 0; i < lines.size(); ++i) {
    std::vector<std::string> f = Split(lines[i], '\t');
    if (f.size() != 7) continue;
    Cookie c;
    c.domain = f[0];
    c.hostOnly = f[1] == "0";
    c.path = f[2];
    c.secure = f[3] == "1";
    ParseInt(f[4], c.expires);
    c.name = f[5];
    c.value = f[6];
    cookies_.push_back(c);
  }
}

// ---------------------------------------------------------------------------
// Network

bool ProxyConfig::UsedFor(const std::string& h) const {
  if (!enabled()) return false;
  std::string host = AsciiLower(h);
  return !(host == "localhost" || StartsWith(host, "127.") || host == "[::1]" || host == "::1" ||
           EndsWith(host, ".localhost"));
}

Network& Network::Get() {
  static Network* n = new Network;
  return *n;
}

Network::Network()
    : userAgent_("Mozilla/5.0 (Windows NT 5.0) Kite/1.0 (like Gecko)") {}

FetchResponse Network::Fetch(const FetchRequest& req) {
  FetchResponse resp;
  Url url = Url::Parse(req.url);
  if (!url.valid()) {
    resp.error = "Ungültige Adresse: " + req.url;
    resp.finalUrl = req.url;
    return resp;
  }
  if (url.scheme() == "file") return FetchFile(url);
  if (url.scheme() == "data") return FetchData(url);
  if (url.scheme() == "about") {
    resp.ok = true;
    resp.status = 200;
    resp.finalUrl = url.Spec();
    resp.headers.push_back(std::make_pair(std::string("Content-Type"), std::string("text/html")));
    return resp;
  }
  if (url.scheme() != "http" && url.scheme() != "https") {
    resp.error = "Nicht unterstütztes Protokoll: " + url.scheme();
    resp.finalUrl = req.url;
    return resp;
  }
  std::string method = req.method;
  std::string body = req.body;
  for (int redirects = 0; redirects < 15; ++redirects) {
    resp = FetchHttp(url, req, method, body);
    resp.finalUrl = url.Spec();
    if (!resp.ok) return resp;
    int st = resp.status;
    if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
      std::string loc = resp.Header("location");
      if (loc.empty()) return resp;
      Url next = url.Resolve(loc);
      if (!next.valid()) return resp;
      if (!next.HasFragment() && url.HasFragment()) {
        // Keep the original fragment.
        next = Url::Parse(next.Spec() + "#" + url.fragment());
      }
      if (st == 303 || ((st == 301 || st == 302) && method == "POST")) {
        method = "GET";
        body.clear();
      }
      url = next;
      if (url.scheme() != "http" && url.scheme() != "https") {
        resp.ok = false;
        resp.error = "Weiterleitung auf nicht unterstütztes Protokoll";
        return resp;
      }
      continue;
    }
    return resp;
  }
  resp.ok = false;
  resp.error = "Zu viele Weiterleitungen";
  return resp;
}

namespace {

class Reader {
 public:
  explicit Reader(Stream* s) : s_(s), pos_(0) {}
  // Ensures at least n buffered bytes; returns false on EOF/error.
  bool Fill(size_t n) {
    while (buf_.size() - pos_ < n) {
      char tmp[16384];
      int r = s_->Read(tmp, sizeof tmp);
      if (r <= 0) return false;
      Compact();
      buf_.append(tmp, r);
    }
    return true;
  }
  bool ReadLine(std::string& line) {
    for (;;) {
      size_t nl = buf_.find('\n', pos_);
      if (nl != std::string::npos) {
        line = buf_.substr(pos_, nl - pos_);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        pos_ = nl + 1;
        return true;
      }
      if (buf_.size() - pos_ > 65536) return false;
      if (!Fill(buf_.size() - pos_ + 1)) return false;
    }
  }
  // Appends up to n bytes to out. Returns bytes read (0 = EOF).
  size_t Read(std::string& out, size_t n) {
    if (pos_ >= buf_.size() && !Fill(1)) return 0;
    size_t take = std::min(n, buf_.size() - pos_);
    out.append(buf_, pos_, take);
    pos_ += take;
    return take;
  }

 private:
  void Compact() {
    if (pos_ > 0) {
      buf_.erase(0, pos_);
      pos_ = 0;
    }
  }
  Stream* s_;
  std::string buf_;
  size_t pos_;
};

}  // namespace

namespace {

// Clears the cancel token's socket when the request ends (the descriptor
// number may be reused by a later connection).
struct CancelSocketGuard {
  CancelToken* token;
  ~CancelSocketGuard() {
    if (token) token->SetSocket(-1);
  }
};

}  // namespace

std::shared_ptr<Http2Connection> Network::PooledConnection(const std::string& key) {
  MutexLock l(mu_);
  std::shared_ptr<Http2Connection> best;
  for (size_t i = 0; i < h2Pool_.size();) {
    if (!h2Pool_[i].second->usable()) {
      h2Pool_.erase(h2Pool_.begin() + i);
      continue;
    }
    if (h2Pool_[i].first == key && h2Pool_[i].second->activeStreams() < 64 &&
        (!best || h2Pool_[i].second->activeStreams() < best->activeStreams()))
      best = h2Pool_[i].second;
    ++i;
  }
  return best;
}

void Network::FinishResponse(const Url& url, const std::string& method, FetchResponse& resp, std::string& raw) {
  for (size_t i = 0; i < resp.headers.size(); ++i)
    if (EqualsIgnoreCase(resp.headers[i].first, "set-cookie"))
      cookies_.SetFromHeader(url, resp.headers[i].second);
  std::string ce = resp.Header("content-encoding");
  if (!ce.empty() && !Inflate(raw, ce, resp.body)) resp.body = raw;
  else if (ce.empty()) resp.body.swap(raw);
  resp.ok = true;
}

namespace {

bool ForbiddenHeader(const std::string& lower) {
  static const char* const names[] = {"host", "connection", "content-length", "cookie", "cookie2", "date", "expect",
                                      "keep-alive", "te", "trailer", "transfer-encoding", "upgrade", "via", "referer",
                                      "user-agent", "accept-encoding", "content-type", "accept", "origin", 0};
  for (int i = 0; names[i]; ++i)
    if (lower == names[i]) return true;
  return StartsWith(lower, "proxy-") || StartsWith(lower, "sec-") || lower.empty() || lower[0] == ':';
}

}  // namespace

bool Network::FetchHttp2(const std::shared_ptr<Http2Connection>& conn, const Url& url, const FetchRequest& req,
                         const std::string& method, const std::string& body, FetchResponse& resp) {
  HeaderList h;
  h.push_back(std::make_pair(std::string("user-agent"), userAgent()));
  h.push_back(std::make_pair(std::string("accept"),
                             req.accept.empty() ? std::string("text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/png,image/jpeg,image/gif,*/*;q=0.8")
                                                : req.accept));
  h.push_back(std::make_pair(std::string("accept-language"), std::string("de-DE,de;q=0.9,en;q=0.7")));
  h.push_back(std::make_pair(std::string("accept-encoding"), std::string("gzip, deflate, br")));
  h.push_back(std::make_pair(std::string("upgrade-insecure-requests"), std::string("1")));
  std::string cookie = cookies_.CookieHeader(url);
  if (!cookie.empty()) h.push_back(std::make_pair(std::string("cookie"), cookie));
  if (!req.referrer.empty()) {
    Url ref = Url::Parse(req.referrer);
    if (ref.valid()) {
      ref.ClearFragment();
      h.push_back(std::make_pair(std::string("referer"), ref.Spec()));
    }
  }
  for (size_t i = 0; i < req.headers.size(); ++i) {
    std::string name = AsciiLower(Trim(req.headers[i].first));
    if (!ForbiddenHeader(name)) h.push_back(std::make_pair(name, req.headers[i].second));
  }
  if (method == "POST" || !body.empty()) {
    h.push_back(std::make_pair(std::string("content-type"),
                               req.contentType.empty() ? std::string("application/x-www-form-urlencoded") : req.contentType));
    h.push_back(std::make_pair(std::string("content-length"), IntToString((long long)body.size())));
  }
  Http2Result r = conn->Request(method, "https", url.HostPort(), url.PathAndQuery(), h, body, req.cancel.get(),
                                req.maxBytes, req.progress, req.progressCtx);
  resp.finalUrl = url.Spec();
  resp.secure = true;
  resp.protocol = "h2";
  if (!r.ok) {
    // Idempotent requests are repeated on a fresh connection.
    if (r.retryable && (method == "GET" || method == "HEAD")) return false;
    resp.error = r.error;
    return true;
  }
  resp.status = r.status;
  resp.headers.swap(r.headers);
  bool hasBody = method != "HEAD" && resp.status != 204 && resp.status != 304;
  if (!hasBody) r.body.clear();
  FinishResponse(url, method, resp, r.body);
  return true;
}

FetchResponse Network::FetchHttp(const Url& url, const FetchRequest& req, const std::string& method,
                                 const std::string& body) {
  FetchResponse resp;
  resp.finalUrl = url.Spec();
  bool tls = url.scheme() == "https";
  resp.secure = tls;
  resp.protocol = "http/1.1";
  ProxyConfig proxy = this->proxy();
  if (!proxy.UsedFor(url.host())) proxy = ProxyConfig();
  std::string ua = userAgent();
  std::string poolKey = url.host() + ":" + IntToString(url.EffectivePort()) + "|" +
                        (proxy.enabled() ? proxy.host + ":" + IntToString(proxy.port) : std::string());
  if (tls) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      std::shared_ptr<Http2Connection> conn = PooledConnection(poolKey);
      if (!conn) {
        // A known HTTP/2 server with a connection being set up right now:
        // wait for it instead of opening another one.
        bool wait;
        {
          MutexLock l(mu_);
          wait = std::find(h2Known_.begin(), h2Known_.end(), poolKey) != h2Known_.end() &&
                 std::find(connecting_.begin(), connecting_.end(), poolKey) != connecting_.end();
        }
        for (int i = 0; wait && i < 500 && !conn; ++i) {
          if (req.cancel && req.cancel->cancelled()) break;
#ifdef _WIN32
          Sleep(20);
#else
          usleep(20000);
#endif
          conn = PooledConnection(poolKey);
          MutexLock l(mu_);
          wait = std::find(connecting_.begin(), connecting_.end(), poolKey) != connecting_.end();
        }
        if (!conn) break;
      }
      if (FetchHttp2(conn, url, req, method, body, resp)) return resp;
    }
  }
  struct ConnectingMark {
    Network* net;
    std::string key;
    bool active;
    void Done() {
      if (!active) return;
      active = false;
      MutexLock l(net->mu_);
      std::vector<std::string>::iterator it = std::find(net->connecting_.begin(), net->connecting_.end(), key);
      if (it != net->connecting_.end()) net->connecting_.erase(it);
    }
    ~ConnectingMark() { Done(); }
  } connecting = {this, poolKey, tls};
  if (tls) {
    MutexLock l(mu_);
    connecting_.push_back(poolKey);
  }
  CancelSocketGuard cancelGuard = {req.cancel.get()};
  std::unique_ptr<TcpSocket> sockOwner(new TcpSocket);
  TcpSocket& sock = *sockOwner;
  std::string connectHost = proxy.enabled() ? proxy.host : url.host();
  int connectPort = proxy.enabled() ? proxy.port : url.EffectivePort();
  if (!sock.Connect(connectHost, connectPort, 20000, req.cancel.get())) {
    resp.error = sock.error();
    return resp;
  }
  if (proxy.enabled() && tls) {
    std::string connect = "CONNECT " + url.host() + ":" + IntToString(url.EffectivePort()) +
                          " HTTP/1.1\r\nHost: " + url.host() + ":" +
                          IntToString(url.EffectivePort()) + "\r\nUser-Agent: " + ua + "\r\n\r\n";
    if (!sock.WriteAll(connect.data(), (int)connect.size())) {
      resp.error = "Proxy-Verbindung fehlgeschlagen";
      return resp;
    }
    // Read the proxy reply byte-wise so no TLS data is consumed.
    std::string reply;
    char c;
    while (reply.find("\r\n\r\n") == std::string::npos && reply.size() < 16384) {
      if (sock.Read(&c, 1) != 1) break;
      reply += c;
    }
    if (reply.find(" 200") == std::string::npos || reply.find(" 200") > reply.find("\r\n")) {
      resp.error = "Proxy hat die Verbindung abgelehnt: " + reply.substr(0, reply.find("\r\n"));
      return resp;
    }
  }
  std::unique_ptr<TlsStream> tlsStream;
  Stream* stream = &sock;
  if (tls) {
    static const char* const kAlpn[] = {"h2", "http/1.1"};
    tlsStream.reset(new TlsStream(&sock));
    bool offerH2 = http2Enabled();
    if (!tlsStream->Handshake(url.host(), offerH2 ? kAlpn : 0, offerH2 ? 2 : 0)) {
      resp.error = "Sichere Verbindung fehlgeschlagen: " + tlsStream->error();
      return resp;
    }
    if (tlsStream->selectedProtocol() == "h2") {
      // The connection is shared from now on: cancelling one request must
      // not close the socket.
      if (req.cancel) req.cancel->SetSocket(-1);
      std::shared_ptr<Http2Connection> conn(new Http2Connection(std::move(sockOwner), std::move(tlsStream)));
      if (!conn->Start()) {
        resp.error = "HTTP/2-Verbindung fehlgeschlagen";
        return resp;
      }
      {
        MutexLock l(mu_);
        h2Pool_.push_back(std::make_pair(poolKey, conn));
        if (std::find(h2Known_.begin(), h2Known_.end(), poolKey) == h2Known_.end()) h2Known_.push_back(poolKey);
      }
      connecting.Done();
      if (!FetchHttp2(conn, url, req, method, body, resp)) resp.error = "HTTP/2-Verbindung abgelehnt";
      return resp;
    }
    stream = tlsStream.get();
  }
  std::string target = proxy.enabled() && !tls ? url.SpecNoFragment() : url.PathAndQuery();
  std::string r = method + " " + target + " HTTP/1.1\r\n";
  r += "Host: " + url.HostPort() + "\r\n";
  r += "User-Agent: " + ua + "\r\n";
  r += "Accept: " + (req.accept.empty() ? std::string("text/html,application/xhtml+xml,application/xml;q=0.9,image/png,image/jpeg,image/gif,*/*;q=0.8") : req.accept) + "\r\n";
  r += "Accept-Language: de-DE,de;q=0.9,en;q=0.7\r\n";
  r += "Accept-Encoding: gzip, deflate, br\r\n";
  r += "Connection: close\r\n";
  r += "Upgrade-Insecure-Requests: 1\r\n";
  std::string cookie = cookies_.CookieHeader(url);
  if (!cookie.empty()) r += "Cookie: " + cookie + "\r\n";
  if (!req.referrer.empty()) {
    Url ref = Url::Parse(req.referrer);
    // Do not leak https referrers to http.
    if (ref.valid() && !(ref.scheme() == "https" && !tls)) {
      ref.ClearFragment();
      r += "Referer: " + ref.Spec() + "\r\n";
    }
  }
  for (size_t i = 0; i < req.headers.size(); ++i) {
    std::string name = AsciiLower(Trim(req.headers[i].first));
    std::string value = req.headers[i].second;
    if (!ForbiddenHeader(name) && value.find_first_of("\r\n") == std::string::npos)
      r += req.headers[i].first + ": " + value + "\r\n";
  }
  if (method == "POST" || !body.empty()) {
    r += "Content-Type: " +
         (req.contentType.empty() ? std::string("application/x-www-form-urlencoded") : req.contentType) +
         "\r\n";
    r += "Content-Length: " + IntToString((long long)body.size()) + "\r\n";
  }
  r += "\r\n";
  r += body;
  if (!stream->WriteAll(r.data(), (int)r.size())) {
    resp.error = "Senden fehlgeschlagen: " + stream->error();
    return resp;
  }

  Reader rd(stream);
  std::string line;
  // Skip interim 1xx responses.
  for (;;) {
    if (!rd.ReadLine(line)) {
      resp.error = req.cancel && req.cancel->cancelled() ? "Abgebrochen"
                                                         : "Keine Antwort vom Server (" + stream->error() + ")";
      return resp;
    }
    std::vector<std::string> sl = SplitWhitespace(line);
    if (sl.size() < 2 || !StartsWith(sl[0], "HTTP/")) {
      resp.error = "Ungültige Serverantwort";
      return resp;
    }
    long long code;
    ParseInt(sl[1], code);
    resp.status = (int)code;
    resp.headers.clear();
    for (;;) {
      if (!rd.ReadLine(line)) {
        resp.error = "Verbindung unterbrochen";
        return resp;
      }
      if (line.empty()) break;
      size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      resp.headers.push_back(std::make_pair(Trim(line.substr(0, colon)), Trim(line.substr(colon + 1))));
    }
    if (resp.status >= 100 && resp.status < 200) continue;
    break;
  }
  std::string raw;
  bool hasBody = method != "HEAD" && resp.status != 204 && resp.status != 304;
  std::string te = AsciiLower(resp.Header("transfer-encoding"));
  long long total = -1;
  ParseInt(resp.Header("content-length"), total);
  if (hasBody) {
    if (te.find("chunked") != std::string::npos) {
      for (;;) {
        if (!rd.ReadLine(line)) break;
        size_t semi = line.find(';');
        std::string hex = Trim(line.substr(0, semi));
        unsigned long size = strtoul(hex.c_str(), 0, 16);
        if (size == 0) break;
        size_t got = 0;
        while (got < size) {
          size_t n = rd.Read(raw, size - got);
          if (n == 0) break;
          got += n;
          if (req.progress) req.progress(req.progressCtx, raw.size(), 0);
          if (req.cancel && req.cancel->cancelled()) break;
        }
        if (got < size) break;
        rd.ReadLine(line);  // CRLF after chunk
        if (raw.size() > req.maxBytes) break;
      }
    } else {
      for (;;) {
        size_t want = 65536;
        if (total >= 0) {
          if ((long long)raw.size() >= total) break;
          want = std::min<size_t>(want, (size_t)(total - (long long)raw.size()));
        }
        size_t n = rd.Read(raw, want);
        if (n == 0) break;
        if (req.progress) req.progress(req.progressCtx, raw.size(), total > 0 ? (size_t)total : 0);
        if (req.cancel && req.cancel->cancelled()) break;
        if (raw.size() > req.maxBytes) break;
      }
    }
  }
  if (req.cancel && req.cancel->cancelled()) {
    resp.error = "Abgebrochen";
    return resp;
  }
  FinishResponse(url, method, resp, raw);
  (void)StatusText;
  return resp;
}

FetchResponse Network::FetchData(const Url& url) {
  FetchResponse resp;
  resp.finalUrl = url.Spec();
  std::string spec = url.path();
  size_t comma = spec.find(',');
  if (comma == std::string::npos) {
    resp.error = "Ungültige data:-URL";
    return resp;
  }
  std::string meta = spec.substr(0, comma);
  std::string data = spec.substr(comma + 1);
  bool b64 = EndsWith(AsciiLower(meta), ";base64");
  if (b64) meta = meta.substr(0, meta.size() - 7);
  if (meta.empty()) meta = "text/plain;charset=US-ASCII";
  resp.body = b64 ? Base64Decode(PercentDecode(data, false)) : PercentDecode(data, false);
  resp.headers.push_back(std::make_pair(std::string("Content-Type"), meta));
  resp.status = 200;
  resp.ok = true;
  return resp;
}

FetchResponse Network::FetchFile(const Url& url) {
  FetchResponse resp;
  resp.finalUrl = url.Spec();
  std::string path = url.ToLocalPath();
#ifdef _WIN32
  std::u16string wpath = Utf8ToUtf16(path);
  DWORD attrs = GetFileAttributesW((LPCWSTR)wpath.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    std::string html = "<!DOCTYPE html><html><head><title>Index von " + HtmlEscape(path) +
                       "</title></head><body><h1>Index von " + HtmlEscape(path) + "</h1><ul>";
    std::u16string pattern = wpath;
    if (!pattern.empty() && pattern[pattern.size() - 1] != u'\\') pattern += u'\\';
    pattern += u"*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((LPCWSTR)pattern.c_str(), &fd);
    std::string base = url.Spec();
    if (!EndsWith(base, "/")) base += "/";
    if (h != INVALID_HANDLE_VALUE) {
      do {
        std::string name = Utf16ToUtf8(std::u16string((const char16_t*)fd.cFileName));
        if (name == ".") continue;
        bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        html += "<li><a href=\"" + HtmlEscape(name) + (dir ? "/" : "") + "\">" + HtmlEscape(name) +
                (dir ? "/" : "") + "</a></li>";
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    html += "</ul></body></html>";
    resp.body = html;
    resp.headers.push_back(std::make_pair(std::string("Content-Type"), std::string("text/html; charset=utf-8")));
    resp.status = 200;
    resp.ok = true;
    if (!EndsWith(resp.finalUrl, "/")) resp.finalUrl += "/";
    return resp;
  }
  FILE* f = _wfopen((const wchar_t*)wpath.c_str(), L"rb");
#else
  struct stat st;
  if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    std::string html = "<!DOCTYPE html><html><head><title>Index von " + HtmlEscape(path) +
                       "</title></head><body><h1>Index von " + HtmlEscape(path) + "</h1><ul>";
    DIR* d = opendir(path.c_str());
    if (d) {
      struct dirent* e;
      while ((e = readdir(d)) != 0) {
        std::string name = e->d_name;
        if (name == ".") continue;
        html += "<li><a href=\"" + HtmlEscape(name) + "\">" + HtmlEscape(name) + "</a></li>";
      }
      closedir(d);
    }
    html += "</ul></body></html>";
    resp.body = html;
    resp.headers.push_back(std::make_pair(std::string("Content-Type"), std::string("text/html; charset=utf-8")));
    resp.status = 200;
    resp.ok = true;
    if (!EndsWith(resp.finalUrl, "/")) resp.finalUrl += "/";
    return resp;
  }
  FILE* f = fopen(path.c_str(), "rb");
#endif
  if (!f) {
    resp.error = "Datei nicht gefunden: " + path;
    return resp;
  }
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) resp.body.append(buf, n);
  fclose(f);
  std::string mime = SniffMimeType(resp.body, url.Spec());
  if (mime.empty()) mime = "application/octet-stream";
  resp.headers.push_back(std::make_pair(std::string("Content-Type"), mime));
  resp.status = 200;
  resp.ok = true;
  return resp;
}

}  // namespace kite
