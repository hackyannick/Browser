// Toolbar icons, drawn with GDI at 4x resolution and down-sampled for smooth
// edges (Windows 2000 has no alpha image lists, so the edges are blended
// against the toolbar color and the rest is masked out).
#include <windows.h>
#include <commctrl.h>

#include <cmath>
#include <vector>

#include "win32/app.h"

namespace kite {

namespace {

const COLORREF kKey = RGB(255, 0, 255);

void Poly(HDC dc, const POINT* pts, int n, COLORREF fill, COLORREF line, int lw) {
  HBRUSH b = CreateSolidBrush(fill);
  HPEN p = CreatePen(PS_SOLID, lw, line);
  HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
  Polygon(dc, pts, n);
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(b);
  DeleteObject(p);
}

void Circle(HDC dc, int x0, int y0, int x1, int y1, COLORREF fill, COLORREF line, int lw) {
  HBRUSH b = CreateSolidBrush(fill);
  HPEN p = CreatePen(PS_SOLID, lw, line);
  HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
  Ellipse(dc, x0, y0, x1, y1);
  SelectObject(dc, ob);
  SelectObject(dc, op);
  DeleteObject(b);
  DeleteObject(p);
}

// Draws icon |i| into a S x S area at (ox, 0) where S = 4 * size.
void DrawIcon(HDC dc, int i, int ox, int S) {
  auto P = [&](float x, float y) {
    POINT p;
    p.x = ox + (LONG)(x * S);
    p.y = (LONG)(y * S);
    return p;
  };
  int lw = S / 16;
  switch (i) {
    case kIconBack:
    case kIconForward: {
      bool back = i == kIconBack;
      COLORREF fill = RGB(46, 125, 205), line = RGB(20, 70, 140);
      Circle(dc, ox + S / 16, S / 16, ox + S - S / 16, S - S / 16, fill, line, lw);
      float d = back ? 1 : -1;
      auto Q = [&](float x, float y) { return P(back ? x : 1 - x, y); };
      POINT pts[7] = {Q(0.22f, 0.5f), Q(0.5f, 0.24f), Q(0.5f, 0.4f), Q(0.78f, 0.4f),
                      Q(0.78f, 0.6f), Q(0.5f, 0.6f), Q(0.5f, 0.76f)};
      (void)d;
      Poly(dc, pts, 7, RGB(255, 255, 255), RGB(255, 255, 255), 1);
      break;
    }
    case kIconReload: {
      // Circular arrow: a thick polyline around the center plus a head.
      HPEN p = CreatePen(PS_SOLID, S / 8, RGB(40, 140, 60));
      HGDIOBJ op = SelectObject(dc, p);
      POINT pts[40];
      int n = 0;
      for (int k = 0; k < 40; ++k) {
        float a = (-60.0f + k * 7.5f) * 3.14159265f / 180.0f;  // -60deg .. 232deg
        pts[n++] = P(0.5f + 0.32f * std::cos(a), 0.52f + 0.32f * std::sin(a));
      }
      Polyline(dc, pts, n);
      SelectObject(dc, op);
      DeleteObject(p);
      // Arrow head at the start of the arc (upper right), pointing up/left.
      POINT tri[3] = {P(0.92f, 0.05f), P(0.94f, 0.42f), P(0.58f, 0.30f)};
      Poly(dc, tri, 3, RGB(40, 140, 60), RGB(40, 140, 60), 1);
      break;
    }
    case kIconStop: {
      POINT oct[8] = {P(0.32f, 0.06f), P(0.68f, 0.06f), P(0.94f, 0.32f), P(0.94f, 0.68f),
                      P(0.68f, 0.94f), P(0.32f, 0.94f), P(0.06f, 0.68f), P(0.06f, 0.32f)};
      Poly(dc, oct, 8, RGB(214, 48, 49), RGB(150, 20, 20), lw);
      HPEN p = CreatePen(PS_SOLID, S / 9, RGB(255, 255, 255));
      HGDIOBJ op = SelectObject(dc, p);
      POINT a = P(0.32f, 0.32f), b = P(0.68f, 0.68f), c = P(0.68f, 0.32f), d = P(0.32f, 0.68f);
      MoveToEx(dc, a.x, a.y, 0);
      LineTo(dc, b.x, b.y);
      MoveToEx(dc, c.x, c.y, 0);
      LineTo(dc, d.x, d.y);
      SelectObject(dc, op);
      DeleteObject(p);
      break;
    }
    case kIconHome: {
      POINT roof[3] = {P(0.5f, 0.06f), P(0.96f, 0.48f), P(0.04f, 0.48f)};
      Poly(dc, roof, 3, RGB(200, 60, 40), RGB(130, 30, 20), lw);
      POINT body[4] = {P(0.18f, 0.46f), P(0.82f, 0.46f), P(0.82f, 0.92f), P(0.18f, 0.92f)};
      Poly(dc, body, 4, RGB(245, 235, 210), RGB(120, 100, 70), lw);
      POINT door[4] = {P(0.42f, 0.62f), P(0.58f, 0.62f), P(0.58f, 0.92f), P(0.42f, 0.92f)};
      Poly(dc, door, 4, RGB(120, 80, 40), RGB(90, 60, 30), 1);
      break;
    }
    case kIconGo: {
      POINT pts[7] = {P(0.92f, 0.5f), P(0.5f, 0.12f), P(0.5f, 0.34f), P(0.1f, 0.34f),
                      P(0.1f, 0.66f), P(0.5f, 0.66f), P(0.5f, 0.88f)};
      Poly(dc, pts, 7, RGB(60, 160, 70), RGB(30, 100, 40), lw);
      break;
    }
    case kIconNewTab: {
      POINT doc[5] = {P(0.18f, 0.06f), P(0.62f, 0.06f), P(0.84f, 0.28f), P(0.84f, 0.94f),
                      P(0.18f, 0.94f)};
      Poly(dc, doc, 5, RGB(255, 255, 255), RGB(110, 110, 110), lw);
      HPEN p = CreatePen(PS_SOLID, S / 9, RGB(40, 120, 200));
      HGDIOBJ op = SelectObject(dc, p);
      POINT a = P(0.51f, 0.38f), b = P(0.51f, 0.78f), c = P(0.31f, 0.58f), d = P(0.71f, 0.58f);
      MoveToEx(dc, a.x, a.y, 0);
      LineTo(dc, b.x, b.y);
      MoveToEx(dc, c.x, c.y, 0);
      LineTo(dc, d.x, d.y);
      SelectObject(dc, op);
      DeleteObject(p);
      break;
    }
    case kIconSearch: {
      // Magnifying glass.
      HPEN handle = CreatePen(PS_SOLID, S / 7, RGB(110, 80, 40));
      HGDIOBJ op = SelectObject(dc, handle);
      POINT a = P(0.6f, 0.6f), b = P(0.92f, 0.92f);
      MoveToEx(dc, a.x, a.y, 0);
      LineTo(dc, b.x, b.y);
      SelectObject(dc, op);
      DeleteObject(handle);
      Circle(dc, ox + S / 12, S / 12, ox + S * 2 / 3, S * 2 / 3, RGB(200, 230, 250), RGB(60, 60, 80), S / 12);
      Circle(dc, ox + S / 5, S / 5, ox + S / 3, S / 3, RGB(255, 255, 255), RGB(255, 255, 255), 1);
      break;
    }
    case kIconSecurity:
    case kIconLockOpen: {
      // Padlock: shackle (closed or swung open) and a golden body.
      bool open = i == kIconLockOpen;
      HPEN sh = CreatePen(PS_SOLID, S / 9, RGB(120, 120, 130));
      HGDIOBJ op = SelectObject(dc, sh);
      HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
      float sx0 = open ? 0.46f : 0.26f, sx1 = open ? 0.86f : 0.74f;
      POINT c0 = P(sx0, 0.08f), c1 = P(sx1, 0.62f);
      Arc(dc, c0.x, c0.y, c1.x, c1.y, c1.x, (c0.y + c1.y) / 2, c0.x, (c0.y + c1.y) / 2);
      POINT l0 = P(sx0 + 0.03f, 0.34f), l1 = P(sx0 + 0.03f, 0.5f), r0 = P(sx1 - 0.03f, 0.34f),
            r1 = P(sx1 - 0.03f, open ? 0.38f : 0.5f);
      if (!open) {
        MoveToEx(dc, l0.x, l0.y, 0);
        LineTo(dc, l1.x, l1.y);
      }
      MoveToEx(dc, r0.x, r0.y, 0);
      LineTo(dc, r1.x, r1.y);
      SelectObject(dc, ob);
      SelectObject(dc, op);
      DeleteObject(sh);
      float bx0 = open ? 0.08f : 0.16f;
      POINT body[4] = {P(bx0, 0.46f), P(bx0 + 0.68f, 0.46f), P(bx0 + 0.68f, 0.94f), P(bx0, 0.94f)};
      Poly(dc, body, 4, RGB(235, 190, 50), RGB(150, 110, 10), lw);
      Circle(dc, P(bx0 + 0.29f, 0.6f).x, P(0, 0.6f).y, P(bx0 + 0.39f, 0.7f).x, P(0, 0.7f).y, RGB(90, 60, 0),
             RGB(90, 60, 0), 1);
      break;
    }
    case kIconPage: {
      POINT doc[5] = {P(0.2f, 0.06f), P(0.6f, 0.06f), P(0.82f, 0.28f), P(0.82f, 0.94f), P(0.2f, 0.94f)};
      Poly(dc, doc, 5, RGB(255, 255, 255), RGB(90, 90, 90), lw);
      HPEN p = CreatePen(PS_SOLID, S / 16, RGB(60, 90, 160));
      HGDIOBJ op = SelectObject(dc, p);
      for (int k = 0; k < 4; ++k) {
        POINT a = P(0.32f, 0.36f + k * 0.14f), b = P(0.7f, 0.36f + k * 0.14f);
        MoveToEx(dc, a.x, a.y, 0);
        LineTo(dc, b.x, b.y);
      }
      SelectObject(dc, op);
      DeleteObject(p);
      break;
    }
    case kIconBookmark: {
      POINT star[10];
      for (int k = 0; k < 10; ++k) {
        float ang = -3.14159265f / 2 + k * 3.14159265f / 5;
        float r = (k % 2 == 0) ? 0.47f : 0.2f;
        star[k] = P(0.5f + r * std::cos(ang), 0.53f + r * std::sin(ang));
      }
      Poly(dc, star, 10, RGB(250, 200, 40), RGB(180, 130, 0), lw);
      break;
    }
  }
}

}  // namespace

HIMAGELIST CreateToolbarImageList(int size, bool disabled) {
  const int S = size * 4;
  const int n = kIconCount;
  HDC screen = GetDC(0);
  BITMAPINFO bi;
  ZeroMemory(&bi, sizeof bi);
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = S * n;
  bi.bmiHeader.biHeight = -S;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bigBits = 0;
  HBITMAP big = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bigBits, 0, 0);
  HDC dc = CreateCompatibleDC(screen);
  HGDIOBJ old = SelectObject(dc, big);
  HBRUSH key = CreateSolidBrush(kKey);
  RECT all = {0, 0, S * n, S};
  ::FillRect(dc, &all, key);
  DeleteObject(key);
  for (int i = 0; i < n; ++i) DrawIcon(dc, i, i * S, S);
  GdiFlush();

