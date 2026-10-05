#include "html/parser.h"

#include <cstring>

#include "base/strings.h"
#include "html/entities.h"

namespace kite {

// ---------------------------------------------------------------------------
// Tokenizer

HtmlTokenizer::HtmlTokenizer(const std::string& input)
    : in_(input), pos_(0), rcdata_(false), plaintext_(false) {}

void HtmlTokenizer::SetRawText(const std::string& endTag, bool rcdata) {
  rawEnd_ = endTag;
  rcdata_ = rcdata;
}

static bool IsTagNameEnd(char c) {
  return IsAsciiSpace((unsigned char)c) || c == '/' || c == '>';
}

bool HtmlTokenizer::Next(HtmlToken& tok) {
  tok = HtmlToken();
  if (pos_ >= in_.size()) {
    tok.type = HtmlToken::kEof;
    return false;
  }
  if (plaintext_) {
    tok.type = HtmlToken::kText;
    tok.data = in_.substr(pos_);
    pos_ = in_.size();
    return true;
  }
  if (!rawEnd_.empty()) {
    // Find "</rawEnd_" case-insensitively.
    size_t p = pos_;
    for (;;) {
      p = in_.find("</", p);
      if (p == std::string::npos) {
        p = in_.size();
        break;
      }
      if (p + 2 + rawEnd_.size() <= in_.size() &&
          EqualsIgnoreCase(in_.substr(p + 2, rawEnd_.size()), rawEnd_) &&
          (p + 2 + rawEnd_.size() == in_.size() ||
           IsTagNameEnd(in_[p + 2 + rawEnd_.size()])))
        break;
      p += 2;
    }
    if (p > pos_) {
      tok.type = HtmlToken::kText;
      tok.data = in_.substr(pos_, p - pos_);
      if (rcdata_) tok.data = DecodeEntities(tok.data, false);
      pos_ = p;
      return true;
    }
    rawEnd_.clear();
    // Falls through to parse the end tag.
  }

  if (in_[pos_] != '<') {
    size_t p = in_.find('<', pos_);
    if (p == std::string::npos) p = in_.size();
    tok.type = HtmlToken::kText;
    tok.data = DecodeEntities(in_.substr(pos_, p - pos_), false);
    pos_ = p;
    return true;
  }

  // At '<'
  if (in_.compare(pos_, 4, "<!--") == 0) {
    size_t end = in_.find("-->", pos_ + 4);
    tok.type = HtmlToken::kComment;
    if (end == std::string::npos) {
      tok.data = in_.substr(pos_ + 4);
      pos_ = in_.size();
    } else {
      tok.data = in_.substr(pos_ + 4, end - pos_ - 4);
      pos_ = end + 3;
    }
    return true;
  }
  if (in_.compare(pos_, 9, "<![CDATA[") == 0) {
    size_t end = in_.find("]]>", pos_ + 9);
    if (end == std::string::npos) end = in_.size();
    tok.type = HtmlToken::kText;
    tok.data = in_.substr(pos_ + 9, end - pos_ - 9);
    pos_ = std::min(in_.size(), end + 3);
    return true;
  }
  if (pos_ + 1 < in_.size() && (in_[pos_ + 1] == '!' || in_[pos_ + 1] == '?')) {
    size_t end = in_.find('>', pos_);
    if (end == std::string::npos) end = in_.size();
    std::string body = in_.substr(pos_ + 2, end - pos_ - 2);
    pos_ = end < in_.size() ? end + 1 : end;
    if (StartsWithIgnoreCase(body, "doctype")) {
      tok.type = HtmlToken::kDoctype;
      std::vector<std::string> parts = SplitWhitespace(body.substr(7));
      if (!parts.empty()) tok.name = AsciiLower(parts[0]);
      tok.data = body;
    } else {
      tok.type = HtmlToken::kComment;
      tok.data = body;
    }
    return true;
  }
  if (pos_ + 1 < in_.size() && in_[pos_ + 1] == '/') {
    if (pos_ + 2 < in_.size() && IsAsciiAlpha((unsigned char)in_[pos_ + 2])) {
      size_t p = pos_ + 2;
      while (p < in_.size() && !IsTagNameEnd(in_[p])) ++p;
      tok.type = HtmlToken::kEndTag;
      tok.name = AsciiLower(in_.substr(pos_ + 2, p - pos_ - 2));
      size_t end = in_.find('>', p);
      pos_ = end == std::string::npos ? in_.size() : end + 1;
      return true;
    }
    if (pos_ + 2 < in_.size() && in_[pos_ + 2] == '>') {
      pos_ += 3;  // "</>" is ignored
      return Next(tok);
    }
    // Bogus comment.
    size_t end = in_.find('>', pos_);
    pos_ = end == std::string::npos ? in_.size() : end + 1;
    tok.type = HtmlToken::kComment;
    return true;
  }
  if (pos_ + 1 < in_.size() && IsAsciiAlpha((unsigned char)in_[pos_ + 1])) {
    ParseTag(tok);
    return true;
  }
  // A lone '<'.
  tok.type = HtmlToken::kText;
  tok.data = "<";
  ++pos_;
  return true;
}

void HtmlTokenizer::ParseTag(HtmlToken& tok) {
  size_t p = pos_ + 1;
  size_t start = p;
  while (p < in_.size() && !IsTagNameEnd(in_[p])) ++p;
  tok.type = HtmlToken::kStartTag;
  tok.name = AsciiLower(in_.substr(start, p - start));
  for (;;) {
    while (p < in_.size() && (IsAsciiSpace((unsigned char)in_[p]) || in_[p] == '/')) {
      if (in_[p] == '/' && p + 1 < in_.size() && in_[p + 1] == '>')
        tok.selfClosing = true;
      ++p;
    }
    if (p >= in_.size()) break;
    if (in_[p] == '>') {
      ++p;
      break;
    }
    size_t ns = p;
    ++p;  // first char may be anything (even '=')
    while (p < in_.size() && !IsAsciiSpace((unsigned char)in_[p]) && in_[p] != '/' &&
           in_[p] != '>' && in_[p] != '=')
      ++p;
    Attribute a;
    a.name = AsciiLower(in_.substr(ns, p - ns));
    while (p < in_.size() && IsAsciiSpace((unsigned char)in_[p])) ++p;
    if (p < in_.size() && in_[p] == '=') {
      ++p;
      while (p < in_.size() && IsAsciiSpace((unsigned char)in_[p])) ++p;
      if (p < in_.size() && (in_[p] == '"' || in_[p] == '\'')) {
        char q = in_[p++];
        size_t vs = p;
        while (p < in_.size() && in_[p] != q) ++p;
        a.value = DecodeEntities(in_.substr(vs, p - vs), true);
        if (p < in_.size()) ++p;
      } else {
        size_t vs = p;
        while (p < in_.size() && !IsAsciiSpace((unsigned char)in_[p]) && in_[p] != '>') ++p;
        a.value = DecodeEntities(in_.substr(vs, p - vs), true);
      }
    }
    bool dup = false;
    for (size_t i = 0; i < tok.attrs.size(); ++i)
      if (tok.attrs[i].name == a.name) dup = true;
    if (!dup) tok.attrs.push_back(a);
  }
  pos_ = p;
}

// ---------------------------------------------------------------------------
// Tree builder

namespace {

bool InList(const std::string& s, const char* const* list) {
  for (int i = 0; list[i]; ++i)
    if (s == list[i]) return true;
  return false;
}

const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img",
                             "input", "link", "meta", "param", "source", "track",
                             "wbr", "keygen", "basefont", "bgsound", "frame", 0};
const char* const kClosesP[] = {
    "address", "article", "aside", "blockquote", "center", "details", "dialog",
    "dir", "div", "dl", "fieldset", "figcaption", "figure", "footer", "header",
    "hgroup", "main", "menu", "nav", "ol", "p", "section", "summary", "ul",
    "h1", "h2", "h3", "h4", "h5", "h6", "pre", "listing", "form", "table",
    "hr", "xmp", "plaintext", "search", 0};
const char* const kHeadings[] = {"h1", "h2", "h3", "h4", "h5", "h6", 0};
const char* const kFormatting[] = {"a", "b", "big", "code", "em", "font", "i",
                                   "nobr", "s", "small", "strike", "strong",
                                   "tt", "u", "span", "abbr", "cite", "mark",
                                   "label", "sub", "sup", "q", "kbd", "var",
                                   "samp", "time", "dfn", 0};
const char* const kScopeBoundary[] = {"html", "table", "td", "th", "caption",
                                      "marquee", "object", "template", "applet",
                                      "svg", 0};
const char* const kSpecial[] = {
    "address", "applet", "area", "article", "aside", "base", "basefont",
    "bgsound", "blockquote", "body", "br", "button", "caption", "center", "col",
    "colgroup", "dd", "details", "dir", "div", "dl", "dt", "embed", "fieldset",
    "figcaption", "figure", "footer", "form", "frame", "frameset", "h1", "h2",
    "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr", "html", "iframe",
    "img", "input", "li", "link", "listing", "main", "marquee", "menu", "meta",
    "nav", "noembed", "noframes", "object", "ol", "p", "param", "plaintext",
    "pre", "script", "section", "select", "source", "style", "summary",
    "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead",
    "title", "tr", "track", "ul", "wbr", "xmp", 0};
const char* const kHeadElems[] = {"title", "meta", "link", "style", "script",
                                  "base", "noscript", "template", 0};
const char* const kImpliedEnd[] = {"dd", "dt", "li", "optgroup", "option", "p",
                                   "rb", "rp", "rt", "rtc", 0};

class TreeBuilder {
 public:
  TreeBuilder(const std::string& input)
      : tokenizer_(input), doc_(new Document), html_(0), head_(0), body_(0),
        form_(0), sawDoctype_(false), dropNextNewline_(false) {}

