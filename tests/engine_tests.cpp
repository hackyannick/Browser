// Unit tests for the Kite engine (portable; run with `ctest` on the build
// host). A tiny self-contained test harness keeps the dependencies at zero.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "base/strings.h"
#include "css/properties.h"
#include "css/resolver.h"
#include "css/style.h"
#include "css/stylesheet.h"
#include "html/entities.h"
#include "html/parser.h"
#include "image/image.h"
#include "image/svg.h"
#include "net/http.h"
#include "net/url.h"
#include "page/animation.h"
#include "page/page.h"
#include "text/fontfile.h"

using namespace kite;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++g_checks;                                                            \
    if (!(cond)) {                                                         \
      ++g_failures;                                                        \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    ++g_checks;                                                                         \
    if (!((a) == (b))) {                                                                \
      ++g_failures;                                                                     \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b); \
    }                                                                                   \
  } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

class NoImages : public ImageProvider {
 public:
  State GetImage(const std::string&, int&, int&) { return kFailed; }
};

// Loads |html| into a page laid out at |width| with approximate fonts.
struct TestPage {
  SimpleFontProvider fonts;
  NoImages images;
  Page page;
  TestPage(const std::string& html, float width = 800) : page(&fonts, &images) {
    page.LoadHtml(html, "http://test.local/dir/page.html");
    page.Restyle(width, 600);
    page.Relayout(width, 600);
  }
  Node* ById(const char* id) { return page.document()->root->FindById(id); }
  Rect RectOf(const char* id) {
    Rect r;
    page.BoxRect(ById(id), r);
    return r;
  }
};

void TestStrings() {
  CHECK_EQ(Trim("  a b \n"), "a b");
  CHECK_EQ(CollapseWhitespace(" a \t b\n c "), "a b c");
  CHECK_EQ(AsciiLower("HeLLo"), "hello");
  CHECK_EQ(Base64Decode("SGVsbG8="), "Hello");
  CHECK_EQ(PercentDecode("a%20b+c", true), "a b c");
  CHECK_EQ(PercentEncodeForm("a b&c"), "a+b%26c");
  CHECK_EQ(Utf8Length("\xC3\xA4\xC3\xB6x"), 3u);
  CHECK_EQ(ConvertToUtf8("\xE4\x80", "windows-1252"), "\xC3\xA4\xE2\x82\xAC");
  std::u16string u = Utf8ToUtf16("\xF0\x9F\x98\x80");  // surrogate pair
  CHECK_EQ(u.size(), 2u);
  CHECK_EQ(Utf16ToUtf8(u), "\xF0\x9F\x98\x80");
}

void TestUrl() {
  Url base = Url::Parse("https://Example.com:443/a/b/c.html?x=1#frag");
  CHECK(base.valid());
  CHECK_EQ(base.host(), "example.com");
  CHECK_EQ(base.port(), -1);  // default port dropped
  CHECK_EQ(base.path(), "/a/b/c.html");
  CHECK_EQ(base.query(), "x=1");
  CHECK_EQ(base.fragment(), "frag");
  CHECK_EQ(base.Resolve("d.html").Spec(), "https://example.com/a/b/d.html");
  CHECK_EQ(base.Resolve("../d").Spec(), "https://example.com/a/d");
  CHECK_EQ(base.Resolve("/root").Spec(), "https://example.com/root");
  CHECK_EQ(base.Resolve("//cdn.net/x.js").Spec(), "https://cdn.net/x.js");
  CHECK_EQ(base.Resolve("?q=2").Spec(), "https://example.com/a/b/c.html?q=2");
  CHECK_EQ(base.Resolve("#top").Spec(), "https://example.com/a/b/c.html?x=1#top");
  CHECK_EQ(base.Resolve("http://other.org").Spec(), "http://other.org/");
  CHECK_EQ(Url::Parse("http://h/a b").path(), "/a%20b");
  CHECK_EQ(Url::Parse("http://h:8080/").HostPort(), "h:8080");
  CHECK(!Url::Parse("not a url").valid());
  CHECK_EQ(Url::Parse("data:text/plain,hi").scheme(), "data");
  CHECK_EQ(FixupUserInput("example.com", "S?q="), "https://example.com");
  CHECK_EQ(FixupUserInput("localhost:8080/x", "S?q="), "http://localhost:8080/x");
  CHECK_EQ(FixupUserInput("windows 2000", "S?q="), "S?q=windows+2000");
  CHECK_EQ(FixupUserInput("https://x.y/z", "S?q="), "https://x.y/z");
}

void TestEntities() {
  CHECK_EQ(DecodeEntities("a &amp; b &lt;c&gt; &#65;&#x42; &euro;", false), "a & b <c> AB \xE2\x82\xAC");
  CHECK_EQ(DecodeEntities("&copy 2000", false), "\xC2\xA9 2000");
  CHECK_EQ(DecodeEntities("?a=1&copy=2", true), "?a=1&copy=2");  // attribute rule
  CHECK_EQ(DecodeEntities("&unknown;", false), "&unknown;");
}

