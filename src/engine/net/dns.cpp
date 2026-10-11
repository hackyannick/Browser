#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

#include "net/dns.h"

#include <cstring>
#include <map>

#include "base/mutex.h"
#include "base/strings.h"
#include "net/socket.h"

namespace kite {

namespace {

struct CacheEntry {
  unsigned long ip;
  long long expires;  // seconds
};

Mutex g_mu;
std::map<std::string, CacheEntry>* g_cache = 0;

long long NowSeconds() {
#ifdef _WIN32
  return (long long)(GetTickCount() / 1000);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
#endif
}

void SleepMs(int ms) {
#ifdef _WIN32
  Sleep(ms);
#else
  usleep(ms * 1000);
#endif
}

bool CacheGet(const std::string& host, unsigned long& ip) {
  MutexLock l(g_mu);
  if (!g_cache) return false;
  std::map<std::string, CacheEntry>::iterator it = g_cache->find(host);
  if (it == g_cache->end()) return false;
  long long now = NowSeconds();
  if (now > it->second.expires || now + 3600 < it->second.expires) {  // also tick wrap
    g_cache->erase(it);
    return false;
  }
  ip = it->second.ip;
  return true;
}

void CachePut(const std::string& host, unsigned long ip, long ttl) {
  if (ttl < 60) ttl = 60;
  if (ttl > 600) ttl = 600;
  MutexLock l(g_mu);
  if (!g_cache) g_cache = new std::map<std::string, CacheEntry>;
  if (g_cache->size() > 500) g_cache->clear();
  CacheEntry e;
  e.ip = ip;
  e.expires = NowSeconds() + ttl;
  (*g_cache)[host] = e;
}

bool SystemResolve(const std::string& host, unsigned long& ip, int& code) {
#ifdef _WIN32
  // gethostbyname uses thread-local storage, so it is safe in worker threads.
  struct hostent* he = gethostbyname(host.c_str());
  if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) {
    code = WSAGetLastError();
    return false;
  }
  memcpy(&ip, he->h_addr_list[0], 4);
  return true;
#else
  struct addrinfo hints, *res = 0;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  int r = getaddrinfo(host.c_str(), 0, &hints, &res);
  if (r != 0 || !res) {
    code = r;
    return false;
  }
  memcpy(&ip, &((struct sockaddr_in*)res->ai_addr)->sin_addr, 4);
  freeaddrinfo(res);
  return true;
#endif
}

bool TransientError(int code) {
#ifdef _WIN32
  return code == WSATRY_AGAIN || code == WSANO_RECOVERY || code == WSAENETDOWN || code == 0;
#else
  return code == EAI_AGAIN;
#endif
}

// One UDP query to |server| (dotted IPv4); |rcode| 3 means the name does not exist.
bool UdpQuery(const std::string& host, const char* server, unsigned long& ip, long& ttl, int& rcode) {
  unsigned short id = (unsigned short)((NowSeconds() * 7919 + (long long)(size_t)&host) & 0xffff);
  std::string q = BuildDnsQuery(host, id);
  if (q.empty()) return false;
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return false;
#else
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return false;
#endif
  struct sockaddr_in to;
  memset(&to, 0, sizeof to);
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  to.sin_addr.s_addr = inet_addr(server);
  bool ok = false;
  for (int attempt = 0; attempt < 1 && !ok; ++attempt) {
    if (sendto(s, q.data(), (int)q.size(), 0, (struct sockaddr*)&to, sizeof to) != (int)q.size()) break;
    for (;;) {
      fd_set rs;
      FD_ZERO(&rs);
      FD_SET(s, &rs);
      struct timeval tv;
      tv.tv_sec = 2;
      tv.tv_usec = 0;
      if (select((int)s + 1, &rs, 0, 0, &tv) <= 0) break;
      char buf[1500];
      int n = (int)recv(s, buf, sizeof buf, 0);
      if (n <= 0) break;
      int rc = 0;
      if (ParseDnsResponse(std::string(buf, n), id, ip, ttl, rc)) {
        ok = true;
        break;
      }
      if (rc != 0) {
        rcode = rc;
        attempt = 2;
        break;
      }
    }
  }
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
  return ok;
}

unsigned short Get16(const std::string& p, size_t at) {
  return (unsigned short)(((unsigned char)p[at] << 8) | (unsigned char)p[at + 1]);
}

// Skips a (possibly compressed) name; returns the offset after it or 0.
size_t SkipName(const std::string& p, size_t at) {
  while (at < p.size()) {
    unsigned char len = (unsigned char)p[at];
    if (len == 0) return at + 1;
    if ((len & 0xc0) == 0xc0) return at + 2 <= p.size() ? at + 2 : 0;
    at += 1 + len;
  }
  return 0;
}

}  // namespace

