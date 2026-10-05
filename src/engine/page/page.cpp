#include "page/page.h"

#include <set>

#include "base/strings.h"
#include "html/parser.h"

namespace kite {

Page::Page(FontProvider* fonts, ImageProvider* images)
    : fonts_(fonts), images_(images), engine_(fonts, images), refreshDelay_(-1) {}

Page::~Page() {}

void Page::LoadHtml(const std::string& utf8, const std::string& url) {
  script_.reset();  // references the old document
  anim_.Clear();
  scripts_.clear();
  formQueue_.clear();
  contentLoadedFired_ = loadFired_ = false;
  url_ = url;
  baseUrl_ = Url::Parse(url);
  doc_ = ParseHtml(utf8);
  root_.reset();
  sheets_.clear();
  requestedFonts_.clear();
  refreshDelay_ = -1;
  refreshUrl_.clear();
  CollectDocumentInfo();
  if (scriptingEnabled() && doc_) {
    std::vector<Node*> ns;
    doc_->root->FindAll("noscript", ns);
    for (size_t i = 0; i < ns.size(); ++i) ns[i]->SetAttr("hidden", "");
    CollectScripts(doc_->root.get());
    script_.reset(new ScriptEngine(this, scriptHost_));
  }
}

void Page::LoadPlainText(const std::string& utf8, const std::string& url) {
  std::string html = "<!DOCTYPE html><html><head><title>" + HtmlEscape(url) +
                     "</title></head><body style=\"margin:8px\"><pre style=\"white-space:pre-wrap;"
                     "font-family:monospace;font-size:13px;margin:0\">" +
                     HtmlEscape(utf8) + "</pre></body></html>";
  LoadHtml(html, url);
}

void Page::LoadImageDocument(const std::string& url) {
  std::string name = url.substr(url.rfind('/') + 1);
  std::string html = "<!DOCTYPE html><html><head><title>" + HtmlEscape(name) +
                     "</title></head><body style=\"margin:0;background:#808080;text-align:center\">"
                     "<img style=\"max-width:100%;margin-top:8px;background:#fff\" src=\"" +
                     HtmlEscape(url) + "\"></body></html>";
  LoadHtml(html, url);
}

std::string Page::Title() const { return doc_ ? doc_->Title() : std::string(); }

void Page::AddSheetsFromNode(Node* n) {
  if (n->IsElement()) {
    if (n->tag == "style") {
      std::string type = AsciiLower(n->Attr("type"));
      if (type.empty() || type == "text/css") {
        SheetEntry e;
        e.isLink = false;
        e.css = n->TextContent();
        e.media = n->Attr("media");
        e.loaded = true;
        e.failed = false;
        e.sheet.reset(new Stylesheet);
        ParseStylesheet(e.css, *e.sheet);
        // @import inside <style>.
        for (size_t i = 0; i < e.sheet->imports.size(); ++i) {
          Url u = baseUrl_.Resolve(e.sheet->imports[i]);
          if (!u.valid()) continue;
          SheetEntry imp;
          imp.isLink = true;
          imp.url = u.SpecNoFragment();
          imp.loaded = imp.failed = false;
          sheets_.push_back(imp);
        }
        sheets_.push_back(e);
      }
    } else if (n->tag == "link") {
      std::vector<std::string> rels = SplitWhitespace(AsciiLower(n->Attr("rel")));
      bool sheet = false, alternate = false, preloadStyle = false;
      for (size_t i = 0; i < rels.size(); ++i) {
        if (rels[i] == "stylesheet") sheet = true;
        if (rels[i] == "alternate") alternate = true;
        if (rels[i] == "preload" && AsciiLower(n->Attr("as")) == "style") preloadStyle = true;
      }
      if ((sheet && !alternate) || preloadStyle) {
        Url u = baseUrl_.Resolve(n->Attr("href"));
        if (u.valid() && !n->HasAttr("disabled")) {
          std::string spec = u.SpecNoFragment();
          bool dup = false;
          for (size_t i = 0; i < sheets_.size(); ++i)
            if (sheets_[i].isLink && sheets_[i].url == spec) dup = true;
          if (!dup) {
            SheetEntry e;
            e.isLink = true;
            e.url = spec;
            e.media = n->Attr("media");
            e.loaded = e.failed = false;
            sheets_.push_back(e);
          }
        }
      }
    } else if (n->tag == "base" && n->HasAttr("href")) {
      Url u = baseUrl_.Resolve(n->Attr("href"));
      if (u.valid()) baseUrl_ = u;
    } else if (n->tag == "meta") {
      if (EqualsIgnoreCase(n->Attr("http-equiv"), "refresh")) {
        std::string c = n->Attr("content");
        size_t semi = c.find_first_of(";,");
        long long d;
        if (ParseInt(c.substr(0, semi), d)) {
          refreshDelay_ = (int)d;
          if (semi != std::string::npos) {
            std::string rest = Trim(c.substr(semi + 1));
            if (StartsWithIgnoreCase(rest, "url")) {
              rest = Trim(rest.substr(3));
              if (!rest.empty() && rest[0] == '=') rest = Trim(rest.substr(1));
            }
            if (rest.size() >= 2 && (rest[0] == '\'' || rest[0] == '"'))
              rest = rest.substr(1, rest.size() - 2);
            Url u = baseUrl_.Resolve(rest);
            refreshUrl_ = u.valid() ? u.Spec() : std::string();
          } else {
            refreshUrl_ = url_;
          }
        }
      }
    }
    if (n->tag == "template" || n->tag == "svg") return;
    if (n->tag == "noscript" && scriptingEnabled()) return;
  }
  for (size_t i = 0; i < n->children.size(); ++i) AddSheetsFromNode(n->children[i].get());
}

void Page::CollectDocumentInfo() {
  if (!doc_) return;
  AddSheetsFromNode(doc_->root.get());
}

static bool IsClassicScript(Node* n) {
  std::string type = AsciiLower(Trim(n->Attr("type")));
  if (type.empty() || type == "text/javascript" || type == "application/javascript" ||
      type == "application/x-javascript" || type == "text/ecmascript" ||
      type == "application/ecmascript" || type == "text/jscript")
    return true;
  return false;
}

void Page::CollectScripts(Node* n) {
  if (n->IsElement()) {
    if (n->tag == "template" || n->tag == "svg") return;
    if (n->tag == "script") {
      if (!n->scriptDone && IsClassicScript(n)) {
        n->scriptDone = true;
        ScriptEntry e;
        e.node = n;
        e.parserInserted = true;
        e.loaded = e.failed = e.requested = e.done = false;
        if (n->HasAttr("src")) {
          Url u = baseUrl_.Resolve(n->Attr("src"));
          if (u.valid() && (u.scheme() == "http" || u.scheme() == "https" || u.scheme() == "file"))
            e.url = u.SpecNoFragment();
          else
            e.failed = true;
        } else {
          e.source = n->TextContent();
          e.loaded = true;
        }
        scripts_.push_back(e);
      }
      return;
    }
  }
  for (size_t i = 0; i < n->children.size(); ++i) CollectScripts(n->children[i].get());
}

void Page::OnScriptInserted(Node* n) {
  if (!scriptingEnabled() || n->scriptDone || !IsClassicScript(n)) return;
  // Only scripts that are actually connected to the document run.
  Node* top = n;
  while (top->parent) top = top->parent;
  if (!doc_ || top != doc_->root.get()) return;
  n->scriptDone = true;
  ScriptEntry e;
  e.node = n;
  e.parserInserted = false;
  e.loaded = e.failed = e.requested = e.done = false;
  if (n->HasAttr("src")) {
    Url u = baseUrl_.Resolve(n->Attr("src"));
    if (u.valid() && (u.scheme() == "http" || u.scheme() == "https" || u.scheme() == "file"))
      e.url = u.SpecNoFragment();
    else
      e.failed = true;
  } else {
    e.source = n->TextContent();
    e.loaded = true;
  }
  scripts_.push_back(e);
}

std::vector<std::string> Page::PendingScripts() {
  std::vector<std::string> out;
  for (size_t i = 0; i < scripts_.size(); ++i) {
    ScriptEntry& e = scripts_[i];
    if (e.url.empty() || e.loaded || e.failed || e.requested) continue;
    e.requested = true;
    bool dup = false;
    for (size_t k = 0; k < out.size(); ++k) dup = dup || out[k] == e.url;
    if (!dup) out.push_back(e.url);
  }
  return out;
}

void Page::ProvideScript(const std::string& url, const std::string& source, bool ok) {
  for (size_t i = 0; i < scripts_.size(); ++i) {
    ScriptEntry& e = scripts_[i];
    if (e.url != url || e.loaded || e.failed) continue;
    if (ok) {
      e.loaded = true;
      e.source = source;
    } else {
      e.failed = true;
    }
  }
}

bool Page::RunScripts() {
  if (!script_ || inScripts_) return false;
  inScripts_ = true;
  bool ran = false;
  bool progress = true;
  while (progress && script_) {
    progress = false;
    bool blocked = false;  // a parser-inserted script is still loading
    for (size_t i = 0; i < scripts_.size(); ++i) {
      ScriptEntry& e = scripts_[i];
      if (e.done) continue;
      if (!e.loaded && !e.failed) {
        if (e.parserInserted) blocked = true;
        continue;
      }
      if (e.parserInserted && blocked) continue;
      e.done = true;
      Node* node = e.node;
      std::string src = e.source, name = e.url.empty() ? url_ : e.url;
      bool failed = e.failed;
      if (failed) {
        script_->DispatchEvent(node, "error");
      } else {
        script_->Execute(src, name, node);
        if (!e.url.empty()) script_->DispatchEvent(node, "load");
      }
      ran = true;
      progress = true;
      break;  // scripts_ may have grown; restart the scan
    }
    if (!progress && !blocked && !contentLoadedFired_) {
      bool parserPending = false;
      for (size_t i = 0; i < scripts_.size(); ++i)
        if (scripts_[i].parserInserted && !scripts_[i].done) parserPending = true;
      if (!parserPending) {
        contentLoadedFired_ = true;
        script_->DispatchDocumentEvent("DOMContentLoaded");
        ran = true;
        progress = true;
      }
    }
  }
  if (script_ && contentLoadedFired_ && !loadFired_) {
    loadFired_ = true;
    script_->DispatchDocumentEvent("load");
    ran = true;
  }
  inScripts_ = false;
  return ran;
}

void Page::RequestRepaint() {
  if (script_) script_->MarkDirty();
}

void Page::EnsureLayout() {
  if (!script_ || !script_->TakeLayoutStale()) {
    if (root_) return;
  }
  RecollectSheets();
  Restyle(lastViewportW_, lastViewportH_);
  Relayout(lastViewportW_, lastViewportH_);
}

void Page::ScriptMutated() {
  if (script_) script_->TakeLayoutStale();
  RecollectSheets();
  Restyle(lastViewportW_, lastViewportH_);
  Relayout(lastViewportW_, lastViewportH_);
}

void Page::RecollectSheets() {
  if (!doc_) return;
  // Keep already fetched external sheets; inline <style> is re-parsed.
  std::vector<SheetEntry> old;
  old.swap(sheets_);
  std::string keepTitleRefreshUrl = refreshUrl_;
  int keepDelay = refreshDelay_;
  AddSheetsFromNode(doc_->root.get());
  refreshUrl_ = keepTitleRefreshUrl;
  refreshDelay_ = keepDelay;
  for (size_t i = 0; i < sheets_.size(); ++i) {
    SheetEntry& e = sheets_[i];
    if (!e.isLink) continue;
    for (size_t k = 0; k < old.size(); ++k) {
      if (old[k].isLink && old[k].url == e.url) {
        std::string media = e.media;
        e = old[k];
        e.media = media;
        break;
      }
    }
  }
  // Sheets @imported by fetched link sheets appear only in the old list:
  // put each back in front of the sheet that followed it.
  for (size_t k = 0; k < old.size(); ++k) {
    if (!old[k].imported) continue;
    bool found = false;
    for (size_t i = 0; i < sheets_.size() && !found; ++i)
      found = sheets_[i].isLink && sheets_[i].url == old[k].url;
    if (found) continue;
    size_t pos = sheets_.size();
    for (size_t j = k + 1; j < old.size() && pos == sheets_.size(); ++j) {
      if (old[j].imported || !old[j].isLink) continue;
      for (size_t i = 0; i < sheets_.size(); ++i)
        if (sheets_[i].isLink && sheets_[i].url == old[j].url) {
          pos = i;
          break;
        }
      if (pos == sheets_.size()) break;  // parent link was removed
    }
    if (pos < sheets_.size()) sheets_.insert(sheets_.begin() + pos, old[k]);
  }
}

bool Page::TakeFormSubmission(FormSubmission& out) {
  if (formQueue_.empty()) return false;
  out = formQueue_.front();
  formQueue_.erase(formQueue_.begin());
  return true;
}

std::vector<std::string> Page::PendingStylesheets() const {
  std::vector<std::string> out;
  for (size_t i = 0; i < sheets_.size(); ++i)
    if (sheets_[i].isLink && !sheets_[i].loaded && !sheets_[i].failed) out.push_back(sheets_[i].url);
  return out;
}

void Page::ProvideStylesheet(const std::string& url, const std::string& css, bool ok) {
  for (size_t i = 0; i < sheets_.size(); ++i) {
    SheetEntry& e = sheets_[i];
    if (!e.isLink || e.url != url || e.loaded || e.failed) continue;
    if (!ok) {
      e.failed = true;
      continue;
    }
    e.loaded = true;
    e.css = css;
    e.sheet.reset(new Stylesheet);
    ParseStylesheet(css, *e.sheet);
    Url base = Url::Parse(url);
    std::vector<SheetEntry> imports;
    for (size_t k = 0; k < e.sheet->imports.size(); ++k) {
      Url u = base.Resolve(e.sheet->imports[k]);
      if (!u.valid()) continue;
      std::string spec = u.SpecNoFragment();
      bool dup = false;
      for (size_t j = 0; j < sheets_.size(); ++j)
        if (sheets_[j].isLink && sheets_[j].url == spec) dup = true;
      if (dup) continue;
      SheetEntry imp;
      imp.isLink = true;
      imp.url = spec;
      imp.media = e.media;
      imp.loaded = imp.failed = false;
      imp.imported = true;
      imports.push_back(imp);
    }
    if (!imports.empty()) {
      sheets_.insert(sheets_.begin() + i, imports.begin(), imports.end());
      i += imports.size();
    }
  }
}

void Page::Restyle(float viewportW, float viewportH) {
  if (!doc_) return;
  root_.reset();  // boxes reference the styles about to be replaced
  resolver_.ClearAuthorSheets();
  MediaContext mc;
  mc.width = viewportW;
  mc.height = viewportH;
  for (size_t i = 0; i < sheets_.size(); ++i) {
    const SheetEntry& e = sheets_[i];
    if (!e.loaded || !e.sheet) continue;
    if (!e.media.empty() && !EvaluateMediaQueryList(e.media, mc)) continue;
    resolver_.AddAuthorSheet(e.sheet, e.isLink ? e.url : baseUrl_.Spec());
  }
  ElementState st;
  anim_.BeforeRestyle(doc_.get());
  resolver_.ResolveDocument(*doc_, mc, baseUrl_.Spec(), st);
  AnimationController::Context ac;
  ac.rootFontSize = resolver_.rootFontSize();
  ac.viewportW = viewportW;
  ac.viewportH = viewportH;
  ac.baseUrl = baseUrl_.Spec();
  anim_.AfterRestyle(doc_.get(), resolver_, ac, AnimationClockMs());
}

int Page::TickAnimations() {
  int r = anim_.Tick(AnimationClockMs());
  std::vector<std::pair<Node*, std::string> > events = anim_.TakeEvents();
  if (script_)
    for (size_t i = 0; i < events.size(); ++i) script_->DispatchEvent(events[i].first, events[i].second.c_str());
  return r;
}

void Page::Relayout(float viewportW, float viewportH) {
  if (!doc_) return;
  root_ = BuildLayoutTree(*doc_, baseUrl_.Spec());
  engine_.Layout(root_.get(), viewportW, viewportH);
  lastViewportW_ = viewportW;
  lastViewportH_ = viewportH;
  Painter p(&engine_, images_);
  p.Paint(root_.get(), viewportW, viewportH, display_);
}

void Page::Repaint() {
  if (!root_) return;
  Painter p(&engine_, images_);
  p.Paint(root_.get(), lastViewportW_, lastViewportH_, display_);
}

std::vector<WebFontRequest> Page::PendingFonts() {
  std::vector<WebFontRequest> out;
  if (!doc_) return out;
  // Families actually referenced by computed styles.
  std::set<std::string> used;
  std::vector<Node*> stack(1, doc_->root.get());
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    if (n->style) {
      std::vector<std::string> fams = Split(n->style->fontFamily, ',');
      for (size_t i = 0; i < fams.size(); ++i) used.insert(Trim(fams[i]));
      for (int k = 0; k < 2; ++k) {
        const ComputedStyle* ps = k == 0 ? n->style->before.get() : n->style->after.get();
        if (!ps) continue;
        std::vector<std::string> pf = Split(ps->fontFamily, ',');
        for (size_t i = 0; i < pf.size(); ++i) used.insert(Trim(pf[i]));
      }
    }
    for (size_t i = 0; i < n->children.size(); ++i) stack.push_back(n->children[i].get());
  }
  for (size_t i = 0; i < sheets_.size(); ++i) {
    const SheetEntry& e = sheets_[i];
    if (!e.loaded || !e.sheet) continue;
    Url base = e.isLink ? Url::Parse(e.url) : baseUrl_;
    for (size_t k = 0; k < e.sheet->fontFaces.size(); ++k) {
      const FontFace& f = e.sheet->fontFaces[k];
      if (!used.count(f.family) || !f.coversLatin) continue;
      std::string chosen;
      for (size_t q = 0; q < f.sources.size() && chosen.empty(); ++q) {
        std::string fmt = f.sources[q].second;
        std::string lower = AsciiLower(f.sources[q].first);
        lower = lower.substr(0, lower.find_first_of("?#"));
        bool ok = fmt == "woff" || fmt == "woff2" || fmt == "truetype" || fmt == "opentype" ||
                  (fmt.empty() && (EndsWith(lower, ".woff") || EndsWith(lower, ".woff2") || EndsWith(lower, ".ttf") ||
                                   EndsWith(lower, ".otf")));
        if (ok) {
          Url u = base.Resolve(f.sources[q].first);
          if (u.valid()) chosen = u.Spec();
        }
      }
      if (chosen.empty() || requestedFonts_.count(chosen) || requestedFonts_.size() >= 24) continue;
      requestedFonts_.insert(chosen);
      WebFontRequest r;
      r.url = chosen;
      r.family = f.family;
      r.weight = f.weight;
      r.italic = f.italic;
      out.push_back(r);
    }
  }
  return out;
}

