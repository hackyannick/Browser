// Kite Browser entry point.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include "base/strings.h"
#include "win32/app.h"

namespace kite {

namespace {

std::string ReadFileUtf8(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
  if (f == INVALID_HANDLE_VALUE) return std::string();
  std::string out;
  char buf[65536];
  DWORD n = 0;
  while (ReadFile(f, buf, sizeof buf, &n, 0) && n > 0) out.append(buf, n);
  CloseHandle(f);
  return out;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& data) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD n = 0;
  WriteFile(f, data.data(), (DWORD)data.size(), &n, 0);
  CloseHandle(f);
  return n == data.size();
}

std::wstring IniPath() { return App::Get().dataDir + L"\\kite.ini"; }

std::string IniGet(const wchar_t* key, const char* def) {
  wchar_t buf[2048];
  GetPrivateProfileStringW(L"Kite", key, Widen(def).c_str(), buf, 2048, IniPath().c_str());
  return Narrow(buf);
}

void IniSet(const wchar_t* key, const std::string& value) {
  WritePrivateProfileStringW(L"Kite", key, Widen(value).c_str(), IniPath().c_str());
}

}  // namespace

App& App::Get() {
  static App* app = new App;
  return *app;
}

void App::Init(HINSTANCE inst) {
  instance = inst;
  OSVERSIONINFOW vi;
  ZeroMemory(&vi, sizeof vi);
  vi.dwOSVersionInfoSize = sizeof vi;
  GetVersionExW(&vi);
  isXpOrLater = vi.dwMajorVersion > 5 || (vi.dwMajorVersion == 5 && vi.dwMinorVersion >= 1);

  // Portable mode: keep data next to kite.exe if that directory is writable,
  // otherwise use %APPDATA%\Kite.
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(0, exe, MAX_PATH);
  std::wstring exeDir = exe;
  exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));
  dataDir = exeDir;
  std::wstring probe = exeDir + L"\\kite.tmp";
  HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, 0);
  if (h == INVALID_HANDLE_VALUE) {
    wchar_t appdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
      dataDir = std::wstring(appdata) + L"\\Kite";
      CreateDirectoryW(dataDir.c_str(), 0);
    }
  } else {
    CloseHandle(h);
  }

  settings.homePage = IniGet(L"HomePage", "about:home");
  settings.searchUrl = IniGet(L"SearchUrl", "https://html.duckduckgo.com/html/?q=");
  settings.proxyHost = IniGet(L"ProxyHost", "");
  long long v;
  if (ParseInt(IniGet(L"ProxyPort", "0"), v)) settings.proxyPort = (int)v;
  settings.loadImages = IniGet(L"LoadImages", "1") != "0";
  settings.javaScript = IniGet(L"JavaScript", "1") != "0";
  settings.http2 = IniGet(L"Http2", "1") != "0";
  settings.tls13 = IniGet(L"Tls13", "1") != "0";
  if (ParseInt(IniGet(L"DefaultZoom", "100"), v)) settings.defaultZoom = (int)v;

  NetInit();
  std::string pem = ReadFileUtf8(exeDir + L"\\cacert.pem");
  if (pem.empty()) pem = ReadFileUtf8(dataDir + L"\\cacert.pem");
  int n = LoadTrustAnchors(pem);
  if (n == 0) {
    MessageBoxW(0,
                L"Die Datei cacert.pem (Stammzertifikate) wurde nicht gefunden.\n"
                L"HTTPS-Seiten k\x00f6nnen ohne sie nicht gepr\x00fc" L"ft werden.\n\n"
                L"Bitte legen Sie cacert.pem neben kite.exe.",
                L"Kite", MB_ICONWARNING);
  }
  Network::Get().cookies().Deserialize(ReadFileUtf8(dataDir + L"\\cookies.txt"));
  localStorage.Deserialize(ReadFileUtf8(dataDir + L"\\storage.txt"));
  ApplyNetworkSettings();
  LoadBookmarks();
}

void App::ApplyNetworkSettings() {
  ProxyConfig pc;
  pc.host = settings.proxyHost;
  pc.port = settings.proxyPort;
  Network::Get().SetProxy(pc);
  Network::Get().SetHttp2Enabled(settings.http2);
  SetTls13Enabled(settings.tls13);
}

void App::SaveSettings() {
  IniSet(L"HomePage", settings.homePage);
  IniSet(L"SearchUrl", settings.searchUrl);
  IniSet(L"ProxyHost", settings.proxyHost);
  IniSet(L"ProxyPort", IntToString(settings.proxyPort));
  IniSet(L"LoadImages", settings.loadImages ? "1" : "0");
  IniSet(L"JavaScript", settings.javaScript ? "1" : "0");
  IniSet(L"Http2", settings.http2 ? "1" : "0");
  IniSet(L"Tls13", settings.tls13 ? "1" : "0");
  IniSet(L"DefaultZoom", IntToString(settings.defaultZoom));
}

void App::SaveCookies() {
  WriteFileUtf8(dataDir + L"\\cookies.txt", Network::Get().cookies().Serialize());
  SaveStorage();
}

void App::SaveStorage() {
  if (!localStorage.dirty()) return;
  WriteFileUtf8(dataDir + L"\\storage.txt", localStorage.Serialize());
  localStorage.ClearDirty();
}

void App::LoadBookmarks() {
  bookmarks.clear();
  std::vector<std::string> lines = Split(ReadFileUtf8(dataDir + L"\\bookmarks.txt"), '\n');
  for (size_t i = 0; i < lines.size(); ++i) {
    std::string line = lines[i];
    if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
    size_t tab = line.find('\t');
    if (line.empty()) continue;
    if (tab == std::string::npos) bookmarks.push_back(std::make_pair(std::string(), line));
    else bookmarks.push_back(std::make_pair(line.substr(tab + 1), line.substr(0, tab)));
  }
}

void App::SaveBookmarks() {
  std::string out;
  for (size_t i = 0; i < bookmarks.size(); ++i)
    out += bookmarks[i].second + "\t" + bookmarks[i].first + "\r\n";
  WriteFileUtf8(dataDir + L"\\bookmarks.txt", out);
}

}  // namespace kite

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
  INITCOMMONCONTROLSEX icc;
  icc.dwSize = sizeof icc;
  icc.dwICC = ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_WIN95_CLASSES;
  InitCommonControlsEx(&icc);

  kite::App::Get().Init(inst);
  kite::InstallAudioOutput();

  std::wstring start;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv && argc > 1) {
    start = argv[1];
    // A local file path?
    if (start.size() > 2 && start[1] == L':' && GetFileAttributesW(start.c_str()) != INVALID_FILE_ATTRIBUTES)
      start = kite::Widen(kite::Url::FromLocalPath(kite::Narrow(start)).Spec());
  }
  if (argv) LocalFree(argv);
  int rc = kite::RunBrowser(inst, start);
  kite::App::Get().SaveCookies();
  return rc;
}