void TestHtmlParser() {
  std::unique_ptr<Document> doc = ParseHtml("<title>T</title><p>one<p>two<ul><li>a<li>b</ul>");
  CHECK_EQ(doc->Title(), "T");
  Node* body = doc->Body();
  CHECK(body != 0);
  std::vector<Node*> ps;
  body->FindAll("p", ps);
  CHECK_EQ(ps.size(), 2u);  // implicit </p>
  std::vector<Node*> lis;
  body->FindAll("li", lis);
  CHECK_EQ(lis.size(), 2u);
  CHECK_EQ(lis[0]->TextContent(), "a");

  doc = ParseHtml("<table><tr><td>1<td>2</table>");
  Node* table = doc->root->FindFirst("table");
  CHECK(table && table->FindFirst("tbody"));  // implied tbody
  std::vector<Node*> tds;
  table->FindAll("td", tds);
  CHECK_EQ(tds.size(), 2u);

  doc = ParseHtml("<div id=x class='a  b'>x<br>y<img src=i.png alt=\"q\"></div><script>if (a<b) {}</script>");
  Node* div = doc->root->FindById("x");
  CHECK(div != 0);
  CHECK_EQ(div->classes.size(), 2u);
  CHECK(div->FindFirst("br") != 0);
  Node* script = doc->root->FindFirst("script");
  CHECK(script && script->TextContent() == "if (a<b) {}");

  doc = ParseHtml("<b>bold <i>both</b> italic</i>");  // misnested formatting
  CHECK(doc->Body()->FindFirst("i") != 0);

  doc = ParseHtml("<svg><path d='M0 0'/><circle r=1 /></svg>");
  Node* svg = doc->root->FindFirst("svg");
  CHECK(svg && svg->children.size() == 2);

  CHECK_EQ(SniffHtmlCharset("<meta charset=\"ISO-8859-1\">"), "iso-8859-1");
  CHECK_EQ(SniffHtmlCharset("<meta http-equiv=Content-Type content='text/html; charset=windows-1252'>"),
           "windows-1252");
}

void TestCssParser() {
  Stylesheet sheet;
  ParseStylesheet(
      "/* c */ a, .b > p:hover { color: red !important; margin: 0 auto }"
      "@media (max-width: 600px) { .m { display: none } }"
      "@supports (display: flex) { .s { display: flex } }"
      "@supports (display: grid) { .g { display: grid } }"
      ".n { color: blue; & .child { color: green } }"
      "@import url(x.css);"
      "*, ::before, ::backdrop, ::file-selector-button { box-sizing: border-box }",
      sheet);
  CHECK_EQ(sheet.rules[0].selectors.size(), 2u);
  CHECK(sheet.rules[0].declarations[0].important);
  bool hasMedia = false, hasFlex = false, hasGrid = false, hasNested = false, hasStar = false;
  for (size_t i = 0; i < sheet.rules.size(); ++i) {
    const StyleRule& r = sheet.rules[i];
    if (!r.media.empty()) hasMedia = true;
    if (r.declarations[0].value == "flex") hasFlex = true;
    if (r.declarations[0].value == "grid") hasGrid = true;
    if (r.declarations[0].value == "green") hasNested = true;
    if (r.declarations[0].property == "box-sizing") hasStar = r.selectors.size() == 4;
  }
  CHECK(hasMedia);
  CHECK(hasFlex);
  CHECK(!hasGrid);  // grid layout reported as unsupported for @supports
  CHECK(hasNested);
  CHECK(hasStar);   // unknown pseudo-elements keep the rule valid
  CHECK_EQ(sheet.imports.size(), 1u);

  std::vector<ComplexSelector> sel;
  CHECK(ParseSelectorList("#a .b:nth-child(2n+1) > c[d^=e]", sel));
  CHECK_EQ(sel[0].specificity, 10000 + 300 + 1);
  sel.clear();
  CHECK(!ParseSelectorList("a:unknown-pseudo", sel));
  sel.clear();
  CHECK(ParseSelectorList(".md\\:flex", sel));
  CHECK_EQ(sel[0].compounds[0].parts[0].name, "md:flex");

  MediaContext ctx;
  ctx.width = 800;
  CHECK(EvaluateMediaQueryList("screen and (min-width: 700px)", ctx));
  CHECK(!EvaluateMediaQueryList("print", ctx));
  CHECK(EvaluateMediaQueryList("(width >= 40em)", ctx));
  CHECK(!EvaluateMediaQueryList("(400px <= width <= 700px)", ctx));
  CHECK(EvaluateMediaQueryList("not all and (min-width: 900px)", ctx));
  CHECK(EvaluateMediaQueryList("(prefers-color-scheme: light)", ctx));
}

void TestCssValues() {
  Color c;
  CHECK(ParseColor("#f00", c, Color()) && c == Color(255, 0, 0));
  CHECK(ParseColor("rgb(0 128 255 / 50%)", c, Color()) && c.b == 255 && c.a == 128);
  CHECK(ParseColor("hsl(120, 100%, 50%)", c, Color()) && c.g == 255 && c.r == 0);
  CHECK(ParseColor("rebeccapurple", c, Color()) && c == Color(0x66, 0x33, 0x99));
  CHECK(!ParseColor("notacolor", c, Color()));
  LengthContext lc = {10, 16, 1000, 500};
  Length l;
  CHECK(ParseLength("2em", lc, l) && l.Resolve(0) == 20);
  CHECK(ParseLength("1.5rem", lc, l) && l.Resolve(0) == 24);
  CHECK(ParseLength("10vw", lc, l) && l.Resolve(0) == 100);
  CHECK(ParseLength("calc(100% - 20px)", lc, l) && l.Resolve(200) == 180);
  CHECK(ParseLength("calc((100% - 2 * 10px) / 2)", lc, l) && l.Resolve(220) == 100);
  CHECK(ParseLength("clamp(1rem, 5vw, 40px)", lc, l) && l.Resolve(0) == 40);
  CHECK(ParseLength("max(10px, 2em)", lc, l) && l.Resolve(0) == 20);
  float n;
  CHECK(ParseNumber("calc(1 / .75)", n) && std::fabs(n - 1.3333f) < 0.01f);
  CHECK(!ParseNumber("12px", n));
  std::vector<std::pair<int, std::string> > out;
  CHECK(ExpandProperty("margin", "1px 2px", out));
  CHECK_EQ(out.size(), 4u);
  CHECK_EQ(out[3].second, "2px");
  out.clear();
  CHECK(ExpandProperty("border", "1px solid red", out));
  CHECK_EQ(out.size(), 12u);
  out.clear();
  CHECK(ExpandProperty("flex", "1", out));
  CHECK_EQ(out[2].second, "0%");
}

