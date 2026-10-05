// Kite Browser main window: tabs, toolbar, address bar, page view, status bar.
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "base/strings.h"
#include "html/parser.h"
#include "image/svg.h"
#include "page/page.h"
#include "win32/app.h"
#include "win32/resource.h"

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef IDC_HAND
#define IDC_HAND MAKEINTRESOURCE(32649)
#endif

namespace kite {

namespace {

const wchar_t kMainClass[] = L"KiteBrowserWindow";
const wchar_t kViewClass[] = L"KitePageView";
const UINT_PTR kTimerRelayout = 1;
const UINT_PTR kTimerResize = 2;
const UINT_PTR kTimerStyleFallbackBase = 0x1000;
const UINT_PTR kTimerRefreshBase = 0x2000;
const int kZoomLevels[] = {30, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300};

struct HistoryEntry {
  std::string url;
  std::string title;
  float scrollY;
  bool isPost;
  FormSubmission post;
  HistoryEntry() : scrollY(0), isPost(false) {}
};

struct PendingNav {
  std::string url;
  bool replace;        // replace the current history entry
  bool fromHistory;    // back/forward: keep entries, restore scroll
  int historyTarget;
  float restoreScroll;
  bool isPost;
  FormSubmission post;
  bool viewSource;
  PendingNav() : replace(false), fromHistory(false), historyTarget(-1), restoreScroll(-1),
                 isPost(false), viewSource(false) {}
};

class Tab {
 public:
  Tab(int id_) : id(id_), historyIndex(-1), loading(false), generation(0), secure(false),
                 scrollX(0), scrollY(0), pendingSheets(0), pendingImages(0), rendered(false),
                 needsRelayout(false), restoreScroll(-1), findIndex(-1) {
    page.reset(new Page(&Fonts(), &Images()));
  }
  int id;
  std::unique_ptr<Page> page;
  std::vector<HistoryEntry> history;
  int historyIndex;
  bool loading;
  int generation;
  std::shared_ptr<CancelToken> cancel;
  PendingNav pending;
  std::string url;          // current document URL
  std::string displayUrl;   // what the address bar shows
  std::string title;
  std::string rawSource;
  bool secure;
  float scrollX, scrollY;   // device pixels
  int pendingSheets;
  int pendingImages;
  bool rendered;
  bool needsRelayout;
  std::string fragment;
  float restoreScroll;
  std::vector<Rect> highlights;
  int findIndex;
  std::vector<Rect> selection;
  std::string selectedText;
  std::string status;
  std::set<std::string> requestedSheets;
  float renderedW = 0, renderedH = 0;
};

class Browser {
 public:
  Browser() : hwnd_(0), tabs_(0), toolbar_(0), toolbar2_(0), address_(0), view_(0), status_(0),
              findBar_(0), findEdit_(0), findVisible_(false), current_(0), nextTabId_(1),
              zoom_(100), uiFont_(0), addressFont_(0), editCtl_(0), editNode_(0),
              relayoutPending_(false), dragging_(false), dragMoved_(false), accel_(0),
              ctxNode_(0), imageList_(0) {}

  bool Create(HINSTANCE inst, const std::wstring& startUrl);
  int Run();

  static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l);
  static LRESULT CALLBACK ViewProc(HWND h, UINT m, WPARAM w, LPARAM l);
  static LRESULT CALLBACK AddressProc(HWND h, UINT m, WPARAM w, LPARAM l);
  static LRESULT CALLBACK EditCtlProc(HWND h, UINT m, WPARAM w, LPARAM l);
  static LRESULT CALLBACK FindEditProc(HWND h, UINT m, WPARAM w, LPARAM l);
  static INT_PTR CALLBACK SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l);

 private:
  LRESULT HandleMain(UINT m, WPARAM w, LPARAM l);
  LRESULT HandleView(HWND h, UINT m, WPARAM w, LPARAM l);
  void CreateChildren();
  void LayoutChildren();
  HMENU BuildMenu();
  void RebuildBookmarkMenu();
  void OnCommand(int id);

  // Tabs.
  Tab* NewTab(const std::string& url, bool activate);
  void CloseTab(Tab* t);
  void ActivateTab(Tab* t);
  int TabIndex(Tab* t) const;
  Tab* TabById(int id) const;
  void UpdateTabLabel(Tab* t);

  // Navigation.
  void Navigate(Tab* t, const std::string& input, const std::string& referrer = std::string(),
                const FormSubmission* post = 0);
  void StartLoad(Tab* t, const PendingNav& nav, const std::string& referrer);
  void LoadInternal(Tab* t, const std::string& html, const std::string& url, const PendingNav& nav);
  void GoHistory(Tab* t, int delta);
  void Reload(Tab* t);
  void Stop(Tab* t);
  void OnFetched(FetchJob* job);
  void OnDocument(Tab* t, FetchJob* job);
  void CommitNavigation(Tab* t, const std::string& finalUrl, bool secure);
  void RequestStylesheets(Tab* t);
  void RenderTab(Tab* t, bool restyle);
  void RequestImages(Tab* t);
  void FinishLoadingIfDone(Tab* t);
  void ScheduleRelayout();
  void SubmitForm(Tab* t, Node* form, Node* submitter, bool newTab);
  void Download(const std::string& url, const std::string& suggestedName, const std::string& referrer);
  void SaveBody(const FetchResponse& r, const std::wstring& path);

  // View helpers.
  float ViewportWidth() const;
  float ViewportHeight() const;
  float ZoomF() const { return zoom_ / 100.0f; }
  void UpdateScrollBars();
  void ScrollTo(float x, float y);
  void ScrollBy(float dx, float dy);
  Node* NodeAt(int x, int y);
  void UpdateUi();
  void SetStatus(const std::string& s);
  void OnClick(int x, int y, bool newTab);
  void BeginEdit(Node* n);
  void CommitEdit(bool submit);
  void ShowSelectPopup(Node* select);
  void ToggleControl(Node* n);
  void ShowContextMenu(int x, int y);
  void UpdateSelection(int x0, int y0, int x1, int y1);
  void CopyText(const std::string& text);
  void Find(bool forward, bool fromStart);
  void ShowFindBar(bool show);
  void SetZoom(int z);
  void AddBookmark();

  HWND hwnd_, tabs_, toolbar_, toolbar2_, address_, view_, status_;
  HWND findBar_, findEdit_;
  bool findVisible_;
  std::vector<Tab*> tabList_;
  Tab* current_;
  int nextTabId_;
  int zoom_;
  HFONT uiFont_, addressFont_;
  HWND editCtl_;
  Node* editNode_;
  bool relayoutPending_;
  bool dragging_, dragMoved_;
  POINT dragStart_;
  HACCEL accel_;
  Renderer renderer_;
  Node* ctxNode_;
  std::string ctxLink_, ctxImage_;
  HIMAGELIST imageList_;
  WNDPROC oldAddressProc_ = 0, oldEditProc_ = 0, oldFindProc_ = 0;
  std::vector<std::pair<std::string, std::string> > globalHistory_;
};

Browser* g_browser = 0;

std::string ShortTitle(const std::string& t, size_t max) {
  if (Utf8Length(t) <= max) return t;
  std::string out;
  size_t i = 0, n = 0;
  while (i < t.size() && n < max - 1) {
    size_t s = i;
    Utf8Next(t, i);
    out.append(t, s, i - s);
    ++n;
  }
  return out + "\xE2\x80\xA6";
}

bool IsHtmlMime(const std::string& m) {
  return m == "text/html" || m == "application/xhtml+xml" || m.empty();
}

bool IsTextMime(const std::string& m) {
  return StartsWith(m, "text/") || m == "application/json" || m == "application/javascript" ||
         m == "application/xml" || m == "application/rss+xml" || m == "application/atom+xml" ||
         EndsWith(m, "+json");
}

bool IsImageMime(const std::string& m) {
  return m == "image/svg+xml" || m == "image/png" || m == "image/jpeg" || m == "image/jpg" || m == "image/gif" ||
         m == "image/bmp" || m == "image/x-ms-bmp" || m == "image/pjpeg" || m == "image/x-png";
}

}  // namespace

// ---------------------------------------------------------------------------
// Window creation

bool Browser::Create(HINSTANCE inst, const std::wstring& startUrl) {
  WNDCLASSEXW wc;
  ZeroMemory(&wc, sizeof wc);
  wc.cbSize = sizeof wc;
  wc.lpfnWndProc = MainProc;
  wc.hInstance = inst;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_KITE));
  wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_KITE), IMAGE_ICON, 16, 16, 0);
  wc.hCursor = LoadCursorW(0, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = kMainClass;
  RegisterClassExW(&wc);

  WNDCLASSEXW vc;
  ZeroMemory(&vc, sizeof vc);
  vc.cbSize = sizeof vc;
  vc.style = CS_DBLCLKS;
  vc.lpfnWndProc = ViewProc;
  vc.hInstance = inst;
  vc.hCursor = LoadCursorW(0, IDC_ARROW);
  vc.lpszClassName = kViewClass;
  RegisterClassExW(&vc);

  RECT work;
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  int w = std::min(1100, (int)(work.right - work.left) * 9 / 10);
  int h = std::min(820, (int)(work.bottom - work.top) * 9 / 10);
  int x = work.left + ((work.right - work.left) - w) / 2;
  int y = work.top + ((work.bottom - work.top) - h) / 2;

  zoom_ = App::Get().settings.defaultZoom;
  if (zoom_ < 30 || zoom_ > 300) zoom_ = 100;
  hwnd_ = CreateWindowExW(0, kMainClass, L"Kite", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h,
                          0, BuildMenu(), inst, this);
  if (!hwnd_) return false;
  CreateChildren();

  ACCEL acc[] = {
      {FCONTROL | FVIRTKEY, 'T', ID_FILE_NEWTAB},
      {FCONTROL | FVIRTKEY, 'W', ID_FILE_CLOSETAB},
      {FCONTROL | FVIRTKEY, VK_F4, ID_FILE_CLOSETAB},
      {FCONTROL | FVIRTKEY, 'O', ID_FILE_OPEN},
      {FCONTROL | FVIRTKEY, 'S', ID_FILE_SAVE},
      {FCONTROL | FVIRTKEY, 'L', ID_GO_ADDRESS},
      {FALT | FVIRTKEY, 'D', ID_GO_ADDRESS},
      {FVIRTKEY, VK_F6, ID_GO_ADDRESS},
      {FVIRTKEY, VK_F5, ID_VIEW_RELOAD},
      {FCONTROL | FVIRTKEY, 'R', ID_VIEW_RELOAD},
      {FALT | FVIRTKEY, VK_LEFT, ID_GO_BACK},
      {FALT | FVIRTKEY, VK_RIGHT, ID_GO_FORWARD},
      {FALT | FVIRTKEY, VK_HOME, ID_GO_HOME},
      {FCONTROL | FVIRTKEY, 'F', ID_EDIT_FIND},
      {FVIRTKEY, VK_F3, ID_EDIT_FINDNEXT},
      {FSHIFT | FVIRTKEY, VK_F3, ID_EDIT_FINDPREV},
      {FCONTROL | FVIRTKEY, 'U', ID_VIEW_SOURCE},
      {FCONTROL | FVIRTKEY, 'D', ID_BM_ADD},
      {FCONTROL | FVIRTKEY, 'B', ID_BM_MANAGE},
      {FCONTROL | FVIRTKEY, VK_ADD, ID_VIEW_ZOOMIN},
      {FCONTROL | FVIRTKEY, VK_OEM_PLUS, ID_VIEW_ZOOMIN},
      {FCONTROL | FVIRTKEY, VK_SUBTRACT, ID_VIEW_ZOOMOUT},
      {FCONTROL | FVIRTKEY, VK_OEM_MINUS, ID_VIEW_ZOOMOUT},
      {FCONTROL | FVIRTKEY, '0', ID_VIEW_ZOOMRESET},
      {FCONTROL | FVIRTKEY, VK_NUMPAD0, ID_VIEW_ZOOMRESET},
      {FCONTROL | FVIRTKEY, VK_TAB, ID_GO_NEXTTAB},
      {FCONTROL | FSHIFT | FVIRTKEY, VK_TAB, ID_GO_PREVTAB},
      {FCONTROL | FVIRTKEY, VK_NEXT, ID_GO_NEXTTAB},
      {FCONTROL | FVIRTKEY, VK_PRIOR, ID_GO_PREVTAB},
  };
  accel_ = CreateAcceleratorTableW(acc, sizeof acc / sizeof acc[0]);

  std::string start = startUrl.empty() ? App::Get().settings.homePage : Narrow(startUrl);
  NewTab(start, true);
  ShowWindow(hwnd_, SW_SHOWNORMAL);
  UpdateWindow(hwnd_);
  return true;
}

