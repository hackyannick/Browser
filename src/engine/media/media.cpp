#include "media/media.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>

#include "base/mutex.h"
#include "base/strings.h"
#include "base/thread.h"
#include "dom/node.h"
#include "net/http.h"
#include "net/url.h"

#ifdef KITE_HAVE_FFMPEG
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}
#endif

#ifndef _WIN32
#include <time.h>
#endif

namespace kite {

namespace {

long long NowMs() {
#ifdef _WIN32
  // GetTickCount wraps after 49 days; extend it to 64 bits.
  static Mutex* mu = new Mutex;
  static DWORD last = 0;
  static long long high = 0;
  MutexLock l(*mu);
  DWORD t = GetTickCount();
  if (t < last) high += 0x100000000LL;
  last = t;
  return high + t;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

Mutex& RegistryMutex() {
  static Mutex* m = new Mutex;
  return *m;
}
std::map<int, MediaPlayer*>& Registry() {
  static std::map<int, MediaPlayer*>* r = new std::map<int, MediaPlayer*>;
  return *r;
}
int g_nextId = 1;
unsigned g_generation = 1;
AudioSinkFactory g_sinkFactory = 0;

void BumpGeneration() {
  {
    MutexLock l(RegistryMutex());
    ++g_generation;
  }
  WakeUi();
}

// ---------------------------------------------------------------- null sink

// Plays nothing but advances like a sound card would.
class NullAudioSink : public AudioSink {
 public:
  bool Open(int rate) override {
    rate_ = rate;
    last_ = NowMs();
    return true;
  }
  void Write(const int16_t*, int frames) override {
    Update();
    written_ += frames;
  }
  long long Played() override {
    Update();
    return pos_;
  }
  void SetPaused(bool p) override {
    Update();
    paused_ = p;
  }
  void Flush() override {
    Update();
    written_ = pos_ = 0;
  }

 private:
  void Update() {
    long long now = NowMs();
    if (!paused_) pos_ = std::min(written_, pos_ + (now - last_) * rate_ / 1000);
    last_ = now;
  }
  int rate_ = 44100;
  long long written_ = 0, pos_ = 0, last_ = 0;
  bool paused_ = true;
};

}  // namespace

void SetAudioSinkFactory(AudioSinkFactory f) { g_sinkFactory = f; }
AudioSink* CreateNullAudioSink() { return new NullAudioSink; }

// ---------------------------------------------------------------- colours

void YuvToBgra(const uint8_t* const planes[3], const int strides[3], int width, int height,
               int chromaShiftX, int chromaShiftY, bool bt709, bool fullRange, uint32_t* out) {
  // Coefficients scaled by 1024.
  int yc, yoff, rv, gu, gv, bu;
  if (fullRange) {
    yc = 1024;
    yoff = 0;
    if (bt709) { rv = 1613; gu = 192; gv = 479; bu = 1900; }
    else { rv = 1436; gu = 352; gv = 731; bu = 1815; }
  } else {
    yc = 1192;
    yoff = 16;
    if (bt709) { rv = 1836; gu = 218; gv = 546; bu = 2163; }
    else { rv = 1634; gu = 401; gv = 833; bu = 2066; }
  }
  for (int y = 0; y < height; ++y) {
    const uint8_t* yr = planes[0] + (size_t)y * strides[0];
    const uint8_t* ur = planes[1] + (size_t)(y >> chromaShiftY) * strides[1];
    const uint8_t* vr = planes[2] + (size_t)(y >> chromaShiftY) * strides[2];
    uint32_t* o = out + (size_t)y * width;
    for (int x = 0; x < width; ++x) {
      int l = (yr[x] - yoff) * yc + 512;
      int u = ur[x >> chromaShiftX] - 128, v = vr[x >> chromaShiftX] - 128;
      int r = (l + rv * v) >> 10;
      int g = (l - gu * u - gv * v) >> 10;
      int b = (l + bu * u) >> 10;
      r = r < 0 ? 0 : (r > 255 ? 255 : r);
      g = g < 0 ? 0 : (g > 255 ? 255 : g);
      b = b < 0 ? 0 : (b > 255 ? 255 : b);
      o[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
  }
}

// ---------------------------------------------------------------- shared state

struct MediaPlayer::Shared {
  Mutex mu;
  WakeEvent wake;
  std::atomic<bool> stop{false};
  std::shared_ptr<CancelToken> cancel = std::make_shared<CancelToken>();
  std::string url, referrer;
  bool audioOnly = false;
  // Commands from the UI thread.
  bool wantPlay = false;
  bool seekPending = false;
  double seekTo = 0;
  double volume = 1;
  bool muted = false;
  bool loop = false;
  // Published state.
  MediaStatus status;
  std::vector<std::string> events;
  DecodedImage frame;
  unsigned frameVersion = 0;

  void Event(const char* name) {  // mu held
    if (events.size() > 200) events.erase(events.begin(), events.begin() + 100);
    events.push_back(name);
  }
};

namespace {

typedef MediaPlayer::Shared Shared;

void Fail(Shared* s, int code, const std::string& msg) {
  {
    MutexLock l(s->mu);
    s->status.error = code;
    s->status.errorMessage = msg;
    s->status.networkState = code == 4 ? 3 : 1;
    s->Event("error");
  }
  BumpGeneration();
}

// ---------------------------------------------------------------- byte source

// Random access to the media resource: HTTP range requests in chunks,
// fetched ahead of the read position by a helper thread. Servers without
// range support (and data:/file: URLs) deliver everything at once.
struct ByteSource {
  static const long long kChunk = 512 * 1024;
  static const int kAhead = 6;       // chunks prefetched past the read position
  static const size_t kKeep = 40;    // chunks kept in memory

  Mutex mu;
  WakeEvent dataReady, wantMore;
  std::atomic<bool> stop{false};
  std::shared_ptr<CancelToken> cancel;
  std::string url, referrer;
  bool memory = false;
  std::string all;
  long long size = -1;
  std::map<long long, std::string> chunks;
  long long readChunk = 0;
  bool failed = false;

  bool FetchRange(long long first, long long last, FetchResponse& r) {
    FetchRequest req;
    req.url = url;
    req.referrer = referrer;
    req.accept = "*/*";
    req.cancel = cancel;
    req.maxBytes = 128 * 1024 * 1024;
    req.headers.push_back(std::make_pair(std::string("Range"),
                                         "bytes=" + IntToString(first) + "-" + IntToString(last)));
    r = Network::Get().Fetch(req);
    return r.ok;
  }

  // First request; decides between range and whole-file mode.
  bool Open(std::string& error) {
    FetchResponse r;
    if (!FetchRange(0, kChunk - 1, r)) {
      error = r.error.empty() ? "Netzwerkfehler" : r.error;
      return false;
    }
    if (r.status >= 400) {
      error = "HTTP " + IntToString(r.status);
      return false;
    }
    std::string range = r.Header("content-range");
    if (r.status == 206 && !range.empty()) {
      size_t slash = range.find('/');
      long long total;
      if (slash != std::string::npos && ParseInt(Trim(range.substr(slash + 1)), total)) size = total;
      MutexLock l(mu);
      chunks[0].swap(r.body);
      return true;
    }
    memory = true;
    all.swap(r.body);
    size = (long long)all.size();
    return true;
  }

  // Prefetch loop (runs on its own thread).
  void Loop() {
    while (!stop) {
      long long want = -1;
      {
        MutexLock l(mu);
        if (failed) break;
        for (long long i = readChunk; i < readChunk + kAhead; ++i) {
          if (size >= 0 && i * kChunk >= size) break;
          if (!chunks.count(i)) {
            want = i;
            break;
          }
        }
      }
      if (want < 0) {
        wantMore.Wait(250);
        continue;
      }
      FetchResponse r;
      bool ok = false;
      for (int attempt = 0; attempt < 3 && !stop && !ok; ++attempt)
        ok = FetchRange(want * kChunk, want * kChunk + kChunk - 1, r) && (r.status == 206 || r.status == 200);
      {
        MutexLock l(mu);
        if (!ok) {
          failed = true;
        } else {
          if (r.status == 200) {
            // The server ignored the range this time.
            if ((long long)r.body.size() > want * kChunk) r.body = r.body.substr((size_t)(want * kChunk), (size_t)kChunk);
            else r.body.clear();
          }
          chunks[want].swap(r.body);
          // Drop the chunks farthest away from the read position.
          while (chunks.size() > kKeep) {
            std::map<long long, std::string>::iterator farthest = chunks.begin();
            long long best = -1;
            for (std::map<long long, std::string>::iterator it = chunks.begin(); it != chunks.end(); ++it) {
              long long d = it->first < readChunk ? (readChunk - it->first) * 4 : it->first - readChunk;
              if (d > best) {
                best = d;
                farthest = it;
              }
            }
            chunks.erase(farthest);
          }
        }
      }
      dataReady.Signal();
    }
    dataReady.Signal();
  }

  // Returns bytes read, 0 at the end, -1 on error or shutdown.
  int Read(long long pos, uint8_t* buf, int n) {
    if (pos < 0) return -1;
    if (memory) {
      if (pos >= (long long)all.size()) return 0;
      int k = (int)std::min<long long>(n, (long long)all.size() - pos);
      memcpy(buf, all.data() + pos, (size_t)k);
      return k;
    }
    if (size >= 0 && pos >= size) return 0;
    long long idx = pos / kChunk;
    MutexLock l(mu);
    readChunk = idx;
    for (;;) {
      std::map<long long, std::string>::iterator it = chunks.find(idx);
      if (it != chunks.end()) {
        long long off = pos - idx * kChunk;
        if (off >= (long long)it->second.size()) return 0;
        int k = (int)std::min<long long>(n, (long long)it->second.size() - off);
        memcpy(buf, it->second.data() + off, (size_t)k);
        wantMore.Signal();
        return k;
      }
      if (stop || failed) return -1;
      mu.Unlock();
      wantMore.Signal();
      dataReady.Wait(100);
      mu.Lock();
    }
  }

  // Bytes available without waiting, starting at |pos|.
  long long ContiguousFrom(long long pos) {
    if (memory) return std::max<long long>(0, (long long)all.size() - pos);
    MutexLock l(mu);
    long long idx = pos / kChunk, end = pos;
    while (true) {
      std::map<long long, std::string>::iterator it = chunks.find(idx);
      if (it == chunks.end()) break;
      end = idx * kChunk + (long long)it->second.size();
      if ((long long)it->second.size() < kChunk) break;
      ++idx;
    }
    return std::max<long long>(0, end - pos);
  }
};

void RunPrefetch(void* arg) {
  std::shared_ptr<ByteSource>* p = (std::shared_ptr<ByteSource>*)arg;
  (*p)->Loop();
  delete p;
}

#ifdef KITE_HAVE_FFMPEG

// ---------------------------------------------------------------- decoding

struct VideoFrame {
  double pts;
  DecodedImage image;
};

// Converts any planar YUV / RGB software frame to opaque or premultiplied BGRA.
bool ConvertFrame(const AVFrame* f, DecodedImage& out) {
  const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)f->format);
  if (!d || f->width <= 0 || f->height <= 0) return false;
  if (d->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM | AV_PIX_FMT_FLAG_HWACCEL)) return false;
  int w = f->width, h = f->height;
  if ((long long)w * h > 4096LL * 2304) return false;
  out.width = w;
  out.height = h;
  out.density = 1;
  out.pixels.resize((size_t)w * h);
  bool rgb = (d->flags & AV_PIX_FMT_FLAG_RGB) != 0;
  bool alpha = (d->flags & AV_PIX_FMT_FLAG_ALPHA) != 0 && d->nb_components == 4;
  out.hasAlpha = alpha;
  bool bt709 = f->colorspace == AVCOL_SPC_BT709 ||
               (f->colorspace == AVCOL_SPC_UNSPECIFIED && h >= 720);
  bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
              f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P;
  // Fast path: 8-bit planar YUV.
  if (!rgb && !alpha && d->nb_components == 3 && d->comp[0].depth == 8 && d->comp[0].step == 1 &&
      d->comp[1].step == 1 && d->comp[2].step == 1 && d->comp[0].plane == 0 && d->comp[1].plane == 1 &&
      d->comp[2].plane == 2) {
    const uint8_t* planes[3] = {f->data[0], f->data[1], f->data[2]};
    int strides[3] = {f->linesize[0], f->linesize[1], f->linesize[2]};
    YuvToBgra(planes, strides, w, h, d->log2_chroma_w, d->log2_chroma_h, bt709, full, &out.pixels[0]);
    return true;
  }
  // Generic path (high bit depth, semi-planar, gray, GBR, alpha).
  int n = d->nb_components;
  auto sample = [&](int c, int x, int y) -> int {
    const AVComponentDescriptor& cd = d->comp[c];
    bool chroma = !rgb && (c == 1 || c == 2);
    int sx = chroma ? x >> d->log2_chroma_w : x, sy = chroma ? y >> d->log2_chroma_h : y;
    const uint8_t* p = f->data[cd.plane] + (size_t)sy * f->linesize[cd.plane] + (size_t)sx * cd.step + cd.offset;
    int v;
    if (cd.depth > 8) {
      v = (d->flags & AV_PIX_FMT_FLAG_BE) ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]);
      v = (v >> cd.shift) & ((1 << cd.depth) - 1);
      v >>= cd.depth - 8;
    } else {
      v = (p[0] >> cd.shift) & ((1 << cd.depth) - 1);
      if (cd.depth < 8) v = v * 255 / ((1 << cd.depth) - 1);
    }
    return v;
  };
  int yc = full ? 1024 : 1192, yoff = full ? 0 : 16;
  int rv, gu, gv, bu;
  if (full) {
    if (bt709) { rv = 1613; gu = 192; gv = 479; bu = 1900; } else { rv = 1436; gu = 352; gv = 731; bu = 1815; }
  } else {
    if (bt709) { rv = 1836; gu = 218; gv = 546; bu = 2163; } else { rv = 1634; gu = 401; gv = 833; bu = 2066; }
  }
  for (int y = 0; y < h; ++y) {
    uint32_t* o = &out.pixels[(size_t)y * w];
    for (int x = 0; x < w; ++x) {
      int r, g, b, a = 255;
      if (rgb) {
        r = sample(0, x, y);
        g = n > 1 ? sample(1, x, y) : r;
        b = n > 2 ? sample(2, x, y) : r;
      } else if (n < 3) {
        int l = ((sample(0, x, y) - yoff) * yc + 512) >> 10;
        r = g = b = l < 0 ? 0 : (l > 255 ? 255 : l);
      } else {
        int l = (sample(0, x, y) - yoff) * yc + 512;
        int u = sample(1, x, y) - 128, v = sample(2, x, y) - 128;
        r = (l + rv * v) >> 10;
        g = (l - gu * u - gv * v) >> 10;
        b = (l + bu * u) >> 10;
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
      }
      if (alpha) {
        a = sample(n - 1, x, y);
        r = r * a / 255;
        g = g * a / 255;
        b = b * a / 255;
      }
      o[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
  }
  return true;
}

int ReadPacket(void* opaque, uint8_t* buf, int size);
int64_t SeekPacket(void* opaque, int64_t offset, int whence);
int Interrupt(void* opaque);

class Decoder {
 public:
  Decoder(const std::shared_ptr<Shared>& s) : s_(s) {}
  ~Decoder() { Close(); }

  void Run();

  // AVIO callbacks.
  int Read(uint8_t* buf, int size) {
    int n = src_->Read(pos_, buf, size);
    if (n < 0) return AVERROR(EIO);
    if (n == 0) return AVERROR_EOF;
    pos_ += n;
    return n;
  }
  int64_t Seek(int64_t offset, int whence) {
    if (whence & AVSEEK_SIZE) return src_->size >= 0 ? src_->size : -1;
    whence &= ~AVSEEK_FORCE;
    long long np = whence == SEEK_SET ? offset : whence == SEEK_CUR ? pos_ + offset
                 : (src_->size >= 0 ? src_->size + offset : -1);
    if (np < 0) return -1;
    pos_ = np;
    return np;
  }
  bool stopped() const { return s_->stop; }

 private:
  bool Open(std::string& error, int& code);
  void Close();
  bool Demux();
  bool DecodeVideo();
  bool DecodeAudio();
  void WriteAudio(AVFrame* f);
  double Clock();
  void DoSeek(double t);
  void Publish(VideoFrame& vf);
  void UpdateStatus(bool forceTime);
  void FinishSeek() {  // s_->mu held
    if (s_->status.seeking && !s_->seekPending) {
      s_->status.seeking = false;
      s_->status.currentTime = Clock();
      s_->Event("timeupdate");
      s_->Event("seeked");
    }
  }

  std::shared_ptr<Shared> s_;
  std::shared_ptr<ByteSource> src_;
  long long pos_ = 0;
  AVFormatContext* fmt_ = 0;
  AVIOContext* io_ = 0;
  AVCodecContext* vctx_ = 0;
  AVCodecContext* actx_ = 0;
  SwrContext* swr_ = 0;
  AVFrame* frame_ = 0;
  AVPacket* pkt_ = 0;
  int vi_ = -1, ai_ = -1;
  double start_ = 0;  // stream start time (seconds)
  std::deque<AVPacket*> vq_, aq_;
  size_t queuedBytes_ = 0;
  bool demuxEof_ = false, videoEof_ = false, audioEof_ = false;
  bool vDrainSent_ = false, aDrainSent_ = false;
  std::deque<VideoFrame> frames_;
  // Audio output.
  std::unique_ptr<AudioSink> sink_;
  int outRate_ = 44100;
  long long written_ = 0;   // frames written since the last flush
  double audioBase_ = 0;     // media time of the first written frame
  bool audioBaseSet_ = false;
  bool audioClockDead_ = false;  // audio ended; the wall clock takes over
  std::vector<int16_t> abuf_;
  // Clock.
  bool playing_ = false;
  double wallBase_ = 0;
  long long wallStart_ = 0;
  // Seeking / presentation.
  double dropUntil_ = -1;
  bool needShow_ = true;     // show the next decoded frame even while paused
  bool firstFrame_ = false;  // readyState reached HAVE_ENOUGH_DATA
  long long lastShownMs_ = 0;
  long long lastTimeUpdate_ = 0;
  double duration_ = std::numeric_limits<double>::quiet_NaN();
  double lastPts_ = 0;
};

int ReadPacket(void* opaque, uint8_t* buf, int size) { return ((Decoder*)opaque)->Read(buf, size); }
int64_t SeekPacket(void* opaque, int64_t offset, int whence) { return ((Decoder*)opaque)->Seek(offset, whence); }
int Interrupt(void* opaque) { return ((Decoder*)opaque)->stopped() ? 1 : 0; }

double TimeOf(int64_t ts, AVRational tb) {
  if (ts == AV_NOPTS_VALUE) return std::numeric_limits<double>::quiet_NaN();
  return ts * av_q2d(tb);
}

AVCodecContext* OpenCodec(AVStream* st) {
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec) return 0;
  AVCodecContext* c = avcodec_alloc_context3(codec);
  if (!c) return 0;
  if (avcodec_parameters_to_context(c, st->codecpar) < 0) {
    avcodec_free_context(&c);
    return 0;
  }
  c->thread_count = 1;
  c->pkt_timebase = st->time_base;
  if (avcodec_open2(c, codec, 0) < 0) {
    avcodec_free_context(&c);
    return 0;
  }
  return c;
}

bool Decoder::Open(std::string& error, int& code) {
  code = 2;
  src_ = std::make_shared<ByteSource>();
  src_->url = s_->url;
  src_->referrer = s_->referrer;
  src_->cancel = s_->cancel;
  if (!src_->Open(error)) return false;
  if (!src_->memory) {
    std::shared_ptr<ByteSource>* arg = new std::shared_ptr<ByteSource>(src_);
    if (!StartThread(RunPrefetch, arg)) delete arg;
  }
  code = 4;
  static bool quiet = (av_log_set_level(AV_LOG_QUIET), true);
  (void)quiet;
  fmt_ = avformat_alloc_context();
  unsigned char* buf = (unsigned char*)av_malloc(64 * 1024);
  io_ = avio_alloc_context(buf, 64 * 1024, 0, this, ReadPacket, 0, SeekPacket);
  if (!fmt_ || !io_) {
    error = "Speicher";
    return false;
  }
  fmt_->pb = io_;
  fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;
  fmt_->interrupt_callback.callback = Interrupt;
  fmt_->interrupt_callback.opaque = this;
  if (avformat_open_input(&fmt_, "", 0, 0) < 0) {
    fmt_ = 0;  // freed by avformat_open_input
    error = src_->failed ? "Netzwerkfehler" : "Format nicht unterstuetzt";
    if (src_->failed) code = 2;
    return false;
  }
  if (avformat_find_stream_info(fmt_, 0) < 0) {
    error = "Keine Streams";
    return false;
  }
  if (!s_->audioOnly) {
    vi_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
    if (vi_ >= 0 && (fmt_->streams[vi_]->disposition & AV_DISPOSITION_ATTACHED_PIC)) vi_ = -1;
  }
  ai_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, vi_, 0, 0);
  if (vi_ >= 0 && !(vctx_ = OpenCodec(fmt_->streams[vi_]))) vi_ = -1;
  if (ai_ >= 0 && !(actx_ = OpenCodec(fmt_->streams[ai_]))) ai_ = -1;
  if (vi_ < 0 && ai_ < 0) {
    error = "Kein unterstuetzter Codec";
    return false;
  }
  for (unsigned i = 0; i < fmt_->nb_streams; ++i)
    if ((int)i != vi_ && (int)i != ai_) fmt_->streams[i]->discard = AVDISCARD_ALL;
  if (fmt_->start_time != AV_NOPTS_VALUE) start_ = fmt_->start_time / (double)AV_TIME_BASE;
  if (fmt_->duration != AV_NOPTS_VALUE && fmt_->duration > 0) duration_ = fmt_->duration / (double)AV_TIME_BASE;
  frame_ = av_frame_alloc();
  pkt_ = av_packet_alloc();
  if (ai_ >= 0) {
    int rate = actx_->sample_rate;
    outRate_ = rate >= 8000 && rate <= 48000 ? rate : 48000;
    sink_.reset(g_sinkFactory ? g_sinkFactory() : 0);
    if (!sink_ || !sink_->Open(outRate_)) {
      sink_.reset(CreateNullAudioSink());
      sink_->Open(outRate_);
    }
    sink_->SetPaused(true);
  }
  return true;
}

