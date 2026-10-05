#include "layout/box.h"

#include "base/strings.h"
#include "net/url.h"

namespace kite {

LayoutBox::LayoutBox(Kind k, Node* n, const ComputedStyle* s)
    : kind(k),
      node(n),
      style(s),
      parent(0),
      coordParent(0),
      inlineContent(false),
      x(0), y(0), w(0), h(0),
      relX(0), relY(0),
      firstBaseline(-1),
      lastBaseline(-1),
      collapsedMarginBottom(0),
      intrinsicW(0),
      intrinsicH(0),
      hasIntrinsic(false),
      colspan(1),
      rowspan(1),
      staticX(0),
      staticY(0),
      isPositionedChild(false),
      intrinsicValid(false),
      minContent(0),
      maxContent(0) {}

bool LayoutBox::IsAtomicInline() const {
  if (kind == kInline || kind == kText || kind == kLineBreak) return false;
  if (IsFloat() || IsOutOfFlow()) return false;
  return style->IsInlineLevel();
}

bool LayoutBox::EstablishesBfc() const {
  if (!parent) return true;
  if (IsFloat() || IsOutOfFlow()) return true;
  if (style->ClipsOverflow()) return true;
  if (kind == kTableCell || kind == kTableCaption || kind == kTable) return true;
  if (style->display == kDisplayInlineBlock) return true;
  if (parent->kind == kFlex || parent->kind == kGrid) return true;
  if (kind == kFlex || kind == kGrid) return true;
  return false;
}

void LayoutBox::AbsolutePosition(float& ax, float& ay) const {
  ax = 0;
  ay = 0;
  for (const LayoutBox* b = this; b; b = b->coordParent) {
    ax += b->x + b->relX;
    ay += b->y + b->relY;
  }
}

LayoutBox* LayoutBox::ContainingBlockForPositioned() {
  if (style->position == kPosFixed) {
    LayoutBox* r = this;
    while (r->parent) r = r->parent;
    return r;
  }
  LayoutBox* p = parent;
  while (p && p->parent) {
    if (p->style->position != kPosStatic && p->kind != kInline) return p;
    p = p->parent;
  }
  return p;
}

static std::string ToRoman(int n, bool upper) {
  if (n <= 0 || n >= 4000) return IntToString(n);
  static const int vals[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
  static const char* syms[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
  std::string out;
  for (int i = 0; i < 13; ++i)
    while (n >= vals[i]) {
      out += syms[i];
      n -= vals[i];
    }
  return upper ? AsciiUpper(out) : out;
}

static std::string ToAlpha(int n, bool upper) {
  if (n <= 0) return IntToString(n);
  std::string out;
  while (n > 0) {
    --n;
    out.insert(out.begin(), char((upper ? 'A' : 'a') + n % 26));
    n /= 26;
  }
  return out;
}

std::string ListMarkerText(ListStyle type, int index) {
  switch (type) {
    case kListDecimal: return IntToString(index) + ".";
    case kListDecimalLeadingZero:
      return (index < 10 && index >= 0 ? "0" : "") + IntToString(index) + ".";
    case kListLowerAlpha: return ToAlpha(index, false) + ".";
    case kListUpperAlpha: return ToAlpha(index, true) + ".";
    case kListLowerRoman: return ToRoman(index, false) + ".";
    case kListUpperRoman: return ToRoman(index, true) + ".";
    default: return std::string();
  }
}

namespace {

bool IsWhitespaceOnly(const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i)
    if (!IsAsciiSpace((unsigned char)s[i])) return false;
  return true;
}

std::string ApplyTextTransform(const std::string& in, TextTransform t, bool& atWordStart) {
  if (t == kTtNone) return in;
  std::string out;
  size_t i = 0;
  while (i < in.size()) {
    Codepoint c = Utf8Next(in, i);
    bool space = c == ' ' || c == '\n' || c == '\t';
    if (t == kTtUppercase || (t == kTtCapitalize && atWordStart && !space)) {
      if (c >= 'a' && c <= 'z') c -= 32;
      else if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) c -= 32;
    } else if (t == kTtLowercase) {
      if (c >= 'A' && c <= 'Z') c += 32;
      else if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32;
    }
    atWordStart = space || c == '-' || c == '(' || c == '"';
    Utf8Append(out, c);
  }
  return out;
}

// Collapses white space according to the white-space property.
std::string ProcessWhitespace(const std::string& in, WhiteSpace ws) {
  if (ws == kWsPre || ws == kWsPreWrap) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
      if (in[i] == '\r') {
        if (i + 1 < in.size() && in[i + 1] == '\n') continue;
        out += '\n';
      } else if (in[i] == '\t') {
        out += "        ";
      } else {
        out += in[i];
      }
    }
    return out;
  }
  bool keepNewlines = ws == kWsPreLine;
  std::string out;
  bool space = false;
  bool newline = false;
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      space = true;
      if (keepNewlines && c == '\n') newline = true;
      continue;
    }
    if (space) {
      out += newline ? '\n' : ' ';
      space = newline = false;
    }
    out += c;
  }
  if (space) out += newline ? '\n' : ' ';
  return out;
}

