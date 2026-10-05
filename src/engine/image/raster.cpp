#include "image/raster.h"

#include <algorithm>

namespace kite {
namespace gfx {

// ---------------------------------------------------------------------------
// PathBuilder

void PathBuilder::MoveTo(float x, float y) {
  Flush(false);
  cur_.clear();
  Add(x, y);
  sx_ = x;
  sy_ = y;
  lx_ = x;
  ly_ = y;
  hasPoint_ = true;
}

void PathBuilder::LineTo(float x, float y) {
  if (!hasPoint_) {
    MoveTo(x, y);
    return;
  }
  if (cur_.empty()) Add(lx_, ly_);
  Add(x, y);
  lx_ = x;
  ly_ = y;
}

int PathBuilder::Steps(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
  float len = std::hypot(x1 - x0, y1 - y0) + std::hypot(x2 - x1, y2 - y1) + std::hypot(x3 - x2, y3 - y2);
  len *= m_.Scale();
  return std::max(2, std::min(128, (int)(std::sqrt(len / tol_) * 2)));
}

void PathBuilder::CubicTo(float x1, float y1, float x2, float y2, float x, float y) {
  if (!hasPoint_) MoveTo(x1, y1);
  int n = Steps(lx_, ly_, x1, y1, x2, y2, x, y);
  float x0 = lx_, y0 = ly_;
  for (int i = 1; i <= n; ++i) {
    float t = (float)i / n, u = 1 - t;
    LineTo(u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x,
           u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y);
  }
}

void PathBuilder::QuadTo(float x1, float y1, float x, float y) {
  if (!hasPoint_) MoveTo(x1, y1);
  float x0 = lx_, y0 = ly_;
  int n = Steps(x0, y0, x1, y1, x1, y1, x, y);
  for (int i = 1; i <= n; ++i) {
    float t = (float)i / n, u = 1 - t;
    LineTo(u * u * x0 + 2 * u * t * x1 + t * t * x, u * u * y0 + 2 * u * t * y1 + t * t * y);
  }
}

void PathBuilder::ArcTo(float rx, float ry, float rotDeg, bool large, bool sweep, float x, float y) {
  float x1 = lx_, y1 = ly_;
  if (rx == 0 || ry == 0 || (x1 == x && y1 == y)) {
    LineTo(x, y);
    return;
  }
  rx = std::fabs(rx);
  ry = std::fabs(ry);
  float phi = rotDeg * kPi / 180, cp = std::cos(phi), sp = std::sin(phi);
  float dx = (x1 - x) / 2, dy = (y1 - y) / 2;
  float x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
  float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
  if (lambda > 1) {
    float s = std::sqrt(lambda);
    rx *= s;
    ry *= s;
  }
  float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
  float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
  float coef = den == 0 ? 0 : std::sqrt(std::max(0.0f, num / den));
  if (large == sweep) coef = -coef;
  float cxp = coef * rx * y1p / ry, cyp = -coef * ry * x1p / rx;
  float cx = cp * cxp - sp * cyp + (x1 + x) / 2, cy = sp * cxp + cp * cyp + (y1 + y) / 2;
  float th1 = std::atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
  float th2 = std::atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);
  float dth = th2 - th1;
  if (sweep && dth < 0) dth += 2 * kPi;
  if (!sweep && dth > 0) dth -= 2 * kPi;
  float r = std::max(rx, ry) * m_.Scale();
  int n = std::max(4, std::min(256, (int)(std::fabs(dth) * std::sqrt(std::max(r, 1.0f)) * 1.5f)));
  for (int i = 1; i <= n; ++i) {
    float t = th1 + dth * i / n;
    float ex = rx * std::cos(t), ey = ry * std::sin(t);
    LineTo(cp * ex - sp * ey + cx, sp * ex + cp * ey + cy);
  }
}

void PathBuilder::Ellipse(float cx, float cy, float rx, float ry, float rot, float a0, float a1, bool ccw) {
  rx = std::fabs(rx);
  ry = std::fabs(ry);
  float sweep = a1 - a0;
  const float tau = 2 * kPi;
  if (!ccw) {
    if (sweep >= tau) sweep = tau;
    else {
      sweep = std::fmod(sweep, tau);
      if (sweep < 0) sweep += tau;
    }
  } else {
    if (-sweep >= tau) sweep = -tau;
    else {
      sweep = std::fmod(sweep, tau);
      if (sweep > 0) sweep -= tau;
    }
  }
  float cr = std::cos(rot), sr = std::sin(rot);
  float r = std::max(rx, ry) * m_.Scale();
  int n = std::max(2, std::min(512, (int)(std::fabs(sweep) * std::sqrt(std::max(r, 1.0f)) * 1.5f) + 1));
  for (int i = 0; i <= n; ++i) {
    float t = a0 + sweep * i / n;
    float ex = rx * std::cos(t), ey = ry * std::sin(t);
    float px = cx + cr * ex - sr * ey, py = cy + sr * ex + cr * ey;
    if (i == 0 && !hasPoint_) MoveTo(px, py);
    else LineTo(px, py);
  }
}

void PathBuilder::Close() {
  if (!cur_.empty()) {
    lx_ = sx_;
    ly_ = sy_;
  }
  Flush(true);
  cur_.clear();
}

void PathBuilder::SetMatrix(const Matrix& m) {
  if (hasPoint_) {
    float dlx, dly, dsx, dsy;
    m_.Apply(lx_, ly_, dlx, dly);
    m_.Apply(sx_, sy_, dsx, dsy);
    Matrix inv;
    if (m.Invert(inv)) {
      inv.Apply(dlx, dly, lx_, ly_);
      inv.Apply(dsx, dsy, sx_, sy_);
    }
  }
  m_ = m;
}

void PathBuilder::Add(float x, float y) {
  Pt p;
  m_.Apply(x, y, p.x, p.y);
  if (!cur_.empty() && cur_.back().x == p.x && cur_.back().y == p.y) return;
  cur_.push_back(p);
}

void PathBuilder::Flush(bool close) {
  if (cur_.size() >= 2 || (close && !cur_.empty())) {
    polys.push_back(cur_);
    closed.push_back(close);
  } else if (cur_.size() == 1) {
    // A lone point: kept so that round/square caps can draw a dot.
    polys.push_back(cur_);
    closed.push_back(false);
  }
  cur_.clear();
}

// ---------------------------------------------------------------------------
// Raster

Raster::Raster(int w, int h) : x0(0), y0(0), x1(0), y1(0), w_(w), h_(h), cov_((size_t)w * h, 0.0f) {}

void Raster::Clear() {
  if (x0 < x1 && y0 < y1)
    for (int y = y0; y < y1; ++y) std::fill(&cov_[(size_t)y * w_ + x0], &cov_[(size_t)y * w_ + x1], 0.0f);
  x0 = y0 = x1 = y1 = 0;
}

void Raster::Fill(const std::vector<Poly>& polys, bool evenOdd) {
  struct Edge {
    float x0, y0, x1, y1;
    int dir;
  };
  std::vector<Edge> edges;
  float minY = 1e9f, maxY = -1e9f, minX = 1e9f, maxX = -1e9f;
  for (size_t i = 0; i < polys.size(); ++i) {
    const Poly& p = polys[i];
    if (p.size() < 2) continue;
    for (size_t k = 0; k < p.size(); ++k) {
      Pt a = p[k], b = p[(k + 1) % p.size()];
      if (!(std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(b.x) && std::isfinite(b.y))) continue;
      minX = std::min(minX, std::min(a.x, b.x));
      maxX = std::max(maxX, std::max(a.x, b.x));
      if (a.y == b.y) continue;
      Edge e;
      e.dir = a.y < b.y ? 1 : -1;
      if (a.y > b.y) std::swap(a, b);
      e.x0 = a.x;
      e.y0 = a.y;
      e.x1 = b.x;
      e.y1 = b.y;
      edges.push_back(e);
      minY = std::min(minY, a.y);
      maxY = std::max(maxY, b.y);
    }
  }
  if (edges.empty()) return;
  int ry0 = std::max(0, (int)std::floor(std::max(-1.0f, minY)));
  int ry1 = std::min(h_, (int)std::ceil(std::min((float)h_ + 1, maxY)));
  int rx0 = std::max(0, (int)std::floor(std::max(-1.0f, minX)));
  int rx1 = std::min(w_, (int)std::ceil(std::min((float)w_ + 1, maxX)) + 1);
  if (ry0 >= ry1 || rx0 >= rx1) return;
  // Sort edges by top so that only active ones are scanned.
  std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.y0 < b.y0; });
  const int S = 4;
  std::vector<std::pair<float, int> > xs;
  std::vector<float> row(w_ + 2);
  std::vector<size_t> active;
  size_t next = 0;
  bool touched = false;
  for (int y = ry0; y < ry1; ++y) {
    // Activate edges that start above the bottom of this row; drop finished ones.
    while (next < edges.size() && edges[next].y0 < y + 1) active.push_back(next++);
    size_t keep = 0;
    for (size_t i = 0; i < active.size(); ++i)
      if (edges[active[i]].y1 > y) active[keep++] = active[i];
    active.resize(keep);
    if (active.empty()) continue;
    bool any = false;
    std::fill(row.begin() + rx0, row.begin() + std::min((int)row.size(), rx1 + 1), 0.0f);
    for (int s = 0; s < S; ++s) {
      float sy = y + (s + 0.5f) / S;
      xs.clear();
      for (size_t i = 0; i < active.size(); ++i) {
        const Edge& e = edges[active[i]];
        if (sy < e.y0 || sy >= e.y1) continue;
        float t = (sy - e.y0) / (e.y1 - e.y0);
        xs.push_back(std::make_pair(e.x0 + (e.x1 - e.x0) * t, e.dir));
      }
      if (xs.size() < 2) continue;
      std::sort(xs.begin(), xs.end());
      int wind = 0;
      for (size_t i = 0; i + 1 < xs.size(); ++i) {
        wind += xs[i].second;
        bool inside = evenOdd ? (i % 2 == 0) : wind != 0;
        if (!inside) continue;
        AddSpan(row, xs[i].first, xs[i + 1].first, 1.0f / S);
        any = true;
      }
    }
    if (!any) continue;
    float* dst = &cov_[(size_t)y * w_];
    for (int x = rx0; x < rx1; ++x)
      if (row[x] > 0) dst[x] = std::min(1.0f, dst[x] + row[x]);
    if (!touched && x0 >= x1) {
      x0 = rx0;
      x1 = rx1;
      y0 = y;
      y1 = y + 1;
    } else {
      x0 = std::min(x0, rx0);
      x1 = std::max(x1, rx1);
      y0 = std::min(y0, y);
      y1 = std::max(y1, y + 1);
    }
    touched = true;
  }
}

