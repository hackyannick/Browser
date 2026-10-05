// Kite Engine - image decoding (PNG, JPEG, GIF, BMP via stb_image)
#ifndef KITE_IMAGE_IMAGE_H
#define KITE_IMAGE_IMAGE_H

#include <cstdint>
#include <string>
#include <vector>

namespace kite {

struct DecodedImage {
  int width;
  int height;
  // Premultiplied BGRA (matches 32-bit Windows DIBs), row-major, top-down.
  std::vector<uint32_t> pixels;
  bool hasAlpha;
  float density;  // pixels per CSS px (2 for vector images rasterized at 2x)
  DecodedImage() : width(0), height(0), hasAlpha(false), density(1) {}
};

// Decodes |data|. Very large images are downsampled so that they fit into
// |maxPixels| (memory is precious on Windows 2000 machines).
bool DecodeImage(const std::string& data, DecodedImage& out, size_t maxPixels = 4096 * 4096);

}  // namespace kite

#endif
