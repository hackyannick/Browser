#include "image/image.h"

#include "src/webp/decode.h"

#include <cstdlib>

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

// Animated WebP: rebuilds a still image from the first ANMF frame.
static std::string FirstWebPFrame(const std::string& d) {
  if (d.size() < 20 || d.compare(0, 4, "RIFF") != 0 || d.compare(8, 4, "WEBP") != 0) return std::string();
  size_t pos = 12;
  while (pos + 8 <= d.size()) {
    uint32_t len = (uint8_t)d[pos + 4] | ((uint8_t)d[pos + 5] << 8) | ((uint8_t)d[pos + 6] << 16) |
                   ((uint32_t)(uint8_t)d[pos + 7] << 24);
    if (len > d.size() - pos - 8) break;
    if (d.compare(pos, 4, "ANMF") == 0 && len > 16) {
      // 16 bytes of frame header, then ALPH/VP8/VP8L sub-chunks.
      std::string frame = d.substr(pos + 8 + 16, len - 16);
      std::string out = "RIFF....WEBP" + frame;
      uint32_t size = (uint32_t)out.size() - 8;
      for (int i = 0; i < 4; ++i) out[4 + i] = (char)((size >> (8 * i)) & 0xFF);
      return out;
    }
    pos += 8 + len + (len & 1);
  }
  return std::string();
}

bool DecodeImage(const std::string& data, DecodedImage& out, size_t maxPixels) {
  if (data.empty() || data.size() > 0x7fffffff) return false;
  int w = 0, h = 0, comp = 0;
  unsigned char* px = 0;
  bool webp = data.size() >= 12 && data.compare(0, 4, "RIFF") == 0 && data.compare(8, 4, "WEBP") == 0;
  if (webp) {
    std::string still;
    const std::string* src = &data;
    WebPBitstreamFeatures f;
    if (WebPGetFeatures((const uint8_t*)data.data(), data.size(), &f) != VP8_STATUS_OK) return false;
    if (f.has_animation) {
      still = FirstWebPFrame(data);
      if (still.empty()) return false;
      src = &still;
    }
    if (!WebPGetInfo((const uint8_t*)src->data(), src->size(), &w, &h)) return false;
    if (w <= 0 || h <= 0 || (long long)w * h > 64LL * 1024 * 1024) return false;
    px = WebPDecodeRGBA((const uint8_t*)src->data(), src->size(), &w, &h);
  } else {
    if (!stbi_info_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h, &comp))
      return false;
    if (w <= 0 || h <= 0 || (long long)w * h > 64LL * 1024 * 1024) return false;
    px = stbi_load_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h, &comp, 4);
  }
  if (!px) return false;
  int step = 1;
  while ((size_t)(w / step) * (size_t)(h / step) > maxPixels) ++step;
  int ow = w / step, oh = h / step;
  if (ow < 1) ow = 1;
  if (oh < 1) oh = 1;
  out.width = ow;
  out.height = oh;
  out.pixels.resize((size_t)ow * oh);
  out.hasAlpha = false;
  for (int y = 0; y < oh; ++y) {
    const unsigned char* row = px + (size_t)(y * step) * w * 4;
    uint32_t* dst = &out.pixels[(size_t)y * ow];
    for (int x = 0; x < ow; ++x) {
      const unsigned char* p = row + (size_t)(x * step) * 4;
      unsigned a = p[3];
      unsigned r = p[0] * a / 255, g = p[1] * a / 255, b = p[2] * a / 255;
      if (a != 255) out.hasAlpha = true;
      dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  if (webp) WebPFree(px);
  else stbi_image_free(px);
  return true;
}

}  // namespace kite
