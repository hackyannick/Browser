#include "canvas/canvas.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "base/strings.h"

namespace kite {

using namespace gfx;

namespace {

std::map<int, Canvas2D*>& Registry() {
  static std::map<int, Canvas2D*>* r = new std::map<int, Canvas2D*>;
  return *r;
}
int g_nextCanvasId = 1;

const int kMaxCanvasPixels = 4096 * 4096;

inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

inline uint32_t Premultiply(Color c, float alpha = 1) {
  float a = c.a / 255.0f * alpha;
  unsigned aa = (unsigned)(a * 255 + 0.5f);
  unsigned r = (unsigned)(c.r * a + 0.5f), g = (unsigned)(c.g * a + 0.5f), b = (unsigned)(c.b * a + 0.5f);
  return (aa << 24) | (r << 16) | (g << 8) | b;
}

// Bilinear (or nearest) sample of a premultiplied image, restricted to the
// source rectangle [x0, x1) x [y0, y1).
uint32_t SampleImage(const DecodedImage& img, float u, float v, float x0, float y0, float x1, float y1,
                     bool smooth) {
  if (u < x0 || v < y0 || u >= x1 || v >= y1) return 0;
  int ix0 = std::max(0, (int)x0), iy0 = std::max(0, (int)y0);
  int ix1 = std::min(img.width, (int)std::ceil(x1)) - 1, iy1 = std::min(img.height, (int)std::ceil(y1)) - 1;
  if (ix1 < ix0 || iy1 < iy0) return 0;
  if (!smooth) {
    int x = std::max(ix0, std::min(ix1, (int)u)), y = std::max(iy0, std::min(iy1, (int)v));
    return img.pixels[(size_t)y * img.width + x];
  }
  float fx = u - 0.5f, fy = v - 0.5f;
  int xa = (int)std::floor(fx), ya = (int)std::floor(fy);
  float ax = fx - xa, ay = fy - ya;
  int xb = std::min(ix1, xa + 1), yb = std::min(iy1, ya + 1);
  xa = std::max(ix0, std::min(ix1, xa));
  ya = std::max(iy0, std::min(iy1, ya));
  xb = std::max(ix0, xb);
  yb = std::max(iy0, yb);
  uint32_t p00 = img.pixels[(size_t)ya * img.width + xa], p01 = img.pixels[(size_t)ya * img.width + xb];
  uint32_t p10 = img.pixels[(size_t)yb * img.width + xa], p11 = img.pixels[(size_t)yb * img.width + xb];
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    float c00 = (float)((p00 >> sh) & 255), c01 = (float)((p01 >> sh) & 255);
    float c10 = (float)((p10 >> sh) & 255), c11 = (float)((p11 >> sh) & 255);
    float top = c00 + (c01 - c00) * ax, bot = c10 + (c11 - c10) * ax;
    unsigned val = (unsigned)(top + (bot - top) * ay + 0.5f);
    out |= (val > 255 ? 255u : val) << sh;
  }
  return out;
}

// Gradient color ramp (premultiplied) with 256 entries.
void BuildRamp(const CanvasGradient& g, float alpha, uint32_t ramp[256]) {
  std::vector<std::pair<float, Color> > stops = g.stops;
  std::stable_sort(stops.begin(), stops.end(),
                   [](const std::pair<float, Color>& a, const std::pair<float, Color>& b) { return a.first < b.first; });
  for (int i = 0; i < 256; ++i) {
    float t = i / 255.0f;
    Color c;
    if (stops.empty()) {
      c = Color(0, 0, 0, 0);
    } else if (t <= stops.front().first) {
      c = stops.front().second;
    } else if (t >= stops.back().first) {
      c = stops.back().second;
    } else {
      size_t k = 1;
      while (k < stops.size() && stops[k].first < t) ++k;
      const std::pair<float, Color>& a = stops[k - 1];
      const std::pair<float, Color>& b = stops[k];
      float f = b.first > a.first ? (t - a.first) / (b.first - a.first) : 1;
      c.r = (uint8_t)(a.second.r + (b.second.r - a.second.r) * f + 0.5f);
      c.g = (uint8_t)(a.second.g + (b.second.g - a.second.g) * f + 0.5f);
      c.b = (uint8_t)(a.second.b + (b.second.b - a.second.b) * f + 0.5f);
      c.a = (uint8_t)(a.second.a + (b.second.a - a.second.a) * f + 0.5f);
    }
    ramp[i] = Premultiply(c, alpha);
  }
}

// Returns the gradient parameter at user-space point (x, y), or < -1 when
// the point is not painted.
float GradientT(const CanvasGradient& g, float x, float y) {
  if (g.kind == CanvasGradient::kLinear) {
    float dx = g.x1 - g.x0, dy = g.y1 - g.y0, len2 = dx * dx + dy * dy;
    if (len2 <= 0) return -2;
    return ((x - g.x0) * dx + (y - g.y0) * dy) / len2;
  }
  if (g.kind == CanvasGradient::kConic) {
    float a = std::atan2(y - g.y0, x - g.x0) - g.r0;
    float t = a / (2 * kPi);
    t -= std::floor(t);
    return t;
  }
  float cdx = g.x1 - g.x0, cdy = g.y1 - g.y0, dr = g.r1 - g.r0;
  float pdx = x - g.x0, pdy = y - g.y0;
  float a = cdx * cdx + cdy * cdy - dr * dr;
  float b = pdx * cdx + pdy * cdy + g.r0 * dr;
  float c = pdx * pdx + pdy * pdy - g.r0 * g.r0;
  float w;
  if (std::fabs(a) < 1e-6f) {
    if (std::fabs(b) < 1e-6f) return -2;
    w = c / (2 * b);
    if (g.r0 + w * dr < 0) return -2;
  } else {
    float disc = b * b - a * c;
    if (disc < 0) return -2;
    float sq = std::sqrt(disc);
    float w1 = (b + sq) / a, w2 = (b - sq) / a;
    if (w1 < w2) std::swap(w1, w2);
    if (g.r0 + w1 * dr >= 0) w = w1;
    else if (g.r0 + w2 * dr >= 0) w = w2;
    else return -2;
  }
  return w;
}

}  // namespace