void TestCascade() {
  TestPage t(
      "<style>:root{--c:#00ff00} p{color:red} .x{color:var(--c)} #y{color:blue!important}"
      "p.x{font-size:20px} span{font-size:2em} .inh{color:inherit}</style>"
      "<p class=x id=y style='color:black'>a<span id=s>b</span></p><p id=z class=x>z</p>"
      "<div style='color:orange'><p class=inh id=i>i</p></div><font color=#123456 id=f>f</font>");
  Node* y = t.ById("y");
  CHECK(y->style->color == Color(0, 0, 255));          // !important beats inline
  CHECK(t.ById("z")->style->color == Color(0, 255, 0)); // custom property
  CHECK_EQ(y->style->fontSize, 20.0f);
  CHECK_EQ(t.ById("s")->style->fontSize, 40.0f);        // em relative to parent
  CHECK(t.ById("i")->style->color == Color(255, 165, 0));
  CHECK(t.ById("f")->style->color == Color(0x12, 0x34, 0x56));  // presentational hint
}

void TestBlockLayout() {
  TestPage t(
      "<body style='margin:0'><div id=a style='width:300px;height:50px;margin:10px auto'></div>"
      "<div id=b style='margin-top:30px;padding:5px;border:2px solid'>x</div>"
      "<div id=c style='width:50%;box-sizing:border-box;padding:10px'></div></body>",
      800);
  Rect a = t.RectOf("a");
  CHECK_NEAR(a.x, 250, 0.5f);  // centered by auto margins
  CHECK_NEAR(a.y, 10, 0.5f);
  CHECK_NEAR(a.w, 300, 0.5f);
  Rect b = t.RectOf("b");
  CHECK_NEAR(b.y, 90, 0.5f);  // max(10, 30) collapsed margin after a (10+50+30)
  Rect c = t.RectOf("c");
  CHECK_NEAR(c.w, 400, 0.5f);
}

void TestMarginCollapseThroughParent() {
  TestPage t("<body style='margin:0'><div id=o><p id=p style='margin:20px 0'>x</p></div></body>");
  CHECK_NEAR(t.RectOf("o").y, 20, 0.5f);
  CHECK_NEAR(t.RectOf("p").y, 20, 0.5f);
}

void TestFloatsAndClear() {
  TestPage t(
      "<body style='margin:0'><div id=w style='width:400px'>"
      "<div id=f style='float:left;width:100px;height:100px'></div>"
      "<div id=r style='float:right;width:50px;height:20px'></div>"
      "<div id=cl style='clear:both'>after</div></div></body>");
  CHECK_NEAR(t.RectOf("f").x, 0, 0.5f);
  CHECK_NEAR(t.RectOf("r").x, 350, 0.5f);
  CHECK_NEAR(t.RectOf("cl").y, 100, 0.5f);
}

void TestInlineWrapping() {
  TestPage t("<body style='margin:0'><p id=p style='width:100px;margin:0;font-size:10px;line-height:12px'>"
             "aaaa bbbb cccc dddd eeee ffff</p></body>");
  // SimpleFontProvider: 5px per char at 10px; "aaaa bbbb cccc dddd" = 95px.
  LayoutBox* box = t.ById("p")->layoutBox;
  CHECK(box != 0);
  CHECK_EQ(box->lines.size(), 2u);
  CHECK_NEAR(box->h, 24, 0.5f);
}

void TestTextAlignAndLists() {
  TestPage t("<body style='margin:0'><div id=d style='width:200px;text-align:center;font-size:10px'>"
             "<span id=s>abcd</span></div><ol><li id=l>x</li></ol></body>");
  LayoutBox* d = t.ById("d")->layoutBox;
  CHECK_EQ(d->lines.size(), 1u);
  bool centered = false;
  for (size_t i = 0; i < d->lines[0].fragments.size(); ++i)
    if (d->lines[0].fragments[i].kind == LineFragment::kText)
      centered = std::fabs(d->lines[0].fragments[i].x - 90) < 0.5f;
  CHECK(centered);
  CHECK_EQ(t.ById("l")->layoutBox->markerText, "1.");
}

void TestTables() {
  TestPage t(
      "<body style='margin:0'><table id=t style='width:300px;border-spacing:0'>"
      "<tr><td id=a style='width:100px;padding:0'>a</td><td id=b style='padding:0'>b</td></tr>"
      "<tr><td colspan=2 id=c style='padding:0'>wide</td></tr></table></body>");
  CHECK_NEAR(t.RectOf("t").w, 300, 0.5f);
  CHECK_NEAR(t.RectOf("a").w, 100, 1);
  CHECK_NEAR(t.RectOf("b").w, 200, 1);
  CHECK_NEAR(t.RectOf("c").w, 300, 1);
  CHECK(t.RectOf("c").y > t.RectOf("a").y);
}

