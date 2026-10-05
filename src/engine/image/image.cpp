#include "image/image.h"

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

bool DecodeImage(const std::string& data, DecodedImage& out, size_t maxPixels) {
  if (data.empty() || data.size() > 0x7fffffff) return false;
  int w = 0, h = 0, comp = 0;
  if (!stbi_info_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h, &comp))
    return false;
  if (w <= 0 || h <= 0 || (long long)w * h > 64LL * 1024 * 1024) return false;
  unsigned char* px = stbi_load_from_memory((const stbi_uc*)data.data(), (int)data.size(), &w, &h,
                                            &comp, 4);
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
  stbi_image_free(px);
  return true;
}

}  // namespace kite