std::vector<std::string> Page::ReferencedImages() const {
  std::vector<std::string> out;
  std::set<std::string> seen;
  std::vector<const LayoutBox*> stack;
  if (root_) stack.push_back(root_.get());
  while (!stack.empty()) {
    const LayoutBox* b = stack.back();
    stack.pop_back();
    if (!b->imageUrl.empty() && seen.insert(b->imageUrl).second) out.push_back(b->imageUrl);
    if (b->kind != LayoutBox::kText && !b->style->backgroundImage.empty() &&
        seen.insert(b->style->backgroundImage).second)
      out.push_back(b->style->backgroundImage);
    for (size_t i = 0; i < b->children.size(); ++i) stack.push_back(b->children[i].get());
  }
  return out;
}

Node* Page::HitTest(float x, float y, float scrollX, float scrollY) const {
  const std::vector<HitRegion>& hits = display_.hits;
  // Fixed content is on top of everything else.
  for (size_t i = hits.size(); i-- > 0;)
    if (hits[i].fixed && hits[i].rect.contains(x - scrollX, y - scrollY)) return hits[i].node;
  for (size_t i = hits.size(); i-- > 0;)
    if (!hits[i].fixed && hits[i].rect.contains(x, y)) return hits[i].node;
  return 0;
}

Node* Page::LinkFor(Node* n) {
  for (; n; n = n->parent)
    if (n->IsElement() && (n->tag == "a" || n->tag == "area") && n->HasAttr("href")) return n;
  return 0;
}