float GradientParameter(const CanvasGradient& g, float x, float y) { return GradientT(g, x, y); }
void GradientRamp(const CanvasGradient& g, float alpha, uint32_t ramp[256]) { BuildRamp(g, alpha, ramp); }

Canvas2D* FindCanvas(int id) {
  std::map<int, Canvas2D*>::iterator it = Registry().find(id);
  return it == Registry().end() ? 0 : it->second;
}

std::string CanvasUrl(int id) { return "kite-canvas:" + IntToString(id); }

const DecodedImage* CanvasPixelsForUrl(const std::string& url, unsigned* version) {
  if (!StartsWith(url, "kite-canvas:")) return 0;
  long long id;
  if (!ParseInt(url.substr(12), id)) return 0;
  Canvas2D* c = FindCanvas((int)id);
  if (!c || c->width() <= 0 || c->height() <= 0) return 0;
  if (version) *version = c->version();
  return &c->pixels();
}

Canvas2D::Canvas2D(int width, int height, FontProvider* fonts) : id_(g_nextCanvasId++), fonts_(fonts) {
  Registry()[id_] = this;
  Resize(width, height);
}

Canvas2D::~Canvas2D() { Registry().erase(id_); }

void Canvas2D::Resize(int w, int h) {
  if (w < 0) w = 0;
  if (h < 0) h = 0;
  if ((long long)w * h > kMaxCanvasPixels) w = h = 0;
  pixels_.width = w;
  pixels_.height = h;
  pixels_.hasAlpha = true;
  pixels_.pixels.assign((size_t)w * h, 0);
  raster_.reset(new Raster(std::max(w, 1), std::max(h, 1)));
  source_.assign((size_t)std::max(w, 1) * std::max(h, 1), 0);
  Reset();
  Touch();
}

void Canvas2D::Reset() {
  st_ = State();
  st_.font.family = "sans-serif";
  st_.font.size = 10;
  stack_.clear();
  savedPaths_.clear();
  path_.reset(new PathBuilder(st_.ctm, 0.25f));
}

void Canvas2D::Save() {
  if (stack_.size() < 512) stack_.push_back(st_);
}

void Canvas2D::Restore() {
  if (stack_.empty()) return;
  st_ = stack_.back();
  stack_.pop_back();
  path_->SetMatrix(st_.ctm);
}

void Canvas2D::SetTransform(const Matrix& m) {
  if (!std::isfinite(m.a) || !std::isfinite(m.b) || !std::isfinite(m.c) || !std::isfinite(m.d) ||
      !std::isfinite(m.e) || !std::isfinite(m.f))
    return;
  st_.ctm = m;
  path_->SetMatrix(m);
}

void Canvas2D::Transform(const Matrix& m) { SetTransform(st_.ctm * m); }