void Decoder::Close() {
  for (size_t i = 0; i < vq_.size(); ++i) av_packet_free(&vq_[i]);
  for (size_t i = 0; i < aq_.size(); ++i) av_packet_free(&aq_[i]);
  vq_.clear();
  aq_.clear();
  if (vctx_) avcodec_free_context(&vctx_);
  if (actx_) avcodec_free_context(&actx_);
  if (swr_) swr_free(&swr_);
  if (frame_) av_frame_free(&frame_);
  if (pkt_) av_packet_free(&pkt_);
  if (fmt_) avformat_close_input(&fmt_);
  if (io_) {
    av_freep(&io_->buffer);
    avio_context_free(&io_);
  }
  if (src_) {
    src_->stop = true;
    src_->wantMore.Signal();
  }
  sink_.reset();
}

bool Decoder::Demux() {
  if (demuxEof_) return false;
  int r = av_read_frame(fmt_, pkt_);
  if (r < 0) {
    demuxEof_ = true;
    return false;
  }
  if (pkt_->stream_index == vi_ || pkt_->stream_index == ai_) {
    AVPacket* p = av_packet_alloc();
    av_packet_move_ref(p, pkt_);
    queuedBytes_ += p->size;
    (p->stream_index == vi_ ? vq_ : aq_).push_back(p);
    // Do not let one stream's queue grow without bound while the other is
    // starved (badly interleaved files).
    while (queuedBytes_ > 48 * 1024 * 1024) {
      std::deque<AVPacket*>& q = vq_.size() > aq_.size() ? vq_ : aq_;
      if (q.empty()) break;
      queuedBytes_ -= q.front()->size;
      av_packet_free(&q.front());
      q.pop_front();
    }
  } else {
    av_packet_unref(pkt_);
  }
  return true;
}