  // Down-sample with coverage-aware averaging.
  bi.bmiHeader.biWidth = size * n;
  bi.bmiHeader.biHeight = -size;
  void* smallBits = 0;
  HBITMAP small = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &smallBits, 0, 0);
  COLORREF face = GetSysColor(COLOR_BTNFACE);
  unsigned fr = GetRValue(face), fg = GetGValue(face), fb = GetBValue(face);
  const uint32_t* src = static_cast<const uint32_t*>(bigBits);
  uint32_t* dst = static_cast<uint32_t*>(smallBits);
  const uint32_t keyPix = 0xFF00FF;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size * n; ++x) {
      unsigned r = 0, g = 0, b = 0, cov = 0;
      for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx) {
          uint32_t p = src[(size_t)(y * 4 + sy) * S * n + x * 4 + sx] & 0xFFFFFF;
          if (p == keyPix) continue;
          r += (p >> 16) & 255;
          g += (p >> 8) & 255;
          b += p & 255;
          ++cov;
        }
      if (cov < 3) {
        dst[(size_t)y * size * n + x] = keyPix;
        continue;
      }
      r /= cov;
      g /= cov;
      b /= cov;
      r = (r * cov + fr * (16 - cov)) / 16;
      g = (g * cov + fg * (16 - cov)) / 16;
      b = (b * cov + fb * (16 - cov)) / 16;
      if (disabled) {
        // Grayed: lightened luminance, like disabled toolbar buttons.
        unsigned l = (r * 3 + g * 6 + b) / 10;
        l = 128 + l / 2;
        r = (l * 2 + fr) / 3;
        g = (l * 2 + fg) / 3;
        b = (l * 2 + fb) / 3;
      }
      uint32_t v = (r << 16) | (g << 8) | b;
      if (v == keyPix) v ^= 1;
      dst[(size_t)y * size * n + x] = v;
    }
  }
  SelectObject(dc, old);
  DeleteObject(big);
  DeleteDC(dc);
  ReleaseDC(0, screen);
  HIMAGELIST il = ImageList_Create(size, size, ILC_COLOR24 | ILC_MASK, n, 0);
  ImageList_AddMasked(il, small, kKey);
  DeleteObject(small);
  return il;
}