bool Canvas2D::SetFont(const std::string& css) {
  std::vector<std::string> tokens;
  std::string cur;
  char quote = 0;
  for (size_t i = 0; i < css.size(); ++i) {
    char c = css[i];
    if (quote) {
      cur += c;
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
      cur += c;
    } else if (IsAsciiSpace((unsigned char)c)) {
      if (!cur.empty()) tokens.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) tokens.push_back(cur);
  FontDesc f;
  bool haveSize = false;
  size_t i = 0;
  for (; i < tokens.size() && !haveSize; ++i) {
    std::string t = AsciiLower(tokens[i]);
    if (t == "italic" || t == "oblique") f.italic = true;
    else if (t == "bold" || t == "bolder") f.weight = 700;
    else if (t == "lighter") f.weight = 300;
    else if (t == "normal" || t == "small-caps" || t.find("condensed") != std::string::npos ||
             t.find("expanded") != std::string::npos)
      continue;
    else {
      long long w;
      if (t.find_first_not_of("0123456789") == std::string::npos && ParseInt(t, w) && w >= 1 && w <= 1000) {
        f.weight = (int)w;
        continue;
      }
      std::string size = t.substr(0, t.find('/'));
      size_t used = 0;
      double v = ParseDoublePrefix(size, used);
      if (used == 0 || v <= 0) return false;
      std::string unit = size.substr(used);
      if (unit == "px" || unit.empty()) f.size = (float)v;
      else if (unit == "pt") f.size = (float)(v * 4 / 3);
      else if (unit == "em" || unit == "rem") f.size = (float)(v * 16);
      else if (unit == "%") f.size = (float)(v * 16 / 100);
      else return false;
      haveSize = true;
    }
  }
  if (!haveSize || i >= tokens.size()) return false;
  std::string family;
  for (; i < tokens.size(); ++i) family += (family.empty() ? "" : " ") + tokens[i];
  std::vector<std::string> fams = Split(family, ',');
  std::string norm;
  for (size_t k = 0; k < fams.size(); ++k) {
    std::string n = AsciiLower(Trim(fams[k]));
    if (n.size() >= 2 && (n[0] == '"' || n[0] == '\'')) n = n.substr(1, n.size() - 2);
    if (n.empty()) continue;
    norm += (norm.empty() ? "" : ",") + n;
  }
  if (norm.empty()) return false;
  f.family = norm;
  f.size = std::min(f.size, 1000.0f);
  st_.font = f;
  st_.fontCss = css;
  return true;
}

// ---------------------------------------------------------------------------
// Paths

void Canvas2D::BeginPath() { path_.reset(new PathBuilder(st_.ctm, 0.25f)); }
void Canvas2D::MoveTo(float x, float y) {
  if (std::isfinite(x) && std::isfinite(y)) path_->MoveTo(x, y);
}
void Canvas2D::LineTo(float x, float y) {
  if (std::isfinite(x) && std::isfinite(y)) path_->LineTo(x, y);
}
void Canvas2D::QuadraticCurveTo(float cx, float cy, float x, float y) { path_->QuadTo(cx, cy, x, y); }
void Canvas2D::BezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
  path_->CubicTo(c1x, c1y, c2x, c2y, x, y);
}
void Canvas2D::Arc(float x, float y, float r, float a0, float a1, bool ccw) {
  if (r < 0 || !std::isfinite(r)) return;
  path_->Ellipse(x, y, r, r, 0, a0, a1, ccw);
}
void Canvas2D::Ellipse(float x, float y, float rx, float ry, float rot, float a0, float a1, bool ccw) {
  if (rx < 0 || ry < 0) return;
  path_->Ellipse(x, y, rx, ry, rot, a0, a1, ccw);
}

void Canvas2D::ArcTo(float x1, float y1, float x2, float y2, float r) {
  if (!path_->HasCurrentPoint()) {
    path_->MoveTo(x1, y1);
    return;
  }
  float x0 = path_->lastX(), y0 = path_->lastY();
  float v1x = x0 - x1, v1y = y0 - y1, v2x = x2 - x1, v2y = y2 - y1;
  float l1 = std::hypot(v1x, v1y), l2 = std::hypot(v2x, v2y);
  float cross = v1x * v2y - v1y * v2x;
  if (r <= 0 || l1 < 1e-6f || l2 < 1e-6f || std::fabs(cross) < 1e-6f) {
    path_->LineTo(x1, y1);
    return;
  }
  v1x /= l1;
  v1y /= l1;
  v2x /= l2;
  v2y /= l2;
  float cosA = v1x * v2x + v1y * v2y;
  float angle = std::acos(std::max(-1.0f, std::min(1.0f, cosA)));
  float dist = r / std::tan(angle / 2);
  float t1x = x1 + v1x * dist, t1y = y1 + v1y * dist;
  float t2x = x1 + v2x * dist, t2y = y1 + v2y * dist;
  float bx = v1x + v2x, by = v1y + v2y, bl = std::hypot(bx, by);
  float cd = r / std::sin(angle / 2);
  float cx = x1 + bx / bl * cd, cy = y1 + by / bl * cd;
  float a0 = std::atan2(t1y - cy, t1x - cx), a1 = std::atan2(t2y - cy, t2x - cx);
  path_->LineTo(t1x, t1y);
  path_->Ellipse(cx, cy, r, r, 0, a0, a1, cross > 0);
}

void Canvas2D::Rect(float x, float y, float w, float h) {
  path_->MoveTo(x, y);
  path_->LineTo(x + w, y);
  path_->LineTo(x + w, y + h);
  path_->LineTo(x, y + h);
  path_->Close();
  path_->MoveTo(x, y);
}

void Canvas2D::RoundRect(float x, float y, float w, float h, const float radii[4]) {
  float r[4];
  for (int i = 0; i < 4; ++i) r[i] = std::max(0.0f, radii[i]);
  if (w < 0) {
    x += w;
    w = -w;
  }
  if (h < 0) {
    y += h;
    h = -h;
  }
  float scale = 1;
  if (r[0] + r[1] > w) scale = std::min(scale, w / (r[0] + r[1]));
  if (r[3] + r[2] > w) scale = std::min(scale, w / (r[3] + r[2]));
  if (r[0] + r[3] > h) scale = std::min(scale, h / (r[0] + r[3]));
  if (r[1] + r[2] > h) scale = std::min(scale, h / (r[1] + r[2]));
  for (int i = 0; i < 4; ++i) r[i] *= scale;
  path_->MoveTo(x + r[0], y);
  path_->LineTo(x + w - r[1], y);
  if (r[1] > 0) path_->Ellipse(x + w - r[1], y + r[1], r[1], r[1], 0, -kPi / 2, 0, false);
  path_->LineTo(x + w, y + h - r[2]);
  if (r[2] > 0) path_->Ellipse(x + w - r[2], y + h - r[2], r[2], r[2], 0, 0, kPi / 2, false);
  path_->LineTo(x + r[3], y + h);
  if (r[3] > 0) path_->Ellipse(x + r[3], y + h - r[3], r[3], r[3], 0, kPi / 2, kPi, false);
  path_->LineTo(x, y + r[0]);
  if (r[0] > 0) path_->Ellipse(x + r[0], y + r[0], r[0], r[0], 0, kPi, kPi * 1.5f, false);
  path_->Close();
  path_->MoveTo(x, y);
}

