// Rasterizes Kite display lists into a 32-bit DIB section. Rectangles and
// images are blended in software (alpha support without needing GDI+ or
// AlphaBlend), text is drawn with GDI.
#include "win32/app.h"

#include <algorithm>
#include <cmath>

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
  for (int ty = iy0; ty < iy1; ++ty) {
    for (int tx = ix0; tx < ix1; ++tx) {
      int dx = tx0 + tx * tw, dy = ty0 + ty * th;
      int x0 = std::max((int)area.left, dx), x1 = std::min((int)area.right, dx + tw);
      int y0 = std::max((int)area.top, dy), y1 = std::min((int)area.bottom, dy + th);
      for (int y = y0; y < y1; ++y) {
        const uint32_t* src = &img->pixels[(size_t)(y - dy) * img->width + (x0 - dx)];
        uint32_t* dst = bits_ + (size_t)y * bw_ + x0;
        if (!img->hasAlpha && alpha == 255) {
          for (int x = x0; x < x1; ++x) *dst++ = *src++ & 0xFFFFFF;
          continue;
        }
        for (int x = x0; x < x1; ++x, ++src, ++dst) {
          uint32_t s = *src;
          unsigned sa = s >> 24;
          unsigned sr = (s >> 16) & 255, sg = (s >> 8) & 255, sb = s & 255;
          if (alpha != 255) {
            sa = sa * alpha / 255;
            sr = sr * alpha / 255;
            sg = sg * alpha / 255;
            sb = sb * alpha / 255;
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

static COLORREF Blend(Color c, Color bg) {
  if (c.a == 255) return RGB(c.r, c.g, c.b);
  unsigned a = c.a, ia = 255 - a;
  return RGB((c.r * a + bg.r * ia) / 255, (c.g * a + bg.g * ia) / 255, (c.b * a + bg.b * ia) / 255);
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
  const float ox = scrollX, oy = scrollY;
  const float viewTop = oy / zoom, viewBottom = (oy + height) / zoom;
  const float viewLeft = ox / zoom, viewRight = (ox + width) / zoom;
  bool gdiDirty = false;
  for (size_t i = 0; i < dl.items.size(); ++i) {
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
        for (size_t k = 0; k < w.size(); ++k) {
          wchar_t c = w[k];
          if ((c >= 0xE000 && c <= 0xF8FF) || c == 0x200B || c == 0x200C || c == 0x200D ||
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