std::string BuildDnsQuery(const std::string& host, unsigned short id) {
  std::string q;
  q += (char)(id >> 8);
  q += (char)(id & 0xff);
  q += std::string("\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);  // RD, one question
  std::vector<std::string> labels = Split(host, '.');
  for (size_t i = 0; i < labels.size(); ++i) {
    if (labels[i].empty()) {
      if (i + 1 == labels.size()) break;  // trailing dot
      return std::string();
    }
    if (labels[i].size() > 63) return std::string();
    q += (char)labels[i].size();
    q += labels[i];
  }
  q += std::string("\x00\x00\x01\x00\x01", 5);  // root, type A, class IN
  return q;
}

bool ParseDnsResponse(const std::string& p, unsigned short id, unsigned long& ip, long& ttl, int& rcode) {
  rcode = 0;
  if (p.size() < 12 || Get16(p, 0) != id || !(p[2] & 0x80)) return false;
  rcode = p[3] & 0x0f;
  if (rcode != 0) return false;
  int qd = Get16(p, 4), an = Get16(p, 6);
  size_t at = 12;
  for (int i = 0; i < qd; ++i) {
    at = SkipName(p, at);
    if (!at || at + 4 > p.size()) return false;
    at += 4;
  }
  for (int i = 0; i < an; ++i) {
    at = SkipName(p, at);
    if (!at || at + 10 > p.size()) return false;
    unsigned short type = Get16(p, at), cls = Get16(p, at + 2);
    unsigned long t = ((unsigned long)Get16(p, at + 4) << 16) | Get16(p, at + 6);
    unsigned short len = Get16(p, at + 8);
    at += 10;
    if (at + len > p.size()) return false;
    if (type == 1 && cls == 1 && len == 4) {
      memcpy(&ip, p.data() + at, 4);
      ttl = (long)(t > 86400 ? 86400 : t);
      return true;
    }
    at += len;
  }
  rcode = 3;  // no address for this name
  return false;
}

void ClearDnsCache() {
  MutexLock l(g_mu);
  if (g_cache) g_cache->clear();
}

bool ResolveHost(const std::string& hostIn, unsigned long& ip, std::string& error) {
  NetInit();
  std::string host = AsciiLower(hostIn);
  if (!host.empty() && host[host.size() - 1] == '.') host.erase(host.size() - 1);
  if (CacheGet(host, ip)) return true;
  int code = 0;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (SystemResolve(host, ip, code)) {
      CachePut(host, ip, 300);
      return true;
    }
    if (!TransientError(code)) break;
    SleepMs(300 * (attempt + 1));
  }
  // Names without a dot are local (WINS, hosts file); only the system knows them.
  if (host.find('.') != std::string::npos && host != "localhost") {
    static const char* const kServers[] = {"1.1.1.1", "8.8.8.8", "9.9.9.9"};
    for (size_t i = 0; i < sizeof kServers / sizeof kServers[0]; ++i) {
      long ttl = 300;
      int rcode = 0;
      if (UdpQuery(host, kServers[i], ip, ttl, rcode)) {
        CachePut(host, ip, ttl);
        return true;
      }
      if (rcode == 3) break;  // the name really does not exist
    }
  }
  error = "Host nicht gefunden: " + host;
  if (code) error += " (Fehler " + IntToString(code) + ")";
  return false;
}

}  // namespace kite
