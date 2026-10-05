// Kite Engine - HTTP/2 client connections (RFC 9113)
//
// One connection per origin is shared by all network worker threads. All
// socket and TLS I/O happens under the connection mutex; a thread waiting
// for its response "pumps" the connection for a short time slice, which
// dispatches incoming frames to whichever stream they belong to.
#ifndef KITE_NET_HTTP2_H
#define KITE_NET_HTTP2_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/mutex.h"
#include "net/hpack.h"
#include "net/socket.h"

namespace kite {

struct Http2Result {
  bool ok = false;
  bool retryable = false;  // the request was not processed (GOAWAY, REFUSED_STREAM)
  int status = 0;
  HeaderList headers;
  std::string body;
  std::string error;
};

class Http2Connection {
 public:
  // Takes ownership of an established TLS connection that negotiated "h2".
  Http2Connection(std::unique_ptr<TcpSocket> sock, std::unique_ptr<TlsStream> tls);
  ~Http2Connection();

  // Sends the connection preface. False if the connection failed.
  bool Start();
  // Performs one request (blocking). |headers| are regular request headers
  // (lower-case names); pseudo headers are added here.
  Http2Result Request(const std::string& method, const std::string& scheme, const std::string& authority,
                      const std::string& path, const HeaderList& headers, const std::string& body,
                      CancelToken* cancel, size_t maxBytes,
                      void (*progress)(void*, size_t, size_t), void* progressCtx);
  // Can accept new streams.
  bool usable();
  int activeStreams();

  // Frame helpers (exposed for tests).
  static std::string Frame(int type, int flags, uint32_t stream, const std::string& payload);

 private:
  struct StreamState {
    bool done = false, failed = false, retryable = false, gotHeaders = false;
    int status = 0;
    HeaderList headers;
    std::string body;
    std::string error;
    int64_t sendWindow = 65535;
    size_t unacked = 0;  // received bytes not yet granted back
    size_t maxBytes = 0;
  };
  bool Pump(int timeoutMs);  // mutex held
  bool ProcessFrame(int type, int flags, uint32_t stream, const uint8_t* p, size_t n);
  bool HandleHeaderBlock(uint32_t stream, bool endStream);
  bool Send(const std::string& data);
  void FailAll(const std::string& error, bool retryable, uint32_t aboveStream);

  Mutex mu_;
  std::unique_ptr<TcpSocket> sock_;
  std::unique_ptr<TlsStream> tls_;
  HpackDecoder decoder_;
  std::map<uint32_t, StreamState*> streams_;
  uint32_t nextStream_ = 1;
  bool dead_ = false, goaway_ = false;
  uint32_t goawayLast_ = 0;
  int64_t sendWindow_ = 65535;
  int64_t peerInitialWindow_ = 65535;
  uint32_t peerMaxFrame_ = 16384;
  uint32_t peerMaxStreams_ = 100;
  size_t connUnacked_ = 0;
  std::string inbuf_;
  // Header block being assembled (HEADERS/PUSH_PROMISE + CONTINUATION).
  std::string headerBlock_;
  uint32_t headerStream_ = 0;
  bool headerEndStream_ = false, headerIsPush_ = false, inHeaders_ = false;
};

}  // namespace kite

#endif
