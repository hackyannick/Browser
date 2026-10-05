// Kite Engine - interfaces the layout engine needs from the platform
#ifndef KITE_LAYOUT_RESOURCES_H
#define KITE_LAYOUT_RESOURCES_H

#include <cstdint>
#include <string>
#include <vector>

#include "base/geometry.h"

namespace kite {

struct ComputedStyle;
struct DecodedImage;

struct FontDesc {
  std::string family;  // CSS font-family list, lower-case, comma separated
  float size;          // px
  int weight;
  bool italic;
  FontDesc() : size(16), weight(400), italic(false) {}
  bool operator<(const FontDesc& o) const {
    if (size != o.size) return size < o.size;
    if (weight != o.weight) return weight < o.weight;
    if (italic != o.italic) return italic < o.italic;
    return family < o.family;
  }
  static FontDesc FromStyle(const ComputedStyle& s);
};

struct FontMetrics {
  float ascent;
  float descent;
  float lineGap;
  FontMetrics() : ascent(12), descent(4), lineGap(0) {}
};

// Grayscale rendering of a text run (used by <canvas> fillText).
struct TextMask {
  int width = 0, height = 0;
  float baseline = 0;  // distance from the top row to the baseline
  std::vector<uint8_t> alpha;
};

// Text measurement. Implemented with GDI on Windows and with a simple
// approximation for headless tests.
class FontProvider {
 public:
  virtual ~FontProvider() {}
  virtual FontMetrics Metrics(const FontDesc& font) = 0;
  virtual float MeasureText(const FontDesc& font, const std::string& utf8) = 0;
  // Optional: rasterizes |utf8| starting at x = 0. Returns false if the
  // platform cannot draw text off-screen.
  virtual bool RasterizeText(const FontDesc& font, const std::string& utf8, TextMask& out) {
    return false;
  }
};

// Information about images (loaded asynchronously by the platform layer).
class ImageProvider {
 public:
  // kUnsupported: a format Kite cannot decode (SVG); shown as empty space
  // instead of alt text.
  enum State { kUnknown, kLoading, kLoaded, kFailed, kUnsupported };
  virtual ~ImageProvider() {}
  virtual State GetImage(const std::string& url, int& width, int& height) = 0;
  // Decoded pixels of a loaded image (for canvas drawImage), or null.
  virtual const DecodedImage* Pixels(const std::string& url) { return 0; }
};

// Approximate metrics, used by the headless tools and tests.
class SimpleFontProvider : public FontProvider {
 public:
  FontMetrics Metrics(const FontDesc& font);
  float MeasureText(const FontDesc& font, const std::string& utf8);
};

}  // namespace kite

#endif
