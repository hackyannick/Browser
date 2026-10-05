#include "image/image.h"

#include "src/webp/decode.h"
#include "image/avif.h"

#include <algorithm>

#include <cstdlib>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace kite {

namespace {

uint32_t Le(const std::string& d, size_t pos, int n) {
  uint32_t v = 0;
  for (int i = n - 1; i >= 0; --i) v = (v << 8) | (uint8_t)d[pos + i];
  return v;
}

// Straight RGBA (byte order R, G, B, A) to premultiplied BGRA, sampling
// every |step|-th pixel.
void ConvertRgba(const unsigned char* px, int w, int h, int step, std::vector<uint32_t>& dst, bool& alpha) {
  int ow = std::max(1, w / step), oh = std::max(1, h / step);
  dst.resize((size_t)ow * oh);
  for (int y = 0; y < oh; ++y) {
    const unsigned char* row = px + (size_t)(y * step) * w * 4;
    uint32_t* d = &dst[(size_t)y * ow];
    for (int x = 0; x < ow; ++x) {
      const unsigned char* p = row + (size_t)(x * step) * 4;
      unsigned a = p[3];
      unsigned r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;
      if (a != 255) alpha = true;
      d[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
}

void Shrink(const std::vector<uint32_t>& src, int w, int h, int step, std::vector<uint32_t>& dst) {
  if (step == 1) {
    dst = src;
    return;
  }
  int ow = std::max(1, w / step), oh = std::max(1, h / step);
  dst.resize((size_t)ow * oh);
  for (int y = 0; y < oh; ++y)
    for (int x = 0; x < ow; ++x) dst[(size_t)y * ow + x] = src[(size_t)(y * step) * w + x * step];
}

int StepFor(int w, int h, size_t maxPixels) {
  int step = 1;
  while ((size_t)(w / step) * (size_t)(h / step) > maxPixels) ++step;
  return step;
}

// Browsers treat very short frame durations as 100 ms.
int FrameDelay(int ms) { return ms <= 10 ? 100 : ms; }

// Finishes an animation: frame 0 becomes the visible picture.
bool FinishFrames(DecodedImage& out, int w, int h, int step) {
  if (out.frames.empty()) return false;
  out.width = std::max(1, w / step);
  out.height = std::max(1, h / step);
  out.pixels = out.frames[0];
  if (out.frames.size() == 1) {
    out.frames.clear();
    out.delays.clear();
  }
  return true;
}

// GIF with all frames (stb_image composites them; we keep the frame from
// two steps back for the "restore to previous" disposal).
bool DecodeGif(const std::string& data, DecodedImage& out, size_t maxPixels, size_t maxBytes) {
  stbi__context s;
  stbi__start_mem(&s, (const stbi_uc*)data.data(), (int)data.size());
  if (!stbi__gif_test(&s)) return false;
  stbi__gif g;
  memset(&g, 0, sizeof g);
  std::vector<unsigned char> back1, back2;
  int comp = 0, step = 0, w = 0, h = 0;
  size_t bytes = 0;
  out.hasAlpha = false;
  for (;;) {
    stbi_uc* u = stbi__gif_load_next(&s, &g, &comp, 4, back2.empty() ? 0 : back2.data());
    if (!u || u == (stbi_uc*)&s) break;
    if (!step) {
      w = g.w;
      h = g.h;
      if (w <= 0 || h <= 0 || (long long)w * h > 64LL * 1024 * 1024) break;
      step = StepFor(w, h, maxPixels);
    }
    out.frames.push_back(std::vector<uint32_t>());
    ConvertRgba(u, w, h, step, out.frames.back(), out.hasAlpha);
    out.delays.push_back(FrameDelay(g.delay));
    bytes += out.frames.back().size() * 4;
    if (bytes > maxBytes || out.frames.size() >= 2000) break;
    back2.swap(back1);
    back1.assign(u, u + (size_t)w * h * 4);
  }
  STBI_FREE(g.out);
  STBI_FREE(g.history);
  STBI_FREE(g.background);
  return step && FinishFrames(out, w, h, step);
}

// Decodes one ANMF frame payload (ALPH/VP8/VP8L sub-chunks) to straight RGBA.
uint8_t* DecodeWebPFrame(const std::string& chunks, int fw, int fh, int& w, int& h) {
  std::string out = "RIFF....WEBP";
  if (chunks.size() >= 4 && chunks.compare(0, 4, "ALPH") == 0) {
    // Alpha only counts inside an extended (VP8X) file.
    out += std::string("VP8X\x0a\0\0\0\x10\0\0\0", 12);
    for (int i = 0; i < 3; ++i) out += (char)(((fw - 1) >> (8 * i)) & 0xFF);
    for (int i = 0; i < 3; ++i) out += (char)(((fh - 1) >> (8 * i)) & 0xFF);
  }
  out += chunks;
  uint32_t size = (uint32_t)out.size() - 8;
  for (int i = 0; i < 4; ++i) out[4 + i] = (char)((size >> (8 * i)) & 0xFF);
  return WebPDecodeRGBA((const uint8_t*)out.data(), out.size(), &w, &h);
}

// Animated WebP: composites the ANMF frames onto the canvas.
bool DecodeWebPAnimation(const std::string& d, DecodedImage& out, size_t maxPixels, size_t maxBytes) {
  size_t pos = 12;
  int cw = 0, ch = 0;
  int loops = 0;
  std::vector<uint32_t> canvas;
  int step = 1;
  size_t bytes = 0;
  bool disposePending = false;
  int px0 = 0, py0 = 0, pw = 0, ph = 0;
  out.hasAlpha = false;
  while (pos + 8 <= d.size()) {
    uint32_t len = Le(d, pos + 4, 4);
    if (len > d.size() - pos - 8) break;
    size_t body = pos + 8;
    if (d.compare(pos, 4, "VP8X") == 0 && len >= 10) {
      cw = (int)Le(d, body + 4, 3) + 1;
      ch = (int)Le(d, body + 7, 3) + 1;
      if ((long long)cw * ch > 64LL * 1024 * 1024) return false;
      canvas.assign((size_t)cw * ch, 0);
      step = StepFor(cw, ch, maxPixels);
    } else if (d.compare(pos, 4, "ANIM") == 0 && len >= 6) {
      loops = (int)Le(d, body + 4, 2);
    } else if (d.compare(pos, 4, "ANMF") == 0 && len > 16 && cw > 0) {
      int fx = (int)Le(d, body, 3) * 2, fy = (int)Le(d, body + 3, 3) * 2;
      int fw = (int)Le(d, body + 6, 3) + 1, fh = (int)Le(d, body + 9, 3) + 1;
      int duration = (int)Le(d, body + 12, 3);
      int flags = (uint8_t)d[body + 15];
      if (disposePending) {
        for (int y = py0; y < py0 + ph && y < ch; ++y)
          for (int x = px0; x < px0 + pw && x < cw; ++x) canvas[(size_t)y * cw + x] = 0;
        disposePending = false;
      }
      int w = 0, h = 0;
      uint8_t* px = DecodeWebPFrame(d.substr(body + 16, len - 16), fw, fh, w, h);
      if (px) {
        bool blend = !(flags & 2);
        for (int y = 0; y < h; ++y) {
          int cy = fy + y;
          if (cy >= ch) break;
          for (int x = 0; x < w; ++x) {
            int cx = fx + x;
            if (cx >= cw) break;
            const uint8_t* p = px + ((size_t)y * w + x) * 4;
            unsigned a = p[3];
            uint32_t src = (a << 24) | ((p[0] * a / 255) << 16) | ((p[1] * a / 255) << 8) | (p[2] * a / 255);
            uint32_t& dst = canvas[(size_t)cy * cw + cx];
            if (!blend || a == 255) {
              dst = src;
            } else if (a) {
              uint32_t r = 0;
              for (int sh = 0; sh < 32; sh += 8)
                r |= (((src >> sh) & 255) + ((dst >> sh) & 255) * (255 - a) / 255) << sh;
              dst = r;
            }
          }
        }
        WebPFree(px);
      }
      out.frames.push_back(std::vector<uint32_t>());
      Shrink(canvas, cw, ch, step, out.frames.back());
      for (size_t i = 0; i < out.frames.back().size() && !out.hasAlpha; ++i)
        if ((out.frames.back()[i] >> 24) != 255) out.hasAlpha = true;
      out.delays.push_back(FrameDelay(duration));
      bytes += out.frames.back().size() * 4;
      if (bytes > maxBytes || out.frames.size() >= 2000) break;
      if (flags & 1) {
        disposePending = true;
        px0 = fx;
        py0 = fy;
        pw = fw;
        ph = fh;
      }
    }
    pos += 8 + len + (len & 1);
  }
  out.loops = loops;
  return FinishFrames(out, cw, ch, step);
}

}  // namespace

int AnimationFrameAt(const DecodedImage& img, unsigned elapsedMs, int* msUntilNext) {
  if (msUntilNext) *msUntilNext = -1;
  if (!img.animated()) return 0;
  unsigned total = 0;
  for (size_t i = 0; i < img.delays.size(); ++i) total += (unsigned)img.delays[i];
  if (!total) return 0;
  if (img.loops > 0 && elapsedMs >= total * (unsigned)img.loops) return (int)img.frames.size() - 1;
  unsigned t = elapsedMs % total;
  for (size_t i = 0; i < img.delays.size(); ++i) {
    if (t < (unsigned)img.delays[i]) {
      if (msUntilNext) *msUntilNext = (int)((unsigned)img.delays[i] - t);
      return (int)i;
    }
    t -= (unsigned)img.delays[i];
  }
  return 0;
}

bool DecodeImage(const std::string& data, DecodedImage& out, size_t maxPixels, size_t maxAnimBytes) {
  if (data.empty() || data.size() > 0x7fffffff) return false;
  out.frames.clear();
  out.delays.clear();
  out.loops = 0;
  if (LooksLikeAvif(data)) {
    AvifAnimation anim;
    if (DecodeAvifSequence(data, anim, maxAnimBytes) && anim.frames.size() > 1) {
      int step = StepFor(anim.width, anim.height, maxPixels);
      for (size_t i = 0; i < anim.frames.size(); ++i) {
        out.frames.push_back(std::vector<uint32_t>());
        Shrink(anim.frames[i], anim.width, anim.height, step, out.frames.back());
        std::vector<uint32_t>().swap(anim.frames[i]);
        out.delays.push_back(FrameDelay(anim.delays[i]));
      }
      out.hasAlpha = anim.hasAlpha;
      return FinishFrames(out, anim.width, anim.height, step);
    }
    int aw = 0, ah = 0;
    std::vector<uint32_t> px;
    bool alpha = false;
    if (!DecodeAvif(data, aw, ah, px, alpha)) {
      // Sequence without a still item: show its first frame.
      if (anim.frames.empty()) return false;
      aw = anim.width;
      ah = anim.height;
      px.swap(anim.frames[0]);
      alpha = anim.hasAlpha;
    }
    int step = StepFor(aw, ah, maxPixels);
    out.width = std::max(1, aw / step);
    out.height = std::max(1, ah / step);
    out.hasAlpha = alpha;
    if (step == 1) out.pixels.swap(px);
    else Shrink(px, aw, ah, step, out.pixels);
    return true;
  }
  int w = 0, h = 0, comp = 0;
  unsigned char* px = 0;
  bool webp = data.size() >= 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0;
  if (webp) {
    WebPBitstreamFeatures f;
    if (WebPGetFeatures((const uint8_t*)data.data(), data.size(), &f) != VP8_STATUS_OK) return false;
    if (f.has_animation) return DecodeWebPAnimation(data, out, maxPixels, maxAnimBytes);
    if (!WebPGetInfo((const uint8_t*)data.data(), data.size(), &w, &h)) return false;
    if (w <= 0 || h <= 0 || (long long)w * h > 64LL * 1024 * 1024) return false;
    px = WebPDecodeRGBA((const uint8_t*)data.data(), data.size(), &w, &h);
  } else {
    if (data.size() >= 6 && data.compare(0, 4, "GIF8") == 0)
      return DecodeGif(data, out, maxPixels, maxAnimBytes);
    if (!stbi_info_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h, &comp))
      return false;
    if (w <= 0 || h <= 0 || (long long)w * h > 64LL * 1024 * 1024) return false;
    px = stbi_load_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h, &comp, 4);
  }
  if (!px) return false;
  int step = StepFor(w, h, maxPixels);
  out.width = std::max(1, w / step);
  out.height = std::max(1, h / step);
  out.hasAlpha = false;
  ConvertRgba(px, w, h, step, out.pixels, out.hasAlpha);
  if (webp) WebPFree(px);
  else stbi_image_free(px);
  return true;
}

}  // namespace kite