double Decoder::Clock() {
  if (sink_ && !audioClockDead_ && audioBaseSet_) return audioBase_ + sink_->Played() / (double)outRate_;
  if (!playing_) return wallBase_;
  return wallBase_ + (NowMs() - wallStart_) / 1000.0;
}

void Decoder::Publish(VideoFrame& vf) {
  {
    MutexLock l(s_->mu);
    s_->frame.width = vf.image.width;
    s_->frame.height = vf.image.height;
    s_->frame.hasAlpha = vf.image.hasAlpha;
    s_->frame.pixels.swap(vf.image.pixels);
    ++s_->frameVersion;
    if (s_->status.videoWidth != vf.image.width || s_->status.videoHeight != vf.image.height) {
      s_->status.videoWidth = vf.image.width;
      s_->status.videoHeight = vf.image.height;
      s_->Event("resize");
    }
  }
  lastShownMs_ = NowMs();
  BumpGeneration();
}

bool Decoder::DecodeVideo() {
  if (vi_ < 0 || videoEof_) return false;
  for (;;) {
    int r = avcodec_receive_frame(vctx_, frame_);
    if (r == 0) {
      double pts = TimeOf(frame_->best_effort_timestamp, fmt_->streams[vi_]->time_base) - start_;
      if (std::isnan(pts)) pts = lastPts_;
      lastPts_ = pts;
      if (dropUntil_ >= 0 && pts < dropUntil_ - 0.001) {
        av_frame_unref(frame_);
        continue;
      }
      // Too late: skip the colour conversion, but still show a frame now and
      // then so slow machines get a (choppy) picture.
      if (playing_ && !needShow_ && pts < Clock() - 0.05 && NowMs() - lastShownMs_ < 200) {
        av_frame_unref(frame_);
        return true;
      }
      VideoFrame vf;
      vf.pts = pts;
      bool ok = ConvertFrame(frame_, vf.image);
      av_frame_unref(frame_);
      if (ok) frames_.push_back(std::move(vf));
      return true;
    }
    if (r == AVERROR_EOF) {
      videoEof_ = true;
      return false;
    }
    if (r != AVERROR(EAGAIN)) {
      videoEof_ = true;
      return false;
    }
    if (vq_.empty() && !demuxEof_) {
      while (vq_.empty() && Demux()) {
      }
    }
    if (vq_.empty()) {
      if (vDrainSent_) {
        videoEof_ = true;
        return false;
      }
      avcodec_send_packet(vctx_, 0);
      vDrainSent_ = true;
      continue;
    }
    AVPacket* p = vq_.front();
    vq_.pop_front();
    queuedBytes_ -= p->size;
    avcodec_send_packet(vctx_, p);
    av_packet_free(&p);
  }
}