void TestFlexbox() {
  TestPage t(
      "<body style='margin:0'><div style='display:flex;width:400px;gap:10px'>"
      "<div id=a style='flex:1'>a</div><div id=b style='width:100px'>b</div>"
      "<div id=c style='flex:2'>c</div></div>"
      "<div id=row style='display:flex;justify-content:space-between;width:300px'>"
      "<span id=l>left</span><span id=r>right</span></div>"
      "<div style='display:flex;flex-direction:column;align-items:center;width:200px'>"
      "<div id=cc style='width:50px;height:10px'></div></div></body>");
  // free space = 400 - 100 - 20 (gaps) - content of a,c (flex-basis 0) = 280
  CHECK_NEAR(t.RectOf("a").w, 280.0f / 3, 1);
  CHECK_NEAR(t.RectOf("c").w, 280.0f * 2 / 3, 1);
  CHECK_NEAR(t.RectOf("b").x, 280.0f / 3 + 10, 1);
  Rect r = t.RectOf("r");
  CHECK_NEAR(r.right(), 300, 1);
  CHECK_NEAR(t.RectOf("cc").x, 75, 0.5f);
}

void TestGrid() {
  TestPage t(
      "<body style='margin:0'><div style='display:grid;grid-template-columns:100px 1fr 1fr;"
      "width:500px;gap:20px'><div id=a>a</div><div id=b>b</div><div id=c>c</div>"
      "<div id=d style='grid-column:1 / -1'>d</div></div></body>");
  CHECK_NEAR(t.RectOf("a").w, 100, 0.5f);
  CHECK_NEAR(t.RectOf("b").w, 180, 0.5f);  // (500 - 100 - 2 * 20) / 2
  CHECK_NEAR(t.RectOf("c").x, 320, 0.5f);
  CHECK_NEAR(t.RectOf("d").w, 500, 0.5f);
  CHECK(t.RectOf("d").y > t.RectOf("a").y);
}

void TestPositioning() {
  TestPage t(
      "<body style='margin:0'><div id=p style='position:relative;width:200px;height:100px;"
      "margin-left:50px'><div id=a style='position:absolute;right:10px;bottom:10px;width:20px;"
      "height:20px'></div><div id=r style='position:relative;left:5px;top:7px'>r</div></div>"
      "<div id=h style='display:none'>hidden</div></body>");
  Rect a = t.RectOf("a");
  CHECK_NEAR(a.x, 50 + 200 - 10 - 20, 0.5f);
  CHECK_NEAR(a.y, 100 - 10 - 20, 0.5f);
  Rect r = t.RectOf("r");
  CHECK_NEAR(r.x, 55, 0.5f);
  CHECK_NEAR(r.y, 7, 0.5f);
  CHECK(t.ById("h")->layoutBox == 0);
}

void TestHitTestAndLinks() {
  TestPage t("<body style='margin:0'><a id=l href='other.html' style='display:block;height:40px'>"
             "link</a></body>");
  Node* hit = t.page.HitTest(5, 5);
  Node* link = Page::LinkFor(hit);
  CHECK(link == t.ById("l"));
  CHECK_EQ(t.page.LinkUrl(link), "http://test.local/dir/other.html");
}

void TestForms() {
  TestPage t(
      "<form id=f action='/s' method=get><input name=q value='a b'><input type=checkbox name=c "
      "checked><input type=checkbox name=d><select name=s><option>x<option selected value=y>Y"
      "</select><textarea name=t>line</textarea><input type=submit name=go value=Go id=go></form>");
  FormSubmission sub;
  CHECK(t.page.BuildFormSubmission(t.ById("f"), t.ById("go"), sub));
  CHECK_EQ(sub.method, "GET");
  CHECK_EQ(sub.url, "http://test.local/s?q=a+b&c=on&s=y&t=line&go=Go");
  Node* q = t.page.document()->root->FindFirst("input");
  q->formValue = "neu";
  q->formValueSet = true;
  CHECK(t.page.BuildFormSubmission(t.ById("f"), 0, sub));
  CHECK(sub.url.find("q=neu") != std::string::npos);
}

void TestAnchors() {
  TestPage t("<body style='margin:0'><div style='height:500px'></div><h2 id=target>T</h2></body>");
  CHECK_NEAR(t.page.AnchorPosition("target"), 500 + 19.92f, 1);
}

void TestHttpHelpers() {
  std::string out;
  CHECK(Inflate("hello", "", out) && out == "hello");
  CHECK_EQ(SniffMimeType("\x89PNG\r\n\x1a\nxxxx", ""), "image/png");
  CHECK_EQ(SniffMimeType("<!DOCTYPE html><p>", ""), "text/html");
  FetchResponse r;
  r.headers.push_back(std::make_pair(std::string("Content-Type"), std::string("text/html; charset=\"UTF-8\"")));
  CHECK_EQ(r.MimeType(), "text/html");
  CHECK_EQ(r.Charset(), "utf-8");
  CookieJar jar;
  Url u = Url::Parse("https://www.example.com/a/b");
  jar.SetFromHeader(u, "sid=1; Path=/; Domain=example.com; Secure");
  jar.SetFromHeader(u, "local=2");
  CHECK_EQ(jar.CookieHeader(Url::Parse("https://shop.example.com/")), "sid=1");
  CHECK_EQ(jar.CookieHeader(Url::Parse("http://www.example.com/a/x")), "local=2");
  CHECK_EQ(jar.CookieHeader(Url::Parse("https://evil.com/")), "");
  jar.SetFromHeader(u, "sid=; Max-Age=0; Path=/; Domain=example.com");
  CHECK_EQ(jar.CookieHeader(Url::Parse("https://shop.example.com/")), "");
}