bool IsReplacedElement(const Node* el) {
  const std::string& t = el->tag;
  if (t == "img") return true;
  if (t == "input") return AsciiLower(el->Attr("type")) != "hidden";
  return t == "textarea" || t == "select" || t == "svg" || t == "video" || t == "iframe" ||
         t == "canvas" || t == "object" || t == "embed" || t == "meter" || t == "progress" ||
         (t == "audio" && el->HasAttr("controls"));
}

bool IsTableInternal(const LayoutBox* b) {
  return b->kind == LayoutBox::kTableRowGroup || b->kind == LayoutBox::kTableRow ||
         b->kind == LayoutBox::kTableCell;
}

bool IsBlockLevelBox(const LayoutBox* b) {
  if (b->kind == LayoutBox::kText || b->kind == LayoutBox::kInline ||
      b->kind == LayoutBox::kLineBreak)
    return false;
  if (b->IsFloat() || b->IsOutOfFlow()) return false;
  return !b->style->IsInlineLevel();
}

bool IsNeutral(const LayoutBox* b) {
  return b->kind != LayoutBox::kText && b->kind != LayoutBox::kInline &&
         (b->IsFloat() || b->IsOutOfFlow());
}

class Builder {
 public:
  explicit Builder(const std::string& baseUrl) : base_(Url::Parse(baseUrl)) {}

  std::unique_ptr<LayoutBox> BuildRoot(Document& doc) {
    Node* html = doc.DocumentElement();
    if (!html || !html->style) return std::unique_ptr<LayoutBox>();
    std::unique_ptr<LayoutBox> root = BuildElement(html);
    if (!root) {
      root.reset(new LayoutBox(LayoutBox::kBlockFlow, 0, html->style));
    }
    if (root->kind != LayoutBox::kBlockFlow && root->kind != LayoutBox::kFlex &&
        root->kind != LayoutBox::kGrid) {
      std::unique_ptr<LayoutBox> wrap(MakeAnonymous(LayoutBox::kBlockFlow, html->style,
                                                    kDisplayBlock));
      Append(wrap.get(), std::move(root));
      FixupBlock(wrap.get());
      root = std::move(wrap);
    }
    return root;
  }

 private:
  LayoutBox* MakeAnonymous(LayoutBox::Kind kind, const ComputedStyle* parentStyle, Display d) {
    ComputedStyle* s = new ComputedStyle;
    s->InheritFrom(*parentStyle);
    s->display = d;
    LayoutBox* b = new LayoutBox(kind, 0, s);
    b->ownStyle.reset(s);
    return b;
  }

  static void Append(LayoutBox* parent, std::unique_ptr<LayoutBox> child) {
    child->parent = parent;
    child->coordParent = parent;
    parent->children.push_back(std::move(child));
  }

  std::unique_ptr<LayoutBox> BuildPseudo(Node* el, const ComputedStyle* ps) {
    if (!ps) return std::unique_ptr<LayoutBox>();
    LayoutBox::Kind k = LayoutBox::kInline;
    switch (ps->display) {
      case kDisplayInline: k = LayoutBox::kInline; break;
      case kDisplayFlex: case kDisplayInlineFlex: k = LayoutBox::kFlex; break;
      default: k = LayoutBox::kBlockFlow; break;
    }
    std::unique_ptr<LayoutBox> box(new LayoutBox(k, el, ps));
    if (!ps->content.empty()) {
      bool ws = true;
      std::string t = ApplyTextTransform(ProcessWhitespace(ps->content, ps->whiteSpace),
                                         ps->textTransform, ws);
      std::unique_ptr<LayoutBox> text(new LayoutBox(LayoutBox::kText, el, ps));
      text->text = t;
      Append(box.get(), std::move(text));
    }
    if (k == LayoutBox::kBlockFlow) box->inlineContent = true;
    if (k == LayoutBox::kFlex) FixupFlex(box.get());
    return box;
  }

