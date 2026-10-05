// Kite Engine - WebSocket client (RFC 6455) on a background thread
#ifndef KITE_NET_WEBSOCKET_H
#define KITE_NET_WEBSOCKET_H

#include <memory>
#include <string>
#include <vector>

#include "base/mutex.h"

namespace kite {

class WebSocketClient {
 public:
  struct Event {
    enum Type { kOpen, kMessage, kError, kClose };
    Type type;
    std::string data;  // message payload, or the chosen subprotocol (kOpen)
    bool binary = false;
    int code = 0;  // kClose
    std::string reason;
    bool clean = false;
  };

  // Starts connecting on a new thread. Events are queued for TakeEvents()
  // and announced with WakeUi().
  static std::shared_ptr<WebSocketClient> Open(const std::string& url, const std::vector<std::string>& protocols,
                                               const std::string& origin);
  ~WebSocketClient();

  // Queue a message (sent by the connection thread).
  void Send(const std::string& data, bool binary);
  // Starts the closing handshake (code 0: no status code).
  void Close(int code, const std::string& reason);
  // Drops the connection without a handshake (page unloaded).
  void Abort();
  bool TakeEvents(std::vector<Event>& out);
  size_t bufferedAmount();

  // Frame helpers (exposed for tests): a masked client frame, and the
  // Sec-WebSocket-Accept value for a key.
  static std::string EncodeFrame(int opcode, const std::string& payload, const unsigned char mask[4]);
  static std::string AcceptKey(const std::string& key);

 private:
  WebSocketClient() {}
  static void ThreadMain(void* arg);
  void Run();
  void Post(const Event& e);

  std::string url_, origin_;
  std::vector<std::string> protocols_;
  Mutex mu_;
  std::vector<Event> events_;
  std::vector<std::pair<int, std::string> > outgoing_;  // opcode, payload
  size_t buffered_ = 0;
  bool closeRequested_ = false, aborted_ = false;
  int closeCode_ = 0;
  std::string closeReason_;
  std::shared_ptr<WebSocketClient> self_;  // keeps the object alive while the thread runs
};

}  // namespace kite

#endif