void TestSvg() {
  DecodedImage img;
  CHECK(RenderSvgDocument("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'>"
                          "<rect width='10' height='5' fill='#f00'/></svg>",
                          20, 20, 1, img));
  CHECK_EQ(img.width, 20);
  uint32_t top = img.pixels[5 * 20 + 10], bottom = img.pixels[15 * 20 + 10];
  CHECK_EQ(top, 0xFFFF0000u);  // opaque red (premultiplied BGRA)
  CHECK_EQ(bottom >> 24, 0u);  // transparent
  float w, h;
  std::unique_ptr<Document> doc = ParseHtml("<svg width=48 viewBox='0 0 24 12'></svg>");
  CHECK(SvgIntrinsicSize(doc->root->FindFirst("svg"), w, h));
  // Real gradients, masks and clip paths.
  DecodedImage g;
  CHECK(RenderSvgDocument("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='10'>"
                          "<linearGradient id='l'><stop offset='0' stop-color='#f00'/><stop offset='1' stop-color='#00f'/>"
                          "</linearGradient><mask id='m'><rect width='50' height='10' fill='#fff'/></mask>"
                          "<rect width='100' height='10' fill='url(#l)' mask='url(#m)'/></svg>",
                          0, 0, 1, g));
  if (g.pixels.size() == 1000) {
    uint32_t left = g.pixels[5 * 100 + 1], mid = g.pixels[5 * 100 + 45], right = g.pixels[5 * 100 + 90];
    CHECK((left >> 16 & 255) > 240 && (left & 255) < 20);  // red end of the gradient
    CHECK((mid >> 16 & 255) > 110 && (mid & 255) > 90);     // mixed
    CHECK_EQ(right >> 24, 0u);                              // masked away
  }
  CHECK_EQ(w, 48.0f);
  CHECK_EQ(h, 24.0f);
}

#include "fixtures.inc"

unsigned SfntU16(const std::string& s, size_t p) { return ((unsigned char)s[p] << 8) | (unsigned char)s[p + 1]; }

// Returns the byte length of table |tag| in an sfnt file (0 if missing).
size_t SfntTableLength(const std::string& s, const char* tag) {
  unsigned n = SfntU16(s, 4);
  for (unsigned i = 0; i < n; ++i) {
    size_t rec = 12 + i * 16;
    if (s.compare(rec, 4, tag) == 0)
      return ((size_t)(unsigned char)s[rec + 12] << 24) | ((size_t)(unsigned char)s[rec + 13] << 16) |
             ((size_t)(unsigned char)s[rec + 14] << 8) | (unsigned char)s[rec + 15];
  }
  return 0;
}

void TestWebPAndWoff2() {
  DecodedImage img;
  CHECK(DecodeImage(kLossyWebp, img));
  CHECK_EQ(img.width, 16);
  if (!img.pixels.empty()) {
    uint32_t p = img.pixels[8 * 16 + 8];
    CHECK((p >> 16 & 255) > 240 && (p >> 8 & 255) < 20 && (p & 255) < 20);  // red
  }
  CHECK(DecodeImage(kLosslessWebp, img));
  CHECK_EQ(img.height, 8);
  CHECK(img.hasAlpha);
  if (img.pixels.size() == 64) {
    CHECK_EQ(img.pixels[0], 0u);                 // transparent top half
    CHECK_EQ(img.pixels[63], 0xFF0000FFu);       // opaque blue
  }
  CHECK(DecodeImage(kAnimatedWebp, img));      // first frame
  if (img.pixels.size() == 64) CHECK_EQ(img.pixels[0], 0xFF00FF00u);

  std::string sfnt;
  CHECK(FontToSfnt(kWoff2Font, sfnt));
  CHECK_EQ(SfntFamilyName(sfnt), "DejaVu Sans");
  CHECK_EQ(SfntTableLength(sfnt, "loca"), 8u * 4);  // 7 glyphs, long offsets
  CHECK_EQ(SfntTableLength(sfnt, "hmtx") >= 7u * 2, true);
  CHECK(SfntTableLength(sfnt, "glyf") > 0);
  std::string broken = kWoff2Font.substr(0, kWoff2Font.size() / 2);
  CHECK(!FontToSfnt(broken, sfnt));
}

class TestHost : public ScriptHost {
 public:
  std::string cookies, title, navigated, alerted;
  float sx = 0, sy = 0;
  std::string GetCookies(const std::string&) { return cookies; }
  void SetCookie(const std::string&, const std::string& c) {
    std::string kv = c.substr(0, c.find(';'));
    cookies = cookies.empty() ? kv : cookies + "; " + kv;
  }
  void Alert(const std::string& m) { alerted += m; }
  bool Confirm(const std::string&) { return true; }
  std::string Prompt(const std::string&, const std::string& d) { return d; }
  void Navigate(const std::string& url, bool) { navigated = url; }
  void SetTitle(const std::string& t) { title = t; }
  void ScrollTo(float x, float y) { sx = x; sy = y; }
  void GetScroll(float& x, float& y) { x = sx; y = sy; }
};

struct ScriptPage {
  SimpleFontProvider fonts;
  NoImages images;
  TestHost host;
  Page page;
  explicit ScriptPage(const std::string& html) : page(&fonts, &images) {
    page.SetScripting(&host, true);
    page.LoadHtml(html, "http://test.local/dir/page.html");
    page.Restyle(800, 600);
    page.Relayout(800, 600);
    page.RunScripts();
    if (page.script()->TakeDirty()) page.ScriptMutated();
  }
  Node* ById(const char* id) { return page.document()->root->FindById(id); }
  std::string Eval(const std::string& expr) {
    page.script()->Execute("document.getElementById('out').textContent = String(" + expr + ")",
                           "test", 0);
    return ById("out")->TextContent();
  }
  std::string Console() {
    std::string s;
    for (size_t i = 0; i < page.script()->console().size(); ++i) s += page.script()->console()[i] + "\n";
    return s;
  }
};