  void ResolveImage(LayoutBox* box, Node* el) {
    std::string src = Trim(el->Attr("src"));
    // Lazy-loading patterns: prefer data-src when src is a placeholder.
    std::string dataSrc = Trim(el->Attr("data-src"));
    if (dataSrc.empty()) dataSrc = Trim(el->Attr("data-lazy-src"));
    if (dataSrc.empty()) dataSrc = Trim(el->Attr("data-original"));
    if (!dataSrc.empty() && (src.empty() || StartsWithIgnoreCase(src, "data:")))
      src = dataSrc;
    // <picture>: the first <source> with a supported type wins (media
    // conditions are not evaluated; such sources are skipped).
    if (el->tag == "img" && el->parent && el->parent->Is("picture")) {
      for (size_t i = 0; i < el->parent->children.size(); ++i) {
        Node* c = el->parent->children[i].get();
        if (c == el) break;
        if (!c->Is("source") || c->HasAttr("media")) continue;
        std::string type = AsciiLower(Trim(c->Attr("type")));
        if (!type.empty() && type != "image/webp" && type != "image/png" && type != "image/jpeg" &&
            type != "image/gif" && type != "image/svg+xml" && type != "image/bmp")
          continue;  // e.g. AVIF, JPEG XL
        std::string set = c->Attr("srcset");
        if (set.empty()) set = c->Attr("data-srcset");
        std::vector<std::string> first = SplitWhitespace(Split(set, ',')[0]);
        if (!first.empty()) {
          src = first[0];
          break;
        }
      }
    }
    if (src.empty()) {
      std::string srcset = el->Attr("srcset");
      if (srcset.empty()) srcset = el->Attr("data-srcset");
      if (!srcset.empty()) {
        std::vector<std::string> parts = Split(srcset, ',');
        std::vector<std::string> first = SplitWhitespace(parts[0]);
        if (!first.empty()) src = first[0];
      }
    }
    if (el->tag == "input") src = Trim(el->Attr("src"));
    if (el->tag == "video") src = Trim(el->Attr("poster"));
    if (src.empty()) return;
    Url u = base_.Resolve(src);
    if (u.valid()) box->imageUrl = u.Spec();
  }

