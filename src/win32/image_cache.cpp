#include "win32/app.h"

#include <algorithm>

#include "base/strings.h"
#include "canvas/canvas.h"

namespace kite {

ImageCache& Images() {
  static ImageCache* c = new ImageCache;
  return *c;
}

ImageProvider::State ImageCache::GetImage(const std::string& url, int& width, int& height) {
  if (StartsWith(url, "kite-canvas:")) {
    const DecodedImage* c = CanvasPixelsForUrl(url, 0);
    if (!c) return kFailed;
    width = c->width;
    height = c->height;
    return kLoaded;
  }
  std::map<std::string, Entry>::iterator it = entries_.find(url);
  if (it == entries_.end()) return App::Get().settings.loadImages ? kLoading : kFailed;
  it->second.lastUse = ++clock_;
  float d = it->second.image.density > 0 ? it->second.image.density : 1;
  width = (int)(it->second.image.width / d + 0.5f);
  height = (int)(it->second.image.height / d + 0.5f);
  return it->second.state;
}

const DecodedImage* ImageCache::Find(const std::string& url) {
  if (StartsWith(url, "kite-canvas:")) return CanvasPixelsForUrl(url, 0);
  std::map<std::string, Entry>::iterator it = entries_.find(url);
  if (it == entries_.end() || it->second.state != kLoaded) return 0;
  it->second.lastUse = ++clock_;
  return &it->second.image;
}

void ImageCache::SetLoading(const std::string& url) {
  Entry& e = entries_[url];
  e.state = kLoading;
  e.lastUse = ++clock_;
}

void ImageCache::SetLoaded(const std::string& url, DecodedImage& img) {
  Entry& e = entries_[url];
  e.state = kLoaded;
  e.image.width = img.width;
  e.image.height = img.height;
  e.image.hasAlpha = img.hasAlpha;
  e.image.density = img.density;
  e.image.pixels.swap(img.pixels);
  e.lastUse = ++clock_;
  Trim();
}

void ImageCache::SetFailed(const std::string& url) {
  Entry& e = entries_[url];
  e.state = kFailed;
  e.lastUse = ++clock_;
}

void ImageCache::SetUnsupported(const std::string& url) {
  Entry& e = entries_[url];
  e.state = kUnsupported;
  e.lastUse = ++clock_;
}

void ImageCache::Trim() {
  // Keep decoded images below ~96 MB.
  size_t total = 0;
  for (std::map<std::string, Entry>::iterator it = entries_.begin(); it != entries_.end(); ++it)
    total += it->second.image.pixels.size() * 4;
  const size_t limit = 96u * 1024 * 1024;
  while (total > limit) {
    std::map<std::string, Entry>::iterator oldest = entries_.end();
    for (std::map<std::string, Entry>::iterator it = entries_.begin(); it != entries_.end(); ++it)
      if (it->second.state == kLoaded && !it->second.image.pixels.empty() &&
          (oldest == entries_.end() || it->second.lastUse < oldest->second.lastUse))
        oldest = it;
    if (oldest == entries_.end()) break;
    total -= oldest->second.image.pixels.size() * 4;
    entries_.erase(oldest);  // will be fetched again if needed
  }
}

namespace {

// Box filter when shrinking, bilinear when enlarging.
void ScaleInto(const DecodedImage* src, DecodedImage& dst, int w, int h) {
  dst.width = w;
  dst.height = h;
  dst.hasAlpha = src->hasAlpha;
  dst.pixels.resize((size_t)w * h);
  const int sw = src->width, sh = src->height;
  for (int y = 0; y < h; ++y) {
    float sy0 = (float)y * sh / h, sy1 = (float)(y + 1) * sh / h;
    for (int x = 0; x < w; ++x) {
      float sx0 = (float)x * sw / w, sx1 = (float)(x + 1) * sw / w;
      uint32_t out;
      if (sx1 - sx0 > 1.0f || sy1 - sy0 > 1.0f) {
        int ix0 = (int)sx0, ix1 = std::max(ix0 + 1, std::min(sw, (int)(sx1 + 0.999f)));
        int iy0 = (int)sy0, iy1 = std::max(iy0 + 1, std::min(sh, (int)(sy1 + 0.999f)));
        unsigned a = 0, r = 0, g = 0, b = 0, n = 0;
        int stepX = std::max(1, (ix1 - ix0) / 4), stepY = std::max(1, (iy1 - iy0) / 4);
        for (int yy = iy0; yy < iy1; yy += stepY)
          for (int xx = ix0; xx < ix1; xx += stepX) {
            uint32_t p = src->pixels[(size_t)yy * sw + xx];
            a += p >> 24;
            r += (p >> 16) & 255;
            g += (p >> 8) & 255;
            b += p & 255;
            ++n;
          }
        out = ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
      } else {
        float fx = (x + 0.5f) * sw / w - 0.5f, fy = (y + 0.5f) * sh / h - 0.5f;
        int x0 = std::max(0, (int)fx), y0 = std::max(0, (int)fy);
        int x1 = std::min(sw - 1, x0 + 1), y1 = std::min(sh - 1, y0 + 1);
        float ax = std::max(0.0f, fx - x0), ay = std::max(0.0f, fy - y0);
        uint32_t p00 = src->pixels[(size_t)y0 * sw + x0], p01 = src->pixels[(size_t)y0 * sw + x1];
        uint32_t p10 = src->pixels[(size_t)y1 * sw + x0], p11 = src->pixels[(size_t)y1 * sw + x1];
        out = 0;
        for (int sh8 = 0; sh8 < 32; sh8 += 8) {
          float c00 = (float)((p00 >> sh8) & 255), c01 = (float)((p01 >> sh8) & 255);
          float c10 = (float)((p10 >> sh8) & 255), c11 = (float)((p11 >> sh8) & 255);
          float top = c00 + (c01 - c00) * ax, bot = c10 + (c11 - c10) * ax;
          unsigned v = (unsigned)(top + (bot - top) * ay + 0.5f);
          out |= (v > 255 ? 255u : v) << sh8;
        }
      }
      dst.pixels[(size_t)y * w + x] = out;
    }
  }
}

}  // namespace

const DecodedImage* ImageCache::Scaled(const std::string& url, int w, int h) {
  if (StartsWith(url, "kite-canvas:")) {
    // Canvas bitmaps change; keep one scaled copy per size and version.
    unsigned version = 0;
    const DecodedImage* src = CanvasPixelsForUrl(url, &version);
    if (!src || w <= 0 || h <= 0 || src->width <= 0 || src->height <= 0) return 0;
    if (src->width == w && src->height == h) return src;
    if ((long long)w * h > 4096LL * 4096) return 0;
    std::pair<unsigned, DecodedImage>& e = canvasScaled_[url];
    if (e.first != version || e.second.width != w || e.second.height != h) {
      ScaleInto(src, e.second, w, h);
      e.first = version;
    }
    return &e.second;
  }
  const DecodedImage* src = Find(url);
  if (!src || w <= 0 || h <= 0) return 0;
  if (src->width == w && src->height == h) return src;
  if ((long long)w * h > 4096LL * 4096) return 0;
  std::string key = url + "|" + IntToString(w) + "x" + IntToString(h);
  std::map<std::string, DecodedImage>::iterator it = scaled_.find(key);
  if (it != scaled_.end()) return &it->second;
  DecodedImage& dst = scaled_[key];
  ScaleInto(src, dst, w, h);
  scaledOrder_.push_back(key);
  scaledBytes_ += dst.pixels.size() * 4;
  while (scaledBytes_ > 32u * 1024 * 1024 && scaledOrder_.size() > 1) {
    std::string victim = scaledOrder_.front();
    scaledOrder_.pop_front();
    std::map<std::string, DecodedImage>::iterator v = scaled_.find(victim);
    if (v != scaled_.end()) {
      scaledBytes_ -= v->second.pixels.size() * 4;
      scaled_.erase(v);
    }
  }
  return &dst;
}

}  // namespace kite