void TestScript() {
  ScriptPage p(
      "<html><head><title>T</title><style>.big{width:300px}</style></head><body>"
      "<div id=out></div><ul id=list><li class=a>1</li><li class='a b'>2</li></ul>"
      "<script>document.getElementById('list').insertAdjacentHTML('beforeend', '<li>3</li>');"
      "var order = ['inline'];"
      "document.addEventListener('DOMContentLoaded', function(){ order.push('dcl'); });"
      "window.onload = function(){ order.push('load'); };</script>"
      "<noscript><p id=ns>no js</p></noscript>"
      "<form id=f action=/search><input name=q value=x><button id=b>go</button></form>"
      "</body></html>");
  CHECK_EQ(p.Eval("document.querySelectorAll('#list li').length"), "3");
  CHECK_EQ(p.Eval("order.join()"), "inline,dcl,load");
  CHECK_EQ(p.Eval("document.title"), "T");
  CHECK_EQ(p.Eval("document.querySelector('.b').textContent"), "2");
  CHECK_EQ(p.Eval("document.getElementsByClassName('a').length"), "2");
  CHECK(p.ById("ns")->parent->HasAttr("hidden"));
  CHECK_EQ(p.Eval("location.pathname + location.search"), "/dir/page.html");
  CHECK_EQ(p.Eval("new URL('../x?y=1#z', location.href).href"), "http://test.local/x?y=1#z");
  CHECK_EQ(p.Eval("new URLSearchParams('a=1&b=x+y').get('b')"), "x y");
  CHECK_EQ(p.Eval("JSON.stringify([1,{a:2}])"), "[1,{\"a\":2}]");
  CHECK_EQ(p.Eval("btoa('Hello') + atob('SGk=')"), "SGVsbG8=Hi");

  // DOM manipulation and serialization.
  p.Eval("(function(){ var d = document.createElement('div'); d.id = 'made'; d.className = 'x y';"
         "d.dataset.fooBar = '7'; d.style.width = '120px'; d.appendChild(document.createTextNode('a<b'));"
         "document.body.appendChild(d); return 1; })()");
  Node* made = p.ById("made");
  CHECK(made != 0);
  CHECK_EQ(made->Attr("data-foo-bar"), "7");
  CHECK_EQ(made->Attr("style"), "width: 120px");
  CHECK_EQ(p.Eval("document.getElementById('made').outerHTML"),
           "<div id=\"made\" class=\"x y\" data-foo-bar=\"7\" style=\"width: 120px\">a&lt;b</div>");
  CHECK_EQ(p.Eval("document.getElementById('made').classList.contains('y')"), "true");
  CHECK_EQ(p.Eval("(function(){var m=document.getElementById('made'); m.classList.toggle('x'); return m.className;})()"), "y");

  // Layout queries see script changes.
  CHECK_EQ(p.Eval("document.getElementById('made').getBoundingClientRect().width"), "120");
  CHECK_EQ(p.Eval("(function(){var m=document.getElementById('made'); m.className='big'; m.removeAttribute('style'); return m.offsetWidth;})()"), "300");
  CHECK_EQ(p.Eval("getComputedStyle(document.getElementById('made')).display"), "block");

  // Events: bubbling, preventDefault, inline handlers.
  p.Eval("(function(){ window.log = []; var b = document.getElementById('b');"
         "document.body.addEventListener('click', function(e){ log.push('body:' + e.target.id); });"
         "b.addEventListener('click', function(e){ log.push('btn'); e.preventDefault(); });"
         "document.getElementById('f').onsubmit = function(){ log.push('submit'); return false; };"
         "return 1; })()");
  CHECK(!p.page.script()->DispatchEvent(p.ById("b"), "click"));
  CHECK_EQ(p.Eval("log.join()"), "btn,body:b");
  CHECK(!p.page.script()->DispatchEvent(p.ById("f"), "submit"));
  CHECK_EQ(p.Eval("log.join()"), "btn,body:b,submit");

  // Form values.
  p.Eval("document.querySelector('input[name=q]').value = 'kite'");
  FormSubmission sub;
  CHECK(p.page.BuildFormSubmission(p.ById("f"), 0, sub));
  CHECK_EQ(sub.url, "http://test.local/search?q=kite");

  // Timers and promises.
  p.Eval("(function(){ window.t = []; setTimeout(function(){ t.push('timeout'); }, 0);"
         "Promise.resolve().then(function(){ t.push('micro'); }); return 1; })()");
  CHECK_EQ(p.Eval("t.join()"), "micro");
  CHECK(p.page.script()->NextTimerDelay() >= 0);
  p.page.script()->RunDueTimers();
  CHECK_EQ(p.Eval("t.join()"), "micro,timeout");

  // fetch() goes through the host's request queue.
  p.Eval("(function(){ window.got = ''; fetch('/api?x=1').then(function(r){ return r.json(); })"
         ".then(function(j){ got = j.v; }); return 1; })()");
  std::vector<ScriptRequest> reqs = p.page.script()->TakeRequests();
  CHECK_EQ(reqs.size(), 1u);
  if (!reqs.empty()) {
    CHECK_EQ(reqs[0].url, "http://test.local/api?x=1");
    std::vector<std::pair<std::string, std::string> > hdrs;
    hdrs.push_back(std::make_pair(std::string("content-type"), std::string("application/json")));
    p.page.script()->DeliverResponse(reqs[0].id, 200, "OK", "{\"v\":42}", hdrs, reqs[0].url, false);
  }
  CHECK_EQ(p.Eval("got"), "42");

  // Cookies, title, navigation and dynamically inserted scripts.
  p.Eval("document.cookie = 'a=1; path=/'");
  CHECK_EQ(p.host.cookies, "a=1");
  p.Eval("document.title = 'Neu'");
  CHECK_EQ(p.host.title, "Neu");
  p.Eval("(function(){ var s = document.createElement('script'); s.textContent = 'window.dyn = 5';"
         "document.head.appendChild(s); return 1; })()");
  p.page.RunScripts();
  CHECK_EQ(p.Eval("window.dyn"), "5");
  p.Eval("location.href = 'other.html'");
  CHECK_EQ(p.host.navigated, "http://test.local/dir/other.html");

  // Errors land in the console instead of aborting.
  p.page.script()->Execute("undefinedFunction()", "err.js", 0);
  CHECK(p.Console().find("undefinedFunction") != std::string::npos);
  // Runaway scripts are interrupted.
  p.page.script()->SetTimeLimit(300);
  p.page.script()->Execute("for(;;){}", "loop.js", 0);
  CHECK_EQ(p.Eval("1+1"), "2");
}

