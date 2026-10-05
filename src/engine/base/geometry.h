// Kite Engine - basic geometry and color types
#ifndef KITE_BASE_GEOMETRY_H
#define KITE_BASE_GEOMETRY_H

#include <cstdint>
#include <algorithm>

namespace kite {

// Layout uses floats in CSS pixels; painting rounds to device pixels.
struct Rect {
  float x, y, w, h;
  Rect() : x(0), y(0), w(0), h(0) {}
  Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
  float right() const { return x + w; }
  float bottom() const { return y + h; }
  bool contains(float px, float py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
  bool intersects(const Rect& o) const {
    return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
  }
  Rect intersect(const Rect& o) const {
    float nx = std::max(x, o.x), ny = std::max(y, o.y);
    float nr = std::min(right(), o.right()), nb = std::min(bottom(), o.bottom());
    if (nr <= nx || nb <= ny) return Rect(nx, ny, 0, 0);
    return Rect(nx, ny, nr - nx, nb - ny);
  }
  bool empty() const { return w <= 0 || h <= 0; }
};

struct Color {
  uint8_t r, g, b, a;
  Color() : r(0), g(0), b(0), a(0) {}
  Color(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255)
      : r(r_), g(g_), b(b_), a(a_) {}
  bool transparent() const { return a == 0; }
  bool operator==(const Color& o) const {
    return r == o.r && g == o.g && b == o.b && a == o.a;
  }
  bool operator!=(const Color& o) const { return !(*this == o); }
  static Color Black() { return Color(0, 0, 0); }
  static Color White() { return Color(255, 255, 255); }
};

}  // namespace kite

#endif