HMENU Browser::BuildMenu() {
  HMENU bar = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, ID_FILE_NEWTAB, L"&Neuer Tab\tStrg+T");
  AppendMenuW(file, MF_STRING, ID_FILE_OPEN, L"Datei \x00f6&ffnen...\tStrg+O");
  AppendMenuW(file, MF_STRING, ID_FILE_SAVE, L"Seite &speichern unter...\tStrg+S");
  AppendMenuW(file, MF_SEPARATOR, 0, 0);
  AppendMenuW(file, MF_STRING, ID_FILE_CLOSETAB, L"Tab s&chlie\x00dfen\tStrg+W");
  AppendMenuW(file, MF_STRING, ID_FILE_EXIT, L"&Beenden");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&Datei");

  HMENU edit = CreatePopupMenu();
  AppendMenuW(edit, MF_STRING, ID_EDIT_COPY, L"&Kopieren\tStrg+C");
  AppendMenuW(edit, MF_STRING, ID_EDIT_SELECTALL, L"&Alles markieren\tStrg+A");
  AppendMenuW(edit, MF_SEPARATOR, 0, 0);
  AppendMenuW(edit, MF_STRING, ID_EDIT_FIND, L"&Suchen...\tStrg+F");
  AppendMenuW(edit, MF_STRING, ID_EDIT_FINDNEXT, L"&Weitersuchen\tF3");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Bearbeiten");

  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, ID_VIEW_RELOAD, L"&Neu laden\tF5");
  AppendMenuW(view, MF_STRING, ID_VIEW_STOP, L"&Abbrechen\tEsc");
  AppendMenuW(view, MF_SEPARATOR, 0, 0);
  AppendMenuW(view, MF_STRING, ID_VIEW_ZOOMIN, L"Ver&gr\x00f6\x00dfern\tStrg++");
  AppendMenuW(view, MF_STRING, ID_VIEW_ZOOMOUT, L"Ver&kleinern\tStrg+-");
  AppendMenuW(view, MF_STRING, ID_VIEW_ZOOMRESET, L"Originalgr\x00f6\x00df&e\tStrg+0");
  AppendMenuW(view, MF_SEPARATOR, 0, 0);
  AppendMenuW(view, MF_STRING, ID_VIEW_SOURCE, L"Seiten&quelltext\tStrg+U");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&Ansicht");

  HMENU go = CreatePopupMenu();
  AppendMenuW(go, MF_STRING, ID_GO_BACK, L"&Zur\x00fc" L"ck\tAlt+Links");
  AppendMenuW(go, MF_STRING, ID_GO_FORWARD, L"&Vor\tAlt+Rechts");
  AppendMenuW(go, MF_STRING, ID_GO_HOME, L"&Startseite\tAlt+Pos1");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)go, L"&Verlauf");

  HMENU bm = CreatePopupMenu();
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)bm, L"&Lesezeichen");

  HMENU tools = CreatePopupMenu();
  AppendMenuW(tools, MF_STRING, ID_TOOLS_SETTINGS, L"&Einstellungen...");
  AppendMenuW(tools, MF_STRING, ID_TOOLS_CLEARCOOKIES, L"&Cookies l\x00f6schen");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, L"E&xtras");

  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, L"\x00dc&" L"ber Kite");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Hilfe");
  return bar;
}

void Browser::RebuildBookmarkMenu() {
  HMENU bar = GetMenu(hwnd_);
  HMENU bm = GetSubMenu(bar, 4);
  while (GetMenuItemCount(bm) > 0) DeleteMenu(bm, 0, MF_BYPOSITION);
  AppendMenuW(bm, MF_STRING, ID_BM_ADD, L"Lesezeichen &hinzuf\x00fcgen\tStrg+D");
  AppendMenuW(bm, MF_STRING, ID_BM_REMOVE, L"Aktuelle Seite &entfernen");
  AppendMenuW(bm, MF_STRING, ID_BM_MANAGE, L"Alle &anzeigen\tStrg+B");
  const std::vector<std::pair<std::string, std::string> >& list = App::Get().bookmarks;
  if (!list.empty()) AppendMenuW(bm, MF_SEPARATOR, 0, 0);
  for (size_t i = 0; i < list.size() && i < 500; ++i) {
    std::string t = list[i].first.empty() ? list[i].second : list[i].first;
    AppendMenuW(bm, MF_STRING, ID_BOOKMARK_BASE + i, Widen(ShortTitle(t, 60)).c_str());
  }
  // History submenu items (global history) live in the "Verlauf" menu.
  HMENU go = GetSubMenu(bar, 3);
  while (GetMenuItemCount(go) > 3) DeleteMenu(go, 3, MF_BYPOSITION);
  if (!globalHistory_.empty()) AppendMenuW(go, MF_SEPARATOR, 0, 0);
  for (size_t i = 0; i < globalHistory_.size() && i < 20; ++i) {
    const std::pair<std::string, std::string>& e = globalHistory_[globalHistory_.size() - 1 - i];
    std::string t = e.second.empty() ? e.first : e.second;
    AppendMenuW(go, MF_STRING, ID_HISTORY_BASE + i, Widen(ShortTitle(t, 60)).c_str());
  }
}

void Browser::CreateChildren() {
  HINSTANCE inst = App::Get().instance;
  NONCLIENTMETRICSW ncm;
  ZeroMemory(&ncm, sizeof ncm);
  ncm.cbSize = sizeof ncm;
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
  uiFont_ = CreateFontIndirectW(&ncm.lfMessageFont);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = -15;
  addressFont_ = CreateFontIndirectW(&lf);

  tabs_ = CreateWindowExW(0, WC_TABCONTROLW, L"",
                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER | TCS_TOOLTIPS |
                              TCS_SINGLELINE,
                          0, 0, 100, 24, hwnd_, 0, inst, 0);
  SendMessageW(tabs_, WM_SETFONT, (WPARAM)uiFont_, TRUE);

  imageList_ = CreateToolbarImageList(20);
  toolbar_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"",
                             WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
                                 CCS_NORESIZE | CCS_NODIVIDER | CCS_NOPARENTALIGN,
                             0, 0, 100, 32, hwnd_, 0, inst, 0);
  SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  SendMessageW(toolbar_, TB_SETIMAGELIST, 0, (LPARAM)imageList_);
  SendMessageW(toolbar_, TB_SETBITMAPSIZE, 0, MAKELONG(20, 20));
  TBBUTTON b[4];
  ZeroMemory(b, sizeof b);
  int ids[4] = {ID_GO_BACK, ID_GO_FORWARD, ID_VIEW_RELOAD, ID_GO_HOME};
  int icons[4] = {kIconBack, kIconForward, kIconReload, kIconHome};
  for (int i = 0; i < 4; ++i) {
    b[i].iBitmap = icons[i];
    b[i].idCommand = ids[i];
    b[i].fsState = TBSTATE_ENABLED;
    b[i].fsStyle = TBSTYLE_BUTTON;
  }
  SendMessageW(toolbar_, TB_ADDBUTTONSW, 4, (LPARAM)b);

  toolbar2_ = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"",
                              WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
                                  CCS_NORESIZE | CCS_NODIVIDER | CCS_NOPARENTALIGN,
                              0, 0, 100, 32, hwnd_, 0, inst, 0);
  SendMessageW(toolbar2_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  SendMessageW(toolbar2_, TB_SETIMAGELIST, 0, (LPARAM)imageList_);
  SendMessageW(toolbar2_, TB_SETBITMAPSIZE, 0, MAKELONG(20, 20));
  int ids2[3] = {ID_GO, ID_BM_ADD, ID_FILE_NEWTAB};
  int icons2[3] = {kIconGo, kIconBookmark, kIconNewTab};
  for (int i = 0; i < 3; ++i) {
    b[i].iBitmap = icons2[i];
    b[i].idCommand = ids2[i];
    b[i].fsState = TBSTATE_ENABLED;
    b[i].fsStyle = TBSTYLE_BUTTON;
  }
  SendMessageW(toolbar2_, TB_ADDBUTTONSW, 3, (LPARAM)b);

  address_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 100, 24, hwnd_, 0, inst, 0);
  SendMessageW(address_, WM_SETFONT, (WPARAM)addressFont_, TRUE);
  SetWindowLongPtrW(address_, GWLP_USERDATA, (LONG_PTR)this);
  oldAddressProc_ = (WNDPROC)SetWindowLongPtrW(address_, GWLP_WNDPROC, (LONG_PTR)AddressProc);

  view_ = CreateWindowExW(WS_EX_CLIENTEDGE, kViewClass, L"",
                          WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN | WS_TABSTOP,
                          0, 0, 100, 100, hwnd_, 0, inst, this);

  findBar_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, 100, 28, hwnd_, 0, inst, 0);
  HWND label = CreateWindowExW(0, L"STATIC", L"Suchen:", WS_CHILD | WS_VISIBLE, 8, 7, 50, 16, findBar_, 0,
                               inst, 0);
  SendMessageW(label, WM_SETFONT, (WPARAM)uiFont_, TRUE);
  findEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 60, 3,
                              220, 22, findBar_, 0, inst, 0);
  SendMessageW(findEdit_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
  SetWindowLongPtrW(findEdit_, GWLP_USERDATA, (LONG_PTR)this);
  oldFindProc_ = (WNDPROC)SetWindowLongPtrW(findEdit_, GWLP_WNDPROC, (LONG_PTR)FindEditProc);
  HWND hint = CreateWindowExW(0, L"STATIC", L"Enter = weiter, Umschalt+Enter = zur\x00fc" L"ck, Esc = schlie\x00df" L"en",
                              WS_CHILD | WS_VISIBLE, 290, 7, 400, 16, findBar_, 0, inst, 0);
  SendMessageW(hint, WM_SETFONT, (WPARAM)uiFont_, TRUE);

  status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
                            hwnd_, 0, inst, 0);
  SendMessageW(status_, WM_SETFONT, (WPARAM)uiFont_, TRUE);
  RebuildBookmarkMenu();
  LayoutChildren();
}