  std::unique_ptr<Document> Run() {
    HtmlToken tok;
    while (tokenizer_.Next(tok)) Process(tok);
    EnsureBody();
    doc_->quirksMode = !sawDoctype_;
    return std::move(doc_);
  }

 private:
  Node* Current() { return stack_.empty() ? doc_->root.get() : stack_.back(); }

  void EnsureHtml() {
    if (html_) return;
    std::unique_ptr<Node> n(new Node(Node::kElement));
    n->tag = "html";
    html_ = doc_->root->AppendChild(std::move(n));
    stack_.push_back(html_);
  }

  void EnsureHead() {
    EnsureHtml();
    if (head_) return;
    std::unique_ptr<Node> n(new Node(Node::kElement));
    n->tag = "head";
    head_ = html_->AppendChild(std::move(n));
  }

  void EnsureBody() {
    EnsureHead();
    if (body_) return;
    std::unique_ptr<Node> n(new Node(Node::kElement));
    n->tag = "body";
    body_ = html_->AppendChild(std::move(n));
    stack_.clear();
    stack_.push_back(html_);
    stack_.push_back(body_);
  }

  Node* Insert(const HtmlToken& tok, Node* parent) {
    std::unique_ptr<Node> n(new Node(Node::kElement));
    n->tag = tok.name;
    n->attrs = tok.attrs;
    n->UpdateCaches();
    return parent->AppendChild(std::move(n));
  }