void Canvas2D::ClosePath() { path_->Close(); }

void Canvas2D::SvgPath(const std::string& d) { ParseSvgPath(d, *path_); }

void Canvas2D::PushPath() {
  if (savedPaths_.size() > 64) return;
  savedPaths_.push_back(std::move(path_));
  path_.reset(new PathBuilder(st_.ctm, 0.25f));
}

void Canvas2D::PopPath() {
  if (savedPaths_.empty()) return;
  path_ = std::move(savedPaths_.back());
  savedPaths_.pop_back();
  path_->SetMatrix(st_.ctm);
}

void Canvas2D::Snapshot(std::vector<Poly>& polys, std::vector<bool>& closed) const {
  // Copy, so that the current subpath stays open for further segments.
  PathBuilder copy = *path_;
  copy.Finish();
  polys.swap(copy.polys);
  closed.swap(copy.closed);
}

std::vector<Poly> Canvas2D::StrokeOutline(const std::vector<Poly>& polys, const std::vector<bool>& closed) const {
  StrokeStyle s = st_.line;
  float scale = st_.ctm.Scale();
  s.width = std::max(0.1f, s.width * scale);
  for (size_t i = 0; i < s.dash.size(); ++i) s.dash[i] *= scale;
  s.dashOffset *= scale;
  return StrokePolys(polys, closed, s);
}

// ---------------------------------------------------------------------------
// Drawing

bool Canvas2D::UnboundedOp() const {
  return st_.op == kOpSourceIn || st_.op == kOpSourceOut || st_.op == kOpDestinationIn ||
         st_.op == kOpDestinationAtop || st_.op == kOpCopy;
}

void Canvas2D::Composite(const CanvasPaint& paint, const uint32_t* image, int imageStride) {
  int W = pixels_.width, Hh = pixels_.height;
  if (W <= 0 || Hh <= 0) return;
  Raster& r = *raster_;
  int x0 = r.x0, y0 = r.y0, x1 = std::min(r.x1, W), y1 = std::min(r.y1, Hh);
  bool unbounded = UnboundedOp();
  if (unbounded) {
    x0 = 0;
    y0 = 0;
    x1 = W;
    y1 = Hh;
  }
  if (x0 >= x1 || y0 >= y1) return;
  // Paint setup.
  uint32_t solid = 0, ramp[256];
  Matrix inv;
  bool invOk = st_.ctm.Invert(inv);
  Matrix patInv;
  if (paint.kind == CanvasPaint::kColor) solid = Premultiply(paint.color);
  else if (paint.kind == CanvasPaint::kGradient && paint.gradient) BuildRamp(*paint.gradient, 1, ramp);
  else if (paint.kind == CanvasPaint::kPattern && paint.pattern) {
    if (!paint.pattern->transform.Invert(patInv)) return;
  }
  const float* clip = st_.clip ? &(*st_.clip)[0] : 0;
  float galpha = st_.alpha;
  bool inRaster;
  for (int y = y0; y < y1; ++y) {
    uint32_t* dstRow = &pixels_.pixels[(size_t)y * W];
    bool rowInRaster = y >= r.y0 && y < r.y1;
    const float* covRow = rowInRaster ? r.Row(y) : 0;
    for (int x = x0; x < x1; ++x) {
      inRaster = covRow && x >= r.x0 && x < r.x1;
      float cov = inRaster ? covRow[x] : 0;
      float c = clip ? clip[(size_t)y * W + x] : 1;
      if (c <= 0) continue;
      if (cov <= 0 && !unbounded) continue;
      uint32_t src = 0;
      if (cov > 0) {
        if (image) {
          src = image[(size_t)y * imageStride + x];
        } else if (paint.kind == CanvasPaint::kColor) {
          src = solid;
        } else if (paint.kind == CanvasPaint::kGradient && paint.gradient) {
          if (!invOk) continue;
          float ux, uy;
          inv.Apply(x + 0.5f, y + 0.5f, ux, uy);
          float t = GradientT(*paint.gradient, ux, uy);
          if (t < -1.5f) src = 0;
          else src = ramp[(int)(Clamp01(t) * 255 + 0.5f)];
        } else if (paint.kind == CanvasPaint::kPattern && paint.pattern) {
          if (!invOk) continue;
          float ux, uy, px, py;
          inv.Apply(x + 0.5f, y + 0.5f, ux, uy);
          patInv.Apply(ux, uy, px, py);
          const DecodedImage& img = paint.pattern->image;
          if (img.width <= 0 || img.height <= 0) continue;
          if (paint.pattern->repeatX) px -= std::floor(px / img.width) * img.width;
          if (paint.pattern->repeatY) py -= std::floor(py / img.height) * img.height;
          if (px < 0 || py < 0 || px >= img.width || py >= img.height) src = 0;
          else src = img.pixels[(size_t)(int)py * img.width + (int)px];
        }
      }
      float k = std::min(1.0f, cov) * galpha;
      float sa = (src >> 24) / 255.0f * k, sr = ((src >> 16) & 255) / 255.0f * k,
            sg = ((src >> 8) & 255) / 255.0f * k, sb = (src & 255) / 255.0f * k;
      uint32_t d = dstRow[x];
      float da = (d >> 24) / 255.0f, dr = ((d >> 16) & 255) / 255.0f, dg = ((d >> 8) & 255) / 255.0f,
            db = (d & 255) / 255.0f;
      float fa, fb;
      switch (st_.op) {
        case kOpSourceIn: fa = da; fb = 0; break;
        case kOpSourceOut: fa = 1 - da; fb = 0; break;
        case kOpSourceAtop: fa = da; fb = 1 - sa; break;
        case kOpDestinationOver: fa = 1 - da; fb = 1; break;
        case kOpDestinationIn: fa = 0; fb = sa; break;
        case kOpDestinationOut: fa = 0; fb = 1 - sa; break;
        case kOpDestinationAtop: fa = 1 - da; fb = sa; break;
        case kOpLighter: fa = 1; fb = 1; break;
        case kOpCopy: fa = 1; fb = 0; break;
        case kOpXor: fa = 1 - da; fb = 1 - sa; break;
        default: fa = 1; fb = 1 - sa; break;
      }
      float oa = std::min(1.0f, sa * fa + da * fb), or_ = std::min(1.0f, sr * fa + dr * fb),
            og = std::min(1.0f, sg * fa + dg * fb), ob = std::min(1.0f, sb * fa + db * fb);
      if (c < 1) {
        oa = da + (oa - da) * c;
        or_ = dr + (or_ - dr) * c;
        og = dg + (og - dg) * c;
        ob = db + (ob - db) * c;
      }
      unsigned ia = (unsigned)(oa * 255 + 0.5f), ir = (unsigned)(or_ * 255 + 0.5f),
               ig = (unsigned)(og * 255 + 0.5f), ib = (unsigned)(ob * 255 + 0.5f);
      ir = std::min(ir, ia);
      ig = std::min(ig, ia);
      ib = std::min(ib, ia);
      dstRow[x] = (ia << 24) | (ir << 16) | (ig << 8) | ib;
    }
  }
  Touch();
}

