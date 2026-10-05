// Kite Browser - Win32 front end (Windows 2000 and later)
#ifndef KITE_WIN32_APP_H
#define KITE_WIN32_APP_H

#include <windows.h>
#include <commctrl.h>

#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "image/image.h"
#include "layout/resources.h"
#include "net/http.h"
#include "paint/display_list.h"

namespace kite {

// ---------------------------------------------------------------------------
// String helpers for the Win32 W APIs.
std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& w);
std::wstring WindowTextW(HWND hwnd);

// ---------------------------------------------------------------------------
// Settings (kite.ini next to the executable).
struct Settings {
  std::string homePage;
  std::string searchUrl;      // prefix; the query is appended
  std::string proxyHost;
  int proxyPort;
  bool loadImages;
  int defaultZoom;            // percent
  Settings() : proxyPort(0), loadImages(true), defaultZoom(100) {}
};

class App {
 public:
  static App& Get();
  void Init(HINSTANCE inst);
  void SaveSettings();
  void SaveCookies();
  void LoadBookmarks();
  void SaveBookmarks();
  void ApplyNetworkSettings();

  HINSTANCE instance;
  std::wstring dataDir;  // where kite.ini, cookies.txt, bookmarks.txt live
  Settings settings;
  std::vector<std::pair<std::string, std::string> > bookmarks;  // title, url
  std::vector<std::string> typedHistory;  // recently typed addresses
  bool isXpOrLater;

 private:
  App() : instance(0), isXpOrLater(false) {}
};

// ---------------------------------------------------------------------------
// GDI based fonts (FontProvider for the layout engine).
class GdiFonts : public FontProvider {
 public:
  GdiFonts();
  ~GdiFonts();
  FontMetrics Metrics(const FontDesc& font);
  float MeasureText(const FontDesc& font, const std::string& utf8);
  // Returns a cached HFONT for drawing at |scale| (zoom).
  HFONT Get(const FontDesc& font, float scale);

 private:
  std::wstring ResolveFamily(const std::string& cssFamilies);
  std::map<std::string, HFONT> fonts_;
  std::map<std::string, std::wstring> familyCache_;
  std::map<std::wstring, bool> installed_;
  HDC dc_;
};
GdiFonts& Fonts();

// ---------------------------------------------------------------------------
// Decoded image cache shared by all tabs.
class ImageCache : public ImageProvider {
 public:
  struct Entry {
    State state;
    DecodedImage image;
    unsigned lastUse;
  };
  State GetImage(const std::string& url, int& width, int& height);
  const DecodedImage* Find(const std::string& url);
  void SetLoading(const std::string& url);
  void SetLoaded(const std::string& url, DecodedImage& img);
  void SetFailed(const std::string& url);
  void SetUnsupported(const std::string& url);
  bool IsKnown(const std::string& url) const { return entries_.count(url) != 0; }
  void Trim();
  // Scaled copy cache for fast repainting.
  const DecodedImage* Scaled(const std::string& url, int w, int h);

 private:
  std::map<std::string, Entry> entries_;
  std::map<std::string, DecodedImage> scaled_;
  std::list<std::string> scaledOrder_;
  size_t scaledBytes_ = 0;
  unsigned clock_ = 0;
};
ImageCache& Images();

// ---------------------------------------------------------------------------
// Background network jobs. Results are posted to |notify| as WM_KITE_FETCHED.
const UINT WM_KITE_FETCHED = WM_APP + 1;

struct FetchJob {
  enum Kind { kDocument, kStylesheet, kImage, kDownload };
  Kind kind;
  int tabId;
  int generation;
  FetchRequest request;
  FetchResponse response;
  DecodedImage image;   // kImage: decoded in the worker thread
  bool imageOk;
  HWND notify;
  FetchJob() : kind(kDocument), tabId(0), generation(0), imageOk(false), notify(0) {}
};

// Starts |job| on a worker thread (limited concurrency, queued otherwise).
void StartFetch(FetchJob* job);
// Must be called by the UI thread when a WM_KITE_FETCHED arrives.
void FetchFinished();

// ---------------------------------------------------------------------------
// Rendering of a display list into a 32-bit DIB.
class Renderer {
 public:
  Renderer();
  ~Renderer();
  // Paints the part of |dl| visible at (scrollX, scrollY) into |hdc|.
  void Paint(HDC hdc, const DisplayList& dl, int width, int height, float scrollX,
             float scrollY, float zoom, const std::vector<Rect>& highlights,
             const std::vector<Rect>& selection);

 private:
  void EnsureBuffer(HDC hdc, int w, int h);
  void FillRect(int x0, int y0, int x1, int y1, Color c);
  void DrawImage(const DisplayItem& it, float ox, float oy, float zoom);
  void DrawSvg(const DisplayItem& it, float ox, float oy, float zoom);
  void Blit(const DecodedImage& img, int dx, int dy, const RECT& area, unsigned alpha);
  std::map<std::string, DecodedImage> svgCache_;

 public:
  // Rasterized inline SVGs reference DOM nodes; drop them when documents change.
  void ClearSvgCache() { svgCache_.clear(); }

 private:
  void ApplyClip();

  HDC memDc_;
  HBITMAP bitmap_;
  HBITMAP oldBitmap_;
  uint32_t* bits_;
  int bw_, bh_;
  RECT clip_;
  std::vector<RECT> clipStack_;
};

// ---------------------------------------------------------------------------
// Built-in pages.
std::string StartPageHtml();
std::string ErrorPageHtml(const std::string& url, const std::string& message);
std::string AboutPageHtml();
std::string BookmarksPageHtml();

// Creates the main window and runs the message loop.
int RunBrowser(HINSTANCE inst, const std::wstring& startUrl);

// Toolbar icons drawn at runtime (no bitmap resources needed).
HIMAGELIST CreateToolbarImageList(int size);
enum ToolbarIcon {
  kIconBack, kIconForward, kIconReload, kIconStop, kIconHome, kIconGo, kIconNewTab,
  kIconBookmark, kIconCount
};

}  // namespace kite

#endif