  void InsertText(const std::string& text, Node* parent) {
    if (text.empty()) return;
    if (!parent->children.empty() && parent->children.back()->type == Node::kText) {
      parent->children.back()->text += text;
      return;
    }
    std::unique_ptr<Node> n(new Node(Node::kText));
    n->text = text;
    parent->AppendChild(std::move(n));
  }

  bool InForeign() const {
    for (size_t i = stack_.size(); i-- > 0;)
      if (stack_[i]->tag == "svg" || stack_[i]->tag == "math") return true;
    return false;
  }

  // Returns index in stack of the element with the given tag if it is in
  // scope; -1 otherwise.
  int FindInScope(const std::string& tag, const char* const* boundaries,
                  const char* extra1 = 0, const char* extra2 = 0) {
    for (int i = (int)stack_.size() - 1; i >= 0; --i) {
      const std::string& t = stack_[i]->tag;
      if (t == tag) return i;
      if (InList(t, boundaries)) return -1;
      if ((extra1 && t == extra1) || (extra2 && t == extra2)) return -1;
    }
    return -1;
  }

  void PopUntilIndex(int idx) {
    if (idx < 1) idx = 1;  // never pop <html>
    while ((int)stack_.size() > idx) stack_.pop_back();
  }

  void CloseP() {
    int i = FindInScope("p", kScopeBoundary, "button");
    if (i >= 0) PopUntilIndex(i);
  }

  void StartRaw(const std::string& tag) {
    if (tag == "script" || tag == "style" || tag == "xmp" || tag == "iframe" ||
        tag == "noembed" || tag == "noframes")
      tokenizer_.SetRawText(tag, false);
    else if (tag == "title" || tag == "textarea")
      tokenizer_.SetRawText(tag, true);
  }