void Canvas2D::CompositeShadow() {
  if (st_.shadowColor.a == 0 || (st_.shadowBlur <= 0 && st_.shadowX == 0 && st_.shadowY == 0)) return;
  int W = pixels_.width, Hh = pixels_.height;
  Raster& r = *raster_;
  if (r.x0 >= r.x1) return;
  // Shadow coverage: shifted (and blurred) copy of the shape coverage.
  int blur = (int)std::ceil(st_.shadowBlur / 2);
  int ox = (int)std::floor(st_.shadowX + 0.5f), oy = (int)std::floor(st_.shadowY + 0.5f);
  int sx0 = std::max(0, r.x0 + ox - 3 * blur), sy0 = std::max(0, r.y0 + oy - 3 * blur);
  int sx1 = std::min(W, r.x1 + ox + 3 * blur), sy1 = std::min(Hh, r.y1 + oy + 3 * blur);
  if (sx0 >= sx1 || sy0 >= sy1) return;
  int bw = sx1 - sx0, bh = sy1 - sy0;
  std::vector<float> buf((size_t)bw * bh, 0.0f), tmp((size_t)bw * bh);
  for (int y = sy0; y < sy1; ++y)
    for (int x = sx0; x < sx1; ++x) {
      int qx = x - ox, qy = y - oy;
      if (qx >= r.x0 && qx < r.x1 && qy >= r.y0 && qy < r.y1)
        buf[(size_t)(y - sy0) * bw + (x - sx0)] = r.At(qx, qy);
    }
  for (int pass = 0; pass < 3 && blur > 0; ++pass) {
    for (int y = 0; y < bh; ++y) {  // horizontal box blur
      float acc = 0;
      for (int x = -blur; x < bw + blur; ++x) {
        if (x + blur < bw) acc += buf[(size_t)y * bw + x + blur];
        if (x - blur - 1 >= 0) acc -= buf[(size_t)y * bw + x - blur - 1];
        if (x >= 0 && x < bw) tmp[(size_t)y * bw + x] = acc / (2 * blur + 1);
      }
    }
    for (int x = 0; x < bw; ++x) {  // vertical box blur
      float acc = 0;
      for (int y = -blur; y < bh + blur; ++y) {
        if (y + blur < bh) acc += tmp[(size_t)(y + blur) * bw + x];
        if (y - blur - 1 >= 0) acc -= tmp[(size_t)(y - blur - 1) * bw + x];
        if (y >= 0 && y < bh) buf[(size_t)y * bw + x] = acc / (2 * blur + 1);
      }
    }
  }
  float a = st_.shadowColor.a / 255.0f * st_.alpha;
  const float* clip = st_.clip ? &(*st_.clip)[0] : 0;
  for (int y = sy0; y < sy1; ++y)
    for (int x = sx0; x < sx1; ++x) {
      float cv = buf[(size_t)(y - sy0) * bw + (x - sx0)] * a;
      if (clip) cv *= clip[(size_t)y * W + x];
      if (cv <= 0.002f) continue;
      uint32_t& d = pixels_.pixels[(size_t)y * W + x];
      float da = (d >> 24) / 255.0f;
      unsigned na = (unsigned)((cv + da * (1 - cv)) * 255 + 0.5f);
      unsigned nr = (unsigned)(st_.shadowColor.r * cv + ((d >> 16) & 255) * (1 - cv) + 0.5f);
      unsigned ng = (unsigned)(st_.shadowColor.g * cv + ((d >> 8) & 255) * (1 - cv) + 0.5f);
      unsigned nb = (unsigned)(st_.shadowColor.b * cv + (d & 255) * (1 - cv) + 0.5f);
      d = (std::min(na, 255u) << 24) | (std::min(nr, na) << 16) | (std::min(ng, na) << 8) | std::min(nb, na);
    }
}

