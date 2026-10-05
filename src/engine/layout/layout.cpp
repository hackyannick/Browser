#include "layout/layout.h"

#include <algorithm>
#include <cmath>

#include "base/strings.h"

namespace kite {

FontDesc FontDesc::FromStyle(const ComputedStyle& s) {
  FontDesc d;
  d.family = s.fontFamily;
  d.size = s.fontSize;
  d.weight = s.fontWeight;
  d.italic = s.italic;
  return d;
}

FontMetrics SimpleFontProvider::Metrics(const FontDesc& f) {
  FontMetrics m;
  m.ascent = f.size * 0.8f;
  m.descent = f.size * 0.2f;
  m.lineGap = f.size * 0.15f;
  return m;
}

float SimpleFontProvider::MeasureText(const FontDesc& f, const std::string& utf8) {
  bool mono = f.family.find("mono") != std::string::npos ||
              f.family.find("courier") != std::string::npos;
  float per = mono ? 0.6f : 0.5f;
  if (f.weight >= 600) per *= 1.08f;
  return (float)Utf8Length(utf8) * f.size * per;
}

// ---------------------------------------------------------------------------
// FloatContext

void FloatContext::Available(float y, float h, float& l, float& r) const {
  if (h <= 0) h = 0.01f;
  for (size_t i = 0; i < floats_.size(); ++i) {
    const Placed& f = floats_[i];
    if (f.y >= y + h || f.y + f.h <= y) continue;
    if (f.left) l = std::max(l, f.x + f.w);
    else r = std::min(r, f.x);
  }
}

float FloatContext::NextBottomBelow(float y) const {
  float best = -1;
  for (size_t i = 0; i < floats_.size(); ++i) {
    float b = floats_[i].y + floats_[i].h;
    if (b > y && (best < 0 || b < best)) best = b;
  }
  return best;
}

void FloatContext::Place(float w, float h, bool left, float minY, float cl, float cr,
                         float& outX, float& outY) {
  float y = std::max(minY, lastTop_);
  for (int guard = 0; guard < 1000; ++guard) {
    float l = cl, r = cr;
    Available(y, h > 0 ? h : 0.01f, l, r);
    if (r - l >= w || (l == cl && r == cr)) {
      outX = left ? l : r - w;
      outY = y;
      break;
    }
    float next = NextBottomBelow(y);
    if (next < 0) {
      outX = left ? l : r - w;
      outY = y;
      break;
    }
    y = next;
  }
  Placed p;
  p.x = outX;
  p.y = outY;
  p.w = w;
  p.h = h;
  p.left = left;
  floats_.push_back(p);
  lastTop_ = outY;
}

float FloatContext::ClearY(Clear side) const {
  float y = -1e9f;
  for (size_t i = 0; i < floats_.size(); ++i) {
    const Placed& f = floats_[i];
    if (side == kClearBoth || (side == kClearLeft && f.left) || (side == kClearRight && !f.left))
      y = std::max(y, f.y + f.h);
  }
  return y;
}

float FloatContext::MaxBottom() const {
  float y = 0;
  for (size_t i = 0; i < floats_.size(); ++i) y = std::max(y, floats_[i].y + floats_[i].h);
  return y;
}

// ---------------------------------------------------------------------------
// Helpers

namespace {

float CollapseMargins(float a, float b) {
  float pos = std::max(std::max(a, 0.0f), std::max(b, 0.0f));
  float neg = std::min(std::min(a, 0.0f), std::min(b, 0.0f));
  return pos + neg;
}

float ResolvePct(const Length& l, float base) {
  if (!l.IsFixed()) return 0;
  return l.Resolve(base);
}

bool IsCjk(Codepoint c) {
  return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
         (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) ||
         (c >= 0x20000 && c <= 0x2FFFF);
}

struct InlineItem {
  enum Type { kText, kOpen, kClose, kAtomic, kBreak, kFloat, kAbs };
  Type type;
  LayoutBox* box;
};

void CollectInlineItems(LayoutBox* b, std::vector<InlineItem>& out) {
  for (size_t i = 0; i < b->children.size(); ++i) {
    LayoutBox* c = b->children[i].get();
    InlineItem it;
    it.box = c;
    if (c->kind == LayoutBox::kText) {
      it.type = InlineItem::kText;
      out.push_back(it);
    } else if (c->kind == LayoutBox::kLineBreak) {
      it.type = InlineItem::kBreak;
      out.push_back(it);
    } else if (c->IsOutOfFlow()) {
      it.type = InlineItem::kAbs;
      out.push_back(it);
    } else if (c->IsFloat()) {
      it.type = InlineItem::kFloat;
      out.push_back(it);
    } else if (c->kind == LayoutBox::kInline) {
      it.type = InlineItem::kOpen;
      out.push_back(it);
      CollectInlineItems(c, out);
      it.type = InlineItem::kClose;
      out.push_back(it);
    } else {
      it.type = InlineItem::kAtomic;
      out.push_back(it);
    }
  }
}

struct TextToken {
  enum Type { kWord, kSpace, kNewline };
  Type type;
  size_t start, len;
};

void Tokenize(const std::string& s, std::vector<TextToken>& out) {
  out.clear();
  size_t i = 0;
  size_t wordStart = std::string::npos;
  while (i < s.size()) {
    size_t at = i;
    Codepoint c = Utf8Next(s, i);
    if (c == ' ' || c == '\n') {
      if (wordStart != std::string::npos) {
        TextToken t = {TextToken::kWord, wordStart, at - wordStart};
        out.push_back(t);
        wordStart = std::string::npos;
      }
      TextToken t = {c == ' ' ? TextToken::kSpace : TextToken::kNewline, at, 1};
      out.push_back(t);
      continue;
    }
    if (IsCjk(c)) {
      if (wordStart != std::string::npos) {
        TextToken t = {TextToken::kWord, wordStart, at - wordStart};
        out.push_back(t);
        wordStart = std::string::npos;
      }
      TextToken t = {TextToken::kWord, at, i - at};
      out.push_back(t);
      continue;
    }
    if (wordStart == std::string::npos) wordStart = at;
    // Break opportunity after a hyphen inside a word.
    if ((c == '-' || c == 0x2013 || c == 0x2014 || c == '/') && at > wordStart &&
        i < s.size() && s[i] != ' ' && s[i] != '\n') {
      TextToken t = {TextToken::kWord, wordStart, i - wordStart};
      out.push_back(t);
      wordStart = std::string::npos;
    }
  }
  if (wordStart != std::string::npos) {
    TextToken t = {TextToken::kWord, wordStart, s.size() - wordStart};
    out.push_back(t);
  }
}

bool Wraps(const ComputedStyle* s) {
  return s->whiteSpace != kWsPre && s->whiteSpace != kWsNowrap;
}

bool CollapsesSpaces(const ComputedStyle* s) {
  return s->whiteSpace != kWsPre && s->whiteSpace != kWsPreWrap;
}

float LineHeightOf(const ComputedStyle* s, const FontMetrics& fm) {
  if (s->lineHeight >= 0) return s->lineHeight;
  return fm.ascent + fm.descent + fm.lineGap;
}

bool CanCollapseTopWithChild(const LayoutBox* b, const LayoutBox* root) {
  if (b == root || b->kind != LayoutBox::kBlockFlow || b->inlineContent) return false;
  if (b->EstablishesBfc()) return false;
  const ComputedStyle* s = b->style;
  if (s->border[0].Used() > 0 || !s->padding[0].IsZero()) return false;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// LayoutEngine

LayoutEngine::LayoutEngine(FontProvider* fonts, ImageProvider* images)
    : fonts_(fonts), images_(images), viewportW_(800), viewportH_(600), root_(0) {}

float LayoutEngine::Measure(const ComputedStyle* style, const std::string& text) {
  if (text.empty()) return 0;
  FontDesc fd = FontDesc::FromStyle(*style);
  std::map<FontDesc, int>::iterator it = fontIds_.find(fd);
  int id;
  if (it == fontIds_.end()) {
    id = (int)fontIds_.size();
    fontIds_[fd] = id;
  } else {
    id = it->second;
  }
  std::string key = IntToString(id);
  key += '\x1f';
  key += text;
  std::unordered_map<std::string, float>::iterator m = measureCache_.find(key);
  float w;
  if (m != measureCache_.end()) {
    w = m->second;
  } else {
    if (measureCache_.size() > 200000) measureCache_.clear();
    w = fonts_->MeasureText(fd, text);
    measureCache_[key] = w;
  }
  if (style->letterSpacing != 0) w += style->letterSpacing * (float)Utf8Length(text);
  return w;
}

FontMetrics LayoutEngine::Metrics(const ComputedStyle* style) {
  FontDesc fd = FontDesc::FromStyle(*style);
  std::map<FontDesc, FontMetrics>::iterator it = metricsCache_.find(fd);
  if (it != metricsCache_.end()) return it->second;
  FontMetrics m = fonts_->Metrics(fd);
  metricsCache_[fd] = m;
  return m;
}

void LayoutEngine::Layout(LayoutBox* root, float viewportW, float viewportH) {
  viewportW_ = viewportW;
  viewportH_ = viewportH;
  root_ = root;
  if (!root) return;
  root->x = 0;
  root->y = 0;
  ResolveEdges(root, viewportW);
  root->x = root->margin.left;
  root->y = CollapsedTopMargin(root, viewportW);
  FloatContext fc;
  LayoutBlockLevel(root, viewportW, viewportH, &fc, root->x, root->y);
}

void LayoutEngine::ResolveEdges(LayoutBox* b, float cbW) {
  const ComputedStyle* s = b->style;
  float base = cbW >= 0 ? cbW : 0;
  b->margin.top = ResolvePct(s->margin[0], base);
  b->margin.right = ResolvePct(s->margin[1], base);
  b->margin.bottom = ResolvePct(s->margin[2], base);
  b->margin.left = ResolvePct(s->margin[3], base);
  b->padding.top = std::max(0.0f, ResolvePct(s->padding[0], base));
  b->padding.right = std::max(0.0f, ResolvePct(s->padding[1], base));
  b->padding.bottom = std::max(0.0f, ResolvePct(s->padding[2], base));
  b->padding.left = std::max(0.0f, ResolvePct(s->padding[3], base));
  b->border.top = s->border[0].Used();
  b->border.right = s->border[1].Used();
  b->border.bottom = s->border[2].Used();
  b->border.left = s->border[3].Used();
  if (b->kind == LayoutBox::kText) {
    b->margin = Edges();
    b->padding = Edges();
    b->border = Edges();
  }
}

float LayoutEngine::ClampWidth(LayoutBox* b, float w, float cbW) {
  const ComputedStyle* s = b->style;
  float pb = b->padding.left + b->padding.right + b->border.left + b->border.right;
  if (s->maxWidth.IsFixed() && !(s->maxWidth.HasPercent() && cbW < 0)) {
    float mx = s->maxWidth.Resolve(cbW);
    if (s->boxSizing == kContentBox) mx += pb;
    w = std::min(w, mx);
  }
  if (s->minWidth.IsFixed() && !(s->minWidth.HasPercent() && cbW < 0)) {
    float mn = s->minWidth.Resolve(cbW);
    if (s->boxSizing == kContentBox) mn += pb;
    w = std::max(w, mn);
  }
  return std::max(w, pb);
}

float LayoutEngine::ClampHeight(LayoutBox* b, float h, float cbH) {
  const ComputedStyle* s = b->style;
  float pb = b->padding.top + b->padding.bottom + b->border.top + b->border.bottom;
  if (s->maxHeight.IsFixed() && !(s->maxHeight.HasPercent() && cbH < 0)) {
    float mx = s->maxHeight.Resolve(cbH);
    if (s->boxSizing == kContentBox) mx += pb;
    h = std::min(h, mx);
  }
  if (s->minHeight.IsFixed() && !(s->minHeight.HasPercent() && cbH < 0)) {
    float mn = s->minHeight.Resolve(cbH);
    if (s->boxSizing == kContentBox) mn += pb;
    h = std::max(h, mn);
  }
  return std::max(h, pb);
}

float LayoutEngine::ResolveSpecifiedHeight(LayoutBox* b, float cbH) {
  const ComputedStyle* s = b->style;
  if (!s->height.IsFixed()) return -1;
  if (s->height.HasPercent() && cbH < 0) return -1;
  float h = s->height.Resolve(cbH);
  if (s->boxSizing == kContentBox)
    h += b->padding.top + b->padding.bottom + b->border.top + b->border.bottom;
  return h;
}

float LayoutEngine::ShrinkToFit(LayoutBox* b, float available) {
  float mn, mx;
  ComputeIntrinsic(b, mn, mx);
  float outer = std::min(std::max(mn, available), mx);
  return outer - b->margin.left - b->margin.right;
}

float LayoutEngine::SolveWidth(LayoutBox* b, float cbW, float available, bool shrinkToFit) {
  const ComputedStyle* s = b->style;
  float pb = b->padding.left + b->padding.right + b->border.left + b->border.right;
  bool autoW = !s->width.IsFixed() || (s->width.HasPercent() && cbW < 0);
  float w;
  if (s->width.kind == Length::kMinContent || s->width.kind == Length::kMaxContent) {
    float mn, mx;
    ComputeIntrinsic(b, mn, mx);
    w = (s->width.kind == Length::kMinContent ? mn : mx) - b->margin.left - b->margin.right;
    shrinkToFit = true;
  } else if (s->width.kind == Length::kFitContent) {
    w = ShrinkToFit(b, available);
    shrinkToFit = true;
  } else if (!autoW) {
    w = s->width.Resolve(cbW);
    if (s->boxSizing == kContentBox) w += pb;
  } else if (shrinkToFit) {
    w = ShrinkToFit(b, available);
  } else {
    w = available - b->margin.left - b->margin.right;
  }
  w = ClampWidth(b, w, cbW);
  if (!shrinkToFit && !b->IsFloat()) {
    bool ml = s->margin[3].IsAuto(), mr = s->margin[1].IsAuto();
    float free = available - w - (ml ? 0 : b->margin.left) - (mr ? 0 : b->margin.right);
    if (ml && mr) {
      b->margin.left = b->margin.right = std::max(0.0f, free / 2);
    } else if (ml) {
      b->margin.left = std::max(0.0f, free);
    } else if (mr) {
      b->margin.right = free;
    }
  }
  return w;
}

float LayoutEngine::CollapsedTopMargin(LayoutBox* b, float cbW) {
  float mt = ResolvePct(b->style->margin[0], cbW >= 0 ? cbW : 0);
  if (!CanCollapseTopWithChild(b, root_)) return mt;
  for (size_t i = 0; i < b->children.size(); ++i) {
    LayoutBox* c = b->children[i].get();
    if (c->IsOutOfFlow() || c->IsFloat()) continue;
    if (c->kind == LayoutBox::kText || c->kind == LayoutBox::kInline) return mt;
    return CollapseMargins(mt, CollapsedTopMargin(c, cbW));
  }
  return mt;
}

void LayoutEngine::ApplyRelativeOffset(LayoutBox* b, float cbW, float cbH) {
  b->relX = b->relY = 0;
  if (b->style->position != kPosRelative) return;
  const Length* in = b->style->inset;
  if (in[3].IsFixed()) b->relX = in[3].Resolve(cbW);
  else if (in[1].IsFixed()) b->relX = -in[1].Resolve(cbW);
  if (in[0].IsFixed() && !(in[0].HasPercent() && cbH < 0)) b->relY = in[0].Resolve(cbH);
  else if (in[2].IsFixed() && !(in[2].HasPercent() && cbH < 0)) b->relY = -in[2].Resolve(cbH);
}

void LayoutEngine::RegisterPositioned(LayoutBox* child, float staticX, float staticY) {
  child->staticX = staticX;
  child->staticY = staticY;
  LayoutBox* cb = child->ContainingBlockForPositioned();
  if (!cb) cb = root_;
  cb->positioned.push_back(child);
}

void LayoutEngine::LayoutBlockLevel(LayoutBox* b, float cbW, float cbH, FloatContext* fc,
                                    float bfcX, float bfcY, float forcedW, float forcedH) {
  b->positioned.clear();
  b->lines.clear();
  b->firstBaseline = b->lastBaseline = -1;
  b->isPositionedChild = false;
  Edges savedMargins = b->margin;
  ResolveEdges(b, cbW);
  if (forcedW >= 0) b->margin = savedMargins;
  b->collapsedMarginBottom = b->margin.bottom;
  if (b->kind == LayoutBox::kReplaced) {
    LayoutReplaced(b, cbW, cbH, forcedW, forcedH);
    ApplyRelativeOffset(b, cbW, cbH);
    return;
  }
  if (b->kind == LayoutBox::kTable) {
    TableLayout tl(this);
    tl.Layout(b, cbW, cbH, forcedW);
    if (forcedH >= 0 && forcedH > b->h) b->h = forcedH;
    ApplyRelativeOffset(b, cbW, cbH);
    if (b->style->position != kPosStatic || b == root_) LayoutPositionedChildren(b);
    return;
  }
  bool stf = b->IsFloat() || b->IsOutOfFlow() || b->style->display == kDisplayInlineBlock ||
             b->style->display == kDisplayInlineFlex || b->style->display == kDisplayInlineGrid;
  float w = forcedW >= 0 ? forcedW : SolveWidth(b, cbW, cbW, stf);
  b->w = w;

  FloatContext own;
  FloatContext* childFc = fc;
  float cbx = bfcX, cby = bfcY;
  if (!fc || b->EstablishesBfc()) {
    childFc = &own;
    cbx = 0;
    cby = 0;
  }
  float pbV = b->padding.top + b->padding.bottom + b->border.top + b->border.bottom;
  float pbH = b->padding.left + b->padding.right + b->border.left + b->border.right;
  float specH = forcedH >= 0 ? forcedH : ResolveSpecifiedHeight(b, cbH);
  if (specH < 0 && b->style->aspectRatio > 0 && b->style->height.IsAuto())
    specH = (w - pbH) / b->style->aspectRatio + pbV;
  float definite = specH >= 0 ? ClampHeight(b, specH, cbH) - pbV : -1;
  LayoutContents(b, cbH, childFc, cbx, cby, definite);
  float contentH = b->h;  // LayoutContents stores the content height in h
  if (childFc == &own) contentH = std::max(contentH, own.MaxBottom() - b->ContentY());
  float h = specH >= 0 ? specH : contentH + pbV;
  b->h = ClampHeight(b, h, cbH);
  ApplyRelativeOffset(b, cbW, cbH);
  if (b->style->position != kPosStatic || b == root_) LayoutPositionedChildren(b);
}

void LayoutEngine::LayoutContents(LayoutBox* b, float cbH, FloatContext* fc, float bfcX,
                                  float bfcY, float definiteContentH) {
  float contentH;
  if (b->kind == LayoutBox::kFlex) {
    FlexLayout fl(this);
    contentH = fl.LayoutFlex(b, definiteContentH);
  } else if (b->kind == LayoutBox::kGrid) {
    FlexLayout fl(this);
    contentH = fl.LayoutGrid(b, definiteContentH);
  } else {
    contentH = LayoutBlockChildren(b, fc, bfcX, bfcY, definiteContentH);
  }
  b->h = contentH;
}

float LayoutEngine::LayoutBlockChildren(LayoutBox* b, FloatContext* fc, float bfcX, float bfcY,
                                        float definiteContentH) {
  float cx = b->ContentX(), cy = b->ContentY(), cw = b->ContentW();
  if (b->inlineContent) {
    float h = LayoutInlineContent(b, fc, bfcX, bfcY);
    return h;
  }
  float cbH = definiteContentH;
  float y = cy;
  float pending = 0;
  bool first = true;
  bool absorbFirst = CanCollapseTopWithChild(b, root_);
  for (size_t i = 0; i < b->children.size(); ++i) {
    LayoutBox* c = b->children[i].get();
    c->coordParent = b;
    if (c->IsOutOfFlow()) {
      ResolveEdges(c, cw);
      RegisterPositioned(c, cx, y + std::max(0.0f, pending));
      continue;
    }
    if (c->IsFloat()) {
      LayoutFloat(c, b, fc, bfcX, bfcY, y + std::max(0.0f, pending));
      continue;
    }
    if (c->kind == LayoutBox::kText || c->kind == LayoutBox::kInline ||
        c->kind == LayoutBox::kLineBreak)
      continue;  // should not happen after fixup
    ResolveEdges(c, cw);
    float mt = CollapsedTopMargin(c, cw);
    float offset;
    if (first && absorbFirst) offset = 0;
    else offset = CollapseMargins(pending, mt);
    float cyPos = y + offset;
    bool cleared = false;
    if (c->style->clear != kClearNone && fc) {
      float clearY = fc->ClearY(c->style->clear) - bfcY;
      if (cyPos < clearY) {
        cyPos = clearY;
        cleared = true;
      }
    }
    c->y = cyPos;
    float forcedW = -1;
    float availL = cx;
    if (c->EstablishesBfc() && fc && !fc->Empty() && c->kind != LayoutBox::kReplaced) {
      float l = bfcX + cx, r = bfcX + cx + cw;
      fc->Available(bfcY + cyPos, 1, l, r);
      if (r - l < cw) {
        bool stf = c->style->display == kDisplayInlineBlock;
        forcedW = SolveWidth(c, cw, r - l, stf);
        availL = l - bfcX;
      }
    }
    if (forcedW < 0 && c->kind != LayoutBox::kReplaced && c->kind != LayoutBox::kTable) {
      bool stf = c->style->display == kDisplayInlineBlock ||
                 c->style->display == kDisplayInlineFlex;
      forcedW = SolveWidth(c, cw, cw, stf);
    }
    if (c->kind == LayoutBox::kReplaced) {
      LayoutReplaced(c, cw, cbH, -1, -1);
      // Auto margins center block-level replaced elements.
      const ComputedStyle* s = c->style;
      float free = cw - c->w - (s->margin[3].IsAuto() ? 0 : c->margin.left) -
                   (s->margin[1].IsAuto() ? 0 : c->margin.right);
      if (s->margin[3].IsAuto() && s->margin[1].IsAuto()) c->margin.left = std::max(0.0f, free / 2);
      else if (s->margin[3].IsAuto()) c->margin.left = std::max(0.0f, free);
    }
    c->x = availL + c->margin.left;
    if (c->kind == LayoutBox::kTable) {
      LayoutBlockLevel(c, cw, cbH, fc, bfcX + c->x, bfcY + c->y, -1, -1);
      // Tables with auto margins are centered by TableLayout setting margins.
      c->x = availL + c->margin.left;
    } else if (c->kind != LayoutBox::kReplaced) {
      LayoutBlockLevel(c, cw, cbH, fc, bfcX + c->x, bfcY + c->y, forcedW, -1);
    } else {
      ApplyRelativeOffset(c, cw, cbH);
    }
    float mb = c->margin.bottom;
    bool empty = c->h == 0 && c->kind == LayoutBox::kBlockFlow && c->border.top == 0 &&
                 c->border.bottom == 0 && c->padding.top == 0 && c->padding.bottom == 0;
    if (empty && !cleared) {
      // Margins collapse through empty blocks.
      if (!(first && absorbFirst)) pending = CollapseMargins(pending, CollapseMargins(mt, mb));
      c->y = y;
      continue;
    }
    if (b->firstBaseline < 0 && c->firstBaseline >= 0) b->firstBaseline = c->y + c->firstBaseline;
    if (c->lastBaseline >= 0) b->lastBaseline = c->y + c->lastBaseline;
    y = c->y + c->h;
    pending = c->collapsedMarginBottom;
    first = false;
  }
  bool canCollapseBottom = b != root_ && b->kind == LayoutBox::kBlockFlow &&
                           !b->EstablishesBfc() && b->style->border[2].Used() == 0 &&
                           b->style->padding[2].IsZero() && b->style->height.IsAuto() &&
                           definiteContentH < 0;
  float end;
  if (canCollapseBottom) {
    b->collapsedMarginBottom = CollapseMargins(b->margin.bottom, pending);
    end = y;
  } else {
    b->collapsedMarginBottom = b->margin.bottom;
    end = y + pending;
  }
  return std::max(0.0f, end - cy);
}

void LayoutEngine::LayoutFloat(LayoutBox* f, LayoutBox* container, FloatContext* fc,
                               float bfcX, float bfcY, float localY) {
  float cw = container->ContentW();
  float cx = container->ContentX();
  ResolveEdges(f, cw);
  f->coordParent = container;
  if (f->kind == LayoutBox::kReplaced) {
    LayoutReplaced(f, cw, -1, -1, -1);
  } else {
    float w = f->kind == LayoutBox::kTable ? -1 : SolveWidth(f, cw, cw, true);
    LayoutBlockLevel(f, cw, -1, 0, 0, 0, w, -1);
  }
  ResolveEdges(f, cw);
  float ow = f->w + f->margin.left + f->margin.right;
  float oh = f->h + f->margin.top + f->margin.bottom;
  float fx = 0, fy = 0;
  if (fc) {
    fc->Place(ow, oh, f->style->floating == kFloatLeft, bfcY + localY, bfcX + cx, bfcX + cx + cw,
              fx, fy);
    f->x = fx - bfcX + f->margin.left;
    f->y = fy - bfcY + f->margin.top;
  } else {
    f->x = cx + f->margin.left;
    f->y = localY + f->margin.top;
  }
  ApplyRelativeOffset(f, cw, -1);
}

void LayoutEngine::LayoutAtomicInline(LayoutBox* b, float cbW) {
  ResolveEdges(b, cbW);
  if (b->kind == LayoutBox::kReplaced) {
    LayoutReplaced(b, cbW, -1, -1, -1);
    ApplyRelativeOffset(b, cbW, -1);
    return;
  }
  float w = b->kind == LayoutBox::kTable ? -1 : SolveWidth(b, cbW, cbW, true);
  LayoutBlockLevel(b, cbW, -1, 0, 0, 0, w, -1);
}

// ---------------------------------------------------------------------------
// Inline formatting

namespace {

struct LineItem {
  enum Type { kWord, kSpace, kOpen, kClose, kAtomic };
  Type type;
  LayoutBox* box;  // text box / inline box / atomic box
  const ComputedStyle* style;
  Node* node;
  std::string text;
  float w;
  bool collapsible;
  float x;
};

float OwnShift(const ComputedStyle* s, float parentFS, float above, float below) {
  switch (s->verticalAlign) {
    case kVaSub: return -parentFS * 0.2f;
    case kVaSuper: return parentFS * 0.35f;
    case kVaLength: return s->verticalAlignPx;
    case kVaMiddle: return parentFS * 0.25f - (above - below) / 2;
    case kVaTextTop: return parentFS * 0.8f - above;
    case kVaTextBottom: return below - parentFS * 0.2f;
    default: return 0;
  }
}

}  // namespace

float LayoutEngine::LayoutInlineContent(LayoutBox* b, FloatContext* fc, float bfcX, float bfcY) {
  const ComputedStyle* bs = b->style;
  float cx = b->ContentX(), cy = b->ContentY(), cw = b->ContentW();
  std::vector<InlineItem> items;
  CollectInlineItems(b, items);

  FontMetrics strutFm = Metrics(bs);
  float strutLH = LineHeightOf(bs, strutFm);
  float strutHalf = (strutLH - (strutFm.ascent + strutFm.descent)) / 2;
  float strutAbove = strutFm.ascent + strutHalf;
  float strutBelow = strutFm.descent + strutHalf;

  float lineY = cy;
  float lineL = cx, lineR = cx + cw;
  bool firstLine = true;
  std::vector<LineItem> line;
  std::vector<LayoutBox*> openStack;      // inline boxes currently open
  std::vector<LayoutBox*> openAtStart;    // open when the current line began
  float pen = 0;  // width used on the current line
  bool hasContent = false;
  bool lastSpace = true;
  float indent = bs->textIndent.Resolve(cw);

  auto updateAvail = [&]() {
    lineL = cx;
    lineR = cx + cw;
    if (fc && !fc->Empty()) {
      float l = bfcX + cx, r = bfcX + cx + cw;
      fc->Available(bfcY + lineY, strutLH, l, r);
      lineL = l - bfcX;
      lineR = r - bfcX;
    }
  };
  auto avail = [&]() { return lineR - lineL - (firstLine ? indent : 0); };

  auto finishLine = [&](bool forced) {
    // Drop trailing collapsible spaces.
    for (int i = (int)line.size() - 1; i >= 0; --i) {
      if (line[i].type == LineItem::kClose) continue;
      if (line[i].type == LineItem::kSpace && line[i].collapsible) {
        pen -= line[i].w;
        line.erase(line.begin() + i);
        continue;
      }
      break;
    }
    bool any = false;
    for (size_t i = 0; i < line.size(); ++i)
      if (line[i].type == LineItem::kWord || line[i].type == LineItem::kAtomic ||
          line[i].type == LineItem::kSpace ||
          ((line[i].type == LineItem::kOpen || line[i].type == LineItem::kClose) && line[i].w > 0))
        any = true;
    if (!any && !forced) {
      line.clear();
      openAtStart = openStack;
      pen = 0;
      return;
    }
    // Horizontal alignment.
    float width = 0;
    for (size_t i = 0; i < line.size(); ++i) width += line[i].w;
    float start = lineL + (firstLine ? indent : 0);
    float free = (lineR - start) - width;
    TextAlign align = bs->textAlign;
    if (align == kAlignJustify && !forced && free > 0) {
      int spaces = 0;
      for (size_t i = 0; i < line.size(); ++i)
        if (line[i].type == LineItem::kSpace) ++spaces;
      if (spaces > 0) {
        float extra = free / spaces;
        for (size_t i = 0; i < line.size(); ++i)
          if (line[i].type == LineItem::kSpace) line[i].w += extra;
        free = 0;
      }
    }
    float offset = 0;
    if (free > 0) {
      if (align == kAlignRight) offset = free;
      else if (align == kAlignCenter) offset = free / 2;
    }
    float xpos = start + offset;
    for (size_t i = 0; i < line.size(); ++i) {
      line[i].x = xpos;
      xpos += line[i].w;
    }

    // Vertical metrics.
    float maxAbove = strutAbove, maxBelow = strutBelow;
    std::vector<float> shifts(line.size(), 0);
    std::vector<std::pair<LayoutBox*, float> > shiftStack;
    auto shiftOf = [&](LayoutBox* box) -> float {
      for (size_t k = 0; k < shiftStack.size(); ++k)
        if (shiftStack[k].first == box) return shiftStack[k].second;
      return 0;
    };
    auto pushBox = [&](LayoutBox* box) {
      float parentShift = shiftStack.empty() ? 0 : shiftStack.back().second;
      float parentFS = shiftStack.empty() ? bs->fontSize : shiftStack.back().first->style->fontSize;
      FontMetrics fm = Metrics(box->style);
      float sh = parentShift + OwnShift(box->style, parentFS, fm.ascent, fm.descent);
      shiftStack.push_back(std::make_pair(box, sh));
      float lh = LineHeightOf(box->style, fm);
      float half = (lh - (fm.ascent + fm.descent)) / 2;
      maxAbove = std::max(maxAbove, fm.ascent + half + sh);
      maxBelow = std::max(maxBelow, fm.descent + half - sh);
    };
    for (size_t i = 0; i < openAtStart.size(); ++i) pushBox(openAtStart[i]);
    std::vector<size_t> topBottomAtomics;
    for (size_t i = 0; i < line.size(); ++i) {
      LineItem& it = line[i];
      float cur = shiftStack.empty() ? 0 : shiftStack.back().second;
      if (it.type == LineItem::kOpen) {
        pushBox(it.box);
        shifts[i] = shiftStack.back().second;
      } else if (it.type == LineItem::kClose) {
        shifts[i] = shiftOf(it.box);
        if (!shiftStack.empty()) shiftStack.pop_back();
      } else if (it.type == LineItem::kWord || it.type == LineItem::kSpace) {
        shifts[i] = cur;
        FontMetrics fm = Metrics(it.style);
        float lh = LineHeightOf(it.style, fm);
        float half = (lh - (fm.ascent + fm.descent)) / 2;
        maxAbove = std::max(maxAbove, fm.ascent + half + cur);
        maxBelow = std::max(maxBelow, fm.descent + half - cur);
      } else if (it.type == LineItem::kAtomic) {
        LayoutBox* a = it.box;
        float mbh = a->h + a->margin.top + a->margin.bottom;
        float bo = mbh;
        if (a->lastBaseline >= 0 && !a->style->ClipsOverflow() && a->kind != LayoutBox::kReplaced)
          bo = a->margin.top + a->lastBaseline;
        VerticalAlign va = a->style->verticalAlign;
        if (va == kVaTop || va == kVaBottom) {
          topBottomAtomics.push_back(i);
          shifts[i] = 0;
          continue;
        }
        float parentFS = shiftStack.empty() ? bs->fontSize : shiftStack.back().first->style->fontSize;
        float sh = cur + OwnShift(a->style, parentFS, bo, mbh - bo);
        shifts[i] = sh;
        maxAbove = std::max(maxAbove, bo + sh);
        maxBelow = std::max(maxBelow, mbh - bo - sh);
      }
    }
    float lineH = maxAbove + maxBelow;
    for (size_t k = 0; k < topBottomAtomics.size(); ++k) {
      LayoutBox* a = line[topBottomAtomics[k]].box;
      lineH = std::max(lineH, a->h + a->margin.top + a->margin.bottom);
    }
    if (!any) {
      // Empty line created by <br>: strut only.
      lineH = strutAbove + strutBelow;
      maxAbove = strutAbove;
    }
    float baseline = lineY + maxAbove;

    LineBox lb;
    lb.y = lineY;
    lb.h = lineH;
    lb.baseline = baseline;
    // Inline box fragments (backgrounds/borders).
    {
      std::vector<std::pair<LayoutBox*, float> > open;
      std::vector<std::pair<LayoutBox*, bool> > openLeftFlag;
      for (size_t i = 0; i < openAtStart.size(); ++i) {
        open.push_back(std::make_pair(openAtStart[i], start + offset));
        openLeftFlag.push_back(std::make_pair(openAtStart[i], true));
      }
      std::vector<LineFragment> boxFrags;
      auto emit = [&](LayoutBox* box, float x0, float x1, bool openL, bool openR, float sh) {
        const ComputedStyle* s = box->style;
        bool visible = !s->backgroundColor.transparent() || box->border.top > 0 ||
                       box->border.bottom > 0 || box->border.left > 0 ||
                       box->border.right > 0 || !s->backgroundImage.empty();
        if (!visible || !s->visible) return;
        FontMetrics fm = Metrics(s);
        LineFragment f;
        f.kind = LineFragment::kInlineBox;
        f.box = box;
        f.style = s;
        f.node = box->node;
        f.x = x0;
        f.w = std::max(0.0f, x1 - x0);
        f.y = baseline - sh - fm.ascent - box->padding.top - box->border.top;
        f.h = fm.ascent + fm.descent + box->padding.top + box->padding.bottom +
              box->border.top + box->border.bottom;
        f.openLeft = openL;
        f.openRight = openR;
        boxFrags.push_back(f);
      };
      size_t startCount = openAtStart.size();
      (void)startCount;
      for (size_t i = 0; i < line.size(); ++i) {
        LineItem& it = line[i];
        if (it.type == LineItem::kOpen) {
          open.push_back(std::make_pair(it.box, it.x + it.box->margin.left));
          openLeftFlag.push_back(std::make_pair(it.box, false));
        } else if (it.type == LineItem::kClose) {
          for (int k = (int)open.size() - 1; k >= 0; --k) {
            if (open[k].first == it.box) {
              emit(it.box, open[k].second, it.x + it.w - it.box->margin.right,
                   openLeftFlag[k].second, false, shifts[i]);
              open.erase(open.begin() + k);
              openLeftFlag.erase(openLeftFlag.begin() + k);
              break;
            }
          }
        }
      }
      float endX = line.empty() ? start + offset : line.back().x + line.back().w;
      for (size_t k = 0; k < open.size(); ++k)
        emit(open[k].first, open[k].second, endX, openLeftFlag[k].second, true,
             shiftOf(open[k].first));
      // Parents before children: emit order already has outer boxes after
      // inner ones for closes; sort by nesting depth.
      std::stable_sort(boxFrags.begin(), boxFrags.end(),
                       [](const LineFragment& a, const LineFragment& c) {
                         int da = 0, dc = 0;
                         for (LayoutBox* p = a.box; p; p = p->parent) ++da;
                         for (LayoutBox* p = c.box; p; p = p->parent) ++dc;
                         return da < dc;
                       });
      lb.fragments = boxFrags;
    }
    // Text and atomic fragments.
    for (size_t i = 0; i < line.size(); ++i) {
      LineItem& it = line[i];
      if (it.type == LineItem::kWord || it.type == LineItem::kSpace) {
        LineFragment* prev = lb.fragments.empty() ? 0 : &lb.fragments.back();
        if (prev && prev->kind == LineFragment::kText && prev->style == it.style &&
            prev->node == it.node && std::fabs(prev->x + prev->w - it.x) < 0.01f &&
            std::fabs(prev->baseline - (baseline - shifts[i])) < 0.01f) {
          prev->text += it.text;
          prev->w += it.w;
          continue;
        }
        FontMetrics fm = Metrics(it.style);
        LineFragment f;
        f.kind = LineFragment::kText;
        f.box = it.box;
        f.style = it.style;
        f.node = it.node;
        f.text = it.text;
        f.x = it.x;
        f.w = it.w;
        f.baseline = baseline - shifts[i];
        f.y = f.baseline - fm.ascent;
        f.h = fm.ascent + fm.descent;
        lb.fragments.push_back(f);
      } else if (it.type == LineItem::kAtomic) {
        LayoutBox* a = it.box;
        float mbh = a->h + a->margin.top + a->margin.bottom;
        float top;
        if (a->style->verticalAlign == kVaTop) top = lineY;
        else if (a->style->verticalAlign == kVaBottom) top = lineY + lineH - mbh;
        else {
          float bo = mbh;
          if (a->lastBaseline >= 0 && !a->style->ClipsOverflow() && a->kind != LayoutBox::kReplaced)
            bo = a->margin.top + a->lastBaseline;
          top = baseline - shifts[i] - bo;
        }
        a->x = it.x + a->margin.left;
        a->y = top + a->margin.top;
        a->coordParent = b;
        LineFragment f;
        f.kind = LineFragment::kAtomic;
        f.box = a;
        f.style = a->style;
        f.node = a->node;
        f.x = a->x;
        f.y = a->y;
        f.w = a->w;
        f.h = a->h;
        lb.fragments.push_back(f);
      }
    }
    if (b->firstBaseline < 0 && any) b->firstBaseline = baseline;
    if (any) b->lastBaseline = baseline;
    b->lines.push_back(lb);
    lineY += lineH;
    line.clear();
    openAtStart = openStack;
    pen = 0;
    hasContent = false;
    lastSpace = true;
    firstLine = false;
    updateAvail();
  };

  updateAvail();
  openAtStart.clear();
  std::vector<TextToken> tokens;
  for (size_t idx = 0; idx < items.size(); ++idx) {
    InlineItem& item = items[idx];
    LayoutBox* box = item.box;
    switch (item.type) {
      case InlineItem::kText: {
        const ComputedStyle* s = box->style;
        bool wrap = Wraps(s);
        bool collapse = CollapsesSpaces(s);
        Tokenize(box->text, tokens);
        float spaceW = Measure(s, " ") + s->wordSpacing;
        for (size_t t = 0; t < tokens.size(); ++t) {
          const TextToken& tk = tokens[t];
          if (tk.type == TextToken::kNewline) {
            finishLine(true);
            continue;
          }
          if (tk.type == TextToken::kSpace) {
            if (collapse && lastSpace) continue;
            LineItem li;
            li.type = LineItem::kSpace;
            li.box = box;
            li.style = s;
            li.node = box->node;
            li.text = " ";
            li.w = spaceW;
            li.collapsible = collapse;
            li.x = 0;
            line.push_back(li);
            pen += spaceW;
            lastSpace = true;
            continue;
          }
          std::string word = box->text.substr(tk.start, tk.len);
          float ww = Measure(s, word);
          bool splitAnywhere = wrap && s->breakAll && Utf8Length(word) > 1;
          // Wrap before the word if it does not fit.
          if (wrap && !splitAnywhere && hasContent && pen + ww > avail() + 0.01f) {
            // Do not break between a word and immediately preceding
            // non-space content (e.g. "foo<b>bar</b>"): find the last
            // break opportunity, which is a space item.
            int lastSpaceIdx = -1;
            for (int k = (int)line.size() - 1; k >= 0; --k) {
              if (line[k].type == LineItem::kSpace) {
                lastSpaceIdx = k;
                break;
              }
              if (line[k].type == LineItem::kAtomic) {
                lastSpaceIdx = k;  // break after an atomic
                break;
              }
            }
            bool directlyAfterSpace =
                !line.empty() && (line.back().type == LineItem::kSpace ||
                                  line.back().type == LineItem::kAtomic ||
                                  (t > 0 && tokens[t - 1].type == TextToken::kWord));
            if (directlyAfterSpace || lastSpaceIdx < 0) {
              finishLine(false);
            } else {
              // Move the trailing items after the last break opportunity to
              // the next line.
              std::vector<LineItem> carry(line.begin() + lastSpaceIdx + 1, line.end());
              line.erase(line.begin() + lastSpaceIdx + 1, line.end());
              // Opens in the carried part must not count as open at start.
              std::vector<LayoutBox*> savedStack = openStack;
              for (size_t k = 0; k < carry.size(); ++k) {
                if (carry[k].type == LineItem::kOpen) {
                  for (size_t m = 0; m < openStack.size(); ++m)
                    if (openStack[m] == carry[k].box) {
                      openStack.erase(openStack.begin() + m);
                      break;
                    }
                } else if (carry[k].type == LineItem::kClose) {
                  openStack.push_back(carry[k].box);
                }
              }
              finishLine(false);
              openStack = savedStack;
              for (size_t k = 0; k < carry.size(); ++k) {
                line.push_back(carry[k]);
                pen += carry[k].w;
                if (carry[k].type == LineItem::kWord || carry[k].type == LineItem::kAtomic)
                  hasContent = true;
              }
              lastSpace = false;
            }
          }
          // Floats may leave no room at all: move down.
          for (int guard = 0; guard < 50 && !hasContent && fc && !fc->Empty() &&
                              pen + ww > avail() && lineR - lineL < cw;
               ++guard) {
            float next = fc->NextBottomBelow(bfcY + lineY);
            if (next < 0) break;
            lineY = next - bfcY;
            updateAvail();
          }
          // Break overly long words.
          if (wrap && (s->overflowWrap || s->breakAll) && pen + ww > avail() &&
              Utf8Length(word) > 1 && (!hasContent || splitAnywhere)) {
            size_t pos = 0;
            while (pos < word.size()) {
              std::string chunk;
              float cwid = 0;
              size_t p = pos;
              while (p < word.size()) {
                size_t q = p;
                Utf8Next(word, q);
                std::string next = chunk + word.substr(p, q - p);
                float nw = Measure(s, next);
                if (pen + nw > avail() && (!chunk.empty() || hasContent)) break;
                chunk = next;
                cwid = nw;
                p = q;
              }
              if (chunk.empty()) {  // nothing fits next to existing content
                finishLine(false);
                continue;
              }
              LineItem li;
              li.type = LineItem::kWord;
              li.box = box;
              li.style = s;
              li.node = box->node;
              li.text = chunk;
              li.w = cwid;
              li.collapsible = false;
              li.x = 0;
              line.push_back(li);
              pen += cwid;
              hasContent = true;
              pos = p;
              if (pos < word.size()) finishLine(false);
            }
            lastSpace = false;
            continue;
          }
          LineItem li;
          li.type = LineItem::kWord;
          li.box = box;
          li.style = s;
          li.node = box->node;
          li.text = word;
          li.w = ww;
          li.collapsible = false;
          li.x = 0;
          line.push_back(li);
          pen += ww;
          hasContent = true;
          lastSpace = false;
        }
        break;
      }
      case InlineItem::kOpen: {
        ResolveEdges(box, cw);
        LineItem li;
        li.type = LineItem::kOpen;
        li.box = box;
        li.style = box->style;
        li.node = box->node;
        li.w = box->margin.left + box->border.left + box->padding.left;
        li.collapsible = false;
        li.x = 0;
        line.push_back(li);
        pen += li.w;
        openStack.push_back(box);
        box->coordParent = b;
        break;
      }
      case InlineItem::kClose: {
        LineItem li;
        li.type = LineItem::kClose;
        li.box = box;
        li.style = box->style;
        li.node = box->node;
        li.w = box->margin.right + box->border.right + box->padding.right;
        li.collapsible = false;
        li.x = 0;
        line.push_back(li);
        pen += li.w;
        for (int k = (int)openStack.size() - 1; k >= 0; --k)
          if (openStack[k] == box) {
            openStack.erase(openStack.begin() + k);
            break;
          }
        break;
      }
      case InlineItem::kAtomic: {
        LayoutAtomicInline(box, cw);
        float ow = box->w + box->margin.left + box->margin.right;
        if (hasContent && pen + ow > avail() + 0.01f && Wraps(bs)) finishLine(false);
        LineItem li;
        li.type = LineItem::kAtomic;
        li.box = box;
        li.style = box->style;
        li.node = box->node;
        li.w = ow;
        li.collapsible = false;
        li.x = 0;
        line.push_back(li);
        pen += ow;
        hasContent = true;
        lastSpace = false;
        break;
      }
      case InlineItem::kBreak:
        finishLine(true);
        break;
      case InlineItem::kFloat: {
        LayoutFloat(box, b, fc, bfcX, bfcY, lineY);
        updateAvail();
        break;
      }
      case InlineItem::kAbs:
        ResolveEdges(box, cw);
        RegisterPositioned(box, lineL + pen, lineY);
        box->staticY = lineY;
        box->coordParent = b;
        break;
    }
  }
  if (!line.empty()) finishLine(false);
  return std::max(0.0f, lineY - cy);
}

// ---------------------------------------------------------------------------
// Intrinsic sizes

void LayoutEngine::InlineIntrinsic(LayoutBox* b, float& minW, float& maxW) {
  std::vector<InlineItem> items;
  CollectInlineItems(b, items);
  float curLine = b->style->textIndent.Resolve(0), curWord = 0;
  minW = maxW = 0;
  std::vector<TextToken> tokens;
  for (size_t i = 0; i < items.size(); ++i) {
    LayoutBox* box = items[i].box;
    switch (items[i].type) {
      case InlineItem::kText: {
        const ComputedStyle* s = box->style;
        bool wrap = Wraps(s);
        Tokenize(box->text, tokens);
        for (size_t t = 0; t < tokens.size(); ++t) {
          const TextToken& tk = tokens[t];
          if (tk.type == TextToken::kNewline) {
            minW = std::max(minW, curWord);
            curWord = 0;
            maxW = std::max(maxW, curLine);
            curLine = 0;
          } else if (tk.type == TextToken::kSpace) {
            float sw = Measure(s, " ");
            if (wrap) {
              minW = std::max(minW, curWord);
              curWord = 0;
            } else {
              curWord += sw;
            }
            curLine += sw;
          } else {
            float w = Measure(s, box->text.substr(tk.start, tk.len));
            if (s->breakAll && wrap) {
              // Words may break anywhere: the minimum is about one glyph.
              minW = std::max(minW, std::min(w, s->fontSize));
              curWord = 0;
              curLine += w;
              continue;
            }
            curWord += w;
            curLine += w;
            if (wrap && t + 1 < tokens.size() && tokens[t + 1].type == TextToken::kWord) {
              minW = std::max(minW, curWord);
              curWord = 0;
            }
          }
        }
        break;
      }
      case InlineItem::kOpen: {
        ResolveEdges(box, -1);
        float e = box->margin.left + box->border.left + box->padding.left;
        curWord += e;
        curLine += e;
        break;
      }
      case InlineItem::kClose: {
        float e = box->margin.right + box->border.right + box->padding.right;
        curWord += e;
        curLine += e;
        break;
      }
      case InlineItem::kAtomic:
      case InlineItem::kFloat: {
        float mn, mx;
        ComputeIntrinsic(box, mn, mx);
        minW = std::max(minW, curWord);
        curWord = 0;
        minW = std::max(minW, mn);
        curLine += mx;
        break;
      }
      case InlineItem::kBreak:
        minW = std::max(minW, curWord);
        curWord = 0;
        maxW = std::max(maxW, curLine);
        curLine = 0;
        break;
      case InlineItem::kAbs:
        break;
    }
  }
  minW = std::max(minW, curWord);
  maxW = std::max(maxW, curLine);
  maxW = std::max(maxW, minW);
}

void LayoutEngine::ComputeIntrinsic(LayoutBox* b, float& minW, float& maxW) {
  if (b->intrinsicValid) {
    minW = b->minContent;
    maxW = b->maxContent;
    return;
  }
  const ComputedStyle* s = b->style;
  ResolveEdges(b, -1);
  float pb = b->padding.left + b->padding.right + b->border.left + b->border.right;
  float margins = b->margin.left + b->margin.right;
  float mn = 0, mx = 0;
  if (s->width.IsFixed() && !s->width.HasPercent() && b->kind != LayoutBox::kTable) {
    float w = s->width.Resolve(0);
    if (s->boxSizing == kContentBox) w += pb;
    w = ClampWidth(b, w, -1);
    mn = mx = w;
  } else {
    switch (b->kind) {
      case LayoutBox::kReplaced: {
        float w, h;
        ReplacedSize(b, -1, -1, w, h);
        mx = w + pb;
        mn = s->width.HasPercent() || s->maxWidth.HasPercent() ? pb : mx;
        break;
      }
      case LayoutBox::kTable: {
        TableLayout tl(this);
        tl.Intrinsic(b, mn, mx);
        break;
      }
      case LayoutBox::kFlex: {
        FlexLayout fl(this);
        fl.FlexIntrinsic(b, mn, mx);
        mn += pb;
        mx += pb;
        break;
      }
      case LayoutBox::kGrid: {
        FlexLayout fl(this);
        fl.GridIntrinsic(b, mn, mx);
        mn += pb;
        mx += pb;
        break;
      }
      default: {
        if (b->inlineContent) {
          InlineIntrinsic(b, mn, mx);
        } else {
          float floatRun = 0;
          for (size_t i = 0; i < b->children.size(); ++i) {
            LayoutBox* c = b->children[i].get();
            if (c->IsOutOfFlow()) continue;
            if (c->kind == LayoutBox::kText || c->kind == LayoutBox::kInline) continue;
            float cmn, cmx;
            ComputeIntrinsic(c, cmn, cmx);
            mn = std::max(mn, cmn);
            if (c->IsFloat()) {
              floatRun += cmx;
              mx = std::max(mx, floatRun);
            } else {
              floatRun = 0;
              mx = std::max(mx, cmx);
            }
          }
        }
        mn += pb;
        mx += pb;
        break;
      }
    }
    if (b->kind != LayoutBox::kReplaced) {
      // Scroll containers can shrink below their content (it is clipped),
      // so they do not force a minimum width on their ancestors.
      if (s->overflowX != kOverflowVisible) mn = pb;
      mx = ClampWidth(b, mx, -1);
      mn = std::min(ClampWidth(b, mn, -1), mx);
    }
  }
  b->minContent = mn + margins;
  b->maxContent = std::max(mn, mx) + margins;
  b->intrinsicValid = true;
  minW = b->minContent;
  maxW = b->maxContent;
}

// ---------------------------------------------------------------------------
// Replaced elements

void LayoutEngine::ReplacedSize(LayoutBox* b, float cbW, float cbH, float& outW, float& outH) {
  const ComputedStyle* s = b->style;
  Node* el = b->node;
  float iw = 0, ih = 0;
  bool ratio = false;
  std::string tag = el ? el->tag : std::string();
  FontMetrics fm = Metrics(s);
  float fontH = fm.ascent + fm.descent;
  if (tag == "img" || (tag == "input" && AsciiLower(el->Attr("type")) == "image") ||
      tag == "video") {
    int w = 0, h = 0;
    ImageProvider::State st =
        b->imageUrl.empty() || !images_ ? ImageProvider::kFailed : images_->GetImage(b->imageUrl, w, h);
    if (st == ImageProvider::kLoaded && w > 0 && h > 0) {
      iw = (float)w;
      ih = (float)h;
      ratio = true;
    } else if (st == ImageProvider::kFailed) {
      std::string alt = el->Attr("alt");
      if (!alt.empty()) {
        iw = Measure(s, alt) + 4;
        ih = fontH + 2;
      }
    }
    if (tag == "video" && !ratio) {
      iw = 300;
      ih = 150;
    }
  } else if (tag == "input") {
    std::string type = AsciiLower(el->Attr("type"));
    if (type == "submit" || type == "button" || type == "reset") {
      std::string label = el->Attr("value");
      if (label.empty() && !el->HasAttr("value"))
        label = type == "submit" ? "Submit" : type == "reset" ? "Reset" : "";
      iw = Measure(s, label);
      ih = fontH;
    } else if (type == "checkbox" || type == "radio") {
      iw = ih = 13;
    } else if (type == "file") {
      iw = 220;
      ih = fontH;
    } else {
      iw = 150;
      ih = fontH;
    }
  } else if (tag == "select") {
    float mxw = 20;
    std::vector<Node*> opts;
    el->FindAll("option", opts);
    for (size_t i = 0; i < opts.size(); ++i)
      mxw = std::max(mxw, Measure(s, CollapseWhitespace(opts[i]->TextContent())));
    iw = mxw + 20;
    ih = fontH;
    long long n;
    if (el->HasAttr("multiple") || (ParseInt(el->Attr("size"), n) && n > 1)) {
      if (!ParseInt(el->Attr("size"), n) || n < 1) n = 4;
      ih = fontH * (float)n;
    }
  } else if (tag == "textarea") {
    iw = 200;
    ih = fontH * 2;
  } else if (tag == "meter" || tag == "progress") {
    iw = 80;
    ih = 16;
  } else if (tag == "svg") {
    std::vector<std::string> vb = SplitWhitespace(ReplaceAll(el->Attr("viewbox"), ",", " "));
    if (vb.size() == 4) {
      size_t u;
      float vw = (float)ParseDoublePrefix(vb[2], u), vh = (float)ParseDoublePrefix(vb[3], u);
      if (vw > 0 && vh > 0) {
        iw = vw;
        ih = vh;
        ratio = true;
      }
    }
    if (!ratio) {
      iw = 24;
      ih = 24;
    }
  } else if (tag == "audio") {
    iw = 300;
    ih = 32;
  } else {
    iw = 300;
    ih = 150;
  }

  float pbH = b->padding.left + b->padding.right + b->border.left + b->border.right;
  float pbV = b->padding.top + b->padding.bottom + b->border.top + b->border.bottom;
  bool wSet = s->width.IsFixed() && !(s->width.HasPercent() && cbW < 0);
  bool hSet = s->height.IsFixed() && !(s->height.HasPercent() && cbH < 0);
  float w = 0, h = 0;
  if (wSet) {
    w = s->width.Resolve(cbW);
    if (s->boxSizing == kBorderBox) w -= pbH;
  }
  if (hSet) {
    h = s->height.Resolve(cbH);
    if (s->boxSizing == kBorderBox) h -= pbV;
  }
  float r = s->aspectRatio > 0 ? s->aspectRatio : (ratio && ih > 0 ? iw / ih : 0);
  if (!wSet && !hSet) {
    w = iw;
    h = ih;
    if (s->aspectRatio > 0 && w > 0) h = w / s->aspectRatio;
  } else if (wSet && !hSet) {
    h = r > 0 ? w / r : ih;
  } else if (!wSet && hSet) {
    w = r > 0 ? h * r : iw;
  }
  // min/max constraints (content box), keeping the aspect ratio when the
  // other dimension is automatic.
  float before = w;
  float bw = ClampWidth(b, w + pbH, cbW) - pbH;
  if (bw != before) {
    w = bw;
    if (!hSet && r > 0) h = w / r;
  }
  float bh = ClampHeight(b, h + pbV, cbH) - pbV;
  if (bh != h) {
    if (!wSet && r > 0) w = bh * r;
    h = bh;
  }
  outW = std::max(0.0f, w);
  outH = std::max(0.0f, h);
}

void LayoutEngine::LayoutReplaced(LayoutBox* b, float cbW, float cbH, float forcedW, float forcedH) {
  ResolveEdges(b, cbW);
  float w, h;
  ReplacedSize(b, cbW, cbH, w, h);
  float pbH = b->padding.left + b->padding.right + b->border.left + b->border.right;
  float pbV = b->padding.top + b->padding.bottom + b->border.top + b->border.bottom;
  b->w = forcedW >= 0 ? forcedW : w + pbH;
  b->h = forcedH >= 0 ? forcedH : h + pbV;
  b->firstBaseline = b->lastBaseline = -1;
  b->collapsedMarginBottom = b->margin.bottom;
}

// ---------------------------------------------------------------------------
// Absolute positioning

void LayoutEngine::LayoutPositionedChildren(LayoutBox* cb) {
  // Copy: laying out children may register more positioned boxes on |cb|
  // (e.g. fixed descendants of an abs box when cb is the root).
  for (size_t i = 0; i < cb->positioned.size(); ++i) LayoutPositioned(cb, cb->positioned[i]);
}

void LayoutEngine::LayoutPositioned(LayoutBox* cb, LayoutBox* child) {
  float cbW = cb->w - cb->border.left - cb->border.right;
  float cbH = cb->h - cb->border.top - cb->border.bottom;
  if (cb == root_ || child->style->position == kPosFixed) {
    cbW = std::max(cbW, viewportW_);
    cbH = viewportH_;
  }
  // Static position relative to cb's border box.
  float sx = child->staticX, sy = child->staticY;
  LayoutBox* p = child->parent;
  // Inline ancestors have no geometry: skip to their block container.
  while (p && p->kind == LayoutBox::kInline) p = p->parent;
  for (LayoutBox* q = p; q && q != cb; q = q->coordParent) {
    sx += q->x + q->relX;
    sy += q->y + q->relY;
    if (!q->coordParent) break;
  }
  ResolveEdges(child, cbW);
  const ComputedStyle* s = child->style;
  const Length* in = s->inset;
  bool lSet = in[3].IsFixed(), rSet = in[1].IsFixed();
  bool tSet = in[0].IsFixed(), bSet = in[2].IsFixed();
  float l = lSet ? in[3].Resolve(cbW) : 0, r = rSet ? in[1].Resolve(cbW) : 0;
  float t = tSet ? in[0].Resolve(cbH) : 0, bt = bSet ? in[2].Resolve(cbH) : 0;
  float pbV = child->padding.top + child->padding.bottom + child->border.top + child->border.bottom;

  float w = -1;
  if (child->kind != LayoutBox::kReplaced && child->kind != LayoutBox::kTable) {
    if (s->width.IsAuto() && lSet && rSet) {
      w = cbW - l - r - child->margin.left - child->margin.right;
      w = ClampWidth(child, w, cbW);
    } else {
      float avail = cbW - (lSet ? l : 0) - (rSet ? r : 0);
      w = SolveWidth(child, cbW, std::max(0.0f, avail), true);
    }
  }
  float forcedH = -1;
  if (s->height.IsAuto() && tSet && bSet && child->kind != LayoutBox::kReplaced &&
      s->aspectRatio <= 0) {
    forcedH = std::max(pbV, cbH - t - bt - child->margin.top - child->margin.bottom);
  }
  child->x = 0;
  child->y = 0;
  LayoutBlockLevel(child, cbW, cbH, 0, 0, 0, w, forcedH);
  // Auto margins center when both insets are set.
  if (lSet && rSet && s->margin[3].IsAuto() && s->margin[1].IsAuto()) {
    float free = cbW - l - r - child->w;
    if (free > 0) child->margin.left = child->margin.right = free / 2;
  }
  if (tSet && bSet && s->margin[0].IsAuto() && s->margin[2].IsAuto()) {
    float free = cbH - t - bt - child->h;
    if (free > 0) child->margin.top = child->margin.bottom = free / 2;
  }
  float x, y;
  if (lSet) x = cb->border.left + l + child->margin.left;
  else if (rSet) x = cb->border.left + cbW - r - child->margin.right - child->w;
  else x = sx + child->margin.left;
  if (tSet) y = cb->border.top + t + child->margin.top;
  else if (bSet) y = cb->border.top + cbH - bt - child->margin.bottom - child->h;
  else y = sy + child->margin.top;
  child->x = x;
  child->y = y;
  child->relX = child->relY = 0;
  child->coordParent = cb;
  child->isPositionedChild = true;
}

void LayoutEngine::ShiftContent(LayoutBox* b, float dy) {
  if (dy == 0) return;
  if (b->inlineContent) {
    for (size_t i = 0; i < b->lines.size(); ++i) {
      LineBox& lb = b->lines[i];
      lb.y += dy;
      lb.baseline += dy;
      for (size_t k = 0; k < lb.fragments.size(); ++k) {
        LineFragment& f = lb.fragments[k];
        f.y += dy;
        f.baseline += dy;
        if (f.kind == LineFragment::kAtomic) f.box->y += dy;
      }
    }
    for (size_t i = 0; i < b->children.size(); ++i)
      if (b->children[i]->IsFloat()) b->children[i]->y += dy;
  } else {
    for (size_t i = 0; i < b->children.size(); ++i)
      if (!b->children[i]->isPositionedChild) b->children[i]->y += dy;
  }
  if (b->firstBaseline >= 0) b->firstBaseline += dy;
  if (b->lastBaseline >= 0) b->lastBaseline += dy;
}

}  // namespace kite