HICON CreateLockIcon(bool closed) {
  HIMAGELIST il = CreateToolbarImageList(16);
  HICON icon = ImageList_GetIcon(il, closed ? kIconSecurity : kIconLockOpen, ILD_NORMAL);
  ImageList_Destroy(il);
  return icon;
}

// Night sky with a kite (our own logo, in the spirit of the classic animated
// browser logos): idle it hangs still, while loading it sways and comets
// fly past. Drawn at 4x and down-sampled.
void DrawThrobber(HDC target, int w, int h, int frame, bool active) {
  const int K = 3;
  int W = w * K, H = h * K;
  BITMAPINFO bi;
  ZeroMemory(&bi, sizeof bi);
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = W;
  bi.bmiHeader.biHeight = -H;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = 0;
  HBITMAP big = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &bits, 0, 0);
  if (!big) return;
  HDC dc = CreateCompatibleDC(target);
  HGDIOBJ old = SelectObject(dc, big);
  // Sky: vertical gradient.
  uint32_t* px = (uint32_t*)bits;
  for (int y = 0; y < H; ++y) {
    float t = (float)y / H;
    unsigned r = (unsigned)(10 + 20 * t), g = (unsigned)(20 + 40 * t), b = (unsigned)(70 + 90 * t);
    for (int x = 0; x < W; ++x) px[(size_t)y * W + x] = (r << 16) | (g << 8) | b;
  }
  // Stars (fixed) and, while active, comets moving down-left.
  static const float stars[][2] = {{0.12f, 0.15f}, {0.8f, 0.1f}, {0.6f, 0.3f}, {0.2f, 0.7f}, {0.88f, 0.62f}, {0.35f, 0.4f}};
  for (size_t k = 0; k < sizeof stars / sizeof stars[0]; ++k) {
    int sx = (int)(stars[k][0] * W), sy = (int)(stars[k][1] * H);
    bool twinkle = active && ((frame + (int)k * 3) % 8 < 2);
    Circle(dc, sx - K, sy - K, sx + K + (twinkle ? K : 0), sy + K + (twinkle ? K : 0), RGB(255, 255, 220),
           RGB(255, 255, 220), 1);
  }
  if (active) {
    for (int c = 0; c < 2; ++c) {
      float t = ((frame * 3 + c * 17) % 30) / 30.0f;
      float cx = 1.1f - t * 1.3f, cy = -0.1f + t * 0.9f + c * 0.25f;
      HPEN tail = CreatePen(PS_SOLID, K, RGB(150, 190, 255));
      HGDIOBJ op = SelectObject(dc, tail);
      MoveToEx(dc, (int)(cx * W), (int)(cy * H), 0);
      LineTo(dc, (int)((cx + 0.25f) * W), (int)((cy - 0.17f) * H));
      SelectObject(dc, op);
      DeleteObject(tail);
      Circle(dc, (int)(cx * W) - 2 * K, (int)(cy * H) - 2 * K, (int)(cx * W) + 2 * K, (int)(cy * H) + 2 * K,
             RGB(255, 255, 255), RGB(200, 220, 255), 1);
    }
  }
  // The kite: a diamond with four colored panels and a tail of bows.
  float sway = active ? 0.06f * std::sin(frame * 0.6f) : 0;
  float cx = 0.5f + sway, cy = 0.42f;
  auto Q = [&](float x, float y) {
    // Rotate slightly with the sway.
    float a = sway * 2.5f;
    float dx = x - cx, dy = y - cy;
    POINT p;
    p.x = (LONG)((cx + dx * std::cos(a) - dy * std::sin(a)) * W);
    p.y = (LONG)((cy + dx * std::sin(a) + dy * std::cos(a)) * H);
    return p;
  };
  POINT top = Q(cx, 0.1f), right = Q(cx + 0.24f, 0.36f), bottom = Q(cx, 0.78f), left = Q(cx - 0.24f, 0.36f),
        mid = Q(cx, 0.36f);
  POINT q1[3] = {top, right, mid}, q2[3] = {right, bottom, mid}, q3[3] = {bottom, left, mid}, q4[3] = {left, top, mid};
  Poly(dc, q1, 3, RGB(230, 60, 50), RGB(230, 60, 50), 1);
  Poly(dc, q2, 3, RGB(250, 200, 40), RGB(250, 200, 40), 1);
  Poly(dc, q3, 3, RGB(50, 170, 80), RGB(50, 170, 80), 1);
  Poly(dc, q4, 3, RGB(60, 120, 230), RGB(60, 120, 230), 1);
  HPEN frameP = CreatePen(PS_SOLID, K, RGB(250, 250, 250));
  HGDIOBJ op = SelectObject(dc, frameP);
  POINT outline[5] = {top, right, bottom, left, top};
  Polyline(dc, outline, 5);
  MoveToEx(dc, top.x, top.y, 0);
  LineTo(dc, bottom.x, bottom.y);
  MoveToEx(dc, left.x, left.y, 0);
  LineTo(dc, right.x, right.y);
  // Tail: a wavy string with bows.
  POINT tail[8];
  for (int k = 0; k < 8; ++k) {
    float ty = 0.78f + k * 0.03f;
    float tx = cx + 0.03f * std::sin(k * 1.3f + (active ? frame * 0.8f : 0)) - k * 0.012f;
    tail[k].x = (LONG)(tx * W);
    tail[k].y = (LONG)(ty * H);
  }
  Polyline(dc, tail, 8);
  SelectObject(dc, op);
  DeleteObject(frameP);
  for (int k = 2; k < 8; k += 3) {
    POINT bow[4] = {{tail[k].x - 2 * K, tail[k].y - K}, {tail[k].x + 2 * K, tail[k].y + K},
                    {tail[k].x + 2 * K, tail[k].y - K}, {tail[k].x - 2 * K, tail[k].y + K}};
    Poly(dc, bow, 4, RGB(250, 120, 160), RGB(250, 120, 160), 1);
  }
  GdiFlush();
  // Down-sample into a small DIB and blit.
  BITMAPINFO sb = bi;
  sb.bmiHeader.biWidth = w;
  sb.bmiHeader.biHeight = -h;
  void* sbits = 0;
  HBITMAP small = CreateDIBSection(target, &sb, DIB_RGB_COLORS, &sbits, 0, 0);
  if (small) {
    uint32_t* d = (uint32_t*)sbits;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        unsigned r = 0, g = 0, b = 0;
        for (int sy = 0; sy < K; ++sy)
          for (int sx = 0; sx < K; ++sx) {
            uint32_t p = px[(size_t)(y * K + sy) * W + x * K + sx];
            r += (p >> 16) & 255;
            g += (p >> 8) & 255;
            b += p & 255;
          }
        d[(size_t)y * w + x] = ((r / (K * K)) << 16) | ((g / (K * K)) << 8) | (b / (K * K));
      }
    HDC sdc = CreateCompatibleDC(target);
    HGDIOBJ so = SelectObject(sdc, small);
    BitBlt(target, 0, 0, w, h, sdc, 0, 0, SRCCOPY);
    SelectObject(sdc, so);
    DeleteDC(sdc);
    DeleteObject(small);
  }
  SelectObject(dc, old);
  DeleteDC(dc);
  DeleteObject(big);
}

}  // namespace kite