void TestCanvas() {
  ScriptPage p("<html><body><div id=out></div><canvas id=c width=100 height=50></canvas></body></html>");
  // px(x, y) -> "r,g,b,a"
  p.page.script()->Execute(
      "var c = document.getElementById('c'), g = c.getContext('2d');"
      "function px(x, y) { return Array.from(g.getImageData(x, y, 1, 1).data).join(); }",
      "setup", 0);
  CHECK_EQ(p.Eval("g === c.getContext('2d') && c.getContext('webgl') === null"), "true");
  CHECK_EQ(p.Eval("px(5, 5)"), "0,0,0,0");
  CHECK_EQ(p.Eval("(g.fillStyle = 'red', g.fillRect(0, 0, 10, 10), px(5, 5))"), "255,0,0,255");
  CHECK_EQ(p.Eval("px(10, 5)"), "0,0,0,0");
  // Semi-transparent blue over red.
  CHECK_EQ(p.Eval("(g.fillStyle = 'rgba(0,0,255,0.5)', g.fillRect(0, 0, 10, 10), px(5, 5))"), "127,0,128,255");
  // Paths: a filled circle and a stroked line.
  CHECK_EQ(p.Eval("(g.fillStyle = '#00ff00', g.beginPath(), g.arc(50, 25, 10, 0, Math.PI * 2), g.fill(), px(50, 25))"),
           "0,255,0,255");
  CHECK_EQ(p.Eval("px(50, 10)"), "0,0,0,0");
  CHECK_EQ(p.Eval("g.isPointInPath(50, 25) + ',' + g.isPointInPath(70, 25)"), "true,false");
  CHECK_EQ(p.Eval("(g.strokeStyle = 'black', g.lineWidth = 4, g.beginPath(), g.moveTo(20, 40), g.lineTo(90, 40),"
                  " g.stroke(), px(60, 40))"),
           "0,0,0,255");
  CHECK_EQ(p.Eval("px(60, 45)"), "0,0,0,0");
  // Transforms, save/restore.
  CHECK_EQ(p.Eval("(g.save(), g.translate(80, 0), g.scale(2, 2), g.fillStyle = 'blue', g.fillRect(0, 0, 5, 5),"
                  " g.restore(), px(89, 9) + '|' + g.fillStyle + '|' + g.getTransform().e)"),
           "0,0,255,255|#00ff00|0");
  // clearRect, globalAlpha, composite operation.
  CHECK_EQ(p.Eval("(g.clearRect(0, 0, 10, 10), px(5, 5))"), "0,0,0,0");
  CHECK_EQ(p.Eval("(g.fillStyle = 'white', g.fillRect(0, 0, 4, 4), g.globalCompositeOperation = 'destination-out',"
                  " g.fillRect(0, 0, 2, 2), g.globalCompositeOperation = 'source-over', px(1, 1) + '|' + px(3, 3))"),
           "0,0,0,0|255,255,255,255");
  // Gradients.
  CHECK_EQ(p.Eval("(function(){ var gr = g.createLinearGradient(0, 0, 100, 0); gr.addColorStop(0, 'black');"
                  " gr.addColorStop(1, 'white'); g.fillStyle = gr; g.fillRect(0, 45, 100, 5);"
                  " var a = g.getImageData(0, 47, 100, 1).data; return a[0] < 10 && a[99*4] > 245 && a[50*4] > 110 && a[50*4] < 145; })()"),
           "true");
  // Image data round trip and drawing a canvas onto another.
  CHECK_EQ(p.Eval("(function(){ var d = g.createImageData(2, 2); for (var i = 0; i < 16; i += 4) { d.data[i] = 10;"
                  " d.data[i + 3] = 255; } g.putImageData(d, 30, 0); return px(31, 1); })()"),
           "10,0,0,255");
  CHECK_EQ(p.Eval("(function(){ var o = document.createElement('canvas'); o.width = 20; o.height = 20;"
                  " var h = o.getContext('2d'); h.drawImage(c, 30, 0, 2, 2, 0, 0, 20, 20);"
                  " return Array.from(h.getImageData(10, 10, 1, 1).data).join(); })()"),
           "10,0,0,255");
  // Path2D (also from SVG path data), dashes, PNG export.
  CHECK_EQ(p.Eval("(function(){ g.clearRect(0, 0, 100, 50); var q = new Path2D('M0 0H20V20H0Z'); g.fillStyle = 'red';"
                  " g.fill(q); return px(10, 10) + '|' + g.isPointInPath(q, 25, 10); })()"),
           "255,0,0,255|false");
  CHECK_EQ(p.Eval("(g.setLineDash([4, 2]), g.getLineDash().join())"), "4,2");
  CHECK_EQ(p.Eval("(g.font = 'bold 20px Arial', g.font + '|' + (g.measureText('Hallo').width > 40))"),
           "bold 20px Arial|true");
  CHECK_EQ(p.Eval("(g.font = 'nonsense', g.font)"), "bold 20px Arial");
  CHECK_EQ(p.Eval("c.toDataURL().slice(0, 22)"), "data:image/png;base64,");
  CHECK_EQ(p.Eval("(c.width = 10, px(5, 5) + '|' + g.fillStyle)"), "0,0,0,0|#000000");
  // The canvas is painted as an image.
  p.page.script()->TakeDirty();
  p.page.ScriptMutated();
  bool painted = false;
  for (size_t i = 0; i < p.page.display().items.size(); ++i)
    if (StartsWith(p.page.display().items[i].imageUrl, "kite-canvas:")) painted = true;
  CHECK(painted);
}

