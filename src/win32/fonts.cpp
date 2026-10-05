#include "win32/app.h"

#include <cmath>

#include "base/strings.h"

namespace kite {

std::wstring Widen(const std::string& utf8) {
  std::u16string u = Utf8ToUtf16(utf8);
  return std::wstring(u.begin(), u.end());
}

std::string Narrow(const std::wstring& w) {
  std::u16string u(w.begin(), w.end());
  return Utf16ToUtf8(u);
}

std::wstring WindowTextW(HWND hwnd) {
  int len = GetWindowTextLengthW(hwnd);
  std::wstring s(len + 1, L'\0');
  GetWindowTextW(hwnd, &s[0], len + 1);
  s.resize(len);
  return s;
}

GdiFonts& Fonts() {
  static GdiFonts* f = new GdiFonts;
  return *f;
}

static int CALLBACK EnumFamProc(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
  std::map<std::wstring, bool>* m = reinterpret_cast<std::map<std::wstring, bool>*>(lp);
  std::wstring name = lf->lfFaceName;
  if (!name.empty() && name[0] != L'@') {
    std::wstring lower = name;
    for (size_t i = 0; i < lower.size(); ++i) lower[i] = (wchar_t)towlower(lower[i]);
    (*m)[lower] = true;
  }
  return 1;
}

GdiFonts::GdiFonts() {
  dc_ = CreateCompatibleDC(0);
  LOGFONTW lf;
  ZeroMemory(&lf, sizeof lf);
  lf.lfCharSet = DEFAULT_CHARSET;
  EnumFontFamiliesExW(dc_, &lf, (FONTENUMPROCW)EnumFamProc, (LPARAM)&installed_, 0);
}

GdiFonts::~GdiFonts() {
  for (std::map<std::string, HFONT>::iterator it = fonts_.begin(); it != fonts_.end(); ++it)
    DeleteObject(it->second);
  DeleteDC(dc_);
}

std::wstring GdiFonts::ResolveFamily(const std::string& css) {
  std::map<std::string, std::wstring>::iterator cached = familyCache_.find(css);
  if (cached != familyCache_.end()) return cached->second;
  std::vector<std::string> fams = Split(css, ',');
  std::wstring result;
  for (size_t i = 0; i < fams.size() && result.empty(); ++i) {
    std::string f = Trim(fams[i]);
    if (f.empty()) continue;
    if (f == "serif" || f == "ui-serif") result = L"Times New Roman";
    else if (f == "sans-serif" || f == "ui-sans-serif") result = L"Arial";
    else if (f == "monospace" || f == "ui-monospace") result = L"Courier New";
    else if (f == "cursive") result = L"Comic Sans MS";
    else if (f == "fantasy") result = L"Impact";
    else if (f == "system-ui" || f == "-apple-system" || f == "blinkmacsystemfont" ||
             f == "caption" || f == "menu")
      result = L"Tahoma";
    else if (f == "math" || f == "emoji" || f == "fangsong") continue;
    else {
      std::wstring w = Widen(f);
      if (installed_.count(w)) {
        result = w;
      } else if (f == "helvetica" || f == "helvetica neue" || f == "arial nova") {
        result = L"Arial";
      } else if (f == "times") {
        result = L"Times New Roman";
      } else if (f == "courier" || f == "monaco" || f == "menlo" || f == "consolas" ||
                 f == "sfmono-regular" || f == "liberation mono" || f == "dejavu sans mono") {
        if (installed_.count(L"lucida console")) result = L"Lucida Console";
        else result = L"Courier New";
      } else if (f == "segoe ui" || f == "roboto" || f == "noto sans" || f == "ubuntu" ||
                 f == "cantarell" || f == "inter" || f == "open sans" || f == "lato") {
        // Common modern UI fonts: fall back to the closest classic face but
        // only after trying the remaining families in the list.
        continue;
      }
    }
  }
  if (result.empty()) {
    bool hasSerifHint = css.find("serif") != std::string::npos &&
                        css.find("sans-serif") == std::string::npos;
    result = hasSerifHint ? L"Times New Roman" : L"Arial";
  }
  familyCache_[css] = result;
  return result;
}

const GdiFonts::WebFont* GdiFonts::FindWebFont(const FontDesc& f) {
  if (webFonts_.empty()) return 0;
  std::vector<std::string> fams = Split(f.family, ',');
  for (size_t i = 0; i < fams.size(); ++i) {
    std::map<std::string, std::vector<WebFont> >::iterator it = webFonts_.find(Trim(fams[i]));
    if (it == webFonts_.end()) {
      // Stop at the first family that is installed locally or generic.
      std::wstring w = Widen(Trim(fams[i]));
      if (installed_.count(w)) return 0;
      continue;
    }
    const WebFont* best = 0;
    int bestScore = 1 << 30;
    for (size_t k = 0; k < it->second.size(); ++k) {
      const WebFont& v = it->second[k];
      int score = std::abs(v.weight - f.weight) + (v.italic != f.italic ? 1000 : 0);
      if (score < bestScore) {
        bestScore = score;
        best = &v;
      }
    }
    return best;
  }
  return 0;
}

bool GdiFonts::IsWebFont(const FontDesc& f) { return FindWebFont(f) != 0; }

bool GdiFonts::RegisterWebFont(const std::string& cssFamily, int weight, bool italic,
                               const std::string& sfnt, const std::string& internalName) {
  if (sfnt.empty() || internalName.empty()) return false;
  DWORD count = 0;
  // AddFontMemResourceEx is available from Windows 2000 on; the font stays
  // private to this process.
  HANDLE h = AddFontMemResourceEx((void*)sfnt.data(), (DWORD)sfnt.size(), 0, &count);
  if (!h || count == 0) return false;
  fontHandles_.push_back(h);
  WebFont wf;
  wf.weight = weight;
  wf.italic = italic;
  wf.face = Widen(internalName);
  webFonts_[cssFamily].push_back(wf);
  for (std::map<std::string, HFONT>::iterator it = fonts_.begin(); it != fonts_.end(); ++it)
    DeleteObject(it->second);
  fonts_.clear();
  familyCache_.clear();
  return true;
}

HFONT GdiFonts::Get(const FontDesc& f, float scale) {
  int px = (int)std::floor(f.size * scale + 0.5f);
  if (px < 1) px = 1;
  if (px > 400) px = 400;
  std::string key = f.family + "|" + IntToString(px) + "|" + IntToString(f.weight) + "|" +
                    (f.italic ? "i" : "n");
  std::map<std::string, HFONT>::iterator it = fonts_.find(key);
  if (it != fonts_.end()) return it->second;
  if (fonts_.size() > 400) {
    for (it = fonts_.begin(); it != fonts_.end(); ++it) DeleteObject(it->second);
    fonts_.clear();
  }
  const WebFont* web = FindWebFont(f);
  std::wstring face = web ? web->face : ResolveFamily(f.family);
  // ClearType exists from Windows XP on; Windows 2000 gets standard smoothing.
  DWORD quality = App::Get().isXpOrLater ? 5 /* CLEARTYPE_QUALITY */ : ANTIALIASED_QUALITY;
  HFONT h = CreateFontW(-px, 0, 0, 0, f.weight >= 600 ? FW_BOLD : (f.weight <= 300 ? FW_LIGHT : FW_NORMAL),
                        f.italic, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        quality, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
  fonts_[key] = h;
  return h;
}

FontMetrics GdiFonts::Metrics(const FontDesc& f) {
  HFONT font = Get(f, 1.0f);
  HGDIOBJ old = SelectObject(dc_, font);
  TEXTMETRICW tm;
  GetTextMetricsW(dc_, &tm);
  SelectObject(dc_, old);
  FontMetrics m;
  m.ascent = (float)tm.tmAscent;
  m.descent = (float)tm.tmDescent;
  m.lineGap = (float)tm.tmExternalLeading;
  return m;
}

float GdiFonts::MeasureText(const FontDesc& f, const std::string& utf8) {
  std::wstring w = Widen(utf8);
  if (w.empty()) return 0;
  HFONT font = Get(f, 1.0f);
  HGDIOBJ old = SelectObject(dc_, font);
  SIZE sz = {0, 0};
  GetTextExtentPoint32W(dc_, w.c_str(), (int)w.size(), &sz);
  SelectObject(dc_, old);
  return (float)sz.cx;
}

}  // namespace kite