  std::unique_ptr<LayoutBox> BuildElement(Node* el) {
    const ComputedStyle* s = el->style;
    if (!s || s->display == kDisplayNone) return std::unique_ptr<LayoutBox>();
    if (el->tag == "br") {
      std::unique_ptr<LayoutBox> br(new LayoutBox(LayoutBox::kLineBreak, el, s));
      el->layoutBox = br.get();
      return br;
    }
    if (el->tag == "wbr") return std::unique_ptr<LayoutBox>();
    bool replaced = IsReplacedElement(el) &&
                    !(el->tag == "input" && AsciiLower(el->Attr("type")) == "image" &&
                      false);
    if (el->tag == "object" || el->tag == "embed") {
      // Plugins are not supported: render the fallback content instead.
      if (el->tag == "object" && !el->children.empty()) replaced = false;
    }
    LayoutBox::Kind kind;
    switch (s->display) {
      case kDisplayInline: kind = LayoutBox::kInline; break;
      case kDisplayTable: case kDisplayInlineTable: kind = LayoutBox::kTable; break;
      case kDisplayTableRowGroup: case kDisplayTableHeaderGroup: case kDisplayTableFooterGroup:
        kind = LayoutBox::kTableRowGroup; break;
      case kDisplayTableRow: kind = LayoutBox::kTableRow; break;
      case kDisplayTableCell: kind = LayoutBox::kTableCell; break;
      case kDisplayTableCaption: kind = LayoutBox::kTableCaption; break;
      case kDisplayTableColumn: case kDisplayTableColumnGroup:
        return std::unique_ptr<LayoutBox>();
      case kDisplayFlex: case kDisplayInlineFlex: kind = LayoutBox::kFlex; break;
      case kDisplayGrid: case kDisplayInlineGrid: kind = LayoutBox::kGrid; break;
      default: kind = LayoutBox::kBlockFlow; break;
    }
    if (replaced) kind = LayoutBox::kReplaced;
    std::unique_ptr<LayoutBox> box(new LayoutBox(kind, el, s));
    el->layoutBox = box.get();
    if (replaced) {
      if (el->tag == "img" || el->tag == "input" || el->tag == "video") ResolveImage(box.get(), el);
      if (el->tag == "td" || el->tag == "th") {}
      return box;
    }
    if (kind == LayoutBox::kTableCell) {
      long long n;
      if (ParseInt(el->Attr("colspan"), n) && n > 1) box->colspan = (int)std::min(n, 1000LL);
      if (ParseInt(el->Attr("rowspan"), n) && n > 1) box->rowspan = (int)std::min(n, 1000LL);
    }
    if (s->display == kDisplayListItem) box->markerText = MarkerFor(el, s);

    std::unique_ptr<LayoutBox> before = BuildPseudo(el, s->before.get());
    if (before) Append(box.get(), std::move(before));
    BuildChildren(el, box.get());
    std::unique_ptr<LayoutBox> after = BuildPseudo(el, s->after.get());
    if (after) Append(box.get(), std::move(after));

    switch (box->kind) {
      case LayoutBox::kInline: {
        bool hasBlock = false;
        for (size_t i = 0; i < box->children.size(); ++i)
          if (IsBlockLevelBox(box->children[i].get())) hasBlock = true;
        if (hasBlock) {
          // Block-in-inline: treat the inline element as a block container.
          box->kind = LayoutBox::kBlockFlow;
          ComputedStyle* ns = new ComputedStyle;
          ns->CopyFrom(*s);
          ns->display = kDisplayBlock;
          box->style = ns;
          box->ownStyle.reset(ns);
          FixupBlock(box.get());
        }
        break;
      }
      case LayoutBox::kBlockFlow:
      case LayoutBox::kTableCell:
      case LayoutBox::kTableCaption:
        FixupBlock(box.get());
        break;
      case LayoutBox::kFlex:
      case LayoutBox::kGrid:
        FixupFlex(box.get());
        break;
      case LayoutBox::kTable:
        FixupTable(box.get());
        break;
      case LayoutBox::kTableRowGroup:
        FixupRowGroup(box.get());
        break;
      case LayoutBox::kTableRow:
        FixupRow(box.get());
        break;
      default:
        break;
    }
    return box;
  }

  std::string MarkerFor(Node* el, const ComputedStyle* s) {
    if (s->listStyleType == kListNone) return std::string();
    if (s->listStyleType == kListDisc || s->listStyleType == kListCircle ||
        s->listStyleType == kListSquare)
      return std::string();
    int index = 1;
    long long v;
    Node* parent = el->parent;
    bool reversed = false;
    int count = 0;
    if (parent && parent->Is("ol")) {
      if (ParseInt(parent->Attr("start"), v)) index = (int)v;
      reversed = parent->HasAttr("reversed");
      if (reversed) {
        for (size_t i = 0; i < parent->children.size(); ++i)
          if (parent->children[i]->Is("li")) ++count;
        if (!ParseInt(parent->Attr("start"), v)) index = count;
      }
    }
    int n = index;
    if (parent) {
      for (size_t i = 0; i < el->index; ++i) {
        Node* sib = parent->children[i].get();
        if (sib->IsElement() && sib->style && sib->style->display == kDisplayListItem) {
          if (ParseInt(sib->Attr("value"), v)) n = (int)v;
          n += reversed ? -1 : 1;
        }
      }
    }
    if (ParseInt(el->Attr("value"), v)) n = (int)v;
    return ListMarkerText(s->listStyleType, n);
  }

