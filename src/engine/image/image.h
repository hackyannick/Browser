// Kite Engine - image decoding (PNG, JPEG, GIF, BMP via stb_image; WebP via libwebp)
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
  // Animated images (GIF, WebP, AVIF sequences): every fully composited
  // frame and its duration in milliseconds. |pixels| holds the frame that
  // is currently shown (initially the first one). Empty for still images.
  std::vector<std::vector<uint32_t> > frames;
  std::vector<int> delays;
  int loops;  // 0 = forever
  DecodedImage() : width(0), height(0), hasAlpha(false), density(1), loops(0) {}
  bool animated() const { return frames.size() > 1; }
};

// Decodes |data|. Very large images are downsampled so that they fit into
// |maxPixels| (memory is precious on Windows 2000 machines). Animations keep
// at most |maxAnimBytes| of frames; longer ones are cut short.
bool DecodeImage(const std::string& data, DecodedImage& out, size_t maxPixels = 4096 * 4096,
                 size_t maxAnimBytes = 48u * 1024 * 1024);

// Frame index to show |elapsedMs| after an animation started.
int AnimationFrameAt(const DecodedImage& img, unsigned elapsedMs, int* msUntilNext);

}  // namespace kite

#endif
