// Rasterizes Kite display lists into a 32-bit DIB section. Rectangles and
// images are blended in software (alpha support without needing GDI+ or
// AlphaBlend), text is drawn with GDI.
#include "win32/app.h"

#include "image/raster.h"

#include <algorithm>
#include <cmath>

#include "base/strings.h"
#include "image/svg.h"

namespace kite {

Renderer::Renderer() : memDc_(0), bitmap_(0), oldBitmap_(0), bits_(0), bw_(0), bh_(0) {}

Renderer::~Renderer() {
  if (memDc_) {
    SelectObject(memDc_, oldBitmap_);
    DeleteObject(bitmap_);
    DeleteDC(memDc_);
  }
}

void Renderer::EnsureBuffer(HDC hdc, int w, int h) {
  if (memDc_ && w <= bw_ && h <= bh_) return;
  if (memDc_) {
    SelectObject(memDc_, oldBitmap_);
    DeleteObject(bitmap_);
    DeleteDC(memDc_);
  }
  bw_ = std::max(w, 64);
  bh_ = std::max(h, 64);
  BITMAPINFO bi;
  ZeroMemory(&bi, sizeof bi);
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = bw_;
  bi.bmiHeader.biHeight = -bh_;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = 0;
  bitmap_ = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
  bits_ = static_cast<uint32_t*>(bits);
  memDc_ = CreateCompatibleDC(hdc);
  oldBitmap_ = (HBITMAP)SelectObject(memDc_, bitmap_);
  SetBkMode(memDc_, TRANSPARENT);
  SetTextAlign(memDc_, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
}

void Renderer::FillRect(int x0, int y0, int x1, int y1, Color c) {
  x0 = std::max(x0, (int)clip_.left);
  y0 = std::max(y0, (int)clip_.top);
  x1 = std::min(x1, (int)clip_.right);
  y1 = std::min(y1, (int)clip_.bottom);
  if (x0 >= x1 || y0 >= y1 || c.a == 0) return;
  if (c.a == 255) {
    uint32_t v = (c.r << 16) | (c.g << 8) | c.b;
    for (int y = y0; y < y1; ++y) {
      uint32_t* row = bits_ + (size_t)y * bw_;
      for (int x = x0; x < x1; ++x) row[x] = v;
    }
    return;
  }
  unsigned a = c.a, ia = 255 - a;
  unsigned pr = c.r * a, pg = c.g * a, pb = c.b * a;
  for (int y = y0; y < y1; ++y) {
    uint32_t* row = bits_ + (size_t)y * bw_;
    for (int x = x0; x < x1; ++x) {
      uint32_t d = row[x];
      unsigned r = (((d >> 16) & 255) * ia + pr) / 255;
      unsigned g = (((d >> 8) & 255) * ia + pg) / 255;
      unsigned b = ((d & 255) * ia + pb) / 255;
      row[x] = (r << 16) | (g << 8) | b;
    }
  }
}

void Renderer::ApplyClip() {
  HRGN rgn = CreateRectRgn(clip_.left, clip_.top, clip_.right, clip_.bottom);
  SelectClipRgn(memDc_, rgn);
  DeleteObject(rgn);
}

// Signed distance from (px, py) to a rounded rectangle with per-corner radii
// (tl, tr, br, bl); negative inside.
static float RRectSdf(float px, float py, float x0, float y0, float x1, float y1, const float r[4]) {
  float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hw = (x1 - x0) / 2, hh = (y1 - y0) / 2;
  float dx = px - cx, dy = py - cy;
  float rad = dx < 0 ? (dy < 0 ? r[0] : r[3]) : (dy < 0 ? r[1] : r[2]);
  float qx = std::fabs(dx) - (hw - rad), qy = std::fabs(dy) - (hh - rad);
  float mx = std::max(qx, 0.0f), my = std::max(qy, 0.0f);
  return std::sqrt(mx * mx + my * my) + std::min(std::max(qx, qy), 0.0f) - rad;
}

static float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

void Renderer::FillCoverage(int x0, int y0, int x1, int y1, Color c,
                            float (*cov)(float, float, const void*), const void* ctx) {
  x0 = std::max(x0, (int)clip_.left);
  y0 = std::max(y0, (int)clip_.top);
  x1 = std::min(x1, (int)clip_.right);
  y1 = std::min(y1, (int)clip_.bottom);
  for (int y = y0; y < y1; ++y) {
    uint32_t* row = bits_ + (size_t)y * bw_;
    for (int x = x0; x < x1; ++x) {
      float a = cov(x + 0.5f, y + 0.5f, ctx);
      if (a <= 0.002f) continue;
      unsigned sa = (unsigned)(a * c.a + 0.5f);
      if (sa == 0) continue;
      unsigned ia = 255 - sa;
      uint32_t d = row[x];
      unsigned r = (c.r * sa + ((d >> 16) & 255) * ia) / 255;
      unsigned g = (c.g * sa + ((d >> 8) & 255) * ia) / 255;
      unsigned b = (c.b * sa + (d & 255) * ia) / 255;
      row[x] = (r << 16) | (g << 8) | b;
    }
  }
}

namespace {
struct RRectCtx {
  float x0, y0, x1, y1, r[4];
  float ring;     // > 0: only the border ring
  float blur;     // > 0: soft shadow
  int side = -1;  // ring part of one border side (see DisplayItem::side)
};
float RRectCoverage(float px, float py, const void* p) {
  const RRectCtx* c = (const RRectCtx*)p;
  float sd = RRectSdf(px, py, c->x0, c->y0, c->x1, c->y1, c->r);
  if (c->blur > 0) {
    // Approximation of a Gaussian blurred edge.
    float t = Clamp01(0.5f - sd / c->blur);
    return t * t * (3 - 2 * t);
  }
  if (c->side >= 0) {
    float cx = (c->x0 + c->x1) / 2, cy = (c->y0 + c->y1) / 2;
    float hw = std::max(0.5f, (c->x1 - c->x0) / 2), hh = std::max(0.5f, (c->y1 - c->y0) / 2);
    float nx = (px - cx) / hw, ny = (py - cy) / hh;
    int side = std::fabs(ny) >= std::fabs(nx) ? (ny < 0 ? 0 : 2) : (nx > 0 ? 1 : 3);
    if (side != c->side) return 0;
  }
  float a = Clamp01(0.5f - sd);
  if (c->ring > 0) {
    float ir[4];
    for (int i = 0; i < 4; ++i) ir[i] = std::max(0.0f, c->r[i] - c->ring);
    float si = RRectSdf(px, py, c->x0 + c->ring, c->y0 + c->ring, c->x1 - c->ring, c->y1 - c->ring, ir);
    a -= Clamp01(0.5f - si);
  }
  return a;
}
}  // namespace

void Renderer::DrawImage(const DisplayItem& it, float ox, float oy, float zoom) {
  int tw = (int)std::floor(it.tileW * zoom + 0.5f), th = (int)std::floor(it.tileH * zoom + 0.5f);
  if (tw <= 0 || th <= 0) return;
  const DecodedImage* img = Images().Scaled(it.imageUrl, tw, th);
  if (!img) return;
  // Destination area (item rect) intersected with the clip.
  RECT area;
  area.left = std::max((LONG)clip_.left, (LONG)std::floor(it.rect.x * zoom - ox));
  area.top = std::max((LONG)clip_.top, (LONG)std::floor(it.rect.y * zoom - oy));
  area.right = std::min((LONG)clip_.right, (LONG)std::ceil(it.rect.right() * zoom - ox));
  area.bottom = std::min((LONG)clip_.bottom, (LONG)std::ceil(it.rect.bottom() * zoom - oy));
  if (area.left >= area.right || area.top >= area.bottom) return;
  int tx0 = (int)std::floor(it.tileX * zoom - ox + 0.5f), ty0 = (int)std::floor(it.tileY * zoom - oy + 0.5f);
  // Tile range.
  int ix0 = 0, ix1 = 1, iy0 = 0, iy1 = 1;
  if (it.repeatX) {
    ix0 = (int)std::floor((float)(area.left - tx0) / tw);
    ix1 = (int)std::ceil((float)(area.right - tx0) / tw);
  }
  if (it.repeatY) {
    iy0 = (int)std::floor((float)(area.top - ty0) / th);
    iy1 = (int)std::ceil((float)(area.bottom - ty0) / th);
  }
  if ((long long)(ix1 - ix0) * (iy1 - iy0) > 20000) return;
  unsigned alpha = (unsigned)(std::max(0.0f, std::min(1.0f, it.alpha)) * 255 + 0.5f);
  bool rounded = it.HasRadii();
  RRectCtx rc;
  if (rounded) {
    rc.x0 = it.rect.x * zoom - ox;
    rc.y0 = it.rect.y * zoom - oy;
    rc.x1 = it.rect.right() * zoom - ox;
    rc.y1 = it.rect.bottom() * zoom - oy;
    for (int k = 0; k < 4; ++k) rc.r[k] = it.radii[k] * zoom;
    rc.ring = rc.blur = 0;
  }
  for (int ty = iy0; ty < iy1; ++ty) {
    for (int tx = ix0; tx < ix1; ++tx) {
      int dx = tx0 + tx * tw, dy = ty0 + ty * th;
      int x0 = std::max((int)area.left, dx), x1 = std::min((int)area.right, dx + tw);
      int y0 = std::max((int)area.top, dy), y1 = std::min((int)area.bottom, dy + th);
      for (int y = y0; y < y1; ++y) {
        const uint32_t* src = &img->pixels[(size_t)(y - dy) * img->width + (x0 - dx)];
        uint32_t* dst = bits_ + (size_t)y * bw_ + x0;
        if (!img->hasAlpha && alpha == 255 && !rounded) {
          for (int x = x0; x < x1; ++x) *dst++ = *src++ & 0xFFFFFF;
          continue;
        }
        for (int x = x0; x < x1; ++x, ++src, ++dst) {
          uint32_t s = *src;
          unsigned sa = s >> 24;
          unsigned sr = (s >> 16) & 255, sg = (s >> 8) & 255, sb = s & 255;
          unsigned a2 = alpha;
          if (rounded) a2 = (unsigned)(alpha * RRectCoverage(x + 0.5f, y + 0.5f, &rc));
          if (a2 != 255) {
            sa = sa * a2 / 255;
            sr = sr * a2 / 255;
            sg = sg * a2 / 255;
            sb = sb * a2 / 255;
          }
          if (sa == 0) continue;
          uint32_t d = *dst;
          unsigned ia = 255 - sa;
          unsigned r = sr + ((d >> 16) & 255) * ia / 255;
          unsigned g = sg + ((d >> 8) & 255) * ia / 255;
          unsigned b = sb + (d & 255) * ia / 255;
          *dst = (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
        }
      }
    }
  }
}

void Renderer::Blit(const DecodedImage& img, int dx, int dy, const RECT& area, unsigned alpha) {
  int x0 = std::max((int)area.left, dx), x1 = std::min((int)area.right, dx + img.width);
  int y0 = std::max((int)area.top, dy), y1 = std::min((int)area.bottom, dy + img.height);
  for (int y = y0; y < y1; ++y) {
    const uint32_t* src = &img.pixels[(size_t)(y - dy) * img.width + (x0 - dx)];
    uint32_t* dst = bits_ + (size_t)y * bw_ + x0;
    for (int x = x0; x < x1; ++x, ++src, ++dst) {
      uint32_t s = *src;
      unsigned sa = (s >> 24) * alpha / 255;
      if (sa == 0) continue;
      unsigned sr = ((s >> 16) & 255) * alpha / 255, sg = ((s >> 8) & 255) * alpha / 255,
               sb = (s & 255) * alpha / 255;
      uint32_t d = *dst;
      unsigned ia = 255 - sa;
      unsigned r = sr + ((d >> 16) & 255) * ia / 255;
      unsigned g = sg + ((d >> 8) & 255) * ia / 255;
      unsigned b = sb + (d & 255) * ia / 255;
      *dst = (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
    }
  }
}

void Renderer::DrawSvg(const DisplayItem& it, float ox, float oy, float zoom) {
  int x = (int)std::floor(it.rect.x * zoom - ox + 0.5f), y = (int)std::floor(it.rect.y * zoom - oy + 0.5f);
  int w = (int)std::floor(it.rect.w * zoom + 0.5f), h = (int)std::floor(it.rect.h * zoom + 0.5f);
  if (w <= 0 || h <= 0 || (long long)w * h > 3000LL * 3000) return;
  char key[128];
  snprintf(key, sizeof key, "%p|%d|%d|%02x%02x%02x", (const void*)it.svgNode, w, h, it.color.r,
           it.color.g, it.color.b);
  std::map<std::string, DecodedImage>::iterator found = svgCache_.find(key);
  if (found == svgCache_.end()) {
    if (svgCache_.size() > 400) svgCache_.clear();
    DecodedImage& img = svgCache_[key];
    if (!RenderSvg(it.svgNode, w, h, it.color, img)) img.width = img.height = 0;
    found = svgCache_.find(key);
  }
  if (found->second.width <= 0) return;
  unsigned alpha = (unsigned)(std::max(0.0f, std::min(1.0f, it.alpha)) * 255 + 0.5f);
  Blit(found->second, x, y, clip_, alpha);
}

static COLORREF Blend(Color c, Color bg) {
  if (c.a == 255) return RGB(c.r, c.g, c.b);
  unsigned a = c.a, ia = 255 - a;
  return RGB((c.r * a + bg.r * ia) / 255, (c.g * a + bg.g * ia) / 255, (c.b * a + bg.b * ia) / 255);
}

void Renderer::PaintRange(PaintCtx& pc, size_t begin, size_t end) {
  const DisplayList& dl = *pc.dl;
  float zoom = pc.zoom;
  int width = bw_, height = bh_;
  float ox = (pc.fixedDepth > 0 ? 0 : pc.scrollX) + pc.layerX;
  float oy = (pc.fixedDepth > 0 ? 0 : pc.scrollY) + pc.layerY;
  float viewTop = oy / zoom, viewBottom = (oy + height) / zoom;
  float viewLeft = ox / zoom, viewRight = (ox + width) / zoom;
  bool gdiDirty = false;
  for (size_t i = begin; i < end; ++i) {
    const DisplayItem& it = dl.items[i];
    if (it.type == DisplayItem::kPushClip) {
      clipStack_.push_back(clip_);
      RECT r;
      r.left = std::max(clip_.left, (LONG)std::floor(it.rect.x * zoom - ox));
      r.top = std::max(clip_.top, (LONG)std::floor(it.rect.y * zoom - oy));
      r.right = std::min(clip_.right, (LONG)std::ceil(it.rect.right() * zoom - ox));
      r.bottom = std::min(clip_.bottom, (LONG)std::ceil(it.rect.bottom() * zoom - oy));
      if (r.right < r.left) r.right = r.left;
      if (r.bottom < r.top) r.bottom = r.top;
      clip_ = r;
      ApplyClip();
      continue;
    }
    if (it.type == DisplayItem::kBeginFixed || it.type == DisplayItem::kEndFixed) {
      pc.fixedDepth += it.type == DisplayItem::kBeginFixed ? 1 : -1;
      ox = (pc.fixedDepth > 0 ? 0 : pc.scrollX) + pc.layerX;
      oy = (pc.fixedDepth > 0 ? 0 : pc.scrollY) + pc.layerY;
      viewTop = oy / zoom;
      viewBottom = (oy + height) / zoom;
      viewLeft = ox / zoom;
      viewRight = (ox + width) / zoom;
      continue;
    }
    if (it.type == DisplayItem::kBeginTransform) {
      size_t stop = it.matchIndex > (int)i && it.matchIndex <= (int)end ? (size_t)it.matchIndex : end;
      if (gdiDirty) {
        GdiFlush();
        gdiDirty = false;
      }
      if (clip_.left < clip_.right && clip_.top < clip_.bottom) DrawTransformed(pc, i, stop, ox, oy);
      i = stop;  // skip to the matching end
      continue;
    }
    if (it.type == DisplayItem::kEndTransform) continue;
    if (it.type == DisplayItem::kPopClip) {
      if (!clipStack_.empty()) {
        clip_ = clipStack_.back();
        clipStack_.pop_back();
        ApplyClip();
      }
      continue;
    }
    if (clip_.left >= clip_.right || clip_.top >= clip_.bottom) continue;
    // Cull items outside the visible area.
    float top = it.rect.y, bottom = it.rect.bottom();
    if (it.type == DisplayItem::kText) {
      top = it.baseline - it.font.size * 1.2f;
      bottom = it.baseline + it.font.size * 0.5f;
    }
    if (it.type == DisplayItem::kShadow) {
      top -= it.blur;
      bottom += it.blur;
    }
    if (bottom < viewTop || top > viewBottom) continue;
    if (it.rect.right() < viewLeft || it.rect.x > viewRight) continue;
    switch (it.type) {
      case DisplayItem::kRect: {
        if (gdiDirty) {
          GdiFlush();
          gdiDirty = false;
        }
        int x0 = (int)std::floor(it.rect.x * zoom - ox + 0.5f);
        int y0 = (int)std::floor(it.rect.y * zoom - oy + 0.5f);
        int x1 = (int)std::floor(it.rect.right() * zoom - ox + 0.5f);
        int y1 = (int)std::floor(it.rect.bottom() * zoom - oy + 0.5f);
        if (x1 == x0 && it.rect.w > 0) x1 = x0 + 1;
        if (y1 == y0 && it.rect.h > 0) y1 = y0 + 1;
        FillRect(x0, y0, x1, y1, it.color);
        break;
      }
      case DisplayItem::kImage:
        if (gdiDirty) {
          GdiFlush();
          gdiDirty = false;
        }
        DrawImage(it, ox, oy, zoom);
        break;
      case DisplayItem::kRoundRect:
      case DisplayItem::kShadow: {
        if (gdiDirty) {
          GdiFlush();
          gdiDirty = false;
        }
        RRectCtx rc;
        rc.x0 = it.rect.x * zoom - ox;
        rc.y0 = it.rect.y * zoom - oy;
        rc.x1 = it.rect.right() * zoom - ox;
        rc.y1 = it.rect.bottom() * zoom - oy;
        for (int k = 0; k < 4; ++k) rc.r[k] = it.radii[k] * zoom;
        rc.ring = it.ring * zoom;
        rc.blur = it.type == DisplayItem::kShadow ? std::max(1.0f, it.blur * zoom) : 0;
        rc.side = it.side;
        int pad = (int)std::ceil(rc.blur) + 1;
        FillCoverage((int)std::floor(rc.x0) - pad, (int)std::floor(rc.y0) - pad,
                     (int)std::ceil(rc.x1) + pad, (int)std::ceil(rc.y1) + pad, it.color,
                     RRectCoverage, &rc);
        break;
      }
      case DisplayItem::kSvg:
        if (gdiDirty) {
          GdiFlush();
          gdiDirty = false;
        }
        DrawSvg(it, ox, oy, zoom);
        break;
      case DisplayItem::kEllipse: {
        HBRUSH brush = CreateSolidBrush(Blend(it.color, dl.background));
        HPEN pen = CreatePen(PS_SOLID, 1, Blend(it.color, dl.background));
        HGDIOBJ ob = SelectObject(memDc_, it.hollow ? GetStockObject(NULL_BRUSH) : brush);
        HGDIOBJ op = SelectObject(memDc_, it.hollow ? pen : GetStockObject(NULL_PEN));
        int x0 = (int)std::floor(it.rect.x * zoom - ox + 0.5f);
        int y0 = (int)std::floor(it.rect.y * zoom - oy + 0.5f);
        int x1 = (int)std::floor(it.rect.right() * zoom - ox + 0.5f);
        int y1 = (int)std::floor(it.rect.bottom() * zoom - oy + 0.5f);
        Ellipse(memDc_, x0, y0, x1 + (it.hollow ? 0 : 1), y1 + (it.hollow ? 0 : 1));
        SelectObject(memDc_, ob);
        SelectObject(memDc_, op);
        DeleteObject(brush);
        DeleteObject(pen);
        gdiDirty = true;
        break;
      }
      case DisplayItem::kText: {
        std::wstring w = Widen(it.text);
        // Drop characters that legacy fonts cannot show (icon fonts use the
        // private use area; zero width / soft hyphen chars render as boxes).
        std::wstring clean;
        clean.reserve(w.size());
        bool webFont = Fonts().IsWebFont(it.font);
        for (size_t k = 0; k < w.size(); ++k) {
          wchar_t c = w[k];
          if ((!webFont && c >= 0xE000 && c <= 0xF8FF) || c == 0x200B || c == 0x200C || c == 0x200D ||
              c == 0xFEFF || c == 0x00AD || c == 0x2060)
            continue;
          if (c >= 0xD800 && c <= 0xDBFF) {  // astral plane (emoji): not drawable
            ++k;
            clean += L'\x25A1';
            continue;
          }
          clean += c;
        }
        if (clean.empty()) break;
        HFONT font = Fonts().Get(it.font, zoom);
        HGDIOBJ old = SelectObject(memDc_, font);
        SetTextColor(memDc_, Blend(it.color, dl.background));
        int x = (int)std::floor(it.rect.x * zoom - ox + 0.5f);
        int y = (int)std::floor(it.baseline * zoom - oy + 0.5f);
        ExtTextOutW(memDc_, x, y, 0, 0, clean.c_str(), (UINT)clean.size(), 0);
        SelectObject(memDc_, old);
        gdiDirty = true;
        if (it.underline || it.lineThrough || it.overline) {
          GdiFlush();
          gdiDirty = false;
          int w = (int)std::floor(it.rect.w * zoom + 0.5f);
          int t = std::max(1, (int)(it.font.size * zoom / 15));
          if (it.underline) FillRect(x, y + std::max(1, t), x + w, y + std::max(1, t) + t, it.decorationColor);
          if (it.lineThrough) {
            int ly = y - (int)(it.font.size * zoom * 0.3f);
            FillRect(x, ly, x + w, ly + t, it.decorationColor);
          }
          if (it.overline) {
            int ly = y - (int)(it.font.size * zoom * 0.9f);
            FillRect(x, ly, x + w, ly + t, it.decorationColor);
          }
        }
        break;
      }
      default:
        break;
    }
  }
  if (gdiDirty) GdiFlush();
}

void Renderer::DrawTransformed(PaintCtx& pc, size_t begin, size_t end, float ox, float oy) {
  const DisplayItem& ti = pc.dl->items[begin];
  float zoom = pc.zoom;
  // Device-space affine map: parent = A * layer.
  //   doc = (layer + o + L) / zoom; parent = zoom * M(doc) - o
  const float* m = ti.matrix;
  // Layer: the untransformed bounds in device space (limited in size).
  float lx0 = std::floor(ti.rect.x * zoom - ox) - 2, ly0 = std::floor(ti.rect.y * zoom - oy) - 2;
  float lx1 = std::ceil(ti.rect.right() * zoom - ox) + 2, ly1 = std::ceil(ti.rect.bottom() * zoom - oy) + 2;
  lx0 = std::max(lx0, -2.0f * bw_);
  ly0 = std::max(ly0, -2.0f * bh_);
  lx1 = std::min(lx1, 3.0f * bw_);
  ly1 = std::min(ly1, 3.0f * bh_);
  int lw = (int)(lx1 - lx0), lh = (int)(ly1 - ly0);
  if (lw <= 0 || lh <= 0 || (long long)lw * lh > 4096LL * 4096) return;
  gfx::Matrix z(zoom, 0, 0, zoom, -ox, -oy);  // doc -> parent device
  gfx::Matrix zl(1 / zoom, 0, 0, 1 / zoom, (ox + lx0) / zoom, (oy + ly0) / zoom);  // layer -> doc
  gfx::Matrix A = z * gfx::Matrix(m[0], m[1], m[2], m[3], m[4], m[5]) * zl;
  gfx::Matrix inv;
  if (!A.Invert(inv)) return;
  // Destination bounds in the parent.
  float xs[4] = {0, (float)lw, (float)lw, 0}, ys[4] = {0, 0, (float)lh, (float)lh};
  float dx0 = 1e30f, dy0 = 1e30f, dx1 = -1e30f, dy1 = -1e30f;
  for (int k = 0; k < 4; ++k) {
    float px, py;
    A.Apply(xs[k], ys[k], px, py);
    dx0 = std::min(dx0, px);
    dy0 = std::min(dy0, py);
    dx1 = std::max(dx1, px);
    dy1 = std::max(dy1, py);
  }
  int ix0 = std::max((int)clip_.left, (int)std::floor(dx0)), iy0 = std::max((int)clip_.top, (int)std::floor(dy0));
  int ix1 = std::min((int)clip_.right, (int)std::ceil(dx1)), iy1 = std::min((int)clip_.bottom, (int)std::ceil(dy1));
  if (ix0 >= ix1 || iy0 >= iy1) return;

  // Render the layer twice (black and white background) to recover alpha,
  // since GDI text does not write an alpha channel.
  BITMAPINFO bi;
  ZeroMemory(&bi, sizeof bi);
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = lw;
  bi.bmiHeader.biHeight = -lh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* lbits = 0;
  HBITMAP lbmp = CreateDIBSection(memDc_, &bi, DIB_RGB_COLORS, &lbits, 0, 0);
  if (!lbmp || !lbits) return;
  HDC ldc = CreateCompatibleDC(memDc_);
  HGDIOBJ lold = SelectObject(ldc, lbmp);
  SetBkMode(ldc, TRANSPARENT);
  SetTextAlign(ldc, TA_BASELINE | TA_LEFT | TA_NOUPDATECP);
  // Swap the render target.
  HDC saveDc = memDc_;
  uint32_t* saveBits = bits_;
  int saveW = bw_, saveH = bh_;
  RECT saveClip = clip_;
  std::vector<RECT> saveStack;
  saveStack.swap(clipStack_);
  memDc_ = ldc;
  bits_ = (uint32_t*)lbits;
  bw_ = lw;
  bh_ = lh;
  PaintCtx inner = pc;
  inner.layerX = pc.layerX + lx0;
  inner.layerY = pc.layerY + ly0;
  std::vector<uint32_t> black((size_t)lw * lh);
  for (int pass = 0; pass < 2; ++pass) {
    clip_.left = clip_.top = 0;
    clip_.right = lw;
    clip_.bottom = lh;
    clipStack_.clear();
    ApplyClip();
    uint32_t bg = pass == 0 ? 0xFF000000u : 0xFFFFFFFFu;
    std::fill(bits_, bits_ + (size_t)lw * lh, bg);
    PaintCtx p2 = inner;
    PaintRange(p2, begin + 1, end);
    GdiFlush();
    if (pass == 0) std::copy(bits_, bits_ + (size_t)lw * lh, black.begin());
  }
  // Premultiplied layer: color = black pass, alpha = 1 - (white - black).
  uint32_t* white = bits_;
  for (size_t i = 0; i < black.size(); ++i) {
    uint32_t bpx = black[i], wpx = white[i];
    int dr = (int)((wpx >> 16) & 255) - (int)((bpx >> 16) & 255);
    int dg = (int)((wpx >> 8) & 255) - (int)((bpx >> 8) & 255);
    int db = (int)(wpx & 255) - (int)(bpx & 255);
    int a = 255 - (dr + dg + db) / 3;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    unsigned r = std::min((unsigned)a, (bpx >> 16) & 255), g = std::min((unsigned)a, (bpx >> 8) & 255),
             b = std::min((unsigned)a, bpx & 255);
    black[i] = ((unsigned)a << 24) | (r << 16) | (g << 8) | b;
  }
  // Restore the target.
  memDc_ = saveDc;
  bits_ = saveBits;
  bw_ = saveW;
  bh_ = saveH;
  clip_ = saveClip;
  clipStack_.swap(saveStack);
  ApplyClip();
  SelectObject(ldc, lold);
  DeleteObject(lbmp);
  DeleteDC(ldc);
  // Map the layer into the parent (bilinear, premultiplied source-over).
  for (int y = iy0; y < iy1; ++y) {
    uint32_t* dst = bits_ + (size_t)y * bw_;
    for (int x = ix0; x < ix1; ++x) {
      float u, v;
      inv.Apply(x + 0.5f, y + 0.5f, u, v);
      u -= 0.5f;
      v -= 0.5f;
      if (u < -1 || v < -1 || u > lw || v > lh) continue;
      int u0 = (int)std::floor(u), v0 = (int)std::floor(v);
      float fu = u - u0, fv = v - v0;
      float acc[4] = {0, 0, 0, 0};
      for (int k = 0; k < 4; ++k) {
        int sx = u0 + (k & 1), sy = v0 + (k >> 1);
        if (sx < 0 || sy < 0 || sx >= lw || sy >= lh) continue;
        float wgt = ((k & 1) ? fu : 1 - fu) * ((k >> 1) ? fv : 1 - fv);
        uint32_t p = black[(size_t)sy * lw + sx];
        acc[0] += (p >> 24) * wgt;
        acc[1] += ((p >> 16) & 255) * wgt;
        acc[2] += ((p >> 8) & 255) * wgt;
        acc[3] += (p & 255) * wgt;
      }
      if (acc[0] < 0.5f) continue;
      float inva = 1 - acc[0] / 255.0f;
      uint32_t d = dst[x];
      unsigned r = (unsigned)(acc[1] + ((d >> 16) & 255) * inva + 0.5f);
      unsigned g = (unsigned)(acc[2] + ((d >> 8) & 255) * inva + 0.5f);
      unsigned b = (unsigned)(acc[3] + (d & 255) * inva + 0.5f);
      dst[x] = (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
    }
  }
}

void Renderer::Paint(HDC hdc, const DisplayList& dl, int width, int height, float scrollX,
                     float scrollY, float zoom, const std::vector<Rect>& highlights,
                     const std::vector<Rect>& selection) {
  if (width <= 0 || height <= 0) return;
  EnsureBuffer(hdc, width, height);
  clip_.left = 0;
  clip_.top = 0;
  clip_.right = width;
  clip_.bottom = height;
  clipStack_.clear();
  ApplyClip();
  FillRect(0, 0, width, height, dl.background);
  PaintCtx pc;
  pc.dl = &dl;
  pc.zoom = zoom;
  pc.scrollX = scrollX;
  pc.scrollY = scrollY;
  pc.width = width;
  pc.height = height;
  PaintRange(pc, 0, dl.items.size());
  float ox = scrollX, oy = scrollY;
  clip_.left = 0;
  clip_.top = 0;
  clip_.right = width;
  clip_.bottom = height;
  for (size_t i = 0; i < highlights.size(); ++i) {
    const Rect& r = highlights[i];
    FillRect((int)(r.x * zoom - ox), (int)(r.y * zoom - oy), (int)(r.right() * zoom - ox) + 1,
             (int)(r.bottom() * zoom - oy) + 1, Color(255, 200, 0, i == 0 ? 140 : 90));
  }
  for (size_t i = 0; i < selection.size(); ++i) {
    const Rect& r = selection[i];
    FillRect((int)(r.x * zoom - ox), (int)(r.y * zoom - oy), (int)(r.right() * zoom - ox) + 1,
             (int)(r.bottom() * zoom - oy) + 1, Color(10, 36, 106, 90));
  }
  SelectClipRgn(memDc_, 0);
  BitBlt(hdc, 0, 0, width, height, memDc_, 0, 0, SRCCOPY);
}

}  // namespace kite