Url Page::ResolveUrl(const std::string& ref) const { return baseUrl_.Resolve(ref); }

std::string Page::LinkUrl(Node* link) const {
  if (!link) return std::string();
  Url u = baseUrl_.Resolve(link->Attr("href"));
  return u.valid() ? u.Spec() : std::string();
}

bool Page::BoxRect(Node* n, Rect& out) const {
  if (!root_ || !n || !n->layoutBox) return false;
  LayoutBox* b = n->layoutBox;
  float ax, ay;
  if (b->kind == LayoutBox::kInline || b->kind == LayoutBox::kText ||
      b->kind == LayoutBox::kLineBreak) {
    // Find the first fragment of this inline box in its container.
    LayoutBox* c = b->parent;
    while (c && c->kind == LayoutBox::kInline) c = c->parent;
    if (!c) return false;
    c->AbsolutePosition(ax, ay);
    for (size_t i = 0; i < c->lines.size(); ++i)
      for (size_t k = 0; k < c->lines[i].fragments.size(); ++k) {
        const LineFragment& f = c->lines[i].fragments[k];
        for (LayoutBox* p = f.box; p; p = p->parent)
          if (p == b) {
            out = Rect(ax + f.x, ay + c->lines[i].y, f.w, c->lines[i].h);
            return true;
          }
        if (f.node == n) {
          out = Rect(ax + f.x, ay + c->lines[i].y, f.w, c->lines[i].h);
          return true;
        }
      }
    out = Rect(ax, ay, c->w, 0);
    return true;
  }
  b->AbsolutePosition(ax, ay);
  out = Rect(ax, ay, b->w, b->h);
  return true;
}