  void BuildChildren(Node* el, LayoutBox* box) {
    const ComputedStyle* s = el->style;
    for (size_t i = 0; i < el->children.size(); ++i) {
      Node* c = el->children[i].get();
      if (c->IsText()) {
        if (c->text.empty()) continue;
        std::string t = ProcessWhitespace(c->text, s->whiteSpace);
        if (t.empty()) continue;
        bool ws = true;
        t = ApplyTextTransform(t, s->textTransform, ws);
        std::unique_ptr<LayoutBox> tb(new LayoutBox(LayoutBox::kText, el, s));
        tb->text = t;
        Append(box, std::move(tb));
      } else if (c->IsElement()) {
        if (c->style && c->style->display == kDisplayContents) {
          BuildChildren(c, box);
          continue;
        }
        std::unique_ptr<LayoutBox> cb = BuildElement(c);
        if (cb) Append(box, std::move(cb));
      }
    }
  }

  bool CollapsesWhitespace(const ComputedStyle* s) {
    return s->whiteSpace != kWsPre && s->whiteSpace != kWsPreWrap;
  }

  // Wraps runs of children for which |inRun| is true into anonymous boxes
  // created by |make|.
  template <typename Pred, typename Make>
  void WrapRuns(LayoutBox* box, Pred inRun, Make make) {
    std::vector<std::unique_ptr<LayoutBox> > old;
    old.swap(box->children);
    LayoutBox* current = 0;
    for (size_t i = 0; i < old.size(); ++i) {
      LayoutBox* c = old[i].get();
      if (inRun(c)) {
        if (!current) {
          std::unique_ptr<LayoutBox> anon(make());
          current = anon.get();
          Append(box, std::move(anon));
        }
        Append(current, std::move(old[i]));
      } else {
        current = 0;
        Append(box, std::move(old[i]));
      }
    }
  }

  void DropWhitespaceText(LayoutBox* box) {
    std::vector<std::unique_ptr<LayoutBox> > old;
    old.swap(box->children);
    for (size_t i = 0; i < old.size(); ++i) {
      LayoutBox* c = old[i].get();
      if (c->kind == LayoutBox::kText && IsWhitespaceOnly(c->text) &&
          CollapsesWhitespace(c->style))
        continue;
      Append(box, std::move(old[i]));
    }
  }

  void FixupBlock(LayoutBox* box) {
    // Orphaned table parts get an anonymous table.
    bool hasTablePart = false;
    for (size_t i = 0; i < box->children.size(); ++i)
      if (IsTableInternal(box->children[i].get())) hasTablePart = true;
    if (hasTablePart) {
      const ComputedStyle* ps = box->style;
      WrapRuns(box, [](LayoutBox* c) { return IsTableInternal(c); },
               [this, ps]() { return MakeAnonymous(LayoutBox::kTable, ps, kDisplayTable); });
      for (size_t i = 0; i < box->children.size(); ++i)
        if (box->children[i]->kind == LayoutBox::kTable && !box->children[i]->node)
          FixupTable(box->children[i].get());
    }

    bool hasBlock = false, hasInline = false;
    for (size_t i = 0; i < box->children.size(); ++i) {
      LayoutBox* c = box->children[i].get();
      if (IsNeutral(c)) continue;
      if (IsBlockLevelBox(c)) hasBlock = true;
      else if (!(c->kind == LayoutBox::kText && IsWhitespaceOnly(c->text) &&
                 CollapsesWhitespace(c->style)))
        hasInline = true;
    }
    if (!hasBlock) {
      box->inlineContent = true;
      return;
    }
    box->inlineContent = false;
    if (!hasInline) {
      DropWhitespaceText(box);
      return;
    }
    // Mixed content: wrap inline runs (plus adjacent neutral boxes) in
    // anonymous block boxes.
    std::vector<std::unique_ptr<LayoutBox> > old;
    old.swap(box->children);
    LayoutBox* current = 0;
    for (size_t i = 0; i < old.size(); ++i) {
      LayoutBox* c = old[i].get();
      bool block = IsBlockLevelBox(c);
      bool neutral = IsNeutral(c);
      if (block || (neutral && !current)) {
        current = 0;
        Append(box, std::move(old[i]));
        continue;
      }
      if (!current) {
        if (c->kind == LayoutBox::kText && IsWhitespaceOnly(c->text) &&
            CollapsesWhitespace(c->style))
          continue;
        std::unique_ptr<LayoutBox> anon(MakeAnonymous(LayoutBox::kBlockFlow, box->style,
                                                      kDisplayBlock));
        anon->inlineContent = true;
        current = anon.get();
        Append(box, std::move(anon));
      }
      Append(current, std::move(old[i]));
    }
  }