void Raster::AddSpan(std::vector<float>& row, float xa, float xb, float weight) {
  if (xb <= 0 || xa >= w_) return;
  xa = std::max(0.0f, xa);
  xb = std::min((float)w_, xb);
  int ia = (int)xa, ib = (int)xb;
  if (ia == ib) {
    row[ia] += (xb - xa) * weight;
    return;
  }
  row[ia] += (ia + 1 - xa) * weight;
  for (int x = ia + 1; x < ib; ++x) row[x] += weight;
  if (ib < w_) row[ib] += (xb - ib) * weight;
}

// ---------------------------------------------------------------------------
// Stroker

namespace {

void AddCircle(std::vector<Poly>& out, Pt c, float r) {
  int n = std::max(8, std::min(64, (int)(r * 2.5f)));
  Poly p;
  for (int i = 0; i < n; ++i) {
    float t = 2 * kPi * i / n;
    Pt q = {c.x + r * std::cos(t), c.y + r * std::sin(t)};
    p.push_back(q);
  }
  out.push_back(p);
}

void AddQuad(std::vector<Poly>& out, Pt a, Pt b, Pt c, Pt d) {
  Poly p(4);
  p[0] = a;
  p[1] = b;
  p[2] = c;
  p[3] = d;
  out.push_back(p);
}

// Splits a polyline into dash segments.
void ApplyDash(const Poly& p, bool closed, const StrokeStyle& st, std::vector<Poly>& out) {
  float total = 0;
  for (size_t i = 0; i < st.dash.size(); ++i) total += st.dash[i];
  if (total <= 0) return;
  size_t idx = 0;
  float off = std::fmod(st.dashOffset, total);
  if (off < 0) off += total;
  while (off >= st.dash[idx]) {
    off -= st.dash[idx];
    idx = (idx + 1) % st.dash.size();
  }
  float left = st.dash[idx] - off;
  bool on = idx % 2 == 0;
  Poly cur;
  if (on) cur.push_back(p[0]);
  size_t n = p.size(), segs = closed ? n : n - 1;
  for (size_t k = 0; k < segs; ++k) {
    Pt a = p[k], b = p[(k + 1) % n];
    float len = std::hypot(b.x - a.x, b.y - a.y), pos = 0;
    while (len - pos > left) {
      pos += left;
      Pt q = {a.x + (b.x - a.x) * pos / len, a.y + (b.y - a.y) * pos / len};
      if (on) {
        cur.push_back(q);
        out.push_back(cur);
        cur.clear();
      } else {
        cur.clear();
        cur.push_back(q);
      }
      on = !on;
      idx = (idx + 1) % st.dash.size();
      left = st.dash[idx];
      if (out.size() > 100000) return;
    }
    left -= len - pos;
    if (on) cur.push_back(b);
  }
  if (on && cur.size() >= 2) out.push_back(cur);
}

void StrokeOne(const Poly& in, bool closed, const StrokeStyle& st, std::vector<Poly>& out) {
  float hw = std::max(0.25f, st.width / 2);
  // Drop repeated points.
  Poly p;
  for (size_t i = 0; i < in.size(); ++i)
    if (p.empty() || std::hypot(in[i].x - p.back().x, in[i].y - p.back().y) > 1e-4f) p.push_back(in[i]);
  if (closed && p.size() > 2 && std::hypot(p[0].x - p.back().x, p[0].y - p.back().y) <= 1e-4f) p.pop_back();
  if (p.size() == 1) {
    if (st.cap == kCapRound) AddCircle(out, p[0], hw);
    else if (st.cap == kCapSquare) {
      Pt a = {p[0].x - hw, p[0].y - hw}, b = {p[0].x + hw, p[0].y - hw}, c = {p[0].x + hw, p[0].y + hw},
         d = {p[0].x - hw, p[0].y + hw};
      AddQuad(out, a, b, c, d);
    }
    return;
  }
  if (p.size() < 2) return;
  size_t n = p.size(), segs = closed ? n : n - 1;
  for (size_t k = 0; k < segs; ++k) {
    Pt a = p[k], b = p[(k + 1) % n];
    float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    float nx = -dy / len * hw, ny = dx / len * hw;
    float ex = 0, ey = 0;  // square cap extension
    if (!closed && st.cap == kCapSquare) {
      if (k == 0) {
        a.x -= dx / len * hw;
        a.y -= dy / len * hw;
      }
      if (k == segs - 1) {
        ex = dx / len * hw;
        ey = dy / len * hw;
      }
    }
    Pt q0 = {a.x + nx, a.y + ny}, q1 = {b.x + ex + nx, b.y + ey + ny}, q2 = {b.x + ex - nx, b.y + ey - ny},
       q3 = {a.x - nx, a.y - ny};
    AddQuad(out, q0, q1, q2, q3);
  }
  // Joins.
  size_t firstJoin = closed ? 0 : 1, lastJoin = closed ? n : n - 1;
  for (size_t k = firstJoin; k < lastJoin; ++k) {
    Pt prev = p[(k + n - 1) % n], c = p[k], next = p[(k + 1) % n];
    float d0x = c.x - prev.x, d0y = c.y - prev.y, d1x = next.x - c.x, d1y = next.y - c.y;
    float l0 = std::hypot(d0x, d0y), l1 = std::hypot(d1x, d1y);
    if (l0 < 1e-6f || l1 < 1e-6f) continue;
    d0x /= l0;
    d0y /= l0;
    d1x /= l1;
    d1y /= l1;
    float cross = d0x * d1y - d0y * d1x;
    if (std::fabs(cross) < 1e-4f && d0x * d1x + d0y * d1y > 0) continue;  // straight
    if (st.join == kJoinRound) {
      AddCircle(out, c, hw);
      continue;
    }
    // Outer side of the turn.
    float s = cross > 0 ? -1.0f : 1.0f;
    Pt o0 = {c.x - d0y * hw * s, c.y + d0x * hw * s};
    Pt o1 = {c.x - d1y * hw * s, c.y + d1x * hw * s};
    if (st.join == kJoinMiter) {
      float cosHalf = std::sqrt(std::max(0.0f, (1 + d0x * d1x + d0y * d1y) / 2));
      if (cosHalf > 1e-4f && 1 / cosHalf <= st.miterLimit) {
        // Miter tip: intersection of the offset lines.
        float bx = -(d0y + d1y), by = d0x + d1x;
        float bl = std::hypot(bx, by);
        if (bl > 1e-6f) {
          float ml = hw / cosHalf;
          Pt tip = {c.x + bx / bl * ml * s, c.y + by / bl * ml * s};
          AddQuad(out, c, o0, tip, o1);
          continue;
        }
      }
    }
    Poly tri(3);
    tri[0] = c;
    tri[1] = o0;
    tri[2] = o1;
    out.push_back(tri);
  }
  if (!closed && st.cap == kCapRound) {
    AddCircle(out, p[0], hw);
    AddCircle(out, p[n - 1], hw);
  }
}

}  // namespace