void Browser::LayoutChildren() {
  if (!hwnd_ || !status_) return;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  int w = rc.right, h = rc.bottom;
  SendMessageW(status_, WM_SIZE, 0, 0);
  RECT sr;
  GetWindowRect(status_, &sr);
  int statusH = sr.bottom - sr.top;
  int parts[4] = {std::max(100, w - 330), std::max(150, w - 190), std::max(200, w - 130), -1};
  SendMessageW(status_, SB_SETPARTS, 4, (LPARAM)parts);

  int y = 2;
  // Tab strip: only the tab row is visible; the page view covers the body.
  RECT tr = {0, 0, w, 100};
  TabCtrl_AdjustRect(tabs_, FALSE, &tr);
  int tabRow = std::max(20, (int)tr.top - 2);
  MoveWindow(tabs_, 2, y, w - 4, tabRow + 4, TRUE);
  y += tabRow + 2;
  int tbH = 32;
  SIZE sz;
  SendMessageW(toolbar_, TB_GETMAXSIZE, 0, (LPARAM)&sz);
  int tb1W = sz.cx > 0 ? sz.cx : 120;
  SendMessageW(toolbar2_, TB_GETMAXSIZE, 0, (LPARAM)&sz);
  int tb2W = sz.cx > 0 ? sz.cx : 90;
  MoveWindow(toolbar_, 2, y, tb1W, tbH, TRUE);
  MoveWindow(toolbar2_, w - tb2W - 4, y, tb2W, tbH, TRUE);
  MoveWindow(address_, tb1W + 8, y + 4, std::max(50, w - tb1W - tb2W - 16), tbH - 8, TRUE);
  y += tbH + 2;
  int findH = findVisible_ ? 28 : 0;
  MoveWindow(view_, 0, y, w, std::max(10, h - y - statusH - findH), TRUE);
  if (findVisible_) MoveWindow(findBar_, 0, h - statusH - findH, w, findH, TRUE);
  SetWindowPos(tabs_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

int Browser::Run() {
  MSG msg;
  while (GetMessageW(&msg, 0, 0, 0) > 0) {
    // Accelerators, except while an inline form editor needs the keys.
    HWND focus = GetFocus();
    bool inEdit = focus && (focus == editCtl_ || focus == findEdit_);
    bool inAddress = focus == address_;
    if (!inEdit && accel_) {
      // Let the address bar keep Ctrl+Left/Right style editing keys.
      bool skip = inAddress && msg.message == WM_KEYDOWN && (msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT) &&
                  (GetKeyState(VK_MENU) & 0x8000) == 0;
      if (!skip && TranslateAcceleratorW(hwnd_, accel_, &msg)) continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return (int)msg.wParam;
}

// ---------------------------------------------------------------------------
// Tabs

Tab* Browser::NewTab(const std::string& url, bool activate) {
  Tab* t = new Tab(nextTabId_++);
  tabList_.push_back(t);
  TCITEMW item;
  ZeroMemory(&item, sizeof item);
  item.mask = TCIF_TEXT | TCIF_PARAM;
  std::wstring label = L"Neuer Tab";
  item.pszText = &label[0];
  item.lParam = t->id;
  TabCtrl_InsertItem(tabs_, (int)tabList_.size() - 1, &item);
  if (activate) ActivateTab(t);
  Navigate(t, url.empty() ? "about:home" : url);
  if (tabList_.size() == 1) LayoutChildren();
  return t;
}

int Browser::TabIndex(Tab* t) const {
  for (size_t i = 0; i < tabList_.size(); ++i)
    if (tabList_[i] == t) return (int)i;
  return -1;
}

Tab* Browser::TabById(int id) const {
  for (size_t i = 0; i < tabList_.size(); ++i)
    if (tabList_[i]->id == id) return tabList_[i];
  return 0;
}

void Browser::CloseTab(Tab* t) {
  int idx = TabIndex(t);
  if (idx < 0) return;
  if (editCtl_ && t == current_) CommitEdit(false);
  if (t->cancel) t->cancel->Cancel();
  KillTimer(hwnd_, kTimerStyleFallbackBase + t->id);
  KillTimer(hwnd_, kTimerRefreshBase + t->id);
  tabList_.erase(tabList_.begin() + idx);
  TabCtrl_DeleteItem(tabs_, idx);
  if (tabList_.empty()) {
    current_ = 0;
    delete t;
    DestroyWindow(hwnd_);
    return;
  }
  if (current_ == t) {
    current_ = 0;
    ActivateTab(tabList_[std::min(idx, (int)tabList_.size() - 1)]);
  }
  delete t;
}

void Browser::ActivateTab(Tab* t) {
  if (editCtl_) CommitEdit(false);
  current_ = t;
  int idx = TabIndex(t);
  if (TabCtrl_GetCurSel(tabs_) != idx) TabCtrl_SetCurSel(tabs_, idx);
  // Re-layout if images arrived or the viewport/zoom changed while the tab
  // was in the background.
  if (t->rendered && (t->needsRelayout || t->renderedW != ViewportWidth() ||
                      t->renderedH != ViewportHeight())) {
    t->needsRelayout = false;
    RenderTab(t, true);
  }
  UpdateScrollBars();
  UpdateUi();
  InvalidateRect(view_, 0, FALSE);
}

void Browser::UpdateTabLabel(Tab* t) {
  int idx = TabIndex(t);
  if (idx < 0) return;
  std::string title = t->title.empty() ? (t->displayUrl.empty() ? "Neuer Tab" : t->displayUrl) : t->title;
  std::wstring label = (t->loading ? L"\x2022 " : L"") + Widen(ShortTitle(title, 22));
  TCITEMW item;
  ZeroMemory(&item, sizeof item);
  item.mask = TCIF_TEXT;
  item.pszText = &label[0];
  TabCtrl_SetItem(tabs_, idx, &item);
  InvalidateRect(tabs_, 0, TRUE);
}

// ---------------------------------------------------------------------------
// Navigation

void Browser::Navigate(Tab* t, const std::string& input, const std::string& referrer,
                       const FormSubmission* post) {
  std::string url = FixupUserInput(input, App::Get().settings.searchUrl);
  if (url == "about:home" || url == "about:newtab" || url == "about:start") {
    PendingNav nav;
    nav.url = "about:home";
    LoadInternal(t, StartPageHtml(), "about:home", nav);
    return;
  }
  if (url == "about:kite" || url == "about:version") {
    PendingNav nav;
    nav.url = url;
    LoadInternal(t, AboutPageHtml(), url, nav);
    return;
  }
  if (url == "about:bookmarks") {
    PendingNav nav;
    nav.url = url;
    LoadInternal(t, BookmarksPageHtml(), url, nav);
    return;
  }
  if (url == "about:blank") {
    PendingNav nav;
    nav.url = url;
    LoadInternal(t, "<html><head><title></title></head><body></body></html>", url, nav);
    return;
  }
  if (StartsWithIgnoreCase(url, "javascript:")) {
    SetStatus("JavaScript wird von Kite nicht unterst\xC3\xBCtzt.");
    return;
  }
  if (StartsWithIgnoreCase(url, "mailto:") || StartsWithIgnoreCase(url, "tel:") ||
      StartsWithIgnoreCase(url, "news:") || StartsWithIgnoreCase(url, "ftp:")) {
    ShellExecuteW(hwnd_, L"open", Widen(url).c_str(), 0, 0, SW_SHOWNORMAL);
    return;
  }
  PendingNav nav;
  nav.url = url;
  if (StartsWithIgnoreCase(url, "view-source:")) {
    nav.viewSource = true;
    nav.url = url.substr(12);
  }
  if (post) {
    nav.isPost = post->method == "POST";
    nav.post = *post;
    if (!nav.isPost) nav.url = post->url;
  }
  // Same-document fragment navigation.
  Url target = Url::Parse(nav.url);
  if (!nav.isPost && !nav.viewSource && t->rendered && target.valid() && target.HasFragment()) {
    Url cur = Url::Parse(t->url);
    if (cur.valid() && cur.SpecNoFragment() == target.SpecNoFragment()) {
      if (t->historyIndex >= 0) t->history[t->historyIndex].scrollY = t->scrollY;
      HistoryEntry e;
      e.url = target.Spec();
      e.title = t->title;
      t->history.resize(t->historyIndex + 1);
      t->history.push_back(e);
      t->historyIndex = (int)t->history.size() - 1;
      t->url = t->displayUrl = target.Spec();
      float y = t->page->AnchorPosition(target.fragment());
      if (y >= 0) ScrollTo(t->scrollX, y * ZoomF());
      UpdateUi();
      return;
    }
  }
  StartLoad(t, nav, referrer);
}

void Browser::StartLoad(Tab* t, const PendingNav& nav, const std::string& referrer) {
  if (t->cancel) t->cancel->Cancel();
  KillTimer(hwnd_, kTimerRefreshBase + t->id);
  ++t->generation;
  t->cancel.reset(new CancelToken);
  t->pending = nav;
  t->loading = true;
  t->displayUrl = nav.viewSource ? "view-source:" + nav.url : nav.url;
  FetchJob* job = new FetchJob;
  job->kind = FetchJob::kDocument;
  job->tabId = t->id;
  job->generation = t->generation;
  job->notify = hwnd_;
  job->request.url = nav.url;
  job->request.cancel = t->cancel;
  job->request.referrer = referrer;
  if (nav.isPost) {
    job->request.method = "POST";
    job->request.body = nav.post.body;
    job->request.contentType = nav.post.contentType;
  }
  Url u = Url::Parse(nav.url);
  t->status = "Verbinde mit " + (u.host().empty() ? nav.url : u.host()) + " ...";
  StartFetch(job);
  UpdateTabLabel(t);
  if (t == current_) UpdateUi();
}

void Browser::LoadInternal(Tab* t, const std::string& html, const std::string& url, const PendingNav& nav) {
  if (t->cancel) t->cancel->Cancel();
  ++t->generation;
  t->pending = nav;
  t->rawSource = html;
  t->page->LoadHtml(html, url);
  CommitNavigation(t, url, false);
  t->loading = false;
  RenderTab(t, true);
  UpdateTabLabel(t);
  if (t == current_) {
    UpdateUi();
    if (url == "about:home") {
      SetWindowTextW(address_, L"");
      SetFocus(address_);
    }
  }
}

void Browser::CommitNavigation(Tab* t, const std::string& finalUrl, bool secure) {
  PendingNav& nav = t->pending;
  if (t->historyIndex >= 0 && t->historyIndex < (int)t->history.size())
    t->history[t->historyIndex].scrollY = t->scrollY;
  HistoryEntry e;
  e.url = nav.viewSource ? "view-source:" + finalUrl : finalUrl;
  e.isPost = nav.isPost;
  e.post = nav.post;
  if (nav.fromHistory && nav.historyTarget >= 0 && nav.historyTarget < (int)t->history.size()) {
    t->historyIndex = nav.historyTarget;
    t->history[t->historyIndex].url = e.url;
    t->restoreScroll = t->history[t->historyIndex].scrollY;
  } else if (nav.replace && t->historyIndex >= 0) {
    t->history[t->historyIndex] = e;
    t->restoreScroll = -1;
  } else {
    t->history.resize(t->historyIndex + 1);
    t->history.push_back(e);
    t->historyIndex = (int)t->history.size() - 1;
    t->restoreScroll = -1;
  }
  t->url = finalUrl;
  t->displayUrl = e.url;
  renderer_.ClearSvgCache();
  t->secure = secure;
  t->scrollX = t->scrollY = 0;
  t->highlights.clear();
  t->selection.clear();
  t->selectedText.clear();
  t->rendered = false;
  t->pendingImages = 0;
  t->requestedSheets.clear();
  Url u = Url::Parse(finalUrl);
  t->fragment = u.valid() && u.HasFragment() ? u.fragment() : std::string();
  t->title = t->page->Title();
  if (t == current_ && editCtl_) {
    DestroyWindow(editCtl_);
    editCtl_ = 0;
    editNode_ = 0;
  }
}

void Browser::GoHistory(Tab* t, int delta) {
  int target = t->historyIndex + delta;
  if (target < 0 || target >= (int)t->history.size()) return;
  const HistoryEntry& e = t->history[target];
  PendingNav nav;
  nav.url = e.url;
  nav.fromHistory = true;
  nav.historyTarget = target;
  nav.isPost = e.isPost;
  nav.post = e.post;
  if (StartsWith(e.url, "about:")) {
    std::string html = e.url == "about:home" ? StartPageHtml()
                       : e.url == "about:kite" ? AboutPageHtml()
                       : e.url == "about:bookmarks" ? BookmarksPageHtml()
                       : std::string("<html></html>");
    LoadInternal(t, html, e.url, nav);
    return;
  }
  if (StartsWith(e.url, "view-source:")) {
    nav.viewSource = true;
    nav.url = e.url.substr(12);
  }
  StartLoad(t, nav, std::string());
}

void Browser::Reload(Tab* t) {
  if (t->historyIndex < 0) return;
  float keep = t->scrollY;
  GoHistory(t, 0);
  if (t->historyIndex >= 0) t->history[t->historyIndex].scrollY = keep;
}

void Browser::Stop(Tab* t) {
  if (t->cancel) t->cancel->Cancel();
  t->loading = false;
  t->status = "Abgebrochen";
  if (!t->rendered && t->page->document()) RenderTab(t, true);
  UpdateTabLabel(t);
  if (t == current_) UpdateUi();
}

void Browser::OnFetched(FetchJob* job) {
  FetchFinished();
  std::unique_ptr<FetchJob> owned(job);
  if (job->kind == FetchJob::kDownload) {
    if (!job->response.ok || job->response.status >= 400) {
      std::string msg = job->response.error.empty() ? "HTTP " + IntToString(job->response.status)
                                                     : job->response.error;
      SetStatus("Download fehlgeschlagen: " + msg);
    } else {
      SaveBody(job->response, std::wstring());
    }
    return;
  }
  if (job->kind == FetchJob::kImage) {
    const std::string& url = job->request.url;
    if (job->imageOk) Images().SetLoaded(url, job->image);
    else if (job->response.ok && job->response.MimeType().find("svg") != std::string::npos)
      Images().SetUnsupported(url);
    else Images().SetFailed(url);
    for (size_t i = 0; i < tabList_.size(); ++i) {
      Tab* t = tabList_[i];
      if (t->id == job->tabId && t->generation == job->generation && t->pendingImages > 0)
        --t->pendingImages;
      if (t->rendered) t->needsRelayout = true;
    }
    ScheduleRelayout();
    Tab* t = TabById(job->tabId);
    if (t) FinishLoadingIfDone(t);
    return;
  }
  Tab* t = TabById(job->tabId);
  if (!t || job->generation != t->generation) return;
  if (job->kind == FetchJob::kDocument) {
    OnDocument(t, job);
    return;
  }
  if (job->kind == FetchJob::kStylesheet) {
    const FetchResponse& r = job->response;
    bool ok = r.ok && r.status < 400;
    std::string css = ok ? ConvertToUtf8(r.body, r.Charset()) : std::string();
    t->page->ProvideStylesheet(job->request.url, css, ok);
    --t->pendingSheets;
    RequestStylesheets(t);  // @import may add more
    if (t->pendingSheets <= 0) {
      KillTimer(hwnd_, kTimerStyleFallbackBase + t->id);
      RenderTab(t, true);
    }
  }
}

void Browser::OnDocument(Tab* t, FetchJob* job) {
  FetchResponse& r = job->response;
  PendingNav nav = t->pending;
  if (!r.ok) {
    t->loading = false;
    if (t->cancel && t->cancel->cancelled()) {
      UpdateTabLabel(t);
      if (t == current_) UpdateUi();
      return;
    }
    t->page->LoadHtml(ErrorPageHtml(nav.url, r.error), nav.url);
    t->rawSource.clear();
    CommitNavigation(t, nav.url, false);
    t->title = "Fehler";
    RenderTab(t, true);
    UpdateTabLabel(t);
    if (t == current_) UpdateUi();
    return;
  }
  std::string mime = r.MimeType();
  std::string disposition = AsciiLower(r.Header("content-disposition"));
  bool attachment = StartsWith(disposition, "attachment");
  if (!nav.viewSource && (attachment || (!IsHtmlMime(mime) && !IsTextMime(mime) && !IsImageMime(mime)))) {
    // Not displayable: offer to save the file.
    t->loading = false;
    UpdateTabLabel(t);
    std::wstring name = Widen(r.FileName());
    wchar_t path[MAX_PATH];
    lstrcpynW(path, name.c_str(), MAX_PATH);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Alle Dateien (*.*)\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Download speichern unter";
    if (GetSaveFileNameW(&ofn)) SaveBody(r, path);
    if (!t->rendered && t->history.empty()) Navigate(t, "about:home");
    if (t == current_) UpdateUi();
    return;
  }
  std::string finalUrl = r.finalUrl;
  if (nav.viewSource) {
    std::string text = ConvertToUtf8(r.body, r.Charset().empty() ? SniffHtmlCharset(r.body) : r.Charset());
    t->page->LoadPlainText(text, "view-source:" + finalUrl);
    t->rawSource = r.body;
  } else if (IsImageMime(mime)) {
    DecodedImage img;
    if (DecodeImage(r.body, img)) Images().SetLoaded(finalUrl, img);
    else if (LooksLikeSvg(r.body) && RenderSvgDocument(r.body, 0, 0, 1.0f, img))
      Images().SetLoaded(finalUrl, img);
    else Images().SetFailed(finalUrl);
    t->page->LoadImageDocument(finalUrl);
    t->rawSource = r.body;
  } else if (IsHtmlMime(mime)) {
    std::string charset = r.Charset();
    if (charset.empty()) charset = SniffHtmlCharset(r.body);
    t->rawSource = r.body;
    if (r.status >= 400 && Trim(r.body).empty()) {
      t->page->LoadHtml(ErrorPageHtml(finalUrl, "Der Server meldet HTTP-Status " + IntToString(r.status) + "."),
                        finalUrl);
    } else {
      t->page->LoadHtml(ConvertToUtf8(r.body, charset), finalUrl);
    }
  } else {
    t->page->LoadPlainText(ConvertToUtf8(r.body, r.Charset()), finalUrl);
    t->rawSource = r.body;
  }
  CommitNavigation(t, finalUrl, r.secure);
  globalHistory_.push_back(std::make_pair(finalUrl, std::string()));
  if (globalHistory_.size() > 200) globalHistory_.erase(globalHistory_.begin());
  t->status = "Lade Stylesheets ...";
  t->pendingSheets = 0;
  RequestStylesheets(t);
  if (t->pendingSheets > 0) {
    // Render anyway if stylesheets take too long.
    SetTimer(hwnd_, kTimerStyleFallbackBase + t->id, 3000, 0);
  } else {
    RenderTab(t, true);
  }
  UpdateTabLabel(t);
  if (t == current_) UpdateUi();
}

void Browser::RequestStylesheets(Tab* t) {
  std::vector<std::string> pending = t->page->PendingStylesheets();
  for (size_t i = 0; i < pending.size(); ++i) {
    if (t->requestedSheets.count(pending[i])) continue;
    t->requestedSheets.insert(pending[i]);
    FetchJob* job = new FetchJob;
    job->kind = FetchJob::kStylesheet;
    job->tabId = t->id;
    job->generation = t->generation;
    job->notify = hwnd_;
    job->request.url = pending[i];
    job->request.cancel = t->cancel;
    job->request.referrer = t->url;
    job->request.accept = "text/css,*/*;q=0.1";
    ++t->pendingSheets;
    StartFetch(job);
  }
}

void Browser::RenderTab(Tab* t, bool restyle) {
  if (!t->page->document()) return;
  float vw = ViewportWidth(), vh = ViewportHeight();
  if (restyle) t->page->Restyle(vw, vh);
  t->page->Relayout(vw, vh);
  t->renderedW = vw;
  t->renderedH = vh;
  bool first = !t->rendered;
  t->rendered = true;
  t->needsRelayout = false;
  t->title = t->page->Title();
  if (!globalHistory_.empty() && globalHistory_.back().first == t->url)
    globalHistory_.back().second = t->title;
  if (t->historyIndex >= 0 && t->historyIndex < (int)t->history.size())
    t->history[t->historyIndex].title = t->title;
  if (first) {
    if (t->restoreScroll > 0) {
      t->scrollY = t->restoreScroll;
    } else if (!t->fragment.empty()) {
      float y = t->page->AnchorPosition(t->fragment);
      if (y >= 0) t->scrollY = y * ZoomF();
    }
    int delay = t->page->refreshDelay();
    if (delay >= 0 && delay <= 30 && !t->page->refreshUrl().empty())
      SetTimer(hwnd_, kTimerRefreshBase + t->id, std::max(delay, 1) * 1000, 0);
  }
  RequestImages(t);
  FinishLoadingIfDone(t);
  UpdateTabLabel(t);
  if (t == current_) {
    UpdateScrollBars();
    InvalidateRect(view_, 0, FALSE);
    UpdateUi();
  }
}

void Browser::RequestImages(Tab* t) {
  if (!App::Get().settings.loadImages) return;
  std::vector<std::string> urls = t->page->ReferencedImages();
  for (size_t i = 0; i < urls.size(); ++i) {
    const std::string& u = urls[i];
    if (Images().IsKnown(u)) continue;

    if (!StartsWith(u, "http") && !StartsWith(u, "data:") && !StartsWith(u, "file:")) {
      Images().SetFailed(u);
      continue;
    }
    Images().SetLoading(u);
    FetchJob* job = new FetchJob;
    job->kind = FetchJob::kImage;
    job->tabId = t->id;
    job->generation = t->generation;
    job->notify = hwnd_;
    job->request.url = u;
    job->request.referrer = t->url;
    job->request.accept = "image/png,image/jpeg,image/gif,image/bmp,*/*;q=0.5";
    job->request.maxBytes = 16 * 1024 * 1024;
    ++t->pendingImages;
    StartFetch(job);
  }
}

void Browser::FinishLoadingIfDone(Tab* t) {
  if (t->pendingSheets <= 0 && t->pendingImages <= 0 && t->rendered && t->loading) {
    t->loading = false;
    t->status = "Fertig";
    UpdateTabLabel(t);
  } else if (t->loading && t->rendered) {
    t->status = "Lade Bilder ... (" + IntToString(t->pendingImages) + " ausstehend)";
  }
  if (t == current_) UpdateUi();
}

void Browser::ScheduleRelayout() {
  if (relayoutPending_) return;
  relayoutPending_ = true;
  SetTimer(hwnd_, kTimerRelayout, 250, 0);
}

void Browser::SubmitForm(Tab* t, Node* form, Node* submitter, bool newTab) {
  FormSubmission sub;
  if (!t->page->BuildFormSubmission(form, submitter, sub)) return;
  std::string target = AsciiLower(form->Attr("target"));
  if (newTab || target == "_blank") {
    Tab* nt = NewTab("about:blank", true);
    Navigate(nt, sub.url, t->url, &sub);
    return;
  }
  Navigate(t, sub.url, t->url, &sub);
}

void Browser::SaveBody(const FetchResponse& r, const std::wstring& path) {
  if (path.empty()) {
    std::wstring name = Widen(r.FileName());
    wchar_t buf[MAX_PATH];
    lstrcpynW(buf, name.c_str(), MAX_PATH);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Alle Dateien (*.*)\0*.*\0";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    SaveBody(r, buf);
    return;
  }
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (f == INVALID_HANDLE_VALUE) {
    SetStatus("Datei konnte nicht gespeichert werden.");
    return;
  }
  DWORD written = 0;
  WriteFile(f, r.body.data(), (DWORD)r.body.size(), &written, 0);
  CloseHandle(f);
  SetStatus("Gespeichert: " + Narrow(path) + " (" + IntToString((long long)r.body.size() / 1024) + " KB)");
}

void Browser::Download(const std::string& url, const std::string& suggestedName, const std::string& referrer) {
  FetchJob* job = new FetchJob;
  job->kind = FetchJob::kDownload;
  job->notify = hwnd_;
  job->request.url = url;
  job->request.referrer = referrer;
  (void)suggestedName;
  SetStatus("Lade herunter: " + url);
  StartFetch(job);
}

// ---------------------------------------------------------------------------
// View

float Browser::ViewportWidth() const {
  RECT rc;
  GetClientRect(view_, &rc);
  return std::max(50.0f, rc.right / ZoomF());
}

float Browser::ViewportHeight() const {
  RECT rc;
  GetClientRect(view_, &rc);
  return std::max(50.0f, rc.bottom / ZoomF());
}

void Browser::UpdateScrollBars() {
  if (!current_) return;
  RECT rc;
  GetClientRect(view_, &rc);
  float z = ZoomF();
  int docH = (int)(current_->page->ContentHeight() * z) + 16;
  int docW = (int)(current_->page->ContentWidth() * z);
  float maxY = std::max(0.0f, (float)docH - rc.bottom);
  float maxX = std::max(0.0f, (float)docW - rc.right);
  current_->scrollY = std::max(0.0f, std::min(current_->scrollY, maxY));
  current_->scrollX = std::max(0.0f, std::min(current_->scrollX, maxX));
  SCROLLINFO si;
  ZeroMemory(&si, sizeof si);
  si.cbSize = sizeof si;
  si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
  si.nMin = 0;
  si.nMax = std::max(docH - 1, 0);
  si.nPage = rc.bottom;
  si.nPos = (int)current_->scrollY;
  SetScrollInfo(view_, SB_VERT, &si, TRUE);
  si.fMask = SIF_ALL;
  si.nMax = docW > rc.right ? docW - 1 : 0;
  si.nPage = docW > rc.right ? rc.right : 0;
  si.nPos = (int)current_->scrollX;
  SetScrollInfo(view_, SB_HORZ, &si, TRUE);
  ShowScrollBar(view_, SB_HORZ, docW > rc.right + 2);
}

void Browser::ScrollTo(float x, float y) {
  if (!current_) return;
  if (editCtl_) CommitEdit(false);
  current_->scrollX = x;
  current_->scrollY = y;
  UpdateScrollBars();
  InvalidateRect(view_, 0, FALSE);
}

void Browser::ScrollBy(float dx, float dy) {
  if (!current_) return;
  ScrollTo(current_->scrollX + dx, current_->scrollY + dy);
}

Node* Browser::NodeAt(int x, int y) {
  if (!current_ || !current_->rendered) return 0;
  float z = ZoomF();
  return current_->page->HitTest((x + current_->scrollX) / z, (y + current_->scrollY) / z);
}

void Browser::SetStatus(const std::string& s) {
  SendMessageW(status_, SB_SETTEXTW, 0, (LPARAM)Widen(s).c_str());
}

void Browser::UpdateUi() {
  if (!current_) return;
  Tab* t = current_;
  if (GetFocus() != address_) SetWindowTextW(address_, Widen(t->displayUrl == "about:home" ? "" : t->displayUrl).c_str());
  InvalidateRect(address_, 0, TRUE);
  std::string title = t->title.empty() ? t->displayUrl : t->title;
  SetWindowTextW(hwnd_, Widen(title.empty() ? "Kite" : title + " - Kite").c_str());
  SendMessageW(toolbar_, TB_ENABLEBUTTON, ID_GO_BACK, MAKELONG(t->historyIndex > 0, 0));
  SendMessageW(toolbar_, TB_ENABLEBUTTON, ID_GO_FORWARD,
               MAKELONG(t->historyIndex + 1 < (int)t->history.size(), 0));
  // Reload button doubles as stop button while loading.
  TBBUTTONINFOW bi;
  ZeroMemory(&bi, sizeof bi);
  bi.cbSize = sizeof bi;
  bi.dwMask = TBIF_IMAGE;
  bi.iImage = t->loading ? kIconStop : kIconReload;
  SendMessageW(toolbar_, TB_SETBUTTONINFOW, ID_VIEW_RELOAD, (LPARAM)&bi);
  SetStatus(t->status);
  std::wstring zoomText = Widen(IntToString(zoom_) + " %");
  SendMessageW(status_, SB_SETTEXTW, 2, (LPARAM)zoomText.c_str());
  std::string sec = StartsWith(t->url, "https:") ? (t->secure ? "Sicher (TLS)" : "") :
                    StartsWith(t->url, "http:") ? "Nicht sicher" : "";
  SendMessageW(status_, SB_SETTEXTW, 3, (LPARAM)Widen(sec).c_str());
  std::string prog = t->loading ? "Laden ..." : "";
  SendMessageW(status_, SB_SETTEXTW, 1, (LPARAM)Widen(prog).c_str());
}

void Browser::SetZoom(int z) {
  zoom_ = z;
  if (current_) {
    float ratio = 1;
    RenderTab(current_, true);
    (void)ratio;
  }
  for (size_t i = 0; i < tabList_.size(); ++i)
    if (tabList_[i] != current_) tabList_[i]->needsRelayout = true;
  UpdateUi();
}

void Browser::ToggleControl(Node* n) {
  std::string type = AsciiLower(n->Attr("type"));
  bool checked = n->checkedSet ? n->checked : n->HasAttr("checked");
  if (type == "radio") {
    if (checked) return;
    Node* form = Page::FormFor(n);
    Node* scope = form ? form : current_->page->document()->root.get();
    std::vector<Node*> inputs;
    scope->FindAll("input", inputs);
    for (size_t i = 0; i < inputs.size(); ++i)
      if (AsciiLower(inputs[i]->Attr("type")) == "radio" && inputs[i]->Attr("name") == n->Attr("name")) {
        inputs[i]->checked = false;
        inputs[i]->checkedSet = true;
      }
    n->checked = true;
  } else {
    n->checked = !checked;
  }
  n->checkedSet = true;
  current_->page->Repaint();
  InvalidateRect(view_, 0, FALSE);
}

void Browser::OnClick(int x, int y, bool newTab) {
  Tab* t = current_;
  Node* n = NodeAt(x, y);
  if (!n) return;
  // Form controls.
  for (Node* p = n; p; p = p->parent) {
    if (!p->IsElement()) continue;
    const std::string& tag = p->tag;
    if (p->HasAttr("disabled") && (tag == "input" || tag == "button" || tag == "select" || tag == "textarea"))
      return;
    if (tag == "input") {
      std::string type = AsciiLower(p->Attr("type"));
      if (type == "checkbox" || type == "radio") {
        ToggleControl(p);
      } else if (type == "submit" || type == "image") {
        SubmitForm(t, Page::FormFor(p), p, newTab);
      } else if (type == "reset") {
        Node* form = Page::FormFor(p);
        if (form) {
          std::vector<Node*> all;
          form->FindAll("input", all);
          form->FindAll("textarea", all);
          form->FindAll("select", all);
          for (size_t i = 0; i < all.size(); ++i) {
            all[i]->formValueSet = false;
            all[i]->checkedSet = false;
            all[i]->selectedIndex = -1;
          }
          t->page->Repaint();
          InvalidateRect(view_, 0, FALSE);
        }
      } else if (type == "button" || type == "file" || type == "range" || type == "color") {
        SetStatus("Diese Funktion ben\xC3\xB6tigt JavaScript bzw. wird nicht unterst\xC3\xBCtzt.");
      } else {
        BeginEdit(p);
      }
      return;
    }
    if (tag == "textarea") {
      BeginEdit(p);
      return;
    }
    if (tag == "select") {
      ShowSelectPopup(p);
      return;
    }
    if (tag == "button") {
      std::string type = AsciiLower(p->Attr("type"));
      if (type.empty() || type == "submit") {
        Node* form = Page::FormFor(p);
        if (form) {
          SubmitForm(t, form, p, newTab);
          return;
        }
      }
      // Buttons inside links behave like the link.
      if (!Page::LinkFor(p)) {
        SetStatus("Diese Schaltfl\xC3\xA4" "che ben\xC3\xB6tigt JavaScript.");
        return;
      }
      break;
    }
    if (tag == "label") {
      Node* target = 0;
      if (p->HasAttr("for")) target = t->page->document()->root->FindById(p->Attr("for"));
      if (!target) target = p->FindFirst("input");
      if (target && target != n) {
        std::string type = AsciiLower(target->Attr("type"));
        if (type == "checkbox" || type == "radio") ToggleControl(target);
        else if (target->tag == "input" || target->tag == "textarea") BeginEdit(target);
        return;
      }
    }
    if (tag == "summary" && p->parent && p->parent->Is("details")) {
      Node* d = p->parent;
      if (d->HasAttr("open")) {
        for (size_t i = 0; i < d->attrs.size(); ++i)
          if (d->attrs[i].name == "open") {
            d->attrs.erase(d->attrs.begin() + i);
            break;
          }
      } else {
        d->SetAttr("open", "");
      }
      RenderTab(t, true);
      return;
    }
    if (tag == "a" || tag == "area") break;
  }
  Node* link = Page::LinkFor(n);
  if (link) {
    std::string url = t->page->LinkUrl(link);
    if (url.empty()) return;
    if (link->HasAttr("download")) {
      Download(url, link->Attr("download"), t->url);
      return;
    }
    std::string target = AsciiLower(link->Attr("target"));
    if (newTab || target == "_blank") {
      Tab* created = NewTab("about:blank", !newTab);
      Navigate(created, url, t->url);
      if (newTab) ActivateTab(t);
    } else {
      Navigate(t, url, t->url);
    }
    return;
  }
  for (Node* p = n; p; p = p->parent) {
    if (p->Is("iframe") && p->HasAttr("src")) {
      Url u = t->page->ResolveUrl(p->Attr("src"));
      if (u.valid()) Navigate(t, u.Spec(), t->url);
      return;
    }
  }
}

LRESULT CALLBACK Browser::EditCtlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Browser* b = g_browser;
  if (m == WM_KEYDOWN) {
    bool multi = (GetWindowLongW(h, GWL_STYLE) & ES_MULTILINE) != 0;
    if (w == VK_RETURN && !multi) {
      b->CommitEdit(true);
      return 0;
    }
    if (w == VK_ESCAPE) {
      b->CommitEdit(false);
      SetFocus(b->view_);
      return 0;
    }
    if (w == VK_TAB) {
      b->CommitEdit(false);
      SetFocus(b->view_);
      return 0;
    }
  }
  if (m == WM_CHAR && (w == VK_RETURN || w == VK_TAB || w == VK_ESCAPE)) {
    bool multi = (GetWindowLongW(h, GWL_STYLE) & ES_MULTILINE) != 0;
    if (!multi || w != VK_RETURN) return 0;
  }
  if (m == WM_KILLFOCUS) {
    LRESULT r = CallWindowProcW(b->oldEditProc_, h, m, w, l);
    PostMessageW(b->hwnd_, WM_APP + 2, 0, (LPARAM)h);
    return r;
  }
  return CallWindowProcW(b->oldEditProc_, h, m, w, l);
}

void Browser::BeginEdit(Node* n) {
  if (editCtl_) CommitEdit(false);
  Rect r;
  if (!current_->page->BoxRect(n, r)) return;
  LayoutBox* box = n->layoutBox;
  float z = ZoomF();
  float cx = r.x, cy = r.y, cw = r.w, ch = r.h;
  if (box) {
    cx += box->border.left + box->padding.left;
    cy += box->border.top + box->padding.top;
    cw = box->ContentW();
    ch = box->ContentH();
  }
  int x = (int)(cx * z - current_->scrollX), y = (int)(cy * z - current_->scrollY);
  int w = std::max(20, (int)(cw * z)), h = std::max(14, (int)(ch * z));
  bool multi = n->tag == "textarea";
  std::string type = AsciiLower(n->Attr("type"));
  DWORD style = WS_CHILD | WS_VISIBLE | (multi ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL
                                               : ES_AUTOHSCROLL);
  if (type == "password") style |= ES_PASSWORD;
  if (type == "number" || type == "tel") style |= 0;
  editCtl_ = CreateWindowExW(0, L"EDIT", L"", style, x, y, w, h, view_, 0, App::Get().instance, 0);
  if (!editCtl_) return;
  editNode_ = n;
  FontDesc fd = n->style ? FontDesc::FromStyle(*n->style) : FontDesc();
  SendMessageW(editCtl_, WM_SETFONT, (WPARAM)Fonts().Get(fd, z), FALSE);
  std::string value = n->formValueSet ? n->formValue : (multi ? n->TextContent() : n->Attr("value"));
  if (multi) value = ReplaceAll(ReplaceAll(value, "\r\n", "\n"), "\n", "\r\n");
  SetWindowTextW(editCtl_, Widen(value).c_str());
  long long maxLen;
  if (ParseInt(n->Attr("maxlength"), maxLen) && maxLen > 0) SendMessageW(editCtl_, EM_LIMITTEXT, (WPARAM)maxLen, 0);
  oldEditProc_ = (WNDPROC)SetWindowLongPtrW(editCtl_, GWLP_WNDPROC, (LONG_PTR)EditCtlProc);
  SetFocus(editCtl_);
  SendMessageW(editCtl_, EM_SETSEL, 0, -1);
  // Hide the painted value under the edit control.
  n->formValue = value;
  n->formValueSet = true;
}

void Browser::CommitEdit(bool submit) {
  if (!editCtl_) return;
  HWND ctl = editCtl_;
  Node* n = editNode_;
  editCtl_ = 0;
  editNode_ = 0;
  if (n && current_) {
    std::string v = Narrow(WindowTextW(ctl));
    if (n->tag == "textarea") v = ReplaceAll(v, "\r\n", "\n");
    n->formValue = v;
    n->formValueSet = true;
  }
  DestroyWindow(ctl);
  if (!current_) return;
  current_->page->Repaint();
  InvalidateRect(view_, 0, FALSE);
  if (submit && n) {
    Node* form = Page::FormFor(n);
    if (form) SubmitForm(current_, form, Page::DefaultSubmitButton(form), false);
  }
}

void Browser::ShowSelectPopup(Node* sel) {
  std::vector<Node*> opts;
  sel->FindAll("option", opts);
  if (opts.empty()) return;
  HMENU menu = CreatePopupMenu();
  int current = sel->selectedIndex;
  if (current < 0)
    for (size_t i = 0; i < opts.size(); ++i)
      if (opts[i]->HasAttr("selected")) current = (int)i;
  if (current < 0) current = 0;
  for (size_t i = 0; i < opts.size() && i < 1000; ++i) {
    std::string label = CollapseWhitespace(opts[i]->TextContent());
    UINT flags = MF_STRING | ((int)i == current ? MF_CHECKED : 0) |
                 (opts[i]->HasAttr("disabled") ? MF_GRAYED : 0);
    if (i > 0 && i % 30 == 0) flags |= MF_MENUBARBREAK;
    AppendMenuW(menu, flags, ID_SELECT_BASE + i, Widen(label.empty() ? " " : label).c_str());
  }
  Rect r;
  POINT pt = {0, 0};
  if (current_->page->BoxRect(sel, r)) {
    pt.x = (LONG)(r.x * ZoomF() - current_->scrollX);
    pt.y = (LONG)(r.bottom() * ZoomF() - current_->scrollY);
  }
  ClientToScreen(view_, &pt);
  int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, 0);
  DestroyMenu(menu);
  if (cmd >= ID_SELECT_BASE && cmd < ID_SELECT_BASE + (int)opts.size()) {
    sel->selectedIndex = cmd - ID_SELECT_BASE;
    current_->page->Repaint();
    InvalidateRect(view_, 0, FALSE);
  }
}

void Browser::CopyText(const std::string& text) {
  if (!OpenClipboard(hwnd_)) return;
  EmptyClipboard();
  std::wstring w = Widen(ReplaceAll(text, "\n", "\r\n"));
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
  if (mem) {
    wchar_t* p = (wchar_t*)GlobalLock(mem);
    memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(mem);
    SetClipboardData(CF_UNICODETEXT, mem);
  }
  CloseClipboard();
}

void Browser::UpdateSelection(int x0, int y0, int x1, int y1) {
  Tab* t = current_;
  float z = ZoomF();
  float ax = (x0 + t->scrollX) / z, ay = (y0 + t->scrollY) / z;
  float bx = (x1 + t->scrollX) / z, by = (y1 + t->scrollY) / z;
  if (ay > by || (std::fabs(ay - by) < 1 && ax > bx)) {
    std::swap(ax, bx);
    std::swap(ay, by);
  }
  t->selection.clear();
  t->selectedText.clear();
  const DisplayList& dl = t->page->display();
  float lastBaseline = -1;
  for (size_t i = 0; i < dl.items.size(); ++i) {
    const DisplayItem& it = dl.items[i];
    if (it.type != DisplayItem::kText) continue;
    float top = it.baseline - it.font.size, bottom = it.baseline + it.font.size * 0.3f;
    if (bottom < ay || top > by) continue;
    bool firstLine = top <= ay && ay <= bottom;
    bool lastLine = top <= by && by <= bottom;
    if (firstLine && lastLine && it.rect.right() < std::min(ax, bx)) continue;
    if (firstLine && !lastLine && it.rect.right() < ax) continue;
    if (lastLine && !firstLine && it.rect.x > bx) continue;
    if (firstLine && lastLine && it.rect.x > std::max(ax, bx)) continue;
    t->selection.push_back(Rect(it.rect.x, it.baseline - it.font.size * 0.95f, it.rect.w, it.font.size * 1.25f));
    if (lastBaseline >= 0) t->selectedText += std::fabs(it.baseline - lastBaseline) > 2 ? "\n" : "";
    t->selectedText += it.text;
    lastBaseline = it.baseline;
  }
  InvalidateRect(view_, 0, FALSE);
}

void Browser::Find(bool forward, bool fromStart) {
  Tab* t = current_;
  if (!t) return;
  std::string needle = AsciiLower(Narrow(WindowTextW(findEdit_)));
  t->highlights.clear();
  if (needle.empty()) {
    InvalidateRect(view_, 0, FALSE);
    return;
  }
  const DisplayList& dl = t->page->display();
  std::vector<Rect> hits;
  for (size_t i = 0; i < dl.items.size(); ++i) {
    const DisplayItem& it = dl.items[i];
    if (it.type != DisplayItem::kText) continue;
    std::string hay = AsciiLower(it.text);
    size_t p = 0;
    while ((p = hay.find(needle, p)) != std::string::npos) {
      float x0 = it.rect.x + Fonts().MeasureText(it.font, it.text.substr(0, p));
      float w = Fonts().MeasureText(it.font, it.text.substr(p, needle.size()));
      hits.push_back(Rect(x0, it.baseline - it.font.size * 0.95f, w, it.font.size * 1.25f));
      p += needle.size();
    }
  }
  if (hits.empty()) {
    SetStatus("\"" + Narrow(WindowTextW(findEdit_)) + "\" wurde nicht gefunden.");
    t->findIndex = -1;
    InvalidateRect(view_, 0, FALSE);
    return;
  }
  if (fromStart) t->findIndex = forward ? 0 : (int)hits.size() - 1;
  else t->findIndex = (t->findIndex + (forward ? 1 : -1) + (int)hits.size()) % (int)hits.size();
  if (t->findIndex < 0 || t->findIndex >= (int)hits.size()) t->findIndex = 0;
  // Current match first (drawn stronger).
  t->highlights.push_back(hits[t->findIndex]);
  for (size_t i = 0; i < hits.size(); ++i)
    if ((int)i != t->findIndex) t->highlights.push_back(hits[i]);
  SetStatus("Treffer " + IntToString(t->findIndex + 1) + " von " + IntToString((long long)hits.size()));
  RECT rc;
  GetClientRect(view_, &rc);
  float z = ZoomF();
  const Rect& h = hits[t->findIndex];
  if (h.y * z < t->scrollY || h.bottom() * z > t->scrollY + rc.bottom)
    ScrollTo(t->scrollX, std::max(0.0f, h.y * z - rc.bottom / 3.0f));
  else
    InvalidateRect(view_, 0, FALSE);
}

void Browser::ShowFindBar(bool show) {
  findVisible_ = show;
  ShowWindow(findBar_, show ? SW_SHOW : SW_HIDE);
  LayoutChildren();
  if (show) {
    SetFocus(findEdit_);
    SendMessageW(findEdit_, EM_SETSEL, 0, -1);
  } else {
    if (current_) {
      current_->highlights.clear();
      InvalidateRect(view_, 0, FALSE);
    }
    SetFocus(view_);
  }
}

void Browser::ShowContextMenu(int x, int y) {
  Tab* t = current_;
  Node* n = NodeAt(x, y);
  ctxLink_.clear();
  ctxImage_.clear();
  Node* link = Page::LinkFor(n);
  if (link) ctxLink_ = t->page->LinkUrl(link);
  if (n && n->Is("img") && n->layoutBox && !n->layoutBox->imageUrl.empty()) ctxImage_ = n->layoutBox->imageUrl;
  HMENU menu = CreatePopupMenu();
  if (!ctxLink_.empty()) {
    AppendMenuW(menu, MF_STRING, ID_CTX_OPENTAB, L"Link in neuem &Tab \x00f6" L"ffnen");
    AppendMenuW(menu, MF_STRING, ID_CTX_SAVELINK, L"Ziel speichern &unter...");
    AppendMenuW(menu, MF_STRING, ID_CTX_COPYLINK, L"Link-Adresse &kopieren");
    AppendMenuW(menu, MF_SEPARATOR, 0, 0);
  }
  if (!ctxImage_.empty()) {
    AppendMenuW(menu, MF_STRING, ID_CTX_OPENIMAGE, L"&Bild in neuem Tab \x00f6" L"ffnen");
    AppendMenuW(menu, MF_STRING, ID_CTX_SAVEIMAGE, L"Bild speichern unter...");
    AppendMenuW(menu, MF_SEPARATOR, 0, 0);
  }
  if (!t->selectedText.empty()) {
    AppendMenuW(menu, MF_STRING, ID_EDIT_COPY, L"&Kopieren");
    AppendMenuW(menu, MF_SEPARATOR, 0, 0);
  }
  AppendMenuW(menu, MF_STRING | (t->historyIndex > 0 ? 0 : MF_GRAYED), ID_GO_BACK, L"&Zur\x00fc" L"ck");
  AppendMenuW(menu, MF_STRING | (t->historyIndex + 1 < (int)t->history.size() ? 0 : MF_GRAYED), ID_GO_FORWARD,
              L"&Vor");
  AppendMenuW(menu, MF_STRING, ID_VIEW_RELOAD, L"&Neu laden");
  AppendMenuW(menu, MF_SEPARATOR, 0, 0);
  AppendMenuW(menu, MF_STRING, ID_EDIT_SELECTALL, L"Alles &markieren");
  AppendMenuW(menu, MF_STRING, ID_BM_ADD, L"&Lesezeichen hinzuf\x00fcgen");
  AppendMenuW(menu, MF_STRING, ID_VIEW_SOURCE, L"Seiten&quelltext anzeigen");
  POINT pt = {x, y};
  ClientToScreen(view_, &pt);
  TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, 0);
  DestroyMenu(menu);
}

void Browser::AddBookmark() {
  Tab* t = current_;
  if (!t || t->url.empty() || StartsWith(t->url, "about:")) return;
  std::vector<std::pair<std::string, std::string> >& bm = App::Get().bookmarks;
  for (size_t i = 0; i < bm.size(); ++i)
    if (bm[i].second == t->url) {
      SetStatus("Diese Seite ist bereits ein Lesezeichen.");
      return;
    }
  bm.push_back(std::make_pair(t->title, t->url));
  App::Get().SaveBookmarks();
  RebuildBookmarkMenu();
  SetStatus("Lesezeichen hinzugef\xC3\xBCgt: " + (t->title.empty() ? t->url : t->title));
}

// ---------------------------------------------------------------------------
// Commands

void Browser::OnCommand(int id) {
  Tab* t = current_;
  if (id >= ID_BOOKMARK_BASE && id < ID_BOOKMARK_BASE + 1000) {
    size_t i = id - ID_BOOKMARK_BASE;
    if (t && i < App::Get().bookmarks.size()) Navigate(t, App::Get().bookmarks[i].second);
    return;
  }
  if (id >= ID_HISTORY_BASE && id < ID_HISTORY_BASE + 100) {
    size_t i = id - ID_HISTORY_BASE;
    if (t && i < globalHistory_.size()) Navigate(t, globalHistory_[globalHistory_.size() - 1 - i].first);
    return;
  }
  switch (id) {
    case ID_FILE_NEWTAB:
      NewTab("about:home", true);
      break;
    case ID_FILE_CLOSETAB:
      if (t) CloseTab(t);
      break;
    case ID_FILE_OPEN: {
      wchar_t path[MAX_PATH] = L"";
      OPENFILENAMEW ofn;
      ZeroMemory(&ofn, sizeof ofn);
      ofn.lStructSize = sizeof ofn;
      ofn.hwndOwner = hwnd_;
      ofn.lpstrFile = path;
      ofn.nMaxFile = MAX_PATH;
      ofn.lpstrFilter = L"Webseiten (*.htm;*.html)\0*.htm;*.html\0Bilder\0*.png;*.jpg;*.jpeg;*.gif;*.bmp\0Textdateien (*.txt)\0*.txt\0Alle Dateien (*.*)\0*.*\0";
      ofn.Flags = OFN_FILEMUSTEXIST;
      if (GetOpenFileNameW(&ofn) && t) Navigate(t, Url::FromLocalPath(Narrow(path)).Spec());
      break;
    }
    case ID_FILE_SAVE: {
      if (!t || t->rawSource.empty()) break;
      FetchResponse r;
      r.body = t->rawSource;
      r.finalUrl = t->url;
      std::string name = (t->title.empty() ? std::string("seite") : t->title);
      for (size_t i = 0; i < name.size(); ++i)
        if (strchr("\\/:*?\"<>|", name[i])) name[i] = '_';
      r.headers.push_back(std::make_pair(std::string("Content-Disposition"),
                                         "attachment; filename=\"" + name + ".html\""));
      SaveBody(r, std::wstring());
      break;
    }
    case ID_FILE_EXIT:
      DestroyWindow(hwnd_);
      break;
    case ID_EDIT_COPY:
      if (GetFocus() == address_) SendMessageW(address_, WM_COPY, 0, 0);
      else if (t && !t->selectedText.empty()) CopyText(t->selectedText);
      break;
    case ID_EDIT_SELECTALL:
      if (GetFocus() == address_) SendMessageW(address_, EM_SETSEL, 0, -1);
      else if (t) {
        RECT rc;
        GetClientRect(view_, &rc);
        UpdateSelection(0, (int)-t->scrollY, rc.right, (int)(t->page->ContentHeight() * ZoomF() - t->scrollY));
      }
      break;
    case ID_EDIT_FIND:
      ShowFindBar(true);
      break;
    case ID_EDIT_FINDNEXT:
      if (!findVisible_) ShowFindBar(true);
      else Find(true, false);
      break;
    case ID_EDIT_FINDPREV:
      Find(false, false);
      break;
    case ID_FIND_CLOSE:
      ShowFindBar(false);
      break;
    case ID_VIEW_RELOAD:
      if (t) {
        if (t->loading) Stop(t);
        else Reload(t);
      }
      break;
    case ID_VIEW_STOP:
      if (t) Stop(t);
      break;
    case ID_VIEW_ZOOMIN:
    case ID_VIEW_ZOOMOUT: {
      int n = sizeof kZoomLevels / sizeof kZoomLevels[0];
      int idx = 0;
      for (int i = 0; i < n; ++i)
        if (kZoomLevels[i] <= zoom_) idx = i;
      idx += id == ID_VIEW_ZOOMIN ? 1 : (kZoomLevels[idx] == zoom_ ? -1 : 0);
      idx = std::max(0, std::min(n - 1, idx));
      SetZoom(kZoomLevels[idx]);
      break;
    }
    case ID_VIEW_ZOOMRESET:
      SetZoom(100);
      break;
    case ID_VIEW_SOURCE:
      if (t && !t->url.empty() && !StartsWith(t->url, "view-source:")) {
        if (StartsWith(t->url, "about:")) {
          Tab* nt = NewTab("about:blank", true);
          nt->page->LoadPlainText(t->rawSource, "view-source:" + t->url);
          PendingNav nav;
          nav.url = "view-source:" + t->url;
          nt->pending = nav;
          CommitNavigation(nt, "view-source:" + t->url, false);
          RenderTab(nt, true);
        } else {
          Tab* nt = NewTab("about:blank", true);
          Navigate(nt, "view-source:" + t->url);
        }
      }
      break;
    case ID_GO_BACK:
      if (t) GoHistory(t, -1);
      break;
    case ID_GO_FORWARD:
      if (t) GoHistory(t, 1);
      break;
    case ID_GO_HOME:
      if (t) Navigate(t, App::Get().settings.homePage);
      break;
    case ID_GO_ADDRESS:
      SetFocus(address_);
      SendMessageW(address_, EM_SETSEL, 0, -1);
      break;
    case ID_GO: {
      std::string text = Narrow(WindowTextW(address_));
      if (t && !Trim(text).empty()) {
        Navigate(t, text);
        SetFocus(view_);
      }
      break;
    }
    case ID_GO_NEXTTAB:
    case ID_GO_PREVTAB: {
      if (tabList_.size() < 2 || !t) break;
      int idx = TabIndex(t) + (id == ID_GO_NEXTTAB ? 1 : -1);
      idx = (idx + (int)tabList_.size()) % (int)tabList_.size();
      ActivateTab(tabList_[idx]);
      break;
    }
    case ID_BM_ADD:
      AddBookmark();
      break;
    case ID_BM_MANAGE:
      if (t) Navigate(t, "about:bookmarks");
      break;
    case ID_BM_REMOVE: {
      std::vector<std::pair<std::string, std::string> >& bm = App::Get().bookmarks;
      for (size_t i = 0; t && i < bm.size(); ++i)
        if (bm[i].second == t->url) {
          bm.erase(bm.begin() + i);
          App::Get().SaveBookmarks();
          RebuildBookmarkMenu();
          SetStatus("Lesezeichen entfernt.");
          break;
        }
      break;
    }
    case ID_TOOLS_SETTINGS:
      if (DialogBoxParamW(App::Get().instance, MAKEINTRESOURCEW(IDD_SETTINGS), hwnd_, SettingsProc, 0) == IDOK) {
        App::Get().ApplyNetworkSettings();
        App::Get().SaveSettings();
      }
      break;
    case ID_TOOLS_CLEARCOOKIES:
      Network::Get().cookies().Clear();
      App::Get().SaveCookies();
      SetStatus("Alle Cookies wurden gel\xC3\xB6scht.");
      break;
    case ID_HELP_ABOUT:
      if (t) NewTab("about:kite", true);
      break;
    case ID_CTX_OPENTAB:
      if (!ctxLink_.empty()) {
        Tab* nt = NewTab("about:blank", false);
        Navigate(nt, ctxLink_, t ? t->url : std::string());
      }
      break;
    case ID_CTX_COPYLINK:
      CopyText(ctxLink_);
      break;
    case ID_CTX_OPENIMAGE:
      if (!ctxImage_.empty()) {
        Tab* nt = NewTab("about:blank", true);
        Navigate(nt, ctxImage_);
      }
      break;
    case ID_CTX_SAVEIMAGE:
      if (!ctxImage_.empty()) Download(ctxImage_, std::string(), t ? t->url : std::string());
      break;
    case ID_CTX_SAVELINK:
      if (!ctxLink_.empty()) Download(ctxLink_, std::string(), t ? t->url : std::string());
      break;
  }
}

// ---------------------------------------------------------------------------
// Window procedures

LRESULT CALLBACK Browser::MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Browser* b = g_browser;
  if (m == WM_NCCREATE) {
    CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
    b = (Browser*)cs->lpCreateParams;
    b->hwnd_ = h;
  }
  if (b && b->hwnd_ == h) return b->HandleMain(m, w, l);
  return DefWindowProcW(h, m, w, l);
}

