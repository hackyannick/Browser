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
#include "base/thread.h"
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
const char* g_testServer = 0;
int g_testDelayMs = 0;

long long NowSeconds() {
#ifdef _WIN32
  return (long long)(GetTickCount() / 1000);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
#endif
}

long long NowMs() {
#ifdef _WIN32
  return (long long)GetTickCount();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

// Asks several public DNS servers at once and takes the first answer.
// |nxdomain| is set when a server says the name does not exist.
bool DirectResolve(const std::string& host, unsigned long& ip, long& ttl, bool& nxdomain) {
  static const char* const kServers[] = {"1.1.1.1", "8.8.8.8", "9.9.9.9"};
  static volatile long counter = 0;
  unsigned short id = (unsigned short)((NowSeconds() * 7919 + (++counter) * 40503 + (long long)(size_t)&host) & 0xffff);
  std::string q = BuildDnsQuery(host, id);
  nxdomain = false;
  if (q.empty()) return false;
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return false;
#else
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return false;
#endif
  bool ok = false;
  // Two rounds of 1.5 s; each round sends the query to every server.
  for (int round = 0; round < 2 && !ok && !nxdomain; ++round) {
    int sent = 0;
    for (size_t i = 0; i < sizeof kServers / sizeof kServers[0]; ++i) {
      if (g_testServer && i > 0) break;
      struct sockaddr_in to;
      memset(&to, 0, sizeof to);
      to.sin_family = AF_INET;
      to.sin_port = htons(53);
      to.sin_addr.s_addr = inet_addr(g_testServer ? g_testServer : kServers[i]);
      if (sendto(s, q.data(), (int)q.size(), 0, (struct sockaddr*)&to, sizeof to) == (int)q.size()) ++sent;
    }
    if (!sent) break;
    long long until = NowMs() + 1500;
    for (;;) {
      long long left = until - NowMs();
      if (left <= 0) break;
      fd_set rs;
      FD_ZERO(&rs);
      FD_SET(s, &rs);
      struct timeval tv;
      tv.tv_sec = (long)(left / 1000);
      tv.tv_usec = (long)(left % 1000) * 1000;
      if (select((int)s + 1, &rs, 0, 0, &tv) <= 0) break;
      char buf[1500];
      int n = (int)recv(s, buf, sizeof buf, 0);
      if (n <= 0) {
#ifdef _WIN32
        // ICMP "port unreachable" from one server shows up as an error
        // (WSAECONNRESET); the others may still answer.
        if (WSAGetLastError() == WSAECONNRESET) continue;
#endif
        break;
      }
      int rc = 0;
      if (ParseDnsResponse(std::string(buf, n), id, ip, ttl, rc)) {
        ok = true;
        break;
      }
      if (rc == 3) {
        nxdomain = true;
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

// The system resolver runs on its own thread so a slow DNS client (the
// Windows 2000 resolver can take well over ten seconds when a configured
// server does not answer) never holds up the direct lookup.
struct SystemLookup {
  Mutex mu;
  int refs = 2;
  bool done = false, ok = false;
  unsigned long ip = 0;
  int code = 0;
  std::string host;
  WakeEvent event;

  void Release() {
    bool last;
    {
      MutexLock l(mu);
      last = --refs == 0;
    }
    if (last) delete this;
  }
  static void Run(void* p) {
    SystemLookup* s = (SystemLookup*)p;
    unsigned long ip = 0;
    int code = 0;
    if (g_testDelayMs > 0) {
#ifdef _WIN32
      Sleep(g_testDelayMs);
#else
      usleep(g_testDelayMs * 1000);
#endif
    }
    bool ok = SystemResolve(s->host, ip, code);
    {
      MutexLock l(s->mu);
      s->done = true;
      s->ok = ok;
      s->ip = ip;
      s->code = code;
    }
    s->event.Signal();
    s->Release();
  }
  // True once finished (within |ms|).
  bool Wait(int ms) {
    long long until = NowMs() + ms;
    for (;;) {
      {
        MutexLock l(mu);
        if (done) return true;
      }
      long long left = until - NowMs();
      if (left <= 0) return false;
      event.Wait((int)left);
    }
  }
};

// Set when the system resolver turned out slow or broken while the direct
// lookup worked; later names are then looked up directly first.
volatile bool g_preferDirect = false;

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
  g_preferDirect = false;
}

void SetDnsTestHooks(const char* server, int systemDelayMs) {
  g_testServer = server;
  g_testDelayMs = systemDelayMs;
}

bool ResolveHost(const std::string& hostIn, unsigned long& ip, std::string& error) {
  NetInit();
  std::string host = AsciiLower(hostIn);
  if (!host.empty() && host[host.size() - 1] == '.') host.erase(host.size() - 1);
  if (CacheGet(host, ip)) return true;
  if (host == "localhost" || EndsWith(host, ".localhost")) {
    ip = htonl(0x7f000001);
    return true;
  }
  // Names without a dot are local (WINS, hosts file); only the system knows them.
  bool direct = host.find('.') != std::string::npos && host != "localhost" && !EndsWith(host, ".localhost") &&
                !EndsWith(host, ".local");
  long ttl = 300;
  bool nxdomain = false, triedDirect = false;
  if (direct && g_preferDirect) {
    triedDirect = true;
    if (DirectResolve(host, ip, ttl, nxdomain)) {
      CachePut(host, ip, ttl);
      return true;
    }
  }
  SystemLookup* sys = new SystemLookup;
  sys->host = host;
  if (!StartThread(&SystemLookup::Run, sys)) SystemLookup::Run(sys);  // releases its reference
  // A name the public DNS does not know may still be an internal one, but
  // a resolver that already proved slow gets only a few seconds for it.
  bool finished = sys->Wait(direct && !triedDirect ? 1500 : nxdomain ? 5000 : 30000);
  if (!finished && direct && !triedDirect) {
    // The system is slow: ask directly meanwhile.
    triedDirect = true;
    if (DirectResolve(host, ip, ttl, nxdomain)) {
      g_preferDirect = true;
      CachePut(host, ip, ttl);
      sys->Release();
      return true;
    }
    finished = sys->Wait(nxdomain ? 5000 : 25000);
  }
  bool ok = false;
  int code = 0;
  {
    MutexLock l(sys->mu);
    if (sys->done) {
      ok = sys->ok;
      ip = sys->ip;
      code = sys->code;
    }
  }
  sys->Release();
  if (ok) {
    CachePut(host, ip, 300);
    return true;
  }
  // The system failed (the Windows 2000 resolver also keeps failures in a
  // negative cache): try directly if not done yet.
  if (direct && !triedDirect && DirectResolve(host, ip, ttl, nxdomain)) {
    g_preferDirect = true;
    CachePut(host, ip, ttl);
    return true;
  }
  error = "Host nicht gefunden: " + host;
  if (nxdomain) error += " (Name existiert nicht)";
  else if (!finished) error += " (DNS antwortet nicht)";
  else if (code) error += " (Fehler " + IntToString(code) + ")";
  return false;
}

}  // namespace kite