std::vector<Poly> StrokePolys(const std::vector<Poly>& polys, const std::vector<bool>& closed,
                              const StrokeStyle& style) {
  std::vector<Poly> out;
  for (size_t i = 0; i < polys.size(); ++i) {
    bool cl = i < closed.size() && closed[i];
    if (!style.dash.empty() && polys[i].size() >= 2) {
      std::vector<Poly> parts;
      ApplyDash(polys[i], cl, style, parts);
      for (size_t k = 0; k < parts.size(); ++k) StrokeOne(parts[k], false, style, out);
    } else {
      StrokeOne(polys[i], cl, style, out);
    }
  }
  // Normalise orientation so that nonzero filling unions everything.
  for (size_t i = 0; i < out.size(); ++i) {
    float area = 0;
    const Poly& q = out[i];
    for (size_t k = 0; k < q.size(); ++k) {
      const Pt& a = q[k];
      const Pt& b = q[(k + 1) % q.size()];
      area += a.x * b.y - b.x * a.y;
    }
    if (area < 0) std::reverse(out[i].begin(), out[i].end());
  }
  return out;
}

bool PolysContain(const std::vector<Poly>& polys, float x, float y, bool evenOdd) {
  int wind = 0, crossings = 0;
  for (size_t i = 0; i < polys.size(); ++i) {
    const Poly& p = polys[i];
    if (p.size() < 3) continue;
    for (size_t k = 0; k < p.size(); ++k) {
      Pt a = p[k], b = p[(k + 1) % p.size()];
      if ((a.y <= y) == (b.y <= y)) continue;
      float t = (y - a.y) / (b.y - a.y);
      if (a.x + (b.x - a.x) * t > x) {
        ++crossings;
        wind += a.y < b.y ? 1 : -1;
      }
    }
  }
  return evenOdd ? (crossings & 1) != 0 : wind != 0;
}

}  // namespace gfx
}  // namespace kite