void Decoder::WriteAudio(AVFrame* f) {
  if (!swr_) {
    int64_t layout = f->channel_layout ? (int64_t)f->channel_layout : av_get_default_channel_layout(f->channels);
    swr_ = swr_alloc_set_opts(0, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, outRate_, layout,
                              (AVSampleFormat)f->format, f->sample_rate, 0, 0);
    if (!swr_ || swr_init(swr_) < 0) {
      if (swr_) swr_free(&swr_);
      audioEof_ = true;
      return;
    }
  }
  double pts = TimeOf(f->best_effort_timestamp, fmt_->streams[ai_]->time_base) - start_;
  int cap = swr_get_out_samples(swr_, f->nb_samples) + 32;
  abuf_.resize((size_t)cap * 2);
  uint8_t* outp = (uint8_t*)&abuf_[0];
  int n = swr_convert(swr_, &outp, cap, (const uint8_t**)f->extended_data, f->nb_samples);
  if (n <= 0) return;
  int skip = 0;
  if (dropUntil_ >= 0 && !std::isnan(pts)) {
    if (pts + n / (double)outRate_ <= dropUntil_) return;
    if (pts < dropUntil_) skip = std::min(n, (int)((dropUntil_ - pts) * outRate_));
    pts += skip / (double)outRate_;
  }
  double vol;
  {
    MutexLock l(s_->mu);
    vol = s_->muted ? 0 : s_->volume;
  }
  if (vol < 0.999) {
    int scale = (int)(vol * 256);
    for (int i = skip * 2; i < n * 2; ++i) abuf_[i] = (int16_t)(abuf_[i] * scale / 256);
  }
  if (!audioBaseSet_) {
    audioBase_ = std::isnan(pts) ? (dropUntil_ >= 0 ? dropUntil_ : 0) : pts;
    audioBaseSet_ = true;
  }
  sink_->Write(&abuf_[skip * 2], n - skip);
  written_ += n - skip;
}