void Canvas2D::Fill(bool evenOdd) {
  std::vector<Poly> polys;
  std::vector<bool> closed;
  Snapshot(polys, closed);
  raster_->Clear();
  raster_->Fill(polys, evenOdd);
  CompositeShadow();
  Composite(st_.fill, 0, 0);
}

void Canvas2D::Stroke() {
  std::vector<Poly> polys;
  std::vector<bool> closed;
  Snapshot(polys, closed);
  raster_->Clear();
  raster_->Fill(StrokeOutline(polys, closed), false);
  CompositeShadow();
  Composite(st_.stroke, 0, 0);
}

void Canvas2D::Clip(bool evenOdd) {
  int W = pixels_.width, Hh = pixels_.height;
  if (W <= 0 || Hh <= 0) return;
  std::vector<Poly> polys;
  std::vector<bool> closed;
  Snapshot(polys, closed);
  raster_->Clear();
  raster_->Fill(polys, evenOdd);
  std::shared_ptr<std::vector<float> > mask(new std::vector<float>((size_t)W * Hh, 0.0f));
  for (int y = raster_->y0; y < std::min(raster_->y1, Hh); ++y)
    for (int x = raster_->x0; x < std::min(raster_->x1, W); ++x) (*mask)[(size_t)y * W + x] = raster_->At(x, y);
  if (st_.clip)
    for (size_t i = 0; i < mask->size(); ++i) (*mask)[i] *= (*st_.clip)[i];
  st_.clip = mask;
  raster_->Clear();
}

bool Canvas2D::IsPointInPath(float x, float y, bool evenOdd) {
  std::vector<Poly> polys;
  std::vector<bool> closed;
  Snapshot(polys, closed);
  float dx, dy;
  st_.ctm.Apply(x, y, dx, dy);  // the path is in device space
  return PolysContain(polys, dx, dy, evenOdd);
}

bool Canvas2D::IsPointInStroke(float x, float y) {
  std::vector<Poly> polys;
  std::vector<bool> closed;
  Snapshot(polys, closed);
  float dx, dy;
  st_.ctm.Apply(x, y, dx, dy);
  return PolysContain(StrokeOutline(polys, closed), dx, dy, false);
}

void Canvas2D::FillRect(float x, float y, float w, float h) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h) || w == 0 || h == 0) return;
  PathBuilder pb(st_.ctm, 0.25f);
  pb.MoveTo(x, y);
  pb.LineTo(x + w, y);
  pb.LineTo(x + w, y + h);
  pb.LineTo(x, y + h);
  pb.Close();
  raster_->Clear();
  raster_->Fill(pb.polys, false);
  CompositeShadow();
  Composite(st_.fill, 0, 0);
}

void Canvas2D::StrokeRect(float x, float y, float w, float h) {
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h)) return;
  PathBuilder pb(st_.ctm, 0.25f);
  pb.MoveTo(x, y);
  pb.LineTo(x + w, y);
  pb.LineTo(x + w, y + h);
  pb.LineTo(x, y + h);
  pb.Close();
  pb.Finish();
  raster_->Clear();
  raster_->Fill(StrokeOutline(pb.polys, pb.closed), false);
  CompositeShadow();
  Composite(st_.stroke, 0, 0);
}

void Canvas2D::ClearRect(float x, float y, float w, float h) {
  int W = pixels_.width, Hh = pixels_.height;
  if (W <= 0 || Hh <= 0 || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h))
    return;
  const Matrix& m = st_.ctm;
  if (m.b == 0 && m.c == 0 && !st_.clip) {
    // Fast path: axis-aligned clear.
    float ax0, ay0, ax1, ay1;
    m.Apply(x, y, ax0, ay0);
    m.Apply(x + w, y + h, ax1, ay1);
    if (ax0 > ax1) std::swap(ax0, ax1);
    if (ay0 > ay1) std::swap(ay0, ay1);
    int ix0 = std::max(0, (int)std::floor(ax0 + 0.5f)), iy0 = std::max(0, (int)std::floor(ay0 + 0.5f));
    int ix1 = std::min(W, (int)std::floor(ax1 + 0.5f)), iy1 = std::min(Hh, (int)std::floor(ay1 + 0.5f));
    for (int yy = iy0; yy < iy1; ++yy)
      std::fill(&pixels_.pixels[(size_t)yy * W + ix0], &pixels_.pixels[(size_t)yy * W + ix0] + std::max(0, ix1 - ix0), 0u);
    Touch();
    return;
  }
  State saved = st_;
  st_.op = kOpDestinationOut;
  st_.alpha = 1;
  st_.shadowColor = Color();
  st_.fill = CanvasPaint();
  FillRect(x, y, w, h);
  st_ = saved;
}

TextMetricsResult Canvas2D::MeasureText(const std::string& text) {
  TextMetricsResult r;
  if (!fonts_) return r;
  r.width = fonts_->MeasureText(st_.font, text);
  FontMetrics fm = fonts_->Metrics(st_.font);
  r.ascent = fm.ascent;
  r.descent = fm.descent;
  return r;
}

