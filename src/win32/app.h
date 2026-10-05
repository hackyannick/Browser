// Kite Browser - Win32 front end (Windows 2000 and later)
#ifndef KITE_WIN32_APP_H
#define KITE_WIN32_APP_H

#include <windows.h>
#include <commctrl.h>

#include <list>
#include <map>
#include <set>
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
  bool javaScript;
  bool http2;                 // kite.ini: Http2=0 disables HTTP/2
  int defaultZoom;            // percent
  Settings() : proxyPort(0), loadImages(true), javaScript(true), http2(true), defaultZoom(100) {}
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
  HFONT Get(const FontDesc& font, float scale, bool grayscale = false);
  // Installs a downloaded font (sfnt data) for this process only and maps the
  // CSS family/weight/style to it.
  bool RegisterWebFont(const std::string& cssFamily, int weight, bool italic,
                       const std::string& sfnt, const std::string& internalName);
  bool IsWebFont(const FontDesc& font);
  bool RasterizeText(const FontDesc& font, const std::string& utf8, TextMask& out);

 private:
  struct WebFont {
    int weight;
    bool italic;
    std::wstring face;
  };
  const WebFont* FindWebFont(const FontDesc& f);
  std::map<std::string, std::vector<WebFont> > webFonts_;
  std::vector<HANDLE> fontHandles_;
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
    unsigned started = 0;  // animations: GetTickCount() when loaded
    int shown = 0;         // animations: frame currently in image.pixels
  };
  State GetImage(const std::string& url, int& width, int& height);
  const DecodedImage* Find(const std::string& url);
  const DecodedImage* Pixels(const std::string& url) { return Find(url); }
  void SetLoading(const std::string& url);
  void SetLoaded(const std::string& url, DecodedImage& img);
  void SetFailed(const std::string& url);
  void SetUnsupported(const std::string& url);
  bool IsKnown(const std::string& url) const { return entries_.count(url) != 0; }
  void Trim();
  // Scaled copy cache for fast repainting.
  const DecodedImage* Scaled(const std::string& url, int w, int h);
  // Animated images: shows the frame due now for each of |urls|. Returns
  // true if one of them changed; |nextMs| receives the time until the next
  // frame change (-1 if none of them animates).
  bool AdvanceAnimations(const std::set<std::string>& urls, int& nextMs);

 private:
  std::map<std::string, Entry> entries_;
  std::map<std::string, DecodedImage> scaled_;
  std::map<std::string, std::pair<unsigned, DecodedImage> > canvasScaled_;
  std::list<std::string> scaledOrder_;
  size_t scaledBytes_ = 0;
  unsigned clock_ = 0;
};
ImageCache& Images();

// ---------------------------------------------------------------------------
// Background network jobs. Results are posted to |notify| as WM_KITE_FETCHED.
const UINT WM_KITE_FETCHED = WM_APP + 1;

struct FetchJob {
  enum Kind { kDocument, kStylesheet, kImage, kDownload, kFont, kScript, kScriptRequest };
  Kind kind;
  int tabId;
  int generation;
  FetchRequest request;
  FetchResponse response;
  DecodedImage image;   // kImage: decoded in the worker thread
  bool imageOk;
  // kFont: converted sfnt data and its internal family name.
  std::string fontData, fontName, fontFamily;
  int fontWeight = 400;
  bool fontItalic = false;
  int scriptRequestId = 0;  // kScriptRequest: fetch()/XMLHttpRequest id
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
  void FillCoverage(int x0, int y0, int x1, int y1, Color c,
                    float (*cov)(float, float, const void*), const void* ctx);
  void Blit(const DecodedImage& img, int dx, int dy, const RECT& area, unsigned alpha);
  std::map<std::string, DecodedImage> svgCache_;

 public:
  // Rasterized inline SVGs reference DOM nodes; drop them when documents change.
  void ClearSvgCache() { svgCache_.clear(); }

 private:
  void ApplyClip();
  struct PaintCtx {
    const DisplayList* dl = 0;
    float zoom = 1, scrollX = 0, scrollY = 0;
    float layerX = 0, layerY = 0;  // offset of the current layer in parent device space
    int width = 0, height = 0;
    int fixedDepth = 0;
  };
  void PaintRange(PaintCtx& pc, size_t begin, size_t end);
  void DrawTransformed(PaintCtx& pc, size_t begin, size_t end, float ox, float oy);

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