bool Decoder::DecodeAudio() {
  if (ai_ < 0 || audioEof_) return false;
  for (;;) {
    int r = avcodec_receive_frame(actx_, frame_);
    if (r == 0) {
      WriteAudio(frame_);
      av_frame_unref(frame_);
      return true;
    }
    if (r != AVERROR(EAGAIN)) {
      audioEof_ = true;
      return false;
    }
    if (aq_.empty() && !demuxEof_) {
      while (aq_.empty() && Demux()) {
      }
    }
    if (aq_.empty()) {
      if (aDrainSent_) {
        audioEof_ = true;
        return false;
      }
      avcodec_send_packet(actx_, 0);
      aDrainSent_ = true;
      continue;
    }
    AVPacket* p = aq_.front();
    aq_.pop_front();
    queuedBytes_ -= p->size;
    avcodec_send_packet(actx_, p);
    av_packet_free(&p);
  }
}

void Decoder::DoSeek(double t) {
  if (t < 0) t = 0;
  if (!std::isnan(duration_) && t > duration_) t = duration_;
  int64_t ts = (int64_t)((t + start_) * AV_TIME_BASE);
  av_seek_frame(fmt_, -1, ts, AVSEEK_FLAG_BACKWARD);
  for (size_t i = 0; i < vq_.size(); ++i) av_packet_free(&vq_[i]);
  for (size_t i = 0; i < aq_.size(); ++i) av_packet_free(&aq_[i]);
  vq_.clear();
  aq_.clear();
  queuedBytes_ = 0;
  frames_.clear();
  if (vctx_) avcodec_flush_buffers(vctx_);
  if (actx_) avcodec_flush_buffers(actx_);
  if (swr_) swr_free(&swr_);
  demuxEof_ = videoEof_ = audioEof_ = false;
  vDrainSent_ = aDrainSent_ = false;
  if (sink_) sink_->Flush();
  written_ = 0;
  audioBaseSet_ = false;
  audioClockDead_ = false;
  audioBase_ = t;
  wallBase_ = t;
  wallStart_ = NowMs();
  dropUntil_ = t;
  needShow_ = true;
}

void Decoder::UpdateStatus(bool forceTime) {
  double now = Clock();
  if (!std::isnan(duration_) && now > duration_) now = duration_;
  if (now < 0) now = 0;
  long long ms = NowMs();
  bool fire = playing_ && (forceTime || ms - lastTimeUpdate_ >= 250);
  double buffered = now;
  if (src_ && src_->size > 0 && !std::isnan(duration_)) {
    long long ahead = src_->ContiguousFrom(pos_);
    buffered = std::min(duration_, (pos_ + ahead) / (double)src_->size * duration_);
    if (src_->memory) buffered = duration_;
  }
  MutexLock l(s_->mu);
  if (!s_->status.seeking) s_->status.currentTime = now;
  s_->status.bufferedEnd = std::max(buffered, now);
  if (fire) {
    lastTimeUpdate_ = ms;
    s_->Event("timeupdate");
  }
}

