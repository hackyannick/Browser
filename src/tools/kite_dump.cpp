// kite-dump: headless driver for the Kite engine (used for development and
// testing on non-Windows hosts). Loads a page, lays it out and prints the
// box tree / display list, optionally rendering a PPM preview.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>

#include "base/strings.h"
#include "html/parser.h"
#include "image/image.h"
#include "image/svg.h"
#include "net/http.h"
#include "page/page.h"

using namespace kite;

namespace {

std::string ReadFile(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return std::string();
  std::string out;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

class HeadlessImages : public ImageProvider {
 public:
  bool fetch = false;
  std::map<std::string, DecodedImage> images;
  std::map<std::string, bool> failed;
  State GetImage(const std::string& url, int& w, int& h) {
    std::map<std::string, DecodedImage>::iterator it = images.find(url);
    if (it != images.end()) {
      w = it->second.width;
      h = it->second.height;
      return kLoaded;
    }
    if (failed.count(url)) return kFailed;
    return fetch ? kLoading : kFailed;
  }
  void Load(const std::string& url) {
    if (images.count(url) || failed.count(url)) return;
    FetchRequest req;
    req.url = url;
    req.accept = "image/png,image/jpeg,image/gif,*/*;q=0.5";
    FetchResponse r = Network::Get().Fetch(req);
    DecodedImage img;
    if (r.ok && r.status == 200 && (DecodeImage(r.body, img) ||
                                    (LooksLikeSvg(r.body) && RenderSvgDocument(r.body, 0, 0, 1, img))))
      images[url] = img;
    else failed[url] = true;
  }
};

void DumpBox(const LayoutBox* b, int depth) {
  std::string pad(depth * 2, ' ');
  static const char* kinds[] = {"Block", "Inline", "Text", "Replaced", "Br", "Table",
                                "RowGroup", "Row", "Cell", "Caption", "Flex", "Grid"};
  std::string name = b->node ? b->node->tag : std::string("(anon)");
  if (b->node && !b->node->id.empty()) name += "#" + b->node->id;
  if (b->node && !b->node->classes.empty()) name += "." + b->node->classes[0];
  if (b->kind == LayoutBox::kText) {
    std::string t = b->text.substr(0, 40);
    printf("%sText \"%s\"\n", pad.c_str(), ReplaceAll(t, "\n", "\\n").c_str());
    return;
  }
  printf("%s%s <%s> x=%.1f y=%.1f w=%.1f h=%.1f%s%s%s\n", pad.c_str(), kinds[b->kind], name.c_str(),
         b->x, b->y, b->w, b->h, b->inlineContent ? " inline" : "",
         b->isPositionedChild ? " positioned" : "", b->IsFloat() ? " FLOAT" : "");
  for (size_t i = 0; i < b->lines.size(); ++i) {
    const LineBox& lb = b->lines[i];
    std::string text;
    for (size_t k = 0; k < lb.fragments.size(); ++k)
      if (lb.fragments[k].kind == LineFragment::kText) text += lb.fragments[k].text;
      else if (lb.fragments[k].kind == LineFragment::kAtomic) text += "[*]";
    printf("%s  line y=%.1f h=%.1f: %s\n", pad.c_str(), lb.y, lb.h, text.substr(0, 100).c_str());
  }
  if (b->kind == LayoutBox::kInline) return;
  for (size_t i = 0; i < b->children.size(); ++i) {
    const LayoutBox* c = b->children[i].get();
    if (c->kind == LayoutBox::kText || c->kind == LayoutBox::kInline) continue;
    DumpBox(c, depth + 1);
  }
}

// Minimal software rasterizer for previews (text drawn as grey bars).
void WritePpm(const DisplayList& dl, int width, int height, const char* path) {
  std::vector<unsigned char> px((size_t)width * height * 3);
  for (size_t i = 0; i < (size_t)width * height; ++i) {
    px[i * 3] = dl.background.r;
    px[i * 3 + 1] = dl.background.g;
    px[i * 3 + 2] = dl.background.b;
  }
  std::vector<Rect> clips;
  auto fill = [&](Rect r, Color c) {
    if (!clips.empty()) r = r.intersect(clips.back());
    int x0 = std::max(0, (int)(r.x + 0.5f)), y0 = std::max(0, (int)(r.y + 0.5f));
    int x1 = std::min(width, (int)(r.right() + 0.5f)), y1 = std::min(height, (int)(r.bottom() + 0.5f));
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        unsigned char* p = &px[((size_t)y * width + x) * 3];
        p[0] = (unsigned char)((p[0] * (255 - c.a) + c.r * c.a) / 255);
        p[1] = (unsigned char)((p[1] * (255 - c.a) + c.g * c.a) / 255);
        p[2] = (unsigned char)((p[2] * (255 - c.a) + c.b * c.a) / 255);
      }
  };
  for (size_t i = 0; i < dl.items.size(); ++i) {
    const DisplayItem& it = dl.items[i];
    switch (it.type) {
      case DisplayItem::kRect: fill(it.rect, it.color); break;
      case DisplayItem::kEllipse: fill(it.rect, it.color); break;
      case DisplayItem::kRoundRect: if (it.ring == 0) fill(it.rect, it.color); break;
      default: break;
      case DisplayItem::kText: {
        Color c = it.color;
        c.a = (unsigned char)(c.a * 0.55f);
        float h = it.font.size * 0.62f;
        fill(Rect(it.rect.x, it.baseline - h, it.rect.w, h), c);
        if (it.underline) fill(Rect(it.rect.x, it.baseline + 1, it.rect.w, 1), it.decorationColor);
        break;
      }
      case DisplayItem::kImage: {
        Rect r(it.tileX, it.tileY, it.tileW, it.tileH);
        if (!it.repeatX && !it.repeatY) r = r.intersect(it.rect);
        else r = it.rect;
        fill(r, Color(150, 180, 210, 200));
        break;
      }
      case DisplayItem::kPushClip:
        clips.push_back(clips.empty() ? it.rect : it.rect.intersect(clips.back()));
        break;
      case DisplayItem::kPopClip:
        if (!clips.empty()) clips.pop_back();
        break;
    }
  }
  FILE* f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", width, height);
  fwrite(&px[0], 1, px.size(), f);
  fclose(f);
}

class HeadlessHost : public ScriptHost {
 public:
  std::string cookies;
  std::string GetCookies(const std::string&) { return cookies; }
  void SetCookie(const std::string&, const std::string& c) {
    std::string kv = c.substr(0, c.find(';'));
    cookies = cookies.empty() ? kv : cookies + "; " + kv;
  }
  void Alert(const std::string& m) { fprintf(stderr, "alert: %s\n", m.c_str()); }
  bool Confirm(const std::string&) { return true; }
  std::string Prompt(const std::string&, const std::string& d) { return d; }
  void Navigate(const std::string& url, bool) { fprintf(stderr, "navigate: %s\n", url.c_str()); }
  void SetTitle(const std::string&) {}
  void ScrollTo(float, float) {}
  void GetScroll(float& x, float& y) { x = y = 0; }
};

// Runs scripts, timers and fetch() requests synchronously for a while.
void RunScriptsHeadless(Page& page) {
  ScriptEngine* js = page.script();
  if (!js) return;
  for (int round = 0; round < 50; ++round) {
    std::vector<std::string> pending = page.PendingScripts();
    for (size_t i = 0; i < pending.size(); ++i) {
      FetchRequest sr;
      sr.url = pending[i];
      sr.accept = "*/*";
      FetchResponse s = Network::Get().Fetch(sr);
      bool ok = s.ok && s.status == 200;
      fprintf(stderr, "script %s: %s (%zu bytes)\n", pending[i].c_str(), ok ? "ok" : "failed", s.body.size());
      page.ProvideScript(pending[i], ConvertToUtf8(s.body, s.Charset().empty() ? "utf-8" : s.Charset()), ok);
    }
    bool ran = page.RunScripts();
    std::vector<ScriptRequest> reqs = js->TakeRequests();
    for (size_t i = 0; i < reqs.size(); ++i) {
      FetchRequest fr;
      fr.url = reqs[i].url;
      fr.method = reqs[i].method;
      fr.body = reqs[i].body;
      for (size_t k = 0; k < reqs[i].headers.size(); ++k)
        if (AsciiLower(reqs[i].headers[k].first) == "content-type") fr.contentType = reqs[i].headers[k].second;
      FetchResponse s = Network::Get().Fetch(fr);
      fprintf(stderr, "fetch() %s %s: %d\n", reqs[i].method.c_str(), reqs[i].url.c_str(), s.status);
      std::vector<std::pair<std::string, std::string> > hdrs;
      for (size_t k = 0; k < s.headers.size(); ++k)
        hdrs.push_back(std::make_pair(AsciiLower(s.headers[k].first), s.headers[k].second));
      js->DeliverResponse(reqs[i].id, s.status, "", ConvertToUtf8(s.body, s.Charset().empty() ? "utf-8" : s.Charset()),
                          hdrs, s.finalUrl, !s.ok);
    }
    int delay = js->NextTimerDelay();
    if (delay >= 0 && delay <= 200 && round < 49) {
      if (delay > 0) usleep(delay * 1000);
      js->RunDueTimers();
      ran = true;
    }
    if (!ran && reqs.empty() && pending.empty() && page.scriptsFinished()) break;
  }
  for (size_t i = 0; i < js->console().size(); ++i) fprintf(stderr, "console: %s\n", js->console()[i].c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::string target, ppm, caFile = "resources/cacert.pem";
  float width = 1024, height = 768;
  bool tree = true, dl = false, images = false, js = false;
  std::string inspect;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--width" && i + 1 < argc) width = (float)atof(argv[++i]);
    else if (a == "--height" && i + 1 < argc) height = (float)atof(argv[++i]);
    else if (a == "--ppm" && i + 1 < argc) ppm = argv[++i];
    else if (a == "--ca" && i + 1 < argc) caFile = argv[++i];
    else if (a == "--no-tree") tree = false;
    else if (a == "--dl") dl = true;
    else if (a == "--images") images = true;
    else if (a == "--js") js = true;
    else if (a == "--inspect" && i + 1 < argc) inspect = argv[++i];
    else target = a;
  }
  if (target.empty()) {
    fprintf(stderr, "usage: kite-dump [--width N] [--ppm out.ppm] [--dl] [--images] [--js] [--ca file] <file|url>\n");
    return 2;
  }
  NetInit();
  int anchors = LoadTrustAnchors(ReadFile(caFile));
  const char* proxyEnv = getenv("HTTPS_PROXY");
  if (!proxyEnv) proxyEnv = getenv("https_proxy");
  if (proxyEnv) {
    Url pu = Url::Parse(proxyEnv);
    if (pu.valid()) {
      ProxyConfig pc;
      pc.host = pu.host();
      pc.port = pu.EffectivePort();
      Network::Get().SetProxy(pc);
    }
  }
  std::string url = target;
  if (target.find("://") == std::string::npos && target.find("about:") != 0) {
    char buf[4096];
    std::string path = target;
    if (target[0] != '/' && getcwd(buf, sizeof buf)) path = std::string(buf) + "/" + target;
    url = Url::FromLocalPath(path).Spec();
  }
  FetchRequest req;
  req.url = url;
  FetchResponse r = Network::Get().Fetch(req);
  if (!r.ok) {
    fprintf(stderr, "fetch failed: %s (anchors loaded: %d)\n", r.error.c_str(), anchors);
    return 1;
  }
  fprintf(stderr, "status %d, %zu bytes, type %s, final %s\n", r.status, r.body.size(),
          r.MimeType().c_str(), r.finalUrl.c_str());
  std::string charset = r.Charset();
  if (charset.empty()) charset = SniffHtmlCharset(r.body);
  std::string html = ConvertToUtf8(r.body, charset);

  SimpleFontProvider fonts;
  HeadlessImages imgs;
  imgs.fetch = images;
  Page page(&fonts, &imgs);
  HeadlessHost host;
  page.SetScripting(&host, js);
  page.LoadHtml(html, r.finalUrl);
  for (int round = 0; round < 4; ++round) {
    std::vector<std::string> pending = page.PendingStylesheets();
    if (pending.empty()) break;
    for (size_t i = 0; i < pending.size(); ++i) {
      FetchRequest sr;
      sr.url = pending[i];
      sr.accept = "text/css,*/*;q=0.1";
      FetchResponse s = Network::Get().Fetch(sr);
      bool ok = s.ok && s.status == 200;
      fprintf(stderr, "stylesheet %s: %s (%zu bytes)\n", pending[i].c_str(),
              ok ? "ok" : (s.error.empty() ? "http error" : s.error.c_str()), s.body.size());
      page.ProvideStylesheet(pending[i], ConvertToUtf8(s.body, s.Charset()), ok);
    }
  }
  page.Restyle(width, height);
  page.Relayout(width, height);
  if (js) {
    RunScriptsHeadless(page);
    page.ScriptMutated();
  }
  if (images) {
    std::vector<std::string> urls = page.ReferencedImages();
    for (size_t i = 0; i < urls.size(); ++i) imgs.Load(urls[i]);
    imgs.fetch = false;
    page.Relayout(width, height);
  }
  fprintf(stderr, "title: %s\ncontent size: %.0f x %.0f, %zu display items, %zu hit regions\n",
          page.Title().c_str(), page.ContentWidth(), page.ContentHeight(),
          page.display().items.size(), page.display().hits.size());
  Node* html_ = page.document()->DocumentElement();
  if (tree && html_ && html_->layoutBox) {
    LayoutBox* root = html_->layoutBox;
    while (root->parent) root = root->parent;
    DumpBox(root, 0);
  }
  if (!inspect.empty()) {
    std::vector<Node*> stack(1, page.document()->root.get());
    while (!stack.empty()) {
      Node* n = stack.back();
      stack.pop_back();
      for (size_t i = 0; i < n->children.size(); ++i) stack.push_back(n->children[i].get());
      if (!n->IsElement() || !n->style) continue;
      bool match = n->tag == inspect || n->id == inspect;
      for (size_t i = 0; i < n->classes.size(); ++i) if (n->classes[i] == inspect) match = true;
      if (!match) continue;
      const ComputedStyle* s = n->style;
      printf("<%s> display=%d pos=%d float=%d width=%d/%.1f/%.1f%% maxw=%d/%.1f margin=[%d %.1f, %d %.1f] flexGrow=%.1f shrink=%.1f basis=%d/%.1f/%.1f%% font=%s %.1fpx\n",
             n->tag.c_str(), s->display, s->position, s->floating, s->width.kind, s->width.px, s->width.pct,
             s->maxWidth.kind, s->maxWidth.px, s->margin[3].kind, s->margin[3].px, s->margin[1].kind,
             s->margin[1].px, s->flexGrow, s->flexShrink, s->flexBasis.kind, s->flexBasis.px,
             s->flexBasis.pct, s->fontFamily.c_str(), s->fontSize);
    }
  }
  {
    // Report items that stick out to the right of the viewport.
    const DisplayList& d = page.display();
    int reported = 0;
    std::vector<Rect> clips;
    for (size_t i = 0; i < d.items.size() && reported < 15; ++i) {
      const DisplayItem& it = d.items[i];
      if (it.type == DisplayItem::kPushClip) { clips.push_back(it.rect); continue; }
      if (it.type == DisplayItem::kPopClip) { if (!clips.empty()) clips.pop_back(); continue; }
      Rect r = it.rect;
      for (size_t c = 0; c < clips.size(); ++c) r = r.intersect(clips[c]);
      if (r.w > 0 && r.right() > width + 1) {
        fprintf(stderr, "overflow: type=%d x=%.0f w=%.0f right=%.0f text=%s\n", it.type, r.x, r.w, r.right(),
                it.text.substr(0, 40).c_str());
        ++reported;
      }
    }
  }
  if (dl) {
    const DisplayList& d = page.display();
    for (size_t i = 0; i < d.items.size(); ++i) {
      const DisplayItem& it = d.items[i];
      if (it.type == DisplayItem::kText)
        printf("text %.0f,%.0f %s %.0fpx w%d: %s\n", it.rect.x, it.baseline, it.font.family.c_str(),
               it.font.size, it.font.weight, it.text.c_str());
      else if (it.type == DisplayItem::kRect)
        printf("rect %.0f,%.0f %.0fx%.0f #%02x%02x%02x/%d\n", it.rect.x, it.rect.y, it.rect.w,
               it.rect.h, it.color.r, it.color.g, it.color.b, it.color.a);
      else if (it.type == DisplayItem::kImage)
        printf("image %.0f,%.0f %.0fx%.0f %s\n", it.tileX, it.tileY, it.tileW, it.tileH,
               it.imageUrl.c_str());
    }
  }
  if (!ppm.empty()) {
    int h = (int)std::min(8000.0f, std::max(height, page.ContentHeight()));
    WritePpm(page.display(), (int)width, h, ppm.c_str());
  }
  return 0;
}