void Canvas2D::FillText(const std::string& text, float x, float y, float maxWidth, bool stroke) {
  if (!fonts_ || text.empty() || !std::isfinite(x) || !std::isfinite(y)) return;
  // Whitespace characters are drawn as spaces.
  std::string t = text;
  for (size_t i = 0; i < t.size(); ++i)
    if (t[i] == '\n' || t[i] == '\t' || t[i] == '\r' || t[i] == '\f') t[i] = ' ';
  TextMask mask;
  if (!fonts_->RasterizeText(st_.font, t, mask) || mask.width <= 0 || mask.height <= 0) return;
  FontMetrics fm = fonts_->Metrics(st_.font);
  float width = fonts_->MeasureText(st_.font, t);
  float sx = 1;
  if (maxWidth > 0 && width > maxWidth) sx = maxWidth / width;
  float ox = 0;
  if (st_.textAlign == 1) ox = -width * sx / 2;
  else if (st_.textAlign == 2) ox = -width * sx;
  // Baseline position relative to the mask's top row.
  float base = mask.baseline;
  float oy;
  switch (st_.textBaseline) {
    case 1: case 4: oy = -(base - fm.ascent); break;           // top / hanging
    case 2: oy = (fm.ascent - fm.descent) / 2 - base; break;      // middle
    case 3: case 5: oy = -(base + fm.descent); break;          // bottom / ideographic
    default: oy = -base; break;                                // alphabetic
  }
  Matrix m = st_.ctm * Matrix(sx, 0, 0, 1, x + ox, y + oy);
  // Mask as an image whose alpha channel carries the glyph coverage.
  DecodedImage img;
  img.width = mask.width;
  img.height = mask.height;
  img.pixels.resize(mask.alpha.size());
  for (size_t i = 0; i < mask.alpha.size(); ++i) img.pixels[i] = (uint32_t)mask.alpha[i] << 24;
  Matrix inv;
  if (!m.Invert(inv)) return;
  Poly quad(4);
  m.Apply(0, 0, quad[0].x, quad[0].y);
  m.Apply((float)img.width, 0, quad[1].x, quad[1].y);
  m.Apply((float)img.width, (float)img.height, quad[2].x, quad[2].y);
  m.Apply(0, (float)img.height, quad[3].x, quad[3].y);
  std::vector<Poly> polys(1, quad);
  raster_->Clear();
  raster_->Fill(polys, false);
  Raster& r = *raster_;
  int W = pixels_.width;
  for (int yy = r.y0; yy < std::min(r.y1, pixels_.height); ++yy) {
    float* cov = r.MutableRow(yy);
    for (int xx = r.x0; xx < std::min(r.x1, W); ++xx) {
      if (cov[xx] <= 0) continue;
      float u, v;
      inv.Apply(xx + 0.5f, yy + 0.5f, u, v);
      uint32_t s = SampleImage(img, u, v, 0, 0, (float)img.width, (float)img.height, true);
      cov[xx] *= (s >> 24) / 255.0f;
    }
  }
  CompositeShadow();
  Composite(stroke ? st_.stroke : st_.fill, 0, 0);
}

void Canvas2D::DrawImage(const DecodedImage& src, float sx, float sy, float sw, float sh, float dx, float dy,
                         float dw, float dh) {
  if (src.width <= 0 || src.height <= 0 || sw == 0 || sh == 0 || dw == 0 || dh == 0) return;
  if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dw) || !std::isfinite(dh)) return;
  if (sw < 0) {
    sx += sw;
    sw = -sw;
  }
  if (sh < 0) {
    sy += sh;
    sh = -sh;
  }
  // Clip the source rect to the image, adjusting the destination.
  float cx0 = std::max(0.0f, sx), cy0 = std::max(0.0f, sy);
  float cx1 = std::min((float)src.width, sx + sw), cy1 = std::min((float)src.height, sy + sh);
  if (cx0 >= cx1 || cy0 >= cy1) return;
  float kx = dw / sw, ky = dh / sh;
  dx += (cx0 - sx) * kx;
  dy += (cy0 - sy) * ky;
  dw = (cx1 - cx0) * kx;
  dh = (cy1 - cy0) * ky;
  sx = cx0;
  sy = cy0;
  sw = cx1 - cx0;
  sh = cy1 - cy0;
  // Source pixel (u, v) -> device.
  Matrix m = st_.ctm * Matrix(dw / sw, 0, 0, dh / sh, dx - sx * dw / sw, dy - sy * dh / sh);
  Matrix inv;
  if (!m.Invert(inv)) return;
  Poly quad(4);
  m.Apply(sx, sy, quad[0].x, quad[0].y);
  m.Apply(sx + sw, sy, quad[1].x, quad[1].y);
  m.Apply(sx + sw, sy + sh, quad[2].x, quad[2].y);
  m.Apply(sx, sy + sh, quad[3].x, quad[3].y);
  std::vector<Poly> polys(1, quad);
  raster_->Clear();
  raster_->Fill(polys, false);
  Raster& r = *raster_;
  int W = pixels_.width;
  bool smooth = st_.smoothing && (std::fabs(m.a * m.d - m.b * m.c) != 1 || m.b != 0 || m.c != 0 ||
                                  m.e != std::floor(m.e) || m.f != std::floor(m.f));
  for (int yy = r.y0; yy < std::min(r.y1, pixels_.height); ++yy)
    for (int xx = r.x0; xx < std::min(r.x1, W); ++xx) {
      if (r.At(xx, yy) <= 0) continue;
      float u, v;
      inv.Apply(xx + 0.5f, yy + 0.5f, u, v);
      source_[(size_t)yy * W + xx] = SampleImage(src, u, v, sx, sy, sx + sw, sy + sh, smooth);
    }
  if (st_.shadowColor.a) {
    // Shadow uses the image alpha.
    for (int yy = r.y0; yy < std::min(r.y1, pixels_.height); ++yy) {
      float* cov = r.MutableRow(yy);
      for (int xx = r.x0; xx < std::min(r.x1, W); ++xx) cov[xx] *= (source_[(size_t)yy * W + xx] >> 24) / 255.0f;
    }
    CompositeShadow();
    // Restore full coverage for the image itself.
    raster_->Clear();
    raster_->Fill(polys, false);
  }
  Composite(st_.fill, &source_[0], W);
}