LRESULT Browser::HandleMain(UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_SIZE:
      LayoutChildren();
      if (current_ && current_->rendered) {
        SetTimer(hwnd_, kTimerResize, 120, 0);
        UpdateScrollBars();
      }
      return 0;
    case WM_SETFOCUS:
      if (current_ && view_) SetFocus(view_);
      return 0;
    case WM_COMMAND: {
      if ((HWND)l == address_ || (HWND)l == findEdit_) return 0;
      OnCommand(LOWORD(w));
      return 0;
    }
    case WM_NOTIFY: {
      NMHDR* nm = (NMHDR*)l;
      if (nm->hwndFrom == tabs_) {
        if (nm->code == TCN_SELCHANGE) {
          int idx = TabCtrl_GetCurSel(tabs_);
          if (idx >= 0 && idx < (int)tabList_.size()) ActivateTab(tabList_[idx]);
        } else if (nm->code == NM_RCLICK) {
          TCHITTESTINFO hti;
          GetCursorPos(&hti.pt);
          ScreenToClient(tabs_, &hti.pt);
          int idx = TabCtrl_HitTest(tabs_, &hti);
          if (idx >= 0 && idx < (int)tabList_.size()) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_TAB_CLOSE, L"Tab &schlie\x00df" L"en");
            AppendMenuW(menu, MF_STRING, ID_TAB_DUPLICATE, L"Tab &duplizieren");
            AppendMenuW(menu, MF_STRING, ID_FILE_NEWTAB, L"&Neuer Tab");
            POINT pt;
            GetCursorPos(&pt);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, 0);
            DestroyMenu(menu);
            Tab* t = tabList_[idx];
            if (cmd == ID_TAB_CLOSE) CloseTab(t);
            else if (cmd == ID_TAB_DUPLICATE) NewTab(t->url, true);
            else if (cmd == ID_FILE_NEWTAB) NewTab("about:home", true);
          }
          return TRUE;
        }
      }
      if (nm->code == TTN_GETDISPINFOW) {
        NMTTDISPINFOW* di = (NMTTDISPINFOW*)l;
        switch (di->hdr.idFrom) {
          case ID_GO_BACK: di->lpszText = (LPWSTR)L"Zur\x00fc" L"ck (Alt+Links)"; break;
          case ID_GO_FORWARD: di->lpszText = (LPWSTR)L"Vor (Alt+Rechts)"; break;
          case ID_VIEW_RELOAD: di->lpszText = (LPWSTR)L"Neu laden / Abbrechen (F5)"; break;
          case ID_GO_HOME: di->lpszText = (LPWSTR)L"Startseite"; break;
          case ID_GO: di->lpszText = (LPWSTR)L"Gehe zu"; break;
          case ID_BM_ADD: di->lpszText = (LPWSTR)L"Lesezeichen hinzuf\x00fcgen (Strg+D)"; break;
          case ID_FILE_NEWTAB: di->lpszText = (LPWSTR)L"Neuer Tab (Strg+T)"; break;
          default: {
            // Tab tooltips: full title.
            if (di->hdr.hwndFrom == TabCtrl_GetToolTips(tabs_) && di->hdr.idFrom < tabList_.size()) {
              static std::wstring tip;
              Tab* t = tabList_[di->hdr.idFrom];
              tip = Widen(t->title.empty() ? t->displayUrl : t->title);
              di->lpszText = &tip[0];
            }
          }
        }
        return 0;
      }
      return 0;
    }
    case WM_CTLCOLOREDIT:
      if ((HWND)l == address_ && current_) {
        HDC dc = (HDC)w;
        static HBRUSH secureBrush = CreateSolidBrush(RGB(255, 255, 225));
        if (StartsWith(current_->url, "https:") && current_->secure && GetFocus() != address_) {
          SetBkColor(dc, RGB(255, 255, 225));
          return (LRESULT)secureBrush;
        }
      }
      break;
    case WM_TIMER: {
      UINT_PTR id = w;
      if (id == kTimerRelayout) {
        KillTimer(hwnd_, kTimerRelayout);
        relayoutPending_ = false;
        if (current_ && current_->needsRelayout && current_->rendered) {
          current_->needsRelayout = false;
          RenderTab(current_, false);
        }
      } else if (id == kTimerResize) {
        KillTimer(hwnd_, kTimerResize);
        if (current_ && current_->rendered) RenderTab(current_, true);
        for (size_t i = 0; i < tabList_.size(); ++i)
          if (tabList_[i] != current_) tabList_[i]->needsRelayout = true;
      } else if (id >= kTimerStyleFallbackBase && id < kTimerRefreshBase) {
        KillTimer(hwnd_, id);
        Tab* t = TabById((int)(id - kTimerStyleFallbackBase));
        if (t && !t->rendered) RenderTab(t, true);
      } else if (id >= kTimerRefreshBase) {
        KillTimer(hwnd_, id);
        Tab* t = TabById((int)(id - kTimerRefreshBase));
        if (t && !t->page->refreshUrl().empty()) {
          PendingNav nav;
          nav.url = t->page->refreshUrl();
          nav.replace = true;
          StartLoad(t, nav, t->url);
        }
      }
      return 0;
    }
    case WM_KITE_FETCHED:
      OnFetched((FetchJob*)l);
      return 0;
    case WM_APP + 2:
      // Inline editor lost focus.
      if (editCtl_ && (HWND)l == editCtl_ && GetFocus() != editCtl_) CommitEdit(false);
      return 0;
    case WM_INITMENUPOPUP: {
      HMENU menu = (HMENU)w;
      if (current_) {
        EnableMenuItem(menu, ID_GO_BACK, current_->historyIndex > 0 ? MF_ENABLED : MF_GRAYED);
        EnableMenuItem(menu, ID_GO_FORWARD,
                       current_->historyIndex + 1 < (int)current_->history.size() ? MF_ENABLED : MF_GRAYED);
      }
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd_);
      return 0;
    case WM_DESTROY:
      for (size_t i = 0; i < tabList_.size(); ++i)
        if (tabList_[i]->cancel) tabList_[i]->cancel->Cancel();
      App::Get().SaveCookies();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, m, w, l);
}

