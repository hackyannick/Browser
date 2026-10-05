#include "base/strings.h"
#include "win32/app.h"

namespace kite {

namespace {

const char* kCommonStyle = R"CSS(
body { margin: 0; font-family: Tahoma, Arial, sans-serif; background: #eef2f7; color: #222; }
.top { background: #2b5797; color: #fff; padding: 28px 0 70px 0; text-align: center; }
.top h1 { margin: 0; font-size: 40px; font-weight: normal; letter-spacing: 1px; }
.top p { margin: 6px 0 0 0; color: #c9d7ee; font-size: 13px; }
.card { background: #fff; border: 1px solid #c5cdd8; max-width: 640px; margin: -48px auto 20px auto; padding: 22px 26px; }
.search { display: flex; gap: 8px; }
.search input[type=text] { flex: 1; font-size: 16px; padding: 6px 8px; width: auto; }
.search input[type=submit] { font-size: 14px; padding: 4px 16px; }
.tiles { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 18px; }
.tile { display: block; width: 136px; background: #f6f8fb; border: 1px solid #d5dce6; padding: 10px; text-decoration: none; color: #1d3f75; }
.tile b { display: block; font-size: 14px; }
.tile span { font-size: 11px; color: #667; }
h2 { font-size: 15px; color: #2b5797; border-bottom: 1px solid #dde3ea; padding-bottom: 4px; margin: 20px 0 8px 0; }
ul.bm { margin: 0; padding-left: 18px; }
ul.bm li { margin: 3px 0; font-size: 13px; }
.foot { text-align: center; color: #889; font-size: 11px; padding-bottom: 24px; }
)CSS";

}  // namespace

std::string StartPageHtml() {
  App& app = App::Get();
  std::string search = app.settings.searchUrl;
  // Split "https://host/path?param=" into action + field name.
  std::string action = search, field = "q";
  size_t q = search.find('?');
  if (q != std::string::npos) {
    action = search.substr(0, q);
    std::string params = search.substr(q + 1);
    size_t eq = params.rfind('=');
    size_t amp = params.rfind('&', eq == std::string::npos ? std::string::npos : eq);
    if (eq != std::string::npos) field = params.substr(amp == std::string::npos ? 0 : amp + 1, eq - (amp == std::string::npos ? 0 : amp + 1));
  }
  std::string html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Neuer Tab</title><style>";
  html += kCommonStyle;
  html += "</style></head><body><div class=\"top\"><h1>Kite</h1><p>Ein moderner Browser f&uuml;r Windows 2000</p></div>";
  html += "<div class=\"card\"><form class=\"search\" action=\"" + HtmlEscape(action) +
          "\" method=\"get\"><input type=\"text\" name=\"" + HtmlEscape(field) +
          "\" placeholder=\"Im Web suchen oder Adresse eingeben\"><input type=\"submit\" value=\"Suchen\"></form>";
  html += "<div class=\"tiles\">";
  const char* tiles[][3] = {
      {"https://html.duckduckgo.com/html/", "DuckDuckGo", "Suche ohne JavaScript"},
      {"https://de.m.wikipedia.org/", "Wikipedia", "Die freie Enzyklop&auml;die"},
      {"https://lite.cnn.com/", "CNN Lite", "Nachrichten (Textversion)"},
      {"https://text.npr.org/", "NPR Text", "Nachrichten (Textversion)"},
      {"https://news.ycombinator.com/", "Hacker News", "Technik-Nachrichten"},
      {"https://old.reddit.com/", "old.reddit", "Diskussionen"},
      {"https://www.gutenberg.org/", "Gutenberg", "Freie E-Books"},
      {"https://pypi.org/", "PyPI", "Python-Pakete"},
  };
  for (size_t i = 0; i < sizeof tiles / sizeof tiles[0]; ++i)
    html += std::string("<a class=\"tile\" href=\"") + tiles[i][0] + "\"><b>" + tiles[i][1] +
            "</b><span>" + tiles[i][2] + "</span></a>";
  html += "</div>";
  if (!app.bookmarks.empty()) {
    html += "<h2>Lesezeichen</h2><ul class=\"bm\">";
    for (size_t i = 0; i < app.bookmarks.size() && i < 40; ++i)
      html += "<li><a href=\"" + HtmlEscape(app.bookmarks[i].second) + "\">" +
              HtmlEscape(app.bookmarks[i].first.empty() ? app.bookmarks[i].second : app.bookmarks[i].first) +
              "</a></li>";
    html += "</ul>";
  }
  html += "</div><div class=\"foot\">Kite Engine &middot; HTML5 &middot; CSS3 &middot; TLS 1.2 &middot; "
          "<a href=\"about:kite\">&Uuml;ber Kite</a></div></body></html>";
  return html;
}

std::string ErrorPageHtml(const std::string& url, const std::string& message) {
  std::string html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Fehler</title><style>";
  html += kCommonStyle;
  html += ".err { border-left: 4px solid #c0392b; } .err h1 { font-size: 20px; color: #c0392b; margin: 0 0 10px 0; }"
          ".url { color: #556; word-wrap: break-word; font-size: 12px; } li { font-size: 13px; margin: 4px 0; }"
          "</style></head><body><div class=\"top\" style=\"padding-bottom:60px\"><h1>Kite</h1></div>"
          "<div class=\"card err\"><h1>Die Seite konnte nicht geladen werden</h1><p>";
  html += HtmlEscape(message);
  html += "</p><p class=\"url\">" + HtmlEscape(url) + "</p><ul>"
          "<li>Pr&uuml;fen Sie die Adresse auf Tippfehler.</li>"
          "<li>Pr&uuml;fen Sie die Netzwerkverbindung und ggf. die Proxy-Einstellungen (Extras &rarr; Einstellungen).</li>"
          "<li>Bei Zertifikatsfehlern: Stimmen Datum und Uhrzeit des Systems?</li></ul>"
          "<p><a href=\"" + HtmlEscape(url) + "\">Erneut versuchen</a></p></div></body></html>";
  return html;
}

std::string AboutPageHtml() {
  std::string html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>&Uuml;ber Kite</title><style>";
  html += kCommonStyle;
  html += "td { padding: 3px 12px 3px 0; font-size: 13px; vertical-align: top; } th { text-align: left; font-size: 13px; padding-right: 12px; }"
          "</style></head><body><div class=\"top\"><h1>Kite 1.0</h1><p>Moderner Webbrowser mit eigener Engine f&uuml;r Windows 2000</p></div>"
          "<div class=\"card\"><h2>Engine</h2><table>"
          "<tr><th>HTML</th><td>HTML5-Parser mit Fehlerkorrektur, Formulare (GET/POST), Tabellen, Listen</td></tr>"
          "<tr><th>CSS</th><td>Kaskade, Selektoren Level 4 (:is, :not, :nth-child ...), @media, @supports, CSS-Variablen, calc(), Verschachtelung</td></tr>"
          "<tr><th>Layout</th><td>Block, Inline, Floats, Tabellen, Flexbox, Grid (Spalten), absolute/relative Positionierung</td></tr>"
          "<tr><th>Bilder</th><td>PNG, JPEG, GIF, BMP</td></tr>"
          "<tr><th>Netzwerk</th><td>HTTP/1.1, TLS 1.2 (BearSSL), gzip, Cookies, Weiterleitungen, Proxy</td></tr>"
          "<tr><th>JavaScript</th><td>nicht unterst&uuml;tzt (Seiten werden wie mit deaktiviertem JavaScript angezeigt)</td></tr>"
          "</table><h2>Lizenzen</h2><p style=\"font-size:12px\">BearSSL &copy; Thomas Pornin (MIT) &middot; "
          "stb_image von Sean Barrett (Public Domain) &middot; Stammzertifikate von Mozilla (MPL 2.0)</p></div></body></html>";
  return html;
}

std::string BookmarksPageHtml() {
  App& app = App::Get();
  std::string html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Lesezeichen</title><style>";
  html += kCommonStyle;
  html += "</style></head><body><div class=\"top\"><h1>Lesezeichen</h1></div><div class=\"card\">";
  if (app.bookmarks.empty()) html += "<p>Noch keine Lesezeichen. Mit Strg+D f&uuml;gen Sie die aktuelle Seite hinzu.</p>";
  else {
    html += "<ul class=\"bm\">";
    for (size_t i = 0; i < app.bookmarks.size(); ++i)
      html += "<li><a href=\"" + HtmlEscape(app.bookmarks[i].second) + "\">" +
              HtmlEscape(app.bookmarks[i].first.empty() ? app.bookmarks[i].second : app.bookmarks[i].first) +
              "</a><br><span style=\"color:#889;font-size:11px\">" + HtmlEscape(app.bookmarks[i].second) +
              "</span></li>";
    html += "</ul>";
  }
  html += "</div></body></html>";
  return html;
}

}  // namespace kite