std::string Canvas2D::GetImageData(int x, int y, int w, int h) const {
  std::string out;
  if (w <= 0 || h <= 0 || (long long)w * h > kMaxCanvasPixels) return out;
  out.assign((size_t)w * h * 4, '\0');
  for (int yy = 0; yy < h; ++yy) {
    int py = y + yy;
    if (py < 0 || py >= pixels_.height) continue;
    for (int xx = 0; xx < w; ++xx) {
      int px = x + xx;
      if (px < 0 || px >= pixels_.width) continue;
      uint32_t p = pixels_.pixels[(size_t)py * pixels_.width + px];
      unsigned a = p >> 24;
      char* o = &out[((size_t)yy * w + xx) * 4];
      if (a) {
        o[0] = (char)std::min(255u, (((p >> 16) & 255) * 255 + a / 2) / a);
        o[1] = (char)std::min(255u, (((p >> 8) & 255) * 255 + a / 2) / a);
        o[2] = (char)std::min(255u, ((p & 255) * 255 + a / 2) / a);
      }
      o[3] = (char)a;
    }
  }
  return out;
}

void Canvas2D::PutImageData(const uint8_t* rgba, int w, int h, int dx, int dy, int dirtyX, int dirtyY,
                            int dirtyW, int dirtyH) {
  if (dirtyW < 0) {
    dirtyX += dirtyW;
    dirtyW = -dirtyW;
  }
  if (dirtyH < 0) {
    dirtyY += dirtyH;
    dirtyH = -dirtyH;
  }
  int x0 = std::max(0, dirtyX), y0 = std::max(0, dirtyY);
  int x1 = std::min(w, dirtyX + dirtyW), y1 = std::min(h, dirtyY + dirtyH);
  for (int yy = y0; yy < y1; ++yy) {
    int py = dy + yy;
    if (py < 0 || py >= pixels_.height) continue;
    for (int xx = x0; xx < x1; ++xx) {
      int px = dx + xx;
      if (px < 0 || px >= pixels_.width) continue;
      const uint8_t* p = rgba + ((size_t)yy * w + xx) * 4;
      unsigned a = p[3];
      unsigned r = (p[0] * a + 127) / 255, g = (p[1] * a + 127) / 255, b = (p[2] * a + 127) / 255;
      pixels_.pixels[(size_t)py * pixels_.width + px] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  Touch();
}

namespace {

uint32_t Crc32(const std::string& s, size_t start) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = start; i < s.size(); ++i) c = table[(c ^ (uint8_t)s[i]) & 255] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void Be32(std::string& s, uint32_t v) {
  s += (char)(v >> 24);
  s += (char)(v >> 16);
  s += (char)(v >> 8);
  s += (char)v;
}

void Chunk(std::string& out, const char* type, const std::string& data) {
  Be32(out, (uint32_t)data.size());
  size_t start = out.size();
  out += type;
  out += data;
  Be32(out, Crc32(out, start));
}

}  // namespace

std::string Canvas2D::ToPng() const {
  int w = std::max(1, pixels_.width), h = std::max(1, pixels_.height);
  std::string raw;
  raw.reserve((size_t)(w * 4 + 1) * h);
  std::string rgba = GetImageData(0, 0, w, h);
  if (rgba.empty()) rgba.assign((size_t)w * h * 4, '\0');
  for (int y = 0; y < h; ++y) {
    raw += '\0';  // filter: none
    raw.append(rgba, (size_t)y * w * 4, (size_t)w * 4);
  }
  // zlib stream with stored (uncompressed) deflate blocks.
  std::string z;
  z += (char)0x78;
  z += (char)0x01;
  size_t pos = 0;
  do {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + n >= raw.size();
    z += (char)(last ? 1 : 0);
    z += (char)(n & 255);
    z += (char)(n >> 8);
    z += (char)(~n & 255);
    z += (char)((~n >> 8) & 255);
    z.append(raw, pos, n);
    pos += n;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw.size(); ++i) {
    a = (a + (uint8_t)raw[i]) % 65521;
    b = (b + a) % 65521;
  }
  Be32(z, (b << 16) | a);
  std::string png("\x89PNG\r\n\x1a\n", 8);
  std::string ihdr;
  Be32(ihdr, (uint32_t)w);
  Be32(ihdr, (uint32_t)h);
  ihdr += (char)8;  // bit depth
  ihdr += (char)6;  // RGBA
  ihdr += std::string(3, '\0');
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", std::string());
  return png;
}

}  // namespace kite