void Decoder::Run() {
  std::string error;
  int code = 0;
  if (!Open(error, code)) {
    if (!s_->stop) Fail(s_.get(), code, error);
    return;
  }
  {
    MutexLock l(s_->mu);
    MediaStatus& st = s_->status;
    st.duration = std::isnan(duration_) && src_->size < 0 ? std::numeric_limits<double>::infinity() : duration_;
    st.hasAudio = ai_ >= 0;
    st.hasVideo = vi_ >= 0;
    if (vi_ >= 0) {
      st.videoWidth = fmt_->streams[vi_]->codecpar->width;
      st.videoHeight = fmt_->streams[vi_]->codecpar->height;
    }
    st.readyState = 1;
    s_->Event("durationchange");
    s_->Event("loadedmetadata");
  }
  BumpGeneration();
  bool ended = false;
  while (!s_->stop) {
    // Commands.
    bool wantPlay, seek = false;
    double seekTo = 0;
    bool loop;
    {
      MutexLock l(s_->mu);
      wantPlay = s_->wantPlay;
      loop = s_->loop;
      if (s_->seekPending) {
        seek = true;
        seekTo = s_->seekTo;
        s_->seekPending = false;
      }
    }
    if (seek) {
      DoSeek(seekTo);
      ended = false;
    }
    if (wantPlay && !playing_) {
      if (ended) {
        DoSeek(0);
        ended = false;
      }
      playing_ = true;
      wallStart_ = NowMs();
      if (sink_) sink_->SetPaused(false);
      if (firstFrame_) {
        MutexLock l(s_->mu);
        s_->Event("playing");
      }
      UpdateStatus(true);
    } else if (!wantPlay && playing_) {
      wallBase_ = Clock();
      playing_ = false;
      if (sink_) sink_->SetPaused(true);
    }

    // Decode ahead.
    if (sink_ && !audioEof_) {
      while (!s_->stop && !audioEof_ && (written_ - sink_->Played()) < outRate_ * 6 / 10) {
        if (!DecodeAudio()) break;
      }
    }
    size_t maxFrames = frames_.empty() || frames_.back().image.width * frames_.back().image.height < 1280 * 720 ? 4 : 2;
    while (!s_->stop && vi_ >= 0 && !videoEof_ && frames_.size() < maxFrames) {
      if (!DecodeVideo()) break;
    }

    // Present.
    bool shown = false;
    if (!frames_.empty() && needShow_) {
      Publish(frames_.front());
      frames_.pop_front();
      shown = true;
    } else if (playing_ && !frames_.empty()) {
      double c = Clock();
      while (!frames_.empty() && frames_.front().pts <= c + 0.005) {
        if (frames_.size() == 1 || frames_[1].pts > c + 0.005) {
          Publish(frames_.front());
          shown = true;
        }
        frames_.pop_front();
      }
    }
    bool audioReady = ai_ < 0 || written_ > 0 || audioEof_;
    if ((shown || (vi_ < 0 && audioReady) || (videoEof_ && audioReady)) && (needShow_ || !firstFrame_)) {
      needShow_ = false;
      dropUntil_ = -1;
      MutexLock l(s_->mu);
      if (!firstFrame_) {
        firstFrame_ = true;
        s_->status.readyState = 4;
        s_->Event("loadeddata");
        s_->Event("canplay");
        s_->Event("canplaythrough");
        if (playing_) s_->Event("playing");
      }
      FinishSeek();
    }
    if (needShow_ && vi_ >= 0 && videoEof_ && frames_.empty()) {  // seek past the end
      needShow_ = false;
      dropUntil_ = -1;
      MutexLock l(s_->mu);
      FinishSeek();
    }

    // Audio ran out: let the wall clock continue (longer video tracks).
    if (sink_ && audioEof_ && !audioClockDead_ && sink_->Played() >= written_) {
      wallBase_ = Clock();
      wallStart_ = NowMs();
      audioClockDead_ = true;
    }

    // End of stream.
    bool done = demuxEof_ && (vi_ < 0 || (videoEof_ && frames_.empty())) && (ai_ < 0 || (audioEof_ && audioClockDead_));
    if (playing_ && done && !needShow_) {
      if (std::isnan(duration_) || Clock() > duration_ + 0.5) {
        double d = Clock();
        MutexLock l(s_->mu);
        if (std::isnan(duration_) || std::isinf(s_->status.duration)) {
          s_->status.duration = d;
          s_->Event("durationchange");
        }
        duration_ = d;
      }
      if (loop) {
        DoSeek(0);
        wallStart_ = NowMs();
        if (sink_) sink_->SetPaused(false);
      } else {
        wallBase_ = Clock();
        playing_ = false;
        ended = true;
        if (sink_) sink_->SetPaused(true);
        MutexLock l(s_->mu);
        s_->wantPlay = false;
        s_->status.paused = true;
        s_->status.ended = true;
        s_->status.currentTime = std::isnan(duration_) ? wallBase_ : duration_;
        s_->Event("timeupdate");
        s_->Event("pause");
        s_->Event("ended");
      }
      BumpGeneration();
    }
    if (!ended) UpdateStatus(false);
    bool pending;
    {
      MutexLock l(s_->mu);
      pending = !s_->events.empty();
    }
    if (pending) WakeUi();  // the UI thread delivers the events

    // Sleep until the next frame is due or a command arrives.
    int wait = 50;
    if (playing_) {
      wait = 15;
      if (!frames_.empty()) {
        double dt = frames_.front().pts - Clock();
        wait = (int)std::max(1.0, std::min(15.0, dt * 1000));
      }
    }
    bool filling = !s_->stop && ((sink_ && !audioEof_ && written_ - sink_->Played() < outRate_ / 2) ||
                                 (vi_ >= 0 && !videoEof_ && frames_.size() < maxFrames));
    if (filling && (playing_ || !firstFrame_ || needShow_)) wait = 1;
    s_->wake.Wait(wait);
  }
}

void RunDecoder(void* arg) {
  std::shared_ptr<Shared>* p = (std::shared_ptr<Shared>*)arg;
  {
    Decoder d(*p);
    d.Run();
  }
  delete p;
}

#else  // !KITE_HAVE_FFMPEG

void RunDecoder(void* arg) {
  std::shared_ptr<Shared>* p = (std::shared_ptr<Shared>*)arg;
  Fail(p->get(), 4, "Ohne Mediendecoder gebaut");
  delete p;
}

#endif

}  // namespace

// ---------------------------------------------------------------- player

MediaPlayer::MediaPlayer(const std::string& url, const std::string& referrer, bool audioOnly, Node* element,
                         const void* owner)
    : url_(url), element_(element), owner_(owner), shared_(std::make_shared<Shared>()) {
  {
    MutexLock l(RegistryMutex());
    id_ = g_nextId++;
    Registry()[id_] = this;
  }
  shared_->url = url;
  shared_->referrer = referrer;
  shared_->audioOnly = audioOnly;
  shared_->status.duration = std::numeric_limits<double>::quiet_NaN();
  shared_->status.networkState = 2;
  shared_->Event("emptied");
  shared_->Event("loadstart");
  std::shared_ptr<Shared>* arg = new std::shared_ptr<Shared>(shared_);
  if (!StartThread(RunDecoder, arg)) {
    delete arg;
    Fail(shared_.get(), 2, "Thread");
  }
}