double g_fakeNow = 1000;
double FakeClock() { return g_fakeNow; }

void TestAnimations() {
  TimingFunction ease = TimingFunction::Parse("ease");
  CHECK_NEAR(ease.Apply(0.5f), 0.8024f, 0.002f);
  CHECK_NEAR(TimingFunction::Parse("linear").Apply(0.3f), 0.3f, 1e-6f);
  CHECK_NEAR(TimingFunction::Parse("steps(4)").Apply(0.3f), 0.25f, 1e-6f);
  CHECK_NEAR(TimingFunction::Parse("steps(4, start)").Apply(0.3f), 0.5f, 1e-6f);
  CHECK_NEAR(TimingFunction::Parse("cubic-bezier(0,0,1,1)").Apply(0.7f), 0.7f, 0.002f);

  SetAnimationClockForTesting(FakeClock);
  g_fakeNow = 1000;
  ScriptPage p(
      "<html><head><style>"
      "@keyframes fade { from { opacity: 0 } to { opacity: 1 } }"
      "@-webkit-keyframes slide { 0% { transform: translateX(0) } 100% { transform: translateX(100px) } }"
      "@keyframes pulse { 50% { background-color: rgb(255, 0, 0) } }"
      "#a { animation: fade 1s linear }"
      "#b { animation: slide 1s linear 2 alternate forwards }"
      "#c { animation: pulse 2s linear infinite; background-color: rgb(0, 0, 255) }"
      "#d { opacity: 1; transition: opacity 1s linear, width 2s }"
      "#d.hide { opacity: 0 }"
      "#e { animation: fade 1s steps(2) 500ms both }"
      "</style></head><body><div id=out></div>"
      "<div id=a>a</div><div id=b>b</div><div id=c>c</div><div id=d>d</div><div id=e>e</div>"
      "<script>window.ends = []; document.getElementById('d').addEventListener('transitionend',"
      " function() { ends.push('d'); }); document.getElementById('a').addEventListener('animationend',"
      " function() { ends.push('a'); });</script></body></html>");
  Node* a = p.ById("a");
  Node* b = p.ById("b");
  Node* c = p.ById("c");
  Node* d = p.ById("d");
  Node* e = p.ById("e");
  CHECK_NEAR(a->style->opacity, 0.0f, 1e-4f);
  CHECK(p.page.AnimationsActive());
  g_fakeNow = 1500;
  CHECK_EQ(p.page.TickAnimations(), 1);  // repaint only
  CHECK_NEAR(a->style->opacity, 0.5f, 1e-3f);
  CHECK_NEAR(b->style->translateX.px, 50.0f, 0.1f);
  CHECK(std::abs((int)c->style->backgroundColor.r - 127) <= 1 && std::abs((int)c->style->backgroundColor.b - 127) <= 1);
  CHECK_NEAR(e->style->opacity, 0.0f, 1e-4f);  // delay, fill backwards
  g_fakeNow = 2250;  // second iteration of #b runs backwards
  p.page.TickAnimations();
  CHECK_NEAR(a->style->opacity, 1.0f, 1e-4f);  // finished, back to the base value
  CHECK_NEAR(b->style->translateX.px, 75.0f, 0.1f);
  CHECK_NEAR(e->style->opacity, 0.5f, 1e-4f);  // steps(2)
  g_fakeNow = 5000;
  p.page.TickAnimations();
  CHECK_NEAR(b->style->translateX.px, 0.0f, 0.1f);  // fill forwards keeps the last frame
  CHECK_NEAR(e->style->opacity, 1.0f, 1e-4f);
  // Transitions start when a style change is applied.
  p.page.script()->Execute("document.getElementById('d').className = 'hide'", "t", 0);
  p.page.script()->TakeDirty();
  p.page.ScriptMutated();
  CHECK_NEAR(d->style->opacity, 1.0f, 1e-4f);
  g_fakeNow = 5250;
  p.page.TickAnimations();
  CHECK_NEAR(d->style->opacity, 0.75f, 1e-3f);
  g_fakeNow = 6100;
  p.page.TickAnimations();
  CHECK_NEAR(d->style->opacity, 0.0f, 1e-4f);
  CHECK_EQ(p.Eval("ends.join()"), "a,d");
  CHECK(p.page.AnimationsActive());  // #c runs forever
  SetAnimationClockForTesting(0);
}

}  // namespace

int main() {
  TestStrings();
  TestUrl();
  TestEntities();
  TestHtmlParser();
  TestCssParser();
  TestCssValues();
  TestCascade();
  TestBlockLayout();
  TestMarginCollapseThroughParent();
  TestFloatsAndClear();
  TestInlineWrapping();
  TestTextAlignAndLists();
  TestTables();
  TestFlexbox();
  TestGrid();
  TestPositioning();
  TestHitTestAndLinks();
  TestForms();
  TestAnchors();
  TestHttpHelpers();
  TestSvg();
  TestScript();
  TestWebPAndWoff2();
  TestCanvas();
  TestAnimations();
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
