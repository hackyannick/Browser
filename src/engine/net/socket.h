// Kite Engine - TCP sockets and TLS streams
#ifndef KITE_NET_SOCKET_H
#define KITE_NET_SOCKET_H

#include <memory>
#include <string>
#include <vector>

#include "base/mutex.h"

namespace kite {

// Shared between the UI thread and a network worker so the UI can abort a
// transfer (closing the socket wakes up a blocking recv()).
class CancelToken {
 public:
  CancelToken() : cancelled_(false), socket_(-1) {}
  void Cancel();
  bool cancelled() const { return cancelled_; }
  void SetSocket(long long s);

 private:
  volatile bool cancelled_;
  long long socket_;
  Mutex mu_;
};

class Stream {
 public:
  virtual ~Stream() {}
  // Returns bytes read, 0 on EOF, -1 on error.
  virtual int Read(char* buf, int len) = 0;
  virtual bool WriteAll(const char* buf, int len) = 0;
  virtual std::string error() const = 0;
};

class TcpSocket : public Stream {
 public:
  TcpSocket();
  ~TcpSocket();
  bool Connect(const std::string& host, int port, int timeoutMs, CancelToken* cancel);
  int Read(char* buf, int len);
  bool WriteAll(const char* buf, int len);
  void Close();
  // True when data (or EOF) is available within |timeoutMs|.
  bool WaitReadable(int timeoutMs);
  std::string error() const { return error_; }
  long long handle() const { return fd_; }

 private:
  long long fd_;
  std::string error_;
};

// TLS client over a TcpSocket using BearSSL.
class TlsStream : public Stream {
 public:
  explicit TlsStream(TcpSocket* sock);
  ~TlsStream();
  // |alpn|: protocols offered via ALPN (e.g. "h2", "http/1.1"), or null.
  bool Handshake(const std::string& host, const char* const* alpn = 0, int alpnCount = 0);
  // Protocol chosen by the server ("" without ALPN).
  std::string selectedProtocol() const;
  int Read(char* buf, int len);
  bool WriteAll(const char* buf, int len);
  // True when decrypted data is buffered or the socket becomes readable.
  bool WaitReadable(int timeoutMs);
  std::string error() const { return error_; }

 private:
  struct Impl;
  Impl* impl_;
  TcpSocket* sock_;
  std::string error_;
};

// Must be called once before using sockets (WSAStartup on Windows).
void NetInit();
// Loads root certificates (PEM bundle). Returns the number loaded.
int LoadTrustAnchors(const std::string& pemData);
int TrustAnchorCount();

}  // namespace kite

#endif