  void FixupFlex(LayoutBox* box) {
    const ComputedStyle* ps = box->style;
    DropWhitespaceText(box);
    // Inline children are blockified into flex items of their own; only
    // runs of bare text get anonymous wrappers.
    for (size_t i = 0; i < box->children.size(); ++i) {
      LayoutBox* c = box->children[i].get();
      if (c->kind != LayoutBox::kInline) continue;
      ComputedStyle* ns = new ComputedStyle;
      ns->CopyFrom(*c->style);
      ns->display = kDisplayBlock;
      c->style = ns;
      c->ownStyle.reset(ns);
      c->kind = LayoutBox::kBlockFlow;
      FixupBlock(c);
    }
    WrapRuns(box,
             [](LayoutBox* c) {
               return c->kind == LayoutBox::kText || c->kind == LayoutBox::kLineBreak;
             },
             [this, ps]() {
               LayoutBox* b = MakeAnonymous(LayoutBox::kBlockFlow, ps, kDisplayBlock);
               b->inlineContent = true;
               return b;
             });
  }

  void FixupTable(LayoutBox* box) {
    DropWhitespaceText(box);
    const ComputedStyle* ps = box->style;
    // Non-table content becomes an anonymous cell.
    WrapRuns(box,
             [](LayoutBox* c) {
               return c->kind != LayoutBox::kTableRowGroup && c->kind != LayoutBox::kTableRow &&
                      c->kind != LayoutBox::kTableCaption && c->kind != LayoutBox::kTableCell &&
                      !c->IsOutOfFlow();
             },
             [this, ps]() {
               LayoutBox* b = MakeAnonymous(LayoutBox::kTableCell, ps, kDisplayTableCell);
               return b;
             });
    for (size_t i = 0; i < box->children.size(); ++i)
      if (box->children[i]->kind == LayoutBox::kTableCell && !box->children[i]->node)
        FixupBlock(box->children[i].get());
    WrapRuns(box, [](LayoutBox* c) { return c->kind == LayoutBox::kTableCell; },
             [this, ps]() { return MakeAnonymous(LayoutBox::kTableRow, ps, kDisplayTableRow); });
    WrapRuns(box, [](LayoutBox* c) { return c->kind == LayoutBox::kTableRow; },
             [this, ps]() {
               return MakeAnonymous(LayoutBox::kTableRowGroup, ps, kDisplayTableRowGroup);
             });
  }

  void FixupRowGroup(LayoutBox* box) {
    DropWhitespaceText(box);
    const ComputedStyle* ps = box->style;
    WrapRuns(box,
             [](LayoutBox* c) { return c->kind != LayoutBox::kTableRow && !c->IsOutOfFlow(); },
             [this, ps]() { return MakeAnonymous(LayoutBox::kTableRow, ps, kDisplayTableRow); });
    for (size_t i = 0; i < box->children.size(); ++i)
      if (box->children[i]->kind == LayoutBox::kTableRow && !box->children[i]->node)
        FixupRow(box->children[i].get());
  }

  void FixupRow(LayoutBox* box) {
    DropWhitespaceText(box);
    const ComputedStyle* ps = box->style;
    WrapRuns(box,
             [](LayoutBox* c) { return c->kind != LayoutBox::kTableCell && !c->IsOutOfFlow(); },
             [this, ps]() {
               return MakeAnonymous(LayoutBox::kTableCell, ps, kDisplayTableCell);
             });
    for (size_t i = 0; i < box->children.size(); ++i)
      if (box->children[i]->kind == LayoutBox::kTableCell && !box->children[i]->node)
        FixupBlock(box->children[i].get());
  }

  Url base_;
};

}  // namespace

static void ClearBoxPointers(Node* n) {
  n->layoutBox = 0;
  for (size_t i = 0; i < n->children.size(); ++i) ClearBoxPointers(n->children[i].get());
}

std::unique_ptr<LayoutBox> BuildLayoutTree(Document& doc, const std::string& baseUrl) {
  ClearBoxPointers(doc.root.get());
  Builder b(baseUrl);
  return b.BuildRoot(doc);
}

}  // namespace kite
