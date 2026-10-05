#include "page/page.h"

#include <set>

#include "base/strings.h"
#include "html/parser.h"

namespace kite {

Page::Page(FontProvider* fonts, ImageProvider* images)
    : fonts_(fonts), images_(images), engine_(fonts, images), refreshDelay_(-1) {}

Page::~Page() {}

void Page::LoadHtml(const std::string& utf8, const std::string& url) {
  url_ = url;
  baseUrl_ = Url::Parse(url);
  doc_ = ParseHtml(utf8);
  root_.reset();
  sheets_.clear();
  refreshDelay_ = -1;
  refreshUrl_.clear();
  CollectDocumentInfo();
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
  }
  for (size_t i = 0; i < n->children.size(); ++i) AddSheetsFromNode(n->children[i].get());
}

void Page::CollectDocumentInfo() {
  if (!doc_) return;
  AddSheetsFromNode(doc_->root.get());
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
  resolver_.ResolveDocument(*doc_, mc, baseUrl_.Spec(), st);
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