LRESULT CALLBACK Browser::AddressProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Browser* b = g_browser;
  if (m == WM_KEYDOWN && w == VK_RETURN) {
    b->OnCommand(ID_GO);
    return 0;
  }
  if (m == WM_KEYDOWN && w == VK_ESCAPE) {
    if (b->current_) SetWindowTextW(h, Widen(b->current_->displayUrl).c_str());
    SetFocus(b->view_);
    return 0;
  }
  if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
  if (m == WM_SETFOCUS) {
    LRESULT r = CallWindowProcW(b->oldAddressProc_, h, m, w, l);
    PostMessageW(h, EM_SETSEL, 0, -1);
    InvalidateRect(h, 0, TRUE);
    return r;
  }
  if (m == WM_KILLFOCUS) InvalidateRect(h, 0, TRUE);
  return CallWindowProcW(b->oldAddressProc_, h, m, w, l);
}

LRESULT CALLBACK Browser::FindEditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Browser* b = g_browser;
  if (m == WM_KEYDOWN && w == VK_RETURN) {
    b->Find((GetKeyState(VK_SHIFT) & 0x8000) == 0, false);
    return 0;
  }
  if (m == WM_KEYDOWN && w == VK_ESCAPE) {
    b->ShowFindBar(false);
    return 0;
  }
  if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;
  LRESULT r = CallWindowProcW(b->oldFindProc_, h, m, w, l);
  if (m == WM_CHAR) b->Find(true, true);
  return r;
}