  bool IsRaw(const std::string& tag) {
    return tag == "script" || tag == "style" || tag == "xmp" || tag == "iframe" ||
           tag == "noembed" || tag == "noframes" || tag == "title" || tag == "textarea";
  }

  void Process(const HtmlToken& tok) {
    switch (tok.type) {
      case HtmlToken::kDoctype:
        sawDoctype_ = true;
        return;
      case HtmlToken::kComment:
        return;
      case HtmlToken::kText:
        ProcessText(tok.data);
        return;
      case HtmlToken::kStartTag:
        ProcessStart(tok);
        return;
      case HtmlToken::kEndTag:
        ProcessEnd(tok);
        return;
      default:
        return;
    }
  }

  void ProcessText(std::string text) {
    if (dropNextNewline_) {
      dropNextNewline_ = false;
      if (!text.empty() && text[0] == '\n') text = text.substr(1);
      else if (text.size() >= 2 && text[0] == '\r' && text[1] == '\n') text = text.substr(2);
    }
    // Replace NUL characters.
    for (size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\0') text[i] = ' ';
    if (!body_) {
      // Inside a raw head element (title/style/script) keep the text.
      Node* cur = Current();
      if (cur != html_ && cur != doc_->root.get() && cur != head_ &&
          (cur->parent == head_ || IsRaw(cur->tag))) {
        InsertText(text, cur);
        return;
      }
      size_t i = 0;
      while (i < text.size() && IsAsciiSpace((unsigned char)text[i])) ++i;
      if (i == text.size()) return;
      EnsureBody();
      text = text.substr(i);
    }
    InsertText(text, Current());
  }

  void ProcessStart(HtmlToken tok) {
    std::string& t = tok.name;
    if (t == "image") t = "img";
    if (t == "html") {
      EnsureHtml();
      for (size_t i = 0; i < tok.attrs.size(); ++i)
        if (!html_->HasAttr(tok.attrs[i].name))
          html_->SetAttr(tok.attrs[i].name, tok.attrs[i].value);
      return;
    }
    if (t == "head") {
      if (!body_) {
        EnsureHead();
        if (stack_.size() == 1) stack_.push_back(head_);
      }
      return;
    }
    if (t == "body") {
      if (!body_) {
        EnsureHead();
        std::unique_ptr<Node> n(new Node(Node::kElement));
        n->tag = "body";
        n->attrs = tok.attrs;
        n->UpdateCaches();
        body_ = html_->AppendChild(std::move(n));
        stack_.clear();
        stack_.push_back(html_);
        stack_.push_back(body_);
      } else {
        for (size_t i = 0; i < tok.attrs.size(); ++i)
          if (!body_->HasAttr(tok.attrs[i].name))
            body_->SetAttr(tok.attrs[i].name, tok.attrs[i].value);
      }
      return;
    }
    if (t == "frameset" || t == "frame") {
      // Framesets are rendered as a list of links to the frames.
      EnsureBody();
      if (t == "frame" && !tok.AttrValue("src").empty()) {
        HtmlToken a;
        a.type = HtmlToken::kStartTag;
        a.name = "a";
        Attribute href;
        href.name = "href";
        href.value = tok.AttrValue("src");
        a.attrs.push_back(href);
        Node* div = Insert(MakeTag("div"), Current());
        Node* link = Insert(a, div);
        InsertText("Frame: " + href.value, link);
      }
      return;
    }

    if (!body_) {
      if (InList(t, kHeadElems)) {
        EnsureHead();
        Node* parent = head_;
        // Elements inside <noscript> in head stay inside it.
        if (Current()->tag == "noscript" && Current()->parent == head_) parent = Current();
        Node* n = Insert(tok, parent);
        if (t == "noscript" || t == "template") {
          stack_.push_back(n);
        } else if (!InList(t, kVoid) && !tok.selfClosing) {
          if (IsRaw(t)) {
            stack_.push_back(n);
            StartRaw(t);
          }
        }
        return;
      }
      EnsureBody();
    }

    if (InForeign()) {
      Node* n = Insert(tok, Current());
      if (!tok.selfClosing) stack_.push_back(n);
      return;
    }

    // Head-type elements appearing in body (style/script/link/meta): insert
    // in place.
    if (InList(t, kClosesP)) CloseP();
    if (InList(t, kHeadings) && InList(Current()->tag, kHeadings)) stack_.pop_back();

    if (t == "li" || t == "dd" || t == "dt") {
      for (int i = (int)stack_.size() - 1; i >= 0; --i) {
        const std::string& ct = stack_[i]->tag;
        bool match = (t == "li") ? ct == "li" : (ct == "dd" || ct == "dt");
        if (match) {
          PopUntilIndex(i);
          break;
        }
        if (InList(ct, kSpecial) && ct != "address" && ct != "div" && ct != "p") break;
      }
      CloseP();
    }
    if (t == "option") {
      if (Current()->tag == "option") stack_.pop_back();
    }
    if (t == "optgroup") {
      if (Current()->tag == "option") stack_.pop_back();
      if (Current()->tag == "optgroup") stack_.pop_back();
    }
    if (t == "a") {
      int i = FindInScope("a", kScopeBoundary);
      if (i >= 0) PopUntilIndex(i);
    }
    if (t == "nobr" || t == "button") {
      int i = FindInScope(t, kScopeBoundary);
      if (i >= 0) PopUntilIndex(i);
    }
    if (t == "form") {
      if (form_ && FindInScope("form", kScopeBoundary) >= 0) return;
    }
    if (t == "select") {
      int i = FindInScope("select", kScopeBoundary);
      if (i >= 0) {
        PopUntilIndex(i);
        return;
      }
    }

    // Table structure fix-ups.
    static const char* const kTableScope[] = {"html", "table", "template", 0};
    if (t == "td" || t == "th") {
      int ti = FindInScope("td", kTableScope);
      int hi = FindInScope("th", kTableScope);
      int ci = ti > hi ? ti : hi;
      if (ci >= 0) PopUntilIndex(ci);
      std::string ctx = TableContext();
      if (ctx == "table" || ctx == "tbody" || ctx == "thead" || ctx == "tfoot") {
        if (ctx == "table") stack_.push_back(Insert(MakeTag("tbody"), Current()));
        stack_.push_back(Insert(MakeTag("tr"), Current()));
      }
    } else if (t == "tr") {
      int ri = FindInScope("tr", kTableScope);
      if (ri >= 0) PopUntilIndex(ri);
      if (TableContext() == "table") stack_.push_back(Insert(MakeTag("tbody"), Current()));
    } else if (t == "tbody" || t == "thead" || t == "tfoot") {
      int i = -1;
      const char* secs[] = {"tbody", "thead", "tfoot"};
      for (int k = 0; k < 3; ++k) {
        int j = FindInScope(secs[k], kTableScope);
        if (j > i) i = j;
      }
      if (i >= 0) PopUntilIndex(i);
    } else if (t == "caption" || t == "colgroup") {
      int ti = FindInScope("table", kTableScope);
      if (ti >= 0) PopUntilIndex(ti + 1);
    } else if (t == "table") {
      // Nested <table> directly inside table context without a cell closes
      // the previous one (per spec).
      std::string ctx = Current()->tag;
      if (ctx == "table") {
        int ti = FindInScope("table", kTableScope);
        if (ti >= 0) PopUntilIndex(ti);
      }
    }

    Node* n = Insert(tok, Current());
    if (t == "form") form_ = n;
    if (InList(t, kVoid)) return;
    if (t == "pre" || t == "textarea" || t == "listing") dropNextNewline_ = true;
    if (t == "plaintext") {
      stack_.push_back(n);
      tokenizer_.SetPlaintext();
      return;
    }
    stack_.push_back(n);
    StartRaw(t);
  }

  std::string TableContext() {
    for (int i = (int)stack_.size() - 1; i >= 0; --i) {
      const std::string& t = stack_[i]->tag;
      if (t == "table" || t == "tbody" || t == "thead" || t == "tfoot" ||
          t == "tr" || t == "td" || t == "th" || t == "caption")
        return t;
      if (t == "html" || t == "body") break;
    }
    return std::string();
  }

  static HtmlToken MakeTag(const char* name) {
    HtmlToken t;
    t.type = HtmlToken::kStartTag;
    t.name = name;
    return t;
  }

  void ProcessEnd(const HtmlToken& tok) {
    const std::string& t = tok.name;
    if (!body_) {
      if (t == "head" || t == "noscript" || t == "template" || IsRaw(t)) {
        for (int i = (int)stack_.size() - 1; i >= 1; --i) {
          if (stack_[i]->tag == t) {
            PopUntilIndex(i);
            return;
          }
        }
        return;
      }
      if (t != "br") return;
    }
    if (t == "html" || t == "body" || t == "head") return;
    if (t == "br") {
      EnsureBody();
      Insert(MakeTag("br"), Current());
      return;
    }
    if (t == "p") {
      int i = FindInScope("p", kScopeBoundary, "button");
      if (i < 0) {
        Insert(MakeTag("p"), Current());
        return;
      }
      PopUntilIndex(i);
      return;
    }
    if (t == "form") form_ = 0;
    if (InForeign() || IsRaw(t)) {
      for (int i = (int)stack_.size() - 1; i >= 1; --i) {
        if (stack_[i]->tag == t) {
          PopUntilIndex(i);
          return;
        }
      }
      return;
    }
    static const char* const kTableScope[] = {"html", "table", "template", 0};
    if (t == "table" || t == "tbody" || t == "thead" || t == "tfoot" ||
        t == "tr" || t == "td" || t == "th" || t == "caption" || t == "colgroup") {
      int i = FindInScope(t, t == "table" ? kTableScope : kTableScope);
      if (i >= 0) PopUntilIndex(i);
      return;
    }
    if (t == "li") {
      int i = FindInScope("li", kScopeBoundary, "ol", "ul");
      if (i >= 0) PopUntilIndex(i);
      return;
    }
    if (InList(t, kHeadings)) {
      for (int i = (int)stack_.size() - 1; i >= 1; --i) {
        if (InList(stack_[i]->tag, kHeadings)) {
          PopUntilIndex(i);
          return;
        }
        if (InList(stack_[i]->tag, kScopeBoundary)) return;
      }
      return;
    }
    if (InList(t, kFormatting) || InList(t, kSpecial) || InList(t, kImpliedEnd)) {
      int i = FindInScope(t, kScopeBoundary);
      if (i >= 0) PopUntilIndex(i);
      return;
    }
    // Any other end tag.
    for (int i = (int)stack_.size() - 1; i >= 1; --i) {
      if (stack_[i]->tag == t) {
        PopUntilIndex(i);
        return;
      }
      if (InList(stack_[i]->tag, kSpecial)) return;
    }
  }

  HtmlTokenizer tokenizer_;
  std::unique_ptr<Document> doc_;
  std::vector<Node*> stack_;
  Node* html_;
  Node* head_;
  Node* body_;
  Node* form_;
  bool sawDoctype_;
  bool dropNextNewline_;
};

}  // namespace

std::unique_ptr<Document> ParseHtml(const std::string& utf8) {
  TreeBuilder b(utf8);
  return b.Run();
}

std::string SniffHtmlCharset(const std::string& bytes) {
  if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
      (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
    return "utf-8";
  if (bytes.size() >= 2) {
    unsigned char a = bytes[0], b = bytes[1];
    if (a == 0xFE && b == 0xFF) return "utf-16be";
    if (a == 0xFF && b == 0xFE) return "utf-16le";
  }
  std::string head = AsciiLower(bytes.substr(0, 4096));
  size_t p = 0;
  while ((p = head.find("<meta", p)) != std::string::npos) {
    size_t end = head.find('>', p);
    if (end == std::string::npos) break;
    std::string tag = head.substr(p, end - p);
    size_t c = tag.find("charset");
    if (c != std::string::npos) {
      size_t i = c + 7;
      while (i < tag.size() && (IsAsciiSpace((unsigned char)tag[i]))) ++i;
      if (i < tag.size() && tag[i] == '=') {
        ++i;
        while (i < tag.size() && (IsAsciiSpace((unsigned char)tag[i]) || tag[i] == '"' ||
                                  tag[i] == '\''))
          ++i;
        size_t s = i;
        while (i < tag.size() && tag[i] != '"' && tag[i] != '\'' && tag[i] != ';' &&
               !IsAsciiSpace((unsigned char)tag[i]) && tag[i] != '/')
          ++i;
        std::string cs = tag.substr(s, i - s);
        if (!cs.empty()) {
          // A meta can't really declare utf-16 (the bytes would be different).
          if (StartsWith(cs, "utf-16")) return "utf-8";
          return cs;
        }
      }
    }
    p = end;
  }
  return std::string();
}

}  // namespace kite
