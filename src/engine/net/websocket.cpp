#include "net/websocket.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "base/strings.h"
#include "base/thread.h"
#include "net/http.h"
#include "net/socket.h"
#include "net/url.h"

extern "C" {
#include "bearssl.h"
}

namespace kite {

namespace {

const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

unsigned RandomByte() {
  static unsigned state = 0;
  if (!state) state = (unsigned)time(0) ^ (unsigned)(size_t)&state ^ 0x9E3779B9u;
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state & 0xFF;
}

// Reads exactly |n| bytes (false on EOF/error/abort).
bool ReadExact(Stream* s, TcpSocket* sock, TlsStream* tls, std::string& buf, size_t n, const volatile bool* abort) {
  while (buf.size() < n) {
    bool ready = tls ? tls->WaitReadable(100) : sock->WaitReadable(100);
    if (*abort) return false;
    if (!ready) continue;
    char tmp[16384];
    int r = s->Read(tmp, (int)std::min(sizeof tmp, n - buf.size()));
    if (r <= 0) return false;
    buf.append(tmp, r);
  }
  return true;
}

}  // namespace

std::string WebSocketClient::AcceptKey(const std::string& key) {
  br_sha1_context c;
  br_sha1_init(&c);
  std::string in = key + kGuid;
  br_sha1_update(&c, in.data(), in.size());
  unsigned char out[20];
  br_sha1_out(&c, out);
  return Base64Encode(std::string((const char*)out, 20));
}

std::string WebSocketClient::EncodeFrame(int opcode, const std::string& payload, const unsigned char mask[4]) {
  std::string f;
  f += (char)(0x80 | (opcode & 15));
  size_t n = payload.size();
  if (n < 126) {
    f += (char)(0x80 | n);
  } else if (n < 65536) {
    f += (char)(0x80 | 126);
    f += (char)(n >> 8);
    f += (char)(n & 0xFF);
  } else {
    f += (char)(0x80 | 127);
    for (int i = 7; i >= 0; --i) f += (char)(i >= 4 ? 0 : ((unsigned long long)n >> (8 * i)) & 0xFF);
  }
  f.append((const char*)mask, 4);
  size_t start = f.size();
  f += payload;
  for (size_t i = 0; i < n; ++i) f[start + i] = (char)(f[start + i] ^ mask[i & 3]);
  return f;
}

std::shared_ptr<WebSocketClient> WebSocketClient::Open(const std::string& url,
                                                       const std::vector<std::string>& protocols,
                                                       const std::string& origin) {
  std::shared_ptr<WebSocketClient> ws(new WebSocketClient);
  ws->url_ = url;
  ws->protocols_ = protocols;
  ws->origin_ = origin;
  ws->self_ = ws;
  if (!StartThread(ThreadMain, ws.get())) {
    ws->self_.reset();
    Event e;
    e.type = Event::kError;
    ws->events_.push_back(e);
    Event c;
    c.type = Event::kClose;
    c.code = 1006;
    ws->events_.push_back(c);
  }
  return ws;
}

WebSocketClient::~WebSocketClient() {}

void WebSocketClient::ThreadMain(void* arg) {
  WebSocketClient* ws = (WebSocketClient*)arg;
  std::shared_ptr<WebSocketClient> keep;
  {
    MutexLock l(ws->mu_);
    keep.swap(ws->self_);
  }
  ws->Run();
}

void WebSocketClient::Post(const Event& e) {
  {
    MutexLock l(mu_);
    events_.push_back(e);
  }
  WakeUi();
}

void WebSocketClient::Send(const std::string& data, bool binary) {
  MutexLock l(mu_);
  if (closeRequested_ || aborted_) return;
  outgoing_.push_back(std::make_pair(binary ? 2 : 1, data));
  buffered_ += data.size();
}

void WebSocketClient::Close(int code, const std::string& reason) {
  MutexLock l(mu_);
  if (closeRequested_) return;
  closeRequested_ = true;
  closeCode_ = code;
  closeReason_ = reason;
}

void WebSocketClient::Abort() {
  MutexLock l(mu_);
  aborted_ = true;
}

bool WebSocketClient::TakeEvents(std::vector<Event>& out) {
  MutexLock l(mu_);
  if (events_.empty()) return false;
  out.insert(out.end(), events_.begin(), events_.end());
  events_.clear();
  return true;
}

size_t WebSocketClient::bufferedAmount() {
  MutexLock l(mu_);
  return buffered_;
}

void WebSocketClient::Run() {
  Event closeEv;
  closeEv.type = Event::kClose;
  closeEv.code = 1006;
  Url u = Url::Parse(url_);
  bool tls = u.scheme() == "wss";
  ProxyConfig proxy = Network::Get().proxy();
  if (!proxy.UsedFor(u.host())) proxy = ProxyConfig();
  TcpSocket sock;
  std::unique_ptr<TlsStream> tlsStream;
  Stream* stream = &sock;
  const volatile bool* abort = &aborted_;
  auto fail = [&](const std::string& why) {
    Event e;
    e.type = Event::kError;
    e.data = why;
    Post(e);
    Post(closeEv);
  };
  int port = u.port() > 0 ? u.port() : (tls ? 443 : 80);
  std::string hostPort = u.host() + ":" + IntToString(port);
  for (int attempt = 0;; ++attempt) {
    sock.Close();
    if (!sock.Connect(proxy.enabled() ? proxy.host : u.host(), proxy.enabled() ? proxy.port : port, 20000, 0)) {
      fail(sock.error());
      return;
    }
    if (proxy.enabled()) {
      std::string connect = "CONNECT " + hostPort + " HTTP/1.1\r\nHost: " + hostPort + "\r\n\r\n";
      std::string reply;
      char c;
      if (!sock.WriteAll(connect.data(), (int)connect.size())) {
        fail("Proxy-Verbindung fehlgeschlagen");
        return;
      }
      while (reply.find("\r\n\r\n") == std::string::npos && reply.size() < 16384) {
        if (sock.Read(&c, 1) != 1) break;
        reply += c;
      }
      if (reply.find(" 200") == std::string::npos || reply.find(" 200") > reply.find("\r\n")) {
        fail("Proxy hat die Verbindung abgelehnt");
        return;
      }
    }
    if (!tls) break;
    static const char* const kAlpn[] = {"http/1.1"};
    tlsStream.reset(new TlsStream(&sock));
    if (tlsStream->Handshake(u.host(), kAlpn, 1)) {
      stream = tlsStream.get();
      break;
    }
    if (tlsStream->needsTls12Retry() && attempt == 0) continue;
    fail("TLS: " + tlsStream->error());
    return;
  }
  // Opening handshake.
  std::string keyRaw;
  for (int i = 0; i < 16; ++i) keyRaw += (char)RandomByte();
  std::string key = Base64Encode(keyRaw);
  std::string req = "GET " + u.PathAndQuery() + " HTTP/1.1\r\nHost: " + u.HostPort() +
                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                    "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: " + Network::Get().userAgent() + "\r\n";
  if (!origin_.empty()) req += "Origin: " + origin_ + "\r\n";
  if (!protocols_.empty()) {
    req += "Sec-WebSocket-Protocol: ";
    for (size_t i = 0; i < protocols_.size(); ++i) req += (i ? ", " : "") + protocols_[i];
    req += "\r\n";
  }
  Url cookieUrl = Url::Parse((tls ? "https://" : "http://") + u.HostPort() + u.PathAndQuery());
  std::string cookie = Network::Get().cookies().CookieHeader(cookieUrl);
  if (!cookie.empty()) req += "Cookie: " + cookie + "\r\n";
  req += "\r\n";
  if (!stream->WriteAll(req.data(), (int)req.size())) {
    fail("Senden fehlgeschlagen");
    return;
  }
  std::string head;
  while (head.find("\r\n\r\n") == std::string::npos) {
    if (head.size() > 65536) {
      fail("Antwort zu lang");
      return;
    }
    std::string one;
    if (!ReadExact(stream, &sock, tlsStream.get(), one, 1, abort)) {
      fail("Keine Antwort vom Server");
      return;
    }
    head += one;
  }
  std::vector<std::string> lines = Split(head.substr(0, head.find("\r\n\r\n")), '\n');
  std::string status = lines.empty() ? std::string() : Trim(lines[0]);
  std::string accept, protocol, upgrade;
  for (size_t i = 1; i < lines.size(); ++i) {
    size_t colon = lines[i].find(':');
    if (colon == std::string::npos) continue;
    std::string name = AsciiLower(Trim(lines[i].substr(0, colon))), value = Trim(lines[i].substr(colon + 1));
    if (name == "sec-websocket-accept") accept = value;
    else if (name == "sec-websocket-protocol") protocol = value;
    else if (name == "upgrade") upgrade = AsciiLower(value);
    else if (name == "set-cookie") Network::Get().cookies().SetFromHeader(cookieUrl, value);
  }
  if (status.find(" 101") == std::string::npos || upgrade != "websocket" || accept != AcceptKey(key)) {
    fail("Handshake abgelehnt: " + status);
    return;
  }
  Event open;
  open.type = Event::kOpen;
  open.data = protocol;
  Post(open);

  // Frames.
  std::string inbuf, message;
  int messageOp = 0;
  bool sentClose = false;
  long long closeSentAt = 0;
  for (;;) {
    // Outgoing messages and close requests.
    std::vector<std::pair<int, std::string> > out;
    bool wantClose = false, aborted;
    int code = 0;
    std::string reason;
    {
      MutexLock l(mu_);
      out.swap(outgoing_);
      wantClose = closeRequested_ && !sentClose;
      code = closeCode_;
      reason = closeReason_;
      aborted = aborted_;
    }
    if (aborted) return;
    for (size_t i = 0; i < out.size(); ++i) {
      unsigned char mask[4];
      for (int k = 0; k < 4; ++k) mask[k] = (unsigned char)RandomByte();
      std::string f = EncodeFrame(out[i].first, out[i].second, mask);
      if (!stream->WriteAll(f.data(), (int)f.size())) {
        fail("Senden fehlgeschlagen");
        return;
      }
      MutexLock l(mu_);
      buffered_ -= std::min(buffered_, out[i].second.size());
    }
    if (wantClose) {
      std::string payload;
      if (code) {
        payload += (char)(code >> 8);
        payload += (char)(code & 0xFF);
        payload += reason;
      }
      unsigned char mask[4];
      for (int k = 0; k < 4; ++k) mask[k] = (unsigned char)RandomByte();
      std::string f = EncodeFrame(8, payload, mask);
      stream->WriteAll(f.data(), (int)f.size());
      sentClose = true;
      closeSentAt = (long long)time(0);
    }
    if (sentClose && (long long)time(0) - closeSentAt > 5) {  // server never answered
      closeEv.code = 1006;
      Post(closeEv);
      return;
    }
    // Incoming data.
    bool ready = tlsStream ? tlsStream->WaitReadable(20) : sock.WaitReadable(20);
    if (!ready) continue;
    char tmp[16384];
    int r = stream->Read(tmp, sizeof tmp);
    if (r <= 0) {
      Post(closeEv);
      return;
    }
    inbuf.append(tmp, r);
    for (;;) {
      if (inbuf.size() < 2) break;
      unsigned char b0 = inbuf[0], b1 = inbuf[1];
      bool fin = (b0 & 0x80) != 0;
      int op = b0 & 15;
      bool masked = (b1 & 0x80) != 0;
      unsigned long long len = b1 & 0x7F;
      size_t pos = 2;
      if (len == 126) {
        if (inbuf.size() < 4) break;
        len = ((unsigned char)inbuf[2] << 8) | (unsigned char)inbuf[3];
        pos = 4;
      } else if (len == 127) {
        if (inbuf.size() < 10) break;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | (unsigned char)inbuf[2 + i];
        pos = 10;
      }
      if (len > 64ull * 1024 * 1024) {
        fail("Nachricht zu groß");
        return;
      }
      if (masked) pos += 4;
      if (inbuf.size() < pos + len) break;
      std::string payload = inbuf.substr(pos, (size_t)len);
      if (masked)
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = (char)(payload[i] ^ inbuf[pos - 4 + (i & 3)]);
      inbuf.erase(0, pos + (size_t)len);
      if (op == 9) {  // ping -> pong
        unsigned char mask[4];
        for (int k = 0; k < 4; ++k) mask[k] = (unsigned char)RandomByte();
        std::string f = EncodeFrame(10, payload, mask);
        stream->WriteAll(f.data(), (int)f.size());
      } else if (op == 10) {
        // pong: ignored
      } else if (op == 8) {
        closeEv.code = payload.size() >= 2 ? (((unsigned char)payload[0] << 8) | (unsigned char)payload[1]) : 1005;
        closeEv.reason = payload.size() > 2 ? payload.substr(2) : std::string();
        closeEv.clean = true;
        if (!sentClose) {
          unsigned char mask[4];
          for (int k = 0; k < 4; ++k) mask[k] = (unsigned char)RandomByte();
          std::string f = EncodeFrame(8, payload.substr(0, 2), mask);
          stream->WriteAll(f.data(), (int)f.size());
        }
        Post(closeEv);
        return;
      } else {
        if (op == 1 || op == 2) {
          messageOp = op;
          message = payload;
        } else if (op == 0) {
          message += payload;
        }
        if (fin && messageOp) {
          Event m;
          m.type = Event::kMessage;
          m.data.swap(message);
          m.binary = messageOp == 2;
          messageOp = 0;
          Post(m);
        }
      }
    }
  }
}

}  // namespace kite
