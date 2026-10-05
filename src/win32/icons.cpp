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

HIMAGELIST CreateToolbarImageList(int size) {
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

}  // namespace kite