float Page::AnchorPosition(const std::string& rawFragment) const {
  if (!doc_) return -1;
  std::string frag = PercentDecode(rawFragment, false);
  if (frag.empty() || frag == "top") return 0;
  Node* target = doc_->root->FindById(frag);
  if (!target) {
    std::vector<Node*> anchors;
    doc_->root->FindAll("a", anchors);
    for (size_t i = 0; i < anchors.size(); ++i)
      if (anchors[i]->Attr("name") == frag) {
        target = anchors[i];
        break;
      }
  }
  // Walk to the nearest node that has a box.
  for (Node* n = target; n; n = n->parent) {
    Rect r;
    if (BoxRect(n, r)) return r.y;
    if (n->FirstElementChild() && n->FirstElementChild()->layoutBox) {
      if (BoxRect(n->FirstElementChild(), r)) return r.y;
    }
  }
  return -1;
}

Node* Page::FormFor(Node* control) {
  if (!control) return 0;
  return control->Closest("form");
}

Node* Page::DefaultSubmitButton(Node* form) {
  if (!form) return 0;
  std::vector<Node*> stack(1, form);
  while (!stack.empty()) {
    Node* n = stack.front();
    stack.erase(stack.begin());
    if (n->IsElement()) {
      std::string type = AsciiLower(n->Attr("type"));
      if (n->tag == "input" && (type == "submit" || type == "image")) return n;
      if (n->tag == "button" && (type.empty() || type == "submit")) return n;
    }
    for (size_t i = 0; i < n->children.size(); ++i) stack.push_back(n->children[i].get());
  }
  return 0;
}

