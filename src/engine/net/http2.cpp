#include "net/http2.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif

#include "base/strings.h"

namespace kite {

namespace {

enum FrameType {
  kData = 0, kHeaders = 1, kPriority = 2, kRstStream = 3, kSettings = 4, kPushPromise = 5,
  kPing = 6, kGoaway = 7, kWindowUpdate = 8, kContinuation = 9
};
enum Flags { kEndStream = 0x1, kAck = 0x1, kEndHeaders = 0x4, kPadded = 0x8, kPriorityFlag = 0x20 };

const uint32_t kOurWindow = 16 * 1024 * 1024;  // per stream and connection
const size_t kOurMaxFrame = 16384;
const size_t kWindowRefill = 4 * 1024 * 1024;

uint32_t Be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

void Put32(std::string& s, uint32_t v) {
  s += (char)(v >> 24);
  s += (char)(v >> 16);
  s += (char)(v >> 8);
  s += (char)v;
}

std::string Setting(int id, uint32_t value) {
  std::string s;
  s += (char)(id >> 8);
  s += (char)id;
  Put32(s, value);
  return s;
}

long long NowMs() {
#ifdef _WIN32
  return (long long)GetTickCount();
#else
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

void RelaxCpu() {
#ifdef _WIN32
  Sleep(0);
#else
  usleep(0);
#endif
}

}  // namespace

std::string Http2Connection::Frame(int type, int flags, uint32_t stream, const std::string& payload) {
  std::string f;
  size_t n = payload.size();
  f += (char)(n >> 16);
  f += (char)(n >> 8);
  f += (char)n;
  f += (char)type;
  f += (char)flags;
  Put32(f, stream & 0x7FFFFFFF);
  f += payload;
  return f;
}

Http2Connection::Http2Connection(std::unique_ptr<TcpSocket> sock, std::unique_ptr<TlsStream> tls)
    : sock_(std::move(sock)), tls_(std::move(tls)) {}

Http2Connection::~Http2Connection() {}

bool Http2Connection::Send(const std::string& data) {
  if (dead_) return false;
  if (!tls_->WriteAll(data.data(), (int)data.size())) {
    FailAll("Verbindung getrennt", true, 0);
    return false;
  }
  return true;
}

bool Http2Connection::Start() {
  MutexLock l(mu_);
  std::string out = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  std::string settings = Setting(2, 0) /* ENABLE_PUSH */ + Setting(3, 100) /* MAX_CONCURRENT_STREAMS */ +
                         Setting(4, kOurWindow) /* INITIAL_WINDOW_SIZE */;
  out += Frame(kSettings, 0, 0, settings);
  std::string wu;
  Put32(wu, kOurWindow - 65535);
  out += Frame(kWindowUpdate, 0, 0, wu);
  return Send(out);
}

bool Http2Connection::usable() {
  MutexLock l(mu_);
  return !dead_ && !goaway_ && nextStream_ < 0x7FFFFF00u;
}

int Http2Connection::activeStreams() {
  MutexLock l(mu_);
  return (int)streams_.size();
}

void Http2Connection::FailAll(const std::string& error, bool retryable, uint32_t aboveStream) {
  for (std::map<uint32_t, StreamState*>::iterator it = streams_.begin(); it != streams_.end(); ++it) {
    StreamState* s = it->second;
    if (s->done || it->first <= aboveStream) continue;
    s->done = s->failed = true;
    s->error = error;
    // Requests that got no response at all can be repeated elsewhere.
    s->retryable = retryable && !s->gotHeaders;
  }
  if (aboveStream == 0) dead_ = true;
}

bool Http2Connection::Pump(int timeoutMs) {
  if (dead_) return false;
  if (!tls_->WaitReadable(timeoutMs)) return true;  // nothing yet
  char buf[16384];
  int r = tls_->Read(buf, sizeof buf);
  if (r <= 0) {
    FailAll(r == 0 ? "Verbindung vom Server geschlossen" : "Lesefehler: " + tls_->error(), true, 0);
    return false;
  }
  inbuf_.append(buf, r);
  size_t pos = 0;
  while (inbuf_.size() - pos >= 9) {
    const uint8_t* h = (const uint8_t*)inbuf_.data() + pos;
    size_t len = ((size_t)h[0] << 16) | ((size_t)h[1] << 8) | h[2];
    if (len > kOurMaxFrame) {
      FailAll("HTTP/2: Frame zu groß", false, 0);
      return false;
    }
    if (inbuf_.size() - pos < 9 + len) break;
    int type = h[3], flags = h[4];
    uint32_t stream = Be32(h + 5) & 0x7FFFFFFF;
    if (!ProcessFrame(type, flags, stream, h + 9, len)) {
      if (!dead_) FailAll("HTTP/2-Protokollfehler", false, 0);
      return false;
    }
    pos += 9 + len;
  }
  inbuf_.erase(0, pos);
  return true;
}

bool Http2Connection::HandleHeaderBlock(uint32_t stream, bool endStream) {
  HeaderList list;
  // Must always be decoded to keep the HPACK state in sync.
  if (!decoder_.Decode((const uint8_t*)headerBlock_.data(), headerBlock_.size(), list)) return false;
  headerBlock_.clear();
  if (headerIsPush_) return true;  // push is disabled; ignore stray promises
  std::map<uint32_t, StreamState*>::iterator it = streams_.find(stream);
  if (it == streams_.end()) return true;
  StreamState* s = it->second;
  int status = 0;
  HeaderList regular;
  for (size_t i = 0; i < list.size(); ++i) {
    if (list[i].first == ":status") {
      long long v;
      if (ParseInt(list[i].second, v)) status = (int)v;
    } else if (!list[i].first.empty() && list[i].first[0] != ':') {
      regular.push_back(list[i]);
    }
  }
  if (!s->gotHeaders) {
    if (status >= 100 && status < 200) return true;  // interim (103 Early Hints ...)
    s->gotHeaders = true;
    s->status = status;
    s->headers.swap(regular);
  }  // later blocks are trailers
  if (endStream) s->done = true;
  return true;
}

bool Http2Connection::ProcessFrame(int type, int flags, uint32_t stream, const uint8_t* p, size_t n) {
  if (inHeaders_ && type != kContinuation) return false;
  switch (type) {
    case kData: {
      if (stream == 0) return false;
      size_t pad = 0, off = 0;
      if (flags & kPadded) {
        if (n < 1) return false;
        pad = p[0];
        off = 1;
      }
      if (off + pad > n) return false;
      size_t dataLen = n - off - pad;
      connUnacked_ += n;
      std::map<uint32_t, StreamState*>::iterator it = streams_.find(stream);
      if (it != streams_.end()) {
        StreamState* s = it->second;
        if (s->body.size() + dataLen <= s->maxBytes + 65536) s->body.append((const char*)p + off, dataLen);
        s->unacked += n;
        if (s->body.size() > s->maxBytes) {  // too large: stop the stream
          std::string code;
          Put32(code, 8);  // CANCEL
          Send(Frame(kRstStream, 0, stream, code));
          s->done = true;
        } else if (flags & kEndStream) {
          s->done = true;
        } else if (s->unacked >= kWindowRefill) {
          std::string wu;
          Put32(wu, (uint32_t)s->unacked);
          s->unacked = 0;
          Send(Frame(kWindowUpdate, 0, stream, wu));
        }
      }
      if (connUnacked_ >= kWindowRefill) {
        std::string wu;
        Put32(wu, (uint32_t)connUnacked_);
        connUnacked_ = 0;
        Send(Frame(kWindowUpdate, 0, 0, wu));
      }
      return true;
    }
    case kHeaders:
    case kPushPromise: {
      if (stream == 0) return false;
      size_t pad = 0, off = 0;
      if (flags & kPadded) {
        if (n < 1) return false;
        pad = p[0];
        off = 1;
      }
      if (type == kHeaders && (flags & kPriorityFlag)) off += 5;
      if (type == kPushPromise) off += 4;  // promised stream id
      if (off + pad > n) return false;
      headerBlock_.assign((const char*)p + off, n - off - pad);
      headerStream_ = stream;
      headerIsPush_ = type == kPushPromise;
      headerEndStream_ = type == kHeaders && (flags & kEndStream);
      if (type == kPushPromise && off >= 4) {
        // Refuse the promised stream.
        uint32_t promised = Be32(p + off - 4) & 0x7FFFFFFF;
        std::string code;
        Put32(code, 7);  // REFUSED_STREAM
        Send(Frame(kRstStream, 0, promised, code));
      }
      if (flags & kEndHeaders) return HandleHeaderBlock(stream, headerEndStream_);
      inHeaders_ = true;
      return true;
    }
    case kContinuation: {
      if (!inHeaders_ || stream != headerStream_) return false;
      headerBlock_.append((const char*)p, n);
      if (headerBlock_.size() > 1024 * 1024) return false;
      if (flags & kEndHeaders) {
        inHeaders_ = false;
        return HandleHeaderBlock(stream, headerEndStream_);
      }
      return true;
    }
    case kPriority:
      return true;
    case kRstStream: {
      if (n != 4 || stream == 0) return false;
      uint32_t code = Be32(p);
      std::map<uint32_t, StreamState*>::iterator it = streams_.find(stream);
      if (it != streams_.end() && !it->second->done) {
        StreamState* s = it->second;
        s->done = true;
        // NO_ERROR after a complete response is fine (server stops reading the body).
        if (!(code == 0 && s->gotHeaders)) {
          s->failed = true;
          s->retryable = code == 7 /* REFUSED_STREAM */ && !s->gotHeaders;
          s->error = "HTTP/2-Stream abgebrochen (Fehler " + IntToString((long long)code) + ")";
        }
      }
      return true;
    }
    case kSettings: {
      if (stream != 0) return false;
      if (flags & kAck) return true;
      if (n % 6) return false;
      for (size_t i = 0; i + 6 <= n; i += 6) {
        int id = (p[i] << 8) | p[i + 1];
        uint32_t v = Be32(p + i + 2);
        if (id == 3) peerMaxStreams_ = v;
        else if (id == 4) {
          if (v > 0x7FFFFFFF) return false;
          int64_t delta = (int64_t)v - peerInitialWindow_;
          peerInitialWindow_ = v;
          for (std::map<uint32_t, StreamState*>::iterator it = streams_.begin(); it != streams_.end(); ++it)
            it->second->sendWindow += delta;
        } else if (id == 5) {
          if (v < 16384 || v > 16777215) return false;
          peerMaxFrame_ = v;
        }
      }
      return Send(Frame(kSettings, kAck, 0, ""));
    }
    case kPing: {
      if (n != 8 || stream != 0) return false;
      if (flags & kAck) return true;
      return Send(Frame(kPing, kAck, 0, std::string((const char*)p, 8)));
    }
    case kGoaway: {
      if (n < 8 || stream != 0) return false;
      goaway_ = true;
      goawayLast_ = Be32(p) & 0x7FFFFFFF;
      FailAll("Server beendet die Verbindung", true, goawayLast_);
      return true;
    }
    case kWindowUpdate: {
      if (n != 4) return false;
      uint32_t inc = Be32(p) & 0x7FFFFFFF;
      if (stream == 0) sendWindow_ += inc;
      else {
        std::map<uint32_t, StreamState*>::iterator it = streams_.find(stream);
        if (it != streams_.end()) it->second->sendWindow += inc;
      }
      return true;
    }
    default:
      return true;  // unknown frame types are ignored
  }
}

Http2Result Http2Connection::Request(const std::string& method, const std::string& scheme,
                                     const std::string& authority, const std::string& path,
                                     const HeaderList& headers, const std::string& body, CancelToken* cancel,
                                     size_t maxBytes, void (*progress)(void*, size_t, size_t),
                                     void* progressCtx) {
  Http2Result res;
  StreamState st;
  st.maxBytes = maxBytes;
  uint32_t id;
  size_t bodySent = 0;
  {
    MutexLock l(mu_);
    if (dead_ || goaway_) {
      res.retryable = true;
      res.error = "Verbindung nicht mehr verfügbar";
      return res;
    }
    id = nextStream_;
    nextStream_ += 2;
    st.sendWindow = peerInitialWindow_;
    streams_[id] = &st;
    HeaderList all;
    all.push_back(std::make_pair(std::string(":method"), method));
    all.push_back(std::make_pair(std::string(":scheme"), scheme));
    all.push_back(std::make_pair(std::string(":authority"), authority));
    all.push_back(std::make_pair(std::string(":path"), path.empty() ? std::string("/") : path));
    all.insert(all.end(), headers.begin(), headers.end());
    std::string block = HpackEncode(all);
    std::string out;
    size_t pos = 0;
    bool first = true;
    do {
      size_t n = std::min<size_t>(block.size() - pos, peerMaxFrame_);
      bool last = pos + n >= block.size();
      int flags = last ? kEndHeaders : 0;
      if (first && body.empty()) flags |= kEndStream;
      out += Frame(first ? kHeaders : kContinuation, flags, id, block.substr(pos, n));
      pos += n;
      first = false;
    } while (pos < block.size());
    if (!Send(out)) {
      streams_.erase(id);
      res.retryable = true;
      res.error = "Senden fehlgeschlagen";
      return res;
    }
  }
  long long lastActivity = NowMs();
  size_t lastSize = 0;
  bool cancelled = false;
  for (;;) {
    {
      MutexLock l(mu_);
      // Request body, as far as the flow-control windows allow.
      while (bodySent < body.size() && !dead_ && !st.done) {
        int64_t room = std::min(sendWindow_, st.sendWindow);
        if (room <= 0) break;
        size_t n = std::min<size_t>((size_t)room, std::min<size_t>(peerMaxFrame_, body.size() - bodySent));
        bool last = bodySent + n >= body.size();
        if (!Send(Frame(kData, last ? kEndStream : 0, id, body.substr(bodySent, n)))) break;
        bodySent += n;
        sendWindow_ -= n;
        st.sendWindow -= n;
      }
      if (!st.done && cancel && cancel->cancelled()) {
        std::string code;
        Put32(code, 8);  // CANCEL
        Send(Frame(kRstStream, 0, id, code));
        cancelled = true;
        break;
      }
      // Process whatever is already readable (non-blocking).
      while (!st.done && !dead_ && tls_->WaitReadable(0))
        if (!Pump(0)) break;
      if (st.done || dead_) break;
    }
    // Wait for the socket without holding the lock, so that other threads
    // can send their requests meanwhile.
    sock_->WaitReadable(20);
    if (st.body.size() != lastSize) {
      lastSize = st.body.size();
      lastActivity = NowMs();
      if (progress) progress(progressCtx, lastSize, 0);
    }
    if (NowMs() - lastActivity > 60000) {
      MutexLock l(mu_);
      std::string code;
      Put32(code, 8);
      Send(Frame(kRstStream, 0, id, code));
      st.failed = st.done = true;
      st.error = "Zeitüberschreitung";
      break;
    }
    RelaxCpu();  // let other threads take the connection
  }
  {
    MutexLock l(mu_);
    streams_.erase(id);
    if (!st.done && dead_ && !st.failed) {
      st.failed = true;
      st.retryable = !st.gotHeaders;
      st.error = "Verbindung getrennt";
    }
  }
  if (cancelled) {
    res.error = "Abgebrochen";
    return res;
  }
  if (st.failed || !st.gotHeaders) {
    res.error = st.error.empty() ? "Keine Antwort vom Server" : st.error;
    res.retryable = st.retryable || (!st.gotHeaders && st.error.empty());
    return res;
  }
  res.ok = true;
  res.status = st.status;
  res.headers.swap(st.headers);
  res.body.swap(st.body);
  return res;
}

}  // namespace kite