MediaPlayer::~MediaPlayer() {
  {
    MutexLock l(RegistryMutex());
    Registry().erase(id_);
  }
  shared_->stop = true;
  shared_->cancel->Cancel();
  shared_->wake.Signal();
}

void MediaPlayer::Play() {
  {
    MutexLock l(shared_->mu);
    MediaStatus& st = shared_->status;
    if (st.error) return;
    st.started = true;
    if (!st.paused) return;
    st.paused = false;
    shared_->wantPlay = true;
    if (st.ended) {
      st.ended = false;
      st.currentTime = 0;
    }
    shared_->Event("play");
    if (st.readyState < 3) shared_->Event("waiting");
  }
  shared_->wake.Signal();
  BumpGeneration();
}

void MediaPlayer::Pause() {
  {
    MutexLock l(shared_->mu);
    MediaStatus& st = shared_->status;
    if (st.paused) return;
    st.paused = true;
    shared_->wantPlay = false;
    shared_->Event("timeupdate");
    shared_->Event("pause");
  }
  shared_->wake.Signal();
  BumpGeneration();
}

void MediaPlayer::Seek(double t) {
  if (std::isnan(t)) return;
  {
    MutexLock l(shared_->mu);
    MediaStatus& st = shared_->status;
    if (t < 0) t = 0;
    if (!std::isnan(st.duration) && !std::isinf(st.duration) && t > st.duration) t = st.duration;
    shared_->seekPending = true;
    shared_->seekTo = t;
    st.seeking = true;
    st.ended = false;
    st.currentTime = t;
    shared_->Event("seeking");
  }
  shared_->wake.Signal();
  BumpGeneration();
}

void MediaPlayer::Init(bool muted, double volume, bool loop) {
  muted_ = muted;
  volume_ = volume;
  MutexLock l(shared_->mu);
  shared_->muted = muted;
  shared_->volume = volume;
  shared_->loop = loop;
}

void MediaPlayer::SetVolume(double v) {
  v = v < 0 ? 0 : (v > 1 ? 1 : v);
  if (v == volume_) return;
  volume_ = v;
  MutexLock l(shared_->mu);
  shared_->volume = v;
  shared_->Event("volumechange");
}

void MediaPlayer::SetMuted(bool m) {
  if (m == muted_) return;
  muted_ = m;
  MutexLock l(shared_->mu);
  shared_->muted = m;
  shared_->Event("volumechange");
}

void MediaPlayer::SetLoop(bool on) {
  MutexLock l(shared_->mu);
  shared_->loop = on;
}

void MediaPlayer::SetPlaybackRate(double r) {
  if (r == rate_ || !(r > 0)) return;
  rate_ = r;
  MutexLock l(shared_->mu);
  shared_->Event("ratechange");
}

MediaStatus MediaPlayer::Status() const {
  MutexLock l(shared_->mu);
  return shared_->status;
}

std::vector<std::string> MediaPlayer::TakeEvents() {
  std::vector<std::string> out;
  MutexLock l(shared_->mu);
  out.swap(shared_->events);
  return out;
}

bool MediaPlayer::HasEvents() const {
  MutexLock l(shared_->mu);
  return !shared_->events.empty();
}

const DecodedImage* MediaPlayer::Frame(unsigned* version) {
  {
    MutexLock l(shared_->mu);
    if (shared_->frameVersion != shownVersion_ && !shared_->frame.pixels.empty()) {
      shown_.width = shared_->frame.width;
      shown_.height = shared_->frame.height;
      shown_.hasAlpha = shared_->frame.hasAlpha;
      shown_.pixels.swap(shared_->frame.pixels);
      shownVersion_ = shared_->frameVersion;
    }
  }
  if (version) *version = shownVersion_;
  return shown_.pixels.empty() ? 0 : &shown_;
}

// ---------------------------------------------------------------- registry

MediaPlayer* FindMediaPlayer(int id) {
  if (!id) return 0;
  MutexLock l(RegistryMutex());
  std::map<int, MediaPlayer*>::iterator it = Registry().find(id);
  return it == Registry().end() ? 0 : it->second;
}

std::string MediaUrl(int id) { return "kite-media:" + IntToString(id); }

const DecodedImage* MediaFrameForUrl(const std::string& url, unsigned* version) {
  if (!StartsWith(url, "kite-media:")) return 0;
  long long id;
  if (!ParseInt(url.substr(11), id)) return 0;
  MediaPlayer* p = FindMediaPlayer((int)id);
  return p ? p->Frame(version) : 0;
}

unsigned MediaGeneration() {
  MutexLock l(RegistryMutex());
  return g_generation;
}

void DestroyMediaPlayer(int id) { delete FindMediaPlayer(id); }

std::string MediaSourceAttr(Node* el) {
  if (!el) return std::string();
  std::string src = Trim(el->Attr("src"));
  if (el->HasAttr("src")) return src;
  for (size_t i = 0; i < el->children.size(); ++i) {
    Node* c = el->children[i].get();
    if (!c->Is("source") || Trim(c->Attr("src")).empty()) continue;
    std::string type = c->Attr("type");
    if (!Trim(type).empty() && MediaCanPlayType(type).empty()) continue;
    return Trim(c->Attr("src"));
  }
  return std::string();
}

MediaPlayer* EnsureMediaPlayer(Node* el, const std::string& baseUrl, const void* owner) {
  if (!el) return 0;
  std::string src = MediaSourceAttr(el);
  MediaPlayer* p = FindMediaPlayer(el->mediaId);
  if (src.empty()) return p;
  Url u = Url::Parse(baseUrl).Resolve(src);
  std::string url = u.valid() ? u.SpecNoFragment() : src;
  if (p && p->url() == url) return p;
  bool muted = p ? p->muted() : el->HasAttr("muted");
  double volume = p ? p->volume() : 1;
  delete p;
  p = new MediaPlayer(url, baseUrl, el->tag == "audio", el, owner);
  el->mediaId = p->id();
  p->Init(muted, volume, el->HasAttr("loop"));
  // Media fragment "#t=10".
  size_t hash = src.find("#t=");
  if (hash != std::string::npos) {
    size_t used;
    double t = ParseDoublePrefix(src.substr(hash + 3), used);
    if (used && t > 0) p->Seek(t);
  }
  return p;
}

std::vector<std::pair<Node*, std::string> > TakeMediaEvents(const void* owner) {
  std::vector<MediaPlayer*> players;
  {
    MutexLock l(RegistryMutex());
    for (std::map<int, MediaPlayer*>::iterator it = Registry().begin(); it != Registry().end(); ++it)
      if (it->second->owner() == owner) players.push_back(it->second);
  }
  std::vector<std::pair<Node*, std::string> > out;
  for (size_t i = 0; i < players.size(); ++i) {
    std::vector<std::string> ev = players[i]->TakeEvents();
    for (size_t k = 0; k < ev.size(); ++k) out.push_back(std::make_pair(players[i]->element(), ev[k]));
  }
  return out;
}

