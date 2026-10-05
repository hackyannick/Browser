// Kite Engine - <canvas> 2D rendering context (software rasterizer).
#ifndef KITE_CANVAS_CANVAS_H
#define KITE_CANVAS_CANVAS_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/geometry.h"
#include "image/image.h"
#include "image/raster.h"
#include "layout/resources.h"

namespace kite {

struct CanvasGradient {
  enum Kind { kLinear, kRadial, kConic };
  Kind kind = kLinear;
  float x0 = 0, y0 = 0, r0 = 0, x1 = 0, y1 = 0, r1 = 0;  // conic: x0/y0 center, r0 start angle
  std::vector<std::pair<float, Color> > stops;
};

struct CanvasPattern {
  DecodedImage image;
  bool repeatX = true, repeatY = true;
  gfx::Matrix transform;
};

struct CanvasPaint {
  enum Kind { kColor, kGradient, kPattern };
  Kind kind = kColor;
  Color color = Color(0, 0, 0);
  std::shared_ptr<CanvasGradient> gradient;
  std::shared_ptr<CanvasPattern> pattern;
};

enum CompositeOp {
  kOpSourceOver, kOpSourceIn, kOpSourceOut, kOpSourceAtop, kOpDestinationOver, kOpDestinationIn,
  kOpDestinationOut, kOpDestinationAtop, kOpLighter, kOpCopy, kOpXor
};

struct TextMetricsResult {
  float width = 0, ascent = 0, descent = 0;
};

class Canvas2D {
 public:
  Canvas2D(int width, int height, FontProvider* fonts);
  ~Canvas2D();
  Canvas2D(const Canvas2D&) = delete;
  Canvas2D& operator=(const Canvas2D&) = delete;

  int id() const { return id_; }
  int width() const { return pixels_.width; }
  int height() const { return pixels_.height; }
  // Clears the bitmap and resets all state (like setting canvas.width).
  void Resize(int w, int h);
  const DecodedImage& pixels() const { return pixels_; }
  unsigned version() const { return version_; }

  // State.
  void Save();
  void Restore();
  void Reset();
  void SetTransform(const gfx::Matrix& m);
  void Transform(const gfx::Matrix& m);
  const gfx::Matrix& transform() const { return st_.ctm; }
  void SetFill(const CanvasPaint& p) { st_.fill = p; }
  void SetStroke(const CanvasPaint& p) { st_.stroke = p; }
  void SetLineWidth(float w) { if (w > 0 && w < 1e6f) st_.line.width = w; }
  void SetLineCap(gfx::LineCap c) { st_.line.cap = c; }
  void SetLineJoin(gfx::LineJoin j) { st_.line.join = j; }
  void SetMiterLimit(float m) { if (m > 0) st_.line.miterLimit = m; }
  void SetLineDash(const std::vector<float>& d) { st_.line.dash = d; }
  void SetLineDashOffset(float o) { st_.line.dashOffset = o; }
  void SetGlobalAlpha(float a) { if (a >= 0 && a <= 1) st_.alpha = a; }
  void SetCompositeOp(CompositeOp op) { st_.op = op; }
  // CSS font shorthand ("bold 16px Arial"). Returns false if invalid.
  bool SetFont(const std::string& css);
  void SetTextAlign(int a) { st_.textAlign = a; }        // 0 start/left 1 center 2 right/end
  void SetTextBaseline(int b) { st_.textBaseline = b; }  // 0 alphabetic 1 top 2 middle 3 bottom 4 hanging 5 ideographic
  void SetImageSmoothing(bool on) { st_.smoothing = on; }
  void SetShadow(Color c, float blur, float ox, float oy) {
    st_.shadowColor = c;
    st_.shadowBlur = blur;
    st_.shadowX = ox;
    st_.shadowY = oy;
  }

  // Paths (coordinates in user space).
  void BeginPath();
  void MoveTo(float x, float y);
  void LineTo(float x, float y);
  void QuadraticCurveTo(float cx, float cy, float x, float y);
  void BezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
  void Arc(float x, float y, float r, float a0, float a1, bool ccw);
  void ArcTo(float x1, float y1, float x2, float y2, float r);
  void Ellipse(float x, float y, float rx, float ry, float rot, float a0, float a1, bool ccw);
  void Rect(float x, float y, float w, float h);
  void RoundRect(float x, float y, float w, float h, const float radii[4]);
  void ClosePath();
  void SvgPath(const std::string& d);
  // Path2D support: temporarily swaps in an empty path.
  void PushPath();
  void PopPath();

  void Fill(bool evenOdd);
  void Stroke();
  void Clip(bool evenOdd);
  bool IsPointInPath(float x, float y, bool evenOdd);
  bool IsPointInStroke(float x, float y);
  void FillRect(float x, float y, float w, float h);
  void StrokeRect(float x, float y, float w, float h);
  void ClearRect(float x, float y, float w, float h);

  void FillText(const std::string& text, float x, float y, float maxWidth, bool stroke);
  TextMetricsResult MeasureText(const std::string& text);

  // Draws |src| (premultiplied BGRA) with the current transform.
  void DrawImage(const DecodedImage& src, float sx, float sy, float sw, float sh, float dx, float dy,
                 float dw, float dh);

  // RGBA, not premultiplied, row-major.
  std::string GetImageData(int x, int y, int w, int h) const;
  void PutImageData(const uint8_t* rgba, int w, int h, int dx, int dy, int dirtyX, int dirtyY,
                    int dirtyW, int dirtyH);
  // PNG file of the current bitmap.
  std::string ToPng() const;

  const FontDesc& font() const { return st_.font; }

 private:
  struct State {
    gfx::Matrix ctm;
    CanvasPaint fill, stroke;
    gfx::StrokeStyle line;  // width in user units
    float alpha = 1;
    CompositeOp op = kOpSourceOver;
    FontDesc font;
    std::string fontCss = "10px sans-serif";
    int textAlign = 0, textBaseline = 0;
    bool smoothing = true;
    std::shared_ptr<std::vector<float> > clip;  // coverage per pixel, null = none
    Color shadowColor;
    float shadowBlur = 0, shadowX = 0, shadowY = 0;
  };
  void Snapshot(std::vector<gfx::Poly>& polys, std::vector<bool>& closed) const;
  std::vector<gfx::Poly> StrokeOutline(const std::vector<gfx::Poly>& polys, const std::vector<bool>& closed) const;
  // Composites the raster's coverage using |paint|. |sampler| (optional)
  // supplies per-pixel premultiplied source colors instead of the paint.
  void Composite(const CanvasPaint& paint, const uint32_t* image, int imageStride);
  void CompositeShadow();
  bool UnboundedOp() const;
  void Touch() { ++version_; }

  int id_;
  FontProvider* fonts_;
  DecodedImage pixels_;
  unsigned version_ = 1;
  State st_;
  std::vector<State> stack_;
  std::unique_ptr<gfx::PathBuilder> path_;
  std::vector<std::unique_ptr<gfx::PathBuilder> > savedPaths_;
  std::unique_ptr<gfx::Raster> raster_;
  // Per-pixel source colors for DrawImage/text (premultiplied BGRA), aligned
  // with the raster.
  std::vector<uint32_t> source_;
};

// Global lookup used by the platform renderer ("kite-canvas:<id>" URLs).
Canvas2D* FindCanvas(int id);
const DecodedImage* CanvasPixelsForUrl(const std::string& url, unsigned* version);
std::string CanvasUrl(int id);

}  // namespace kite

#endif