INT_PTR CALLBACK Browser::SettingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Settings& s = App::Get().settings;
  switch (m) {
    case WM_INITDIALOG:
      SetDlgItemTextW(h, IDC_SET_HOME, Widen(s.homePage).c_str());
      SetDlgItemTextW(h, IDC_SET_SEARCH, Widen(s.searchUrl).c_str());
      SetDlgItemTextW(h, IDC_SET_PROXYHOST, Widen(s.proxyHost).c_str());
      SetDlgItemInt(h, IDC_SET_PROXYPORT, s.proxyPort, FALSE);
      SetDlgItemInt(h, IDC_SET_ZOOM, s.defaultZoom, FALSE);
      CheckDlgButton(h, IDC_SET_IMAGES, s.loadImages ? BST_CHECKED : BST_UNCHECKED);
      SetDlgItemTextW(h, IDC_SET_COOKIEINFO,
                      Widen("Gespeicherte Cookies: " + IntToString((long long)Network::Get().cookies().Count())).c_str());
      return TRUE;
    case WM_COMMAND:
      switch (LOWORD(w)) {
        case IDC_SET_CLEARCOOKIES:
          Network::Get().cookies().Clear();
          App::Get().SaveCookies();
          SetDlgItemTextW(h, IDC_SET_COOKIEINFO, L"Alle Cookies gel\x00f6scht.");
          return TRUE;
        case IDOK: {
          wchar_t buf[2048];
          GetDlgItemTextW(h, IDC_SET_HOME, buf, 2048);
          s.homePage = Narrow(buf);
          GetDlgItemTextW(h, IDC_SET_SEARCH, buf, 2048);
          s.searchUrl = Narrow(buf);
          GetDlgItemTextW(h, IDC_SET_PROXYHOST, buf, 2048);
          s.proxyHost = Trim(Narrow(buf));
          s.proxyPort = (int)GetDlgItemInt(h, IDC_SET_PROXYPORT, 0, FALSE);
          int z = (int)GetDlgItemInt(h, IDC_SET_ZOOM, 0, FALSE);
          if (z >= 30 && z <= 300) s.defaultZoom = z;
          s.loadImages = IsDlgButtonChecked(h, IDC_SET_IMAGES) == BST_CHECKED;
          if (s.homePage.empty()) s.homePage = "about:home";
          EndDialog(h, IDOK);
          return TRUE;
        }
        case IDCANCEL:
          EndDialog(h, IDCANCEL);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

LRESULT CALLBACK Browser::ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Browser* b = g_browser;
  if (m == WM_NCCREATE) {
    CREATESTRUCTW* cs = (CREATESTRUCTW*)l;
    b = (Browser*)cs->lpCreateParams;
  }
  if (b) return b->HandleView(h, m, w, l);
  return DefWindowProcW(h, m, w, l);
}

LRESULT Browser::HandleView(HWND h, UINT m, WPARAM w, LPARAM l) {
  Tab* t = current_;
  switch (m) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT rc;
      GetClientRect(h, &rc);
      if (t && t->rendered) {
        renderer_.Paint(dc, t->page->display(), rc.right, rc.bottom, t->scrollX, t->scrollY, ZoomF(),
                        t->highlights, t->selection);
      } else {
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
      }
      EndPaint(h, &ps);
      return 0;
    }
    case WM_SIZE:
      UpdateScrollBars();
      return 0;
    case WM_VSCROLL:
    case WM_HSCROLL: {
      if (!t) return 0;
      bool vert = m == WM_VSCROLL;
      RECT rc;
      GetClientRect(h, &rc);
      float page = vert ? rc.bottom * 0.9f : rc.right * 0.9f;
      float pos = vert ? t->scrollY : t->scrollX;
      switch (LOWORD(w)) {
        case SB_LINEUP: pos -= 40; break;
        case SB_LINEDOWN: pos += 40; break;
        case SB_PAGEUP: pos -= page; break;
        case SB_PAGEDOWN: pos += page; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = 1e9f; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
          SCROLLINFO si;
          si.cbSize = sizeof si;
          si.fMask = SIF_TRACKPOS;
          GetScrollInfo(h, vert ? SB_VERT : SB_HORZ, &si);
          pos = (float)si.nTrackPos;
          break;
        }
        default: return 0;
      }
      if (vert) ScrollTo(t->scrollX, pos);
      else ScrollTo(pos, t->scrollY);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      int delta = (short)HIWORD(w);
      if (GetKeyState(VK_CONTROL) & 0x8000) {
        OnCommand(delta > 0 ? ID_VIEW_ZOOMIN : ID_VIEW_ZOOMOUT);
        return 0;
      }
      UINT lines = 3;
      SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
      ScrollBy(0, -(float)delta / WHEEL_DELTA * (float)lines * 40.0f);
      return 0;
    }
    case WM_KEYDOWN: {
      if (!t) return 0;
      RECT rc;
      GetClientRect(h, &rc);
      bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      switch (w) {
        case VK_UP: ScrollBy(0, -40); break;
        case VK_DOWN: ScrollBy(0, 40); break;
        case VK_LEFT: ScrollBy(-40, 0); break;
        case VK_RIGHT: ScrollBy(40, 0); break;
        case VK_PRIOR: ScrollBy(0, -rc.bottom * 0.9f); break;
        case VK_NEXT: ScrollBy(0, rc.bottom * 0.9f); break;
        case VK_SPACE: ScrollBy(0, (shift ? -1 : 1) * rc.bottom * 0.9f); break;
        case VK_HOME: ScrollTo(0, 0); break;
        case VK_END: ScrollTo(0, 1e9f); break;
        case VK_BACK: GoHistory(t, shift ? 1 : -1); break;
        case VK_ESCAPE:
          if (t->loading) Stop(t);
          else if (findVisible_) ShowFindBar(false);
          break;
        case 'C':
          if (ctrl) OnCommand(ID_EDIT_COPY);
          break;
        case 'A':
          if (ctrl) OnCommand(ID_EDIT_SELECTALL);
          break;
      }
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      SetFocus(h);
      if (!t) return 0;
      int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
      dragStart_.x = x;
      dragStart_.y = y;
      dragging_ = true;
      dragMoved_ = false;
      SetCapture(h);
      if (!t->selection.empty()) {
        t->selection.clear();
        t->selectedText.clear();
        InvalidateRect(h, 0, FALSE);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!t) return 0;
      int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
      if (dragging_ && (w & MK_LBUTTON)) {
        if (std::abs(x - dragStart_.x) + std::abs(y - dragStart_.y) > 4) dragMoved_ = true;
        if (dragMoved_) {
          UpdateSelection(dragStart_.x, dragStart_.y, x, y);
          RECT rc;
          GetClientRect(h, &rc);
          if (y > rc.bottom) ScrollBy(0, 20);
          if (y < 0) ScrollBy(0, -20);
        }
        return 0;
      }
      Node* n = NodeAt(x, y);
      Node* link = Page::LinkFor(n);
      bool pointer = link != 0;
      bool text = false;
      for (Node* p = n; p && !pointer; p = p->parent) {
        if (!p->IsElement()) continue;
        if (p->tag == "button" || p->tag == "select" || p->tag == "summary" ||
            (p->tag == "label") || (p->style && p->style->cursor == kCursorPointer))
          pointer = true;
        if (p->tag == "input") {
          std::string type = AsciiLower(p->Attr("type"));
          if (type == "submit" || type == "checkbox" || type == "radio" || type == "image" || type == "reset")
            pointer = true;
          else
            text = true;
          break;
        }
        if (p->tag == "textarea") {
          text = true;
          break;
        }
      }
      SetCursor(LoadCursorW(0, pointer ? IDC_HAND : text ? IDC_IBEAM : IDC_ARROW));
      static std::string lastLink;
      std::string href = link ? t->page->LinkUrl(link) : std::string();
      if (href != lastLink) {
        lastLink = href;
        SetStatus(href.empty() ? t->status : href);
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!t) return 0;
      ReleaseCapture();
      bool wasDragging = dragging_;
      dragging_ = false;
      if (wasDragging && !dragMoved_) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        OnClick(GET_X_LPARAM(l), GET_Y_LPARAM(l), ctrl);
      }
      return 0;
    }
    case WM_MBUTTONUP: {
      if (!t) return 0;
      Node* link = Page::LinkFor(NodeAt(GET_X_LPARAM(l), GET_Y_LPARAM(l)));
      if (link) {
        std::string url = t->page->LinkUrl(link);
        if (!url.empty()) {
          Tab* nt = NewTab("about:blank", false);
          Navigate(nt, url, t->url);
        }
      }
      return 0;
    }
    case WM_XBUTTONUP:
      if (t) GoHistory(t, HIWORD(w) == 1 ? -1 : 1);
      return TRUE;
    case WM_CONTEXTMENU: {
      if (!t) return 0;
      POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (pt.x == -1 && pt.y == -1) {
        pt.x = 10;
        pt.y = 10;
      } else {
        ScreenToClient(h, &pt);
      }
      ShowContextMenu(pt.x, pt.y);
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
      break;
    case WM_COMMAND:
      return SendMessageW(hwnd_, WM_COMMAND, w, l);
  }
  return DefWindowProcW(h, m, w, l);
}

int RunBrowser(HINSTANCE inst, const std::wstring& startUrl) {
  static Browser browser;
  g_browser = &browser;
  if (!browser.Create(inst, startUrl)) return 1;
  return browser.Run();
}

}  // namespace kite
