#include "net/socket.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cstring>

#include "base/strings.h"

namespace kite {

#ifdef _WIN32
static void CloseRaw(long long fd) { closesocket((SOCKET)fd); }
static int LastSocketError() { return WSAGetLastError(); }
#else
static void CloseRaw(long long fd) {
  shutdown((int)fd, SHUT_RDWR);
  close((int)fd);
}
static int LastSocketError() { return errno; }
#endif

void NetInit() {
#ifdef _WIN32
  static bool done = false;
  if (!done) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    done = true;
  }
#endif
}

void CancelToken::Cancel() {
  MutexLock l(mu_);
  cancelled_ = true;
  if (socket_ >= 0) {
#ifdef _WIN32
    // Closing from another thread aborts a blocking recv() on Winsock.
    shutdown((SOCKET)socket_, 2);
    closesocket((SOCKET)socket_);
#else
    shutdown((int)socket_, SHUT_RDWR);
#endif
    socket_ = -1;
  }
}

void CancelToken::SetSocket(long long s) {
  MutexLock l(mu_);
  socket_ = s;
}

TcpSocket::TcpSocket() : fd_(-1) {}
TcpSocket::~TcpSocket() { Close(); }

void TcpSocket::Close() {
  if (fd_ >= 0) {
    CloseRaw(fd_);
    fd_ = -1;
  }
}

bool TcpSocket::Connect(const std::string& host, int port, int timeoutMs, CancelToken* cancel) {
  NetInit();
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  unsigned long ip = inet_addr(host.c_str());
  if (ip == INADDR_NONE || host.find_first_not_of("0123456789.") != std::string::npos) {
#ifdef _WIN32
    // gethostbyname is available on Windows 2000 (getaddrinfo is not) and
    // uses thread-local storage, so it is safe in worker threads.
    struct hostent* he = gethostbyname(host.c_str());
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) {
      error_ = "Host nicht gefunden: " + host;
      return false;
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], 4);
#else
    struct addrinfo hints, *res = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), 0, &hints, &res) != 0 || !res) {
      error_ = "Host nicht gefunden: " + host;
      return false;
    }
    memcpy(&addr.sin_addr, &((struct sockaddr_in*)res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
#endif
  } else {
    addr.sin_addr.s_addr = ip;
  }
  if (cancel && cancel->cancelled()) {
    error_ = "Abgebrochen";
    return false;
  }
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    error_ = "socket() fehlgeschlagen";
    return false;
  }
  fd_ = (long long)s;
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
#else
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    error_ = "socket() fehlgeschlagen";
    return false;
  }
  fd_ = s;
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
  if (cancel) cancel->SetSocket(fd_);
  int r = connect(s, (struct sockaddr*)&addr, sizeof addr);
  if (r != 0) {
    int err = LastSocketError();
#ifdef _WIN32
    bool pending = err == WSAEWOULDBLOCK;
#else
    bool pending = err == EINPROGRESS;
#endif
    if (!pending) {
      error_ = "Verbindung fehlgeschlagen";
      Close();
      return false;
    }
    fd_set wset, eset;
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    FD_SET(s, &wset);
    FD_SET(s, &eset);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    r = select((int)s + 1, 0, &wset, &eset, &tv);
    if (r <= 0 || FD_ISSET(s, &eset)) {
      error_ = r == 0 ? "Zeitüberschreitung beim Verbinden" : "Verbindung abgelehnt";
      Close();
      return false;
    }
    int soerr = 0;
    socklen_t len = sizeof soerr;
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
    if (soerr != 0) {
      error_ = "Verbindung abgelehnt";
      Close();
      return false;
    }
  }
#ifdef _WIN32
  nb = 0;
  ioctlsocket(s, FIONBIO, &nb);
  DWORD tmo = 30000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof tmo);
#else
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
  struct timeval tmo;
  tmo.tv_sec = 30;
  tmo.tv_usec = 0;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo);
#endif
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
  return true;
}

int TcpSocket::Read(char* buf, int len) {
  if (fd_ < 0) return -1;
  for (;;) {
#ifdef _WIN32
    int r = recv((SOCKET)fd_, buf, len, 0);
#else
    int r = (int)recv((int)fd_, buf, len, 0);
    if (r < 0 && errno == EINTR) continue;
#endif
    if (r < 0) error_ = "Lesefehler";
    return r;
  }
}

bool TcpSocket::WriteAll(const char* buf, int len) {
  if (fd_ < 0) return false;
  while (len > 0) {
#ifdef _WIN32
    int r = send((SOCKET)fd_, buf, len, 0);
#else
    int r = (int)send((int)fd_, buf, len, MSG_NOSIGNAL);
    if (r < 0 && errno == EINTR) continue;
#endif
    if (r <= 0) {
      error_ = "Schreibfehler";
      return false;
    }
    buf += r;
    len -= r;
  }
  return true;
}

}  // namespace kite