bool Page::BuildFormSubmission(Node* form, Node* submitter, FormSubmission& out) const {
  if (!form) return false;
  std::vector<std::pair<std::string, std::string> > fields;
  std::vector<Node*> stack(1, form);
  std::vector<Node*> ordered;
  // Pre-order traversal.
  std::vector<Node*> work;
  work.push_back(form);
  while (!work.empty()) {
    Node* n = work.back();
    work.pop_back();
    if (n->IsElement()) ordered.push_back(n);
    for (size_t i = n->children.size(); i-- > 0;) work.push_back(n->children[i].get());
  }
  for (size_t i = 0; i < ordered.size(); ++i) {
    Node* n = ordered[i];
    std::string name = n->Attr("name");
    if (n->HasAttr("disabled")) continue;
    if (n->tag == "input") {
      std::string type = AsciiLower(n->Attr("type"));
      std::string value = n->formValueSet ? n->formValue : n->Attr("value");
      if (type == "checkbox" || type == "radio") {
        bool checked = n->checkedSet ? n->checked : n->HasAttr("checked");
        if (!checked || name.empty()) continue;
        if (!n->HasAttr("value")) value = "on";
        fields.push_back(std::make_pair(name, value));
      } else if (type == "submit" || type == "button" || type == "reset") {
        if (n == submitter && !name.empty() && type == "submit") fields.push_back(std::make_pair(name, value));
      } else if (type == "image") {
        if (n == submitter) {
          std::string p = name.empty() ? "" : name + ".";
          fields.push_back(std::make_pair(p + "x", "0"));
          fields.push_back(std::make_pair(p + "y", "0"));
        }
      } else if (type == "file") {
        if (!name.empty()) fields.push_back(std::make_pair(name, std::string()));
      } else if (!name.empty()) {
        fields.push_back(std::make_pair(name, value));
      }
    } else if (n->tag == "textarea" && !name.empty()) {
      std::string v = n->formValueSet ? n->formValue : n->TextContent();
      fields.push_back(std::make_pair(name, ReplaceAll(v, "\n", "\r\n")));
    } else if (n->tag == "select" && !name.empty()) {
      std::vector<Node*> opts;
      n->FindAll("option", opts);
      int sel = n->selectedIndex;
      if (sel < 0) {
        for (size_t k = 0; k < opts.size(); ++k)
          if (opts[k]->HasAttr("selected")) sel = (int)k;
        if (sel < 0 && !opts.empty() && !n->HasAttr("multiple")) sel = 0;
      }
      if (sel >= 0 && sel < (int)opts.size()) {
        Node* o = opts[sel];
        std::string v = o->HasAttr("value") ? o->Attr("value") : CollapseWhitespace(o->TextContent());
        fields.push_back(std::make_pair(name, v));
      }
    } else if (n->tag == "button" && n == submitter && !name.empty()) {
      fields.push_back(std::make_pair(name, n->Attr("value")));
    }
  }
  std::string query;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) query += "&";
    query += PercentEncodeForm(fields[i].first) + "=" + PercentEncodeForm(fields[i].second);
  }
  std::string action = form->Attr("action");
  std::string method = AsciiUpper(form->Attr("method"));
  if (submitter && submitter->HasAttr("formaction")) action = submitter->Attr("formaction");
  if (submitter && submitter->HasAttr("formmethod")) method = AsciiUpper(submitter->Attr("formmethod"));
  Url target = Trim(action).empty() ? Url::Parse(url_) : baseUrl_.Resolve(action);
  if (!target.valid()) return false;
  target.ClearFragment();
  if (method == "POST") {
    out.method = "POST";
    out.url = target.Spec();
    out.body = query;
    out.contentType = "application/x-www-form-urlencoded";
  } else {
    out.method = "GET";
    target.SetQuery(query);
    out.url = target.Spec();
  }
  return true;
}

std::vector<std::pair<Rect, std::string> > Page::TextRuns() const {
  std::vector<std::pair<Rect, std::string> > out;
  for (size_t i = 0; i < display_.items.size(); ++i) {
    const DisplayItem& it = display_.items[i];
    if (it.type == DisplayItem::kText) out.push_back(std::make_pair(it.rect, it.text));
  }
  return out;
}

}  // namespace kite