bool MediaEventsPending(const void* owner) {
  MutexLock l(RegistryMutex());
  for (std::map<int, MediaPlayer*>::iterator it = Registry().begin(); it != Registry().end(); ++it)
    if (it->second->owner() == owner && it->second->HasEvents()) return true;
  return false;
}

bool UpdateMediaElements(Node* root, const std::string& baseUrl, const void* owner) {
  if (!root) return false;
  bool created = false;
  std::vector<Node*> stack(1, root);
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    for (size_t i = 0; i < n->children.size(); ++i) stack.push_back(n->children[i].get());
    if (n->shadowRoot) stack.push_back(n->shadowRoot.get());
    if (!n->IsElement() || (n->tag != "video" && n->tag != "audio")) continue;
    MediaPlayer* existing = FindMediaPlayer(n->mediaId);
    std::string preload = AsciiLower(Trim(n->Attr("preload")));
    bool autoplay = n->HasAttr("autoplay");
    // Like other browsers, load metadata (and a video's first frame) unless
    // preload=none; invisible <audio> elements wait until a script asks.
    bool want = existing || autoplay ||
                ((n->tag == "video" || n->HasAttr("controls")) ? preload != "none"
                                                              : (preload == "auto" || preload == "metadata"));
    if (!want || MediaSourceAttr(n).empty()) continue;
    MediaPlayer* p = EnsureMediaPlayer(n, baseUrl, owner);
    if (!p) continue;
    if (p != existing) created = true;
    // Autoplay only without sound (like other browsers' default policy).
    if (p != existing && autoplay && p->muted()) p->Play();
  }
  return created;
}

MediaControls LayoutMediaControls(const Rect& c, bool audio) {
  MediaControls m;
  float h = audio ? c.h : std::min(32.0f, c.h);
  m.bar = Rect(c.x, c.bottom() - h, c.w, h);
  float cy = m.bar.y + h / 2;
  m.play = Rect(c.x + 4, cy - 12, 24, 24);
  float timeW = c.w >= 220 ? 84 : 0;
  m.time = Rect(m.play.right() + 4, m.bar.y, timeW, h);
  m.mute = c.w >= 120 ? Rect(c.right() - 28, cy - 12, 24, 24) : Rect(c.right(), cy, 0, 0);
  float tx = m.time.right() + 6, tr = m.mute.x - 8;
  m.track = Rect(tx, cy - 6, std::max(0.0f, tr - tx), 12);
  return m;
}

std::string FormatMediaTime(double t) {
  if (!(t >= 0) || std::isinf(t)) return "--:--";
  long long s = (long long)t;
  std::string sec = IntToString(s % 60);
  if (sec.size() < 2) sec = "0" + sec;
  if (s >= 3600) {
    std::string min = IntToString(s / 60 % 60);
    if (min.size() < 2) min = "0" + min;
    return IntToString(s / 3600) + ":" + min + ":" + sec;
  }
  return IntToString(s / 60) + ":" + sec;
}

bool MediaControlsClick(Node* el, const Rect& content, float x, float y) {
  if (!el || !el->HasAttr("controls")) return false;
  bool audio = el->tag == "audio";
  MediaControls m = LayoutMediaControls(content, audio);
  MediaPlayer* p = FindMediaPlayer(el->mediaId);
  if (!m.bar.contains(x, y)) {
    // Clicking the picture toggles playback, like other browsers.
    if (!p || !content.contains(x, y)) return false;
    MediaStatus st = p->Status();
    if (st.paused) p->Play();
    else p->Pause();
    return true;
  }
  if (!p) return true;
  MediaStatus st = p->Status();
  if (x < m.play.right() + 2) {
    if (st.paused) p->Play();
    else p->Pause();
  } else if (m.mute.w > 0 && x >= m.mute.x - 2) {
    p->SetMuted(!p->muted());
  } else if (m.track.w > 0 && x >= m.track.x - 4 && x <= m.track.right() + 4 && st.duration > 0 &&
             !std::isinf(st.duration)) {
    double f = (x - m.track.x) / m.track.w;
    f = f < 0 ? 0 : (f > 1 ? 1 : f);
    p->Seek(f * st.duration);
  }
  BumpGeneration();
  return true;
}

bool MediaSupported() {
#ifdef KITE_HAVE_FFMPEG
  return true;
#else
  return false;
#endif
}

std::string MediaCanPlayType(const std::string& typeIn) {
  if (!MediaSupported()) return std::string();
  std::string type = AsciiLower(Trim(typeIn));
  std::string params;
  size_t semi = type.find(';');
  if (semi != std::string::npos) {
    params = type.substr(semi + 1);
    type = Trim(type.substr(0, semi));
  }
  static const char* const types[] = {
      "video/mp4", "audio/mp4", "audio/x-m4a", "audio/m4a", "video/quicktime", "video/webm", "audio/webm",
      "video/ogg", "audio/ogg", "application/ogg", "audio/mpeg", "audio/mp3", "audio/mpeg3", "audio/x-mp3",
      "audio/wav", "audio/wave", "audio/x-wav", "audio/aac", "audio/x-aac", "audio/aacp", "audio/flac",
      "audio/x-flac", "video/x-matroska", "audio/x-matroska", "audio/opus", 0};
  bool known = false;
  for (int i = 0; types[i]; ++i)
    if (type == types[i]) known = true;
  if (!known) return std::string();
  size_t c = params.find("codecs");
  if (c == std::string::npos) return "maybe";
  std::string list = params.substr(c + 6);
  size_t eq = list.find('=');
  if (eq != std::string::npos) list = list.substr(eq + 1);
  list = ReplaceAll(ReplaceAll(list, "\"", ""), "'", "");
  std::vector<std::string> codecs = Split(list, ',');
  static const char* const ok[] = {"avc1", "avc3", "hev1", "hvc1", "mp4a", "vp8", "vp9", "vp09", "vp8.0",
                                   "opus", "vorbis", "flac", "mp3", "theora", "1", "alac", "ulaw", "alaw", 0};
  for (size_t i = 0; i < codecs.size(); ++i) {
    std::string cdc = Trim(codecs[i]);
    std::string head = cdc.substr(0, cdc.find('.'));
    bool good = false;
    for (int k = 0; ok[k]; ++k)
      if (head == ok[k] || cdc == ok[k]) good = true;
    if (!good) return std::string();
  }
  return "probably";
}

}  // namespace kite
