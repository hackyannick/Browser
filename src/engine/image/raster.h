// Kite Engine - 2D vector graphics core shared by the SVG renderer and
// <canvas>: affine matrices, path flattening, an anti-aliased scanline
// rasterizer and a stroker.
#ifndef KITE_IMAGE_RASTER_H
#define KITE_IMAGE_RASTER_H

#include <cmath>
#include <string>
#include <vector>

namespace kite {
namespace gfx {

const float kPi = 3.14159265358979f;

struct Matrix {
  float a, b, c, d, e, f;
  Matrix() : a(1), b(0), c(0), d(1), e(0), f(0) {}
  Matrix(float a_, float b_, float c_, float d_, float e_, float f_)
      : a(a_), b(b_), c(c_), d(d_), e(e_), f(f_) {}
  Matrix operator*(const Matrix& m) const {  // this * m (m applied first)
    return Matrix(a * m.a + c * m.b, b * m.a + d * m.b, a * m.c + c * m.d, b * m.c + d * m.d,
                  a * m.e + c * m.f + e, b * m.e + d * m.f + f);
  }
  void Apply(float x, float y, float& ox, float& oy) const {
    ox = a * x + c * y + e;
    oy = b * x + d * y + f;
  }
  float Scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
  bool Invert(Matrix& out) const {
    float det = a * d - b * c;
    if (std::fabs(det) < 1e-12f) return false;
    float id = 1 / det;
    out = Matrix(d * id, -b * id, -c * id, a * id, (c * f - d * e) * id, (b * e - a * f) * id);
    return true;
  }
  bool IsIdentity() const { return a == 1 && b == 0 && c == 0 && d == 1 && e == 0 && f == 0; }
};

struct Pt {
  float x, y;
};
typedef std::vector<Pt> Poly;

// Flattens path segments (given in user space) into device-space polylines.
class PathBuilder {
 public:
  PathBuilder(const Matrix& m, float tolerance) : m_(m), tol_(tolerance) {}
  std::vector<Poly> polys;
  std::vector<bool> closed;

  void MoveTo(float x, float y);
  void LineTo(float x, float y);
  void CubicTo(float x1, float y1, float x2, float y2, float x, float y);
  void QuadTo(float x1, float y1, float x, float y);
  // SVG elliptical arc to (x, y).
  void ArcTo(float rx, float ry, float rotDeg, bool large, bool sweep, float x, float y);
  // Canvas-style arc: center, radii, rotation and start/end angles (radians).
  void Ellipse(float cx, float cy, float rx, float ry, float rot, float a0, float a1, bool ccw);
  void Close();
  void Finish() { Flush(false); }
  bool HasCurrentPoint() const { return hasPoint_; }
  float lastX() const { return lx_; }
  float lastY() const { return ly_; }
  // Changes the transform for subsequent segments (canvas semantics: points
  // already added keep their device position).
  void SetMatrix(const Matrix& m);
  const Matrix& matrix() const { return m_; }

 private:
  int Steps(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3);
  void Add(float x, float y);
  void Flush(bool close);
  Matrix m_;
  float tol_;
  Poly cur_;
  bool hasPoint_ = false;
  float sx_ = 0, sy_ = 0, lx_ = 0, ly_ = 0;  // user space
};

// Anti-aliased coverage rasterizer (4x vertical supersampling with exact
// horizontal coverage). Tracks the touched area so that clearing and
// compositing stay cheap for small shapes on big surfaces.
class Raster {
 public:
  Raster(int w, int h);
  void Clear();
  void Fill(const std::vector<Poly>& polys, bool evenOdd);
  int width() const { return w_; }
  int height() const { return h_; }
  float At(int x, int y) const { return cov_[(size_t)y * w_ + x]; }
  const float* Row(int y) const { return &cov_[(size_t)y * w_]; }
  float* MutableRow(int y) { return &cov_[(size_t)y * w_]; }
  // Bounding box of non-zero coverage (x1/y1 exclusive); empty if x0 >= x1.
  int x0, y0, x1, y1;

 private:
  void AddSpan(std::vector<float>& row, float xa, float xb, float weight);
  int w_, h_;
  std::vector<float> cov_;
};

enum LineJoin { kJoinMiter, kJoinRound, kJoinBevel };
enum LineCap { kCapButt, kCapRound, kCapSquare };

struct StrokeStyle {
  float width = 1;  // device pixels
  LineJoin join = kJoinMiter;
  LineCap cap = kCapButt;
  float miterLimit = 10;
  std::vector<float> dash;  // device pixels; empty = solid
  float dashOffset = 0;
};

// Converts polylines into polygons covering their stroke. Fill the result
// with the nonzero rule.
std::vector<Poly> StrokePolys(const std::vector<Poly>& polys, const std::vector<bool>& closed,
                              const StrokeStyle& style);

// Appends SVG path data ("M0 0L10 10Z") to |pb| (implemented in svg.cpp).
void ParseSvgPath(const std::string& d, PathBuilder& pb);

// Point-in-polygon test (nonzero or even-odd).
bool PolysContain(const std::vector<Poly>& polys, float x, float y, bool evenOdd);

}  // namespace gfx
}  // namespace kite

#endif
