#include <algorithm>
#include <cmath>

#include "base/strings.h"
#include "layout/layout.h"
#include "paint/display_list.h"

namespace kite {

namespace {

Color Darken(Color c, float f) {
  return Color((uint8_t)(c.r * f), (uint8_t)(c.g * f), (uint8_t)(c.b * f), c.a);
}

Color Lighten(Color c, float f) {
  return Color((uint8_t)(c.r + (255 - c.r) * f), (uint8_t)(c.g + (255 - c.g) * f),
               (uint8_t)(c.b + (255 - c.b) * f), c.a);
}

Color WithAlpha(Color c, float alpha) {
  if (alpha >= 1) return c;
  return Color(c.r, c.g, c.b, (uint8_t)(c.a * alpha));
}

std::string FormValue(Node* el) {
  if (el->formValueSet) return el->formValue;
  if (el->tag == "textarea") return el->TextContent();
  return el->Attr("value");
}

}  // namespace

void Painter::Extend(const Rect& raw) {
  Rect r = clipStack_.empty() ? raw : raw.intersect(clipStack_.back());
  if (r.w <= 0 || r.h <= 0) return;
  out_->width = std::max(out_->width, r.right());
  out_->height = std::max(out_->height, r.bottom());
}

void Painter::FillRect(const Rect& r, Color c, float alpha) {
  if (r.w <= 0 || r.h <= 0) return;
  c = WithAlpha(c, alpha);
  if (c.a == 0) return;
  DisplayItem it;
  it.type = DisplayItem::kRect;
  it.rect = r;
  it.color = c;
  out_->items.push_back(it);
}

void Painter::Text(float x, float baseline, const std::string& text, const ComputedStyle* s,
                   Color color, float alpha, bool decorations) {
  if (text.empty()) return;
  DisplayItem it;
  it.type = DisplayItem::kText;
  it.text = text;
  it.font = FontDesc::FromStyle(*s);
  it.baseline = baseline;
  it.color = WithAlpha(color, alpha);
  if (decorations) {
    it.underline = s->underline;
    it.lineThrough = s->lineThrough;
    it.overline = s->overline;
    it.decorationColor = WithAlpha(s->decorationColorSet ? s->decorationColor : color, alpha);
  }
  float w = engine_->Measure(s, text);
  it.rect = Rect(x, baseline - s->fontSize, w, s->fontSize * 1.3f);
  out_->items.push_back(it);
}

void Painter::Paint(LayoutBox* root, float viewportW, float viewportH, DisplayList& out) {
  out_ = &out;
  out.Clear();
  clipStack_.clear();
  skipBackgroundOf_ = 0;
  out.width = viewportW;
  out.height = 0;
  if (!root) return;
  // Canvas background: root, or propagated from <body>.
  Color bg = root->style->backgroundColor;
  if (bg.transparent() && root->style->backgroundImage.empty()) {
    for (size_t i = 0; i < root->children.size(); ++i) {
      LayoutBox* c = root->children[i].get();
      if (c->node && c->node->tag == "body") {
        bg = c->style->backgroundColor;
        if (!c->style->backgroundImage.empty()) {
          // Body background images are painted over the whole canvas.
          Rect canvas(0, 0, std::max(viewportW, root->w), std::max(viewportH, root->h));
          PaintBackground(c->style, canvas, canvas, 1);
        }
        skipBackgroundOf_ = c;
        break;
      }
    }
  }
  out.background = bg.transparent() ? Color(255, 255, 255) : Color(bg.r, bg.g, bg.b);
  PaintBox(root, 0, 0, 1);
}

void Painter::PaintBackground(const ComputedStyle* s, const Rect& border, const Rect& padding,
                              float alpha) {
  if (s->backgroundClipText) return;  // gradient text: draw the text only
  FillRect(border, s->backgroundColor, alpha);
  if (s->backgroundImage.empty() || !images_) return;
  int iw = 0, ih = 0;
  if (images_->GetImage(s->backgroundImage, iw, ih) != ImageProvider::kLoaded || iw <= 0 ||
      ih <= 0)
    return;
  float tw = (float)iw, th = (float)ih;
  if (s->backgroundSizeMode == 1 || s->backgroundSizeMode == 2) {
    float sx = padding.w / iw, sy = padding.h / ih;
    float sc = s->backgroundSizeMode == 1 ? std::max(sx, sy) : std::min(sx, sy);
    tw = iw * sc;
    th = ih * sc;
  } else if (s->backgroundSizeMode == 3) {
    bool wAuto = !s->backgroundSizeW.IsFixed(), hAuto = !s->backgroundSizeH.IsFixed();
    if (!wAuto) tw = s->backgroundSizeW.Resolve(padding.w);
    if (!hAuto) th = s->backgroundSizeH.Resolve(padding.h);
    if (!wAuto && hAuto) th = tw * ih / iw;
    if (wAuto && !hAuto) tw = th * iw / ih;
  }
  if (tw < 1 || th < 1) return;
  DisplayItem it;
  it.type = DisplayItem::kImage;
  it.imageUrl = s->backgroundImage;
  it.rect = border;
  it.tileW = tw;
  it.tileH = th;
  const Length& px = s->backgroundPosX;
  const Length& py = s->backgroundPosY;
  it.tileX = padding.x + px.px + px.pct * (padding.w - tw) / 100;
  it.tileY = padding.y + py.px + py.pct * (padding.h - th) / 100;
  it.repeatX = s->backgroundRepeat == kRepeat || s->backgroundRepeat == kRepeatX;
  it.repeatY = s->backgroundRepeat == kRepeat || s->backgroundRepeat == kRepeatY;
  it.alpha = alpha;
  out_->items.push_back(it);
}

void Painter::PaintBorders(const ComputedStyle* s, const Rect& r, const float w[4], float alpha,
                           bool skipLeft, bool skipRight) {
  for (int side = 0; side < 4; ++side) {
    if (w[side] <= 0) continue;
    if ((side == 3 && skipLeft) || (side == 1 && skipRight)) continue;
    const BorderSide& bs = s->border[side];
    Color c = bs.colorIsCurrent ? s->color : bs.color;
    float t = w[side];
    Rect rect;
    switch (side) {
      case 0: rect = Rect(r.x, r.y, r.w, t); break;
      case 1: rect = Rect(r.right() - t, r.y, t, r.h); break;
      case 2: rect = Rect(r.x, r.bottom() - t, r.w, t); break;
      default: rect = Rect(r.x, r.y, t, r.h); break;
    }
    bool topLeft = side == 0 || side == 3;
    switch (bs.style) {
      case kBorderInset:
      case kBorderOutset: {
        bool dark = (bs.style == kBorderInset) == topLeft;
        if (t >= 2) {
          // Windows 2000 style two-tone 3D edge.
          Color outer = dark ? Darken(c, 0.6f) : Lighten(c, 0.85f);
          Color inner = dark ? Darken(c, 0.3f) : Lighten(c, 0.4f);
          if (bs.style == kBorderOutset && !topLeft) { outer = Darken(c, 0.3f); inner = Darken(c, 0.6f); }
          if (bs.style == kBorderInset && !topLeft) { outer = Lighten(c, 0.85f); inner = Lighten(c, 0.4f); }
          float h = t / 2;
          Rect a = rect, b = rect;
          if (side == 0) { a.h = h; b.y += h; b.h = t - h; }
          else if (side == 2) { b.h = h; a.y += t - h; a.h = h; b.h = t - h; b.y = rect.y; }
          else if (side == 3) { a.w = h; b.x += h; b.w = t - h; }
          else { a.x += t - h; a.w = h; b.w = t - h; }
          FillRect(a, outer, alpha);
          FillRect(b, inner, alpha);
        } else {
          FillRect(rect, dark ? Darken(c, 0.5f) : Lighten(c, 0.6f), alpha);
        }
        break;
      }
      case kBorderGroove:
      case kBorderRidge: {
        bool firstDark = (bs.style == kBorderGroove) == topLeft;
        float h = t / 2;
        Rect a = rect, b = rect;
        if (side == 0 || side == 2) { a.h = h; b.y += h; b.h = t - h; }
        else { a.w = h; b.x += h; b.w = t - h; }
        FillRect(a, firstDark ? Darken(c, 0.5f) : Lighten(c, 0.6f), alpha);
        FillRect(b, firstDark ? Lighten(c, 0.6f) : Darken(c, 0.5f), alpha);
        break;
      }
      case kBorderDouble: {
        if (t < 3) {
          FillRect(rect, c, alpha);
          break;
        }
        float lw = std::floor(t / 3 + 0.5f);
        Rect a = rect, b = rect;
        if (side == 0 || side == 2) { a.h = lw; b.y += t - lw; b.h = lw; }
        else { a.w = lw; b.x += t - lw; b.w = lw; }
        FillRect(a, c, alpha);
        FillRect(b, c, alpha);
        break;
      }
      case kBorderDashed:
      case kBorderDotted: {
        bool horiz = side == 0 || side == 2;
        float len = horiz ? rect.w : rect.h;
        float dash = bs.style == kBorderDotted ? t : t * 3;
        float gap = bs.style == kBorderDotted ? t : t * 2;
        if (len > 2000) {
          FillRect(rect, c, alpha);
          break;
        }
        for (float p = 0; p < len; p += dash + gap) {
          float l = std::min(dash, len - p);
          Rect seg = horiz ? Rect(rect.x + p, rect.y, l, rect.h) : Rect(rect.x, rect.y + p, rect.w, l);
          FillRect(seg, c, alpha);
        }
        break;
      }
      default:
        FillRect(rect, c, alpha);
    }
  }
}

void Painter::PaintBoxDecorations(LayoutBox* b, const Rect& r, float alpha, bool skipLeft,
                                  bool skipRight) {
  const ComputedStyle* s = b->style;
  float w[4] = {b->border.top, b->border.right, b->border.bottom, b->border.left};
  Rect pad(r.x + w[3], r.y + w[0], r.w - w[1] - w[3], r.h - w[0] - w[2]);
  if (b != skipBackgroundOf_) PaintBackground(s, r, pad, alpha);
  PaintBorders(s, r, w, alpha, skipLeft, skipRight);
}

void Painter::PaintMarker(LayoutBox* b, float ax, float ay, float alpha) {
  const ComputedStyle* s = b->style;
  if (s->listStyleType == kListNone || !s->visible) return;
  FontMetrics fm = engine_->Metrics(s);
  float baseline = b->firstBaseline >= 0 ? ay + b->firstBaseline
                                         : ay + b->ContentY() + fm.ascent;
  float cx = ax + b->ContentX();
  if (!b->markerText.empty()) {
    float w = engine_->Measure(s, b->markerText);
    Text(cx - w - s->fontSize * 0.4f, baseline, b->markerText, s, s->color, alpha, false);
    return;
  }
  float size = std::max(4.0f, std::floor(s->fontSize * 0.36f));
  float x = cx - size - s->fontSize * 0.55f;
  float y = baseline - s->fontSize * 0.33f - size / 2;
  Rect r(std::floor(x), std::floor(y), size, size);
  if (s->listStyleType == kListSquare) {
    FillRect(r, s->color, alpha);
  } else {
    DisplayItem it;
    it.type = DisplayItem::kEllipse;
    it.rect = r;
    it.color = WithAlpha(s->color, alpha);
    it.hollow = s->listStyleType == kListCircle;
    out_->items.push_back(it);
  }
}

void Painter::PaintLines(LayoutBox* b, float ax, float ay, float alpha) {
  for (size_t li = 0; li < b->lines.size(); ++li) {
    const LineBox& lb = b->lines[li];
    for (size_t k = 0; k < lb.fragments.size(); ++k) {
      const LineFragment& f = lb.fragments[k];
      switch (f.kind) {
        case LineFragment::kInlineBox: {
          Rect r(ax + f.x, ay + f.y, f.w, f.h);
          LayoutBox* box = f.box;
          float w[4] = {box->border.top, box->border.right, box->border.bottom, box->border.left};
          Rect pad(r.x + (f.openLeft ? 0 : w[3]), r.y + w[0], r.w, r.h - w[0] - w[2]);
          PaintBackground(f.style, r, pad, alpha);
          PaintBorders(f.style, r, w, alpha, f.openLeft, f.openRight);
          if (f.node) {
            HitRegion hr = {r, f.node};
            out_->hits.push_back(hr);
          }
          break;
        }
        case LineFragment::kText: {
          if (!f.style->visible) break;
          Text(ax + f.x, ay + f.baseline, f.text, f.style, f.style->color, alpha, true);
          Rect r(ax + f.x, ay + lb.y, f.w, lb.h);
          if (f.node) {
            HitRegion hr = {r, f.node};
            out_->hits.push_back(hr);
          }
          Extend(Rect(ax + f.x, ay + f.y, f.w, f.h));
          break;
        }
        case LineFragment::kAtomic:
          if (!f.box->isPositionedChild) PaintBox(f.box, ax, ay, alpha);
          break;
        default:
          break;
      }
    }
  }
}

void Painter::PaintFloatsInInline(LayoutBox* container, LayoutBox* b, float ax, float ay,
                                  float alpha) {
  for (size_t i = 0; i < b->children.size(); ++i) {
    LayoutBox* c = b->children[i].get();
    if (c->IsFloat() && !c->isPositionedChild) PaintBox(c, ax, ay, alpha);
    else if (c->kind == LayoutBox::kInline) PaintFloatsInInline(container, c, ax, ay, alpha);
  }
}

void Painter::PaintReplaced(LayoutBox* b, float ax, float ay, float alpha) {
  Node* el = b->node;
  const ComputedStyle* s = b->style;
  if (!el) return;
  Rect content(ax + b->ContentX(), ay + b->ContentY(), b->ContentW(), b->ContentH());
  const std::string& tag = el->tag;
  FontMetrics fm = engine_->Metrics(s);
  float textBaseline = content.y + (content.h - (fm.ascent + fm.descent)) / 2 + fm.ascent;
  Color gray(128, 128, 128);
  if (tag == "img" || tag == "video" || (tag == "input" && AsciiLower(el->Attr("type")) == "image")) {
    int iw = 0, ih = 0;
    ImageProvider::State st = b->imageUrl.empty() || !images_
                                  ? ImageProvider::kFailed
                                  : images_->GetImage(b->imageUrl, iw, ih);
    if (st == ImageProvider::kLoaded && iw > 0 && ih > 0) {
      DisplayItem it;
      it.type = DisplayItem::kImage;
      it.imageUrl = b->imageUrl;
      it.rect = content;
      float tw = content.w, th = content.h;
      float tx = content.x, ty = content.y;
      if (s->objectFit == kFitContain || s->objectFit == kFitCover || s->objectFit == kFitScaleDown) {
        float sx = content.w / iw, sy = content.h / ih;
        float sc = s->objectFit == kFitCover ? std::max(sx, sy) : std::min(sx, sy);
        if (s->objectFit == kFitScaleDown) sc = std::min(sc, 1.0f);
        tw = iw * sc;
        th = ih * sc;
        tx = content.x + (content.w - tw) / 2;
        ty = content.y + (content.h - th) / 2;
      }
      it.tileX = tx;
      it.tileY = ty;
      it.tileW = tw;
      it.tileH = th;
      it.alpha = alpha;
      out_->items.push_back(it);
    } else if (st == ImageProvider::kUnsupported || st == ImageProvider::kLoading) {
      // Nothing to draw (yet).
    } else if (tag == "video") {
      FillRect(content, Color(32, 32, 32), alpha);
      Text(content.x + content.w / 2 - 8, content.y + content.h / 2 + 6, "\xE2\x96\xB6", s,
           Color(220, 220, 220), alpha, false);
    } else {
      std::string alt = el->Attr("alt");
      if (!alt.empty() && content.w > 4 && content.h > 4) {
        out_->items.push_back(DisplayItem());
        DisplayItem& clip = out_->items.back();
        clip.type = DisplayItem::kPushClip;
        clip.rect = content;
        float bw[4] = {1, 1, 1, 1};
        ComputedStyle tmp;
        tmp.CopyFrom(*s);
        for (int k = 0; k < 4; ++k) {
          tmp.border[k].style = kBorderInset;
          tmp.border[k].color = Color(192, 192, 192);
          tmp.border[k].colorIsCurrent = false;
        }
        PaintBorders(&tmp, content, bw, alpha, false, false);
        Text(content.x + 2, textBaseline, alt, s, s->color, alpha, false);
        DisplayItem pop;
        pop.type = DisplayItem::kPopClip;
        out_->items.push_back(pop);
      }
    }
    return;
  }
  if (tag == "input") {
    std::string type = AsciiLower(el->Attr("type"));
    if (type == "checkbox" || type == "radio") {
      bool checked = el->checkedSet ? el->checked : el->HasAttr("checked");
      Rect r(ax + b->ContentX(), ay + b->ContentY(), 13, 13);
      if (type == "checkbox") {
        FillRect(r, Color(255, 255, 255), alpha);
        ComputedStyle tmp;
        for (int k = 0; k < 4; ++k) {
          tmp.border[k].style = kBorderInset;
          tmp.border[k].color = Color(212, 208, 200);
          tmp.border[k].colorIsCurrent = false;
        }
        float bw[4] = {2, 2, 2, 2};
        PaintBorders(&tmp, r, bw, alpha, false, false);
        if (checked) {
          // Check mark drawn from small rectangles.
          static const int pts[][2] = {{3, 5}, {4, 6}, {5, 7}, {6, 6}, {7, 5}, {8, 4}, {9, 3}};
          for (int k = 0; k < 7; ++k)
            FillRect(Rect(r.x + pts[k][0], r.y + pts[k][1], 1, 3), Color(0, 0, 0), alpha);
        }
      } else {
        DisplayItem outer;
        outer.type = DisplayItem::kEllipse;
        outer.rect = r;
        outer.color = WithAlpha(Color(255, 255, 255), alpha);
        out_->items.push_back(outer);
        outer.hollow = true;
        outer.color = WithAlpha(Color(128, 128, 128), alpha);
        out_->items.push_back(outer);
        if (checked) {
          DisplayItem dot;
          dot.type = DisplayItem::kEllipse;
          dot.rect = Rect(r.x + 4, r.y + 4, 5, 5);
          dot.color = WithAlpha(Color(0, 0, 0), alpha);
          out_->items.push_back(dot);
        }
      }
      return;
    }
    std::string text;
    Color color = s->color;
    bool centered = false;
    if (type == "submit" || type == "button" || type == "reset") {
      text = el->Attr("value");
      if (text.empty() && !el->HasAttr("value"))
        text = type == "submit" ? "Submit" : type == "reset" ? "Reset" : "";
      centered = true;
    } else if (type == "file") {
      text = "Durchsuchen...";
    } else if (type == "range") {
      FillRect(Rect(content.x, content.y + content.h / 2 - 2, content.w, 4), Color(160, 160, 160), alpha);
      FillRect(Rect(content.x + content.w / 2 - 4, content.y, 8, content.h), Color(212, 208, 200), alpha);
      return;
    } else if (type == "color") {
      Color c;
      if (!ParseColor(el->Attr("value"), c, Color())) c = Color(0, 0, 0);
      FillRect(content, c, alpha);
      return;
    } else {
      text = FormValue(el);
      if (type == "password") {
        std::string dots;
        for (size_t i = 0; i < Utf8Length(text); ++i) dots += "*";
        text = dots;
      }
      if (text.empty()) {
        text = el->Attr("placeholder");
        color = gray;
      }
    }
    DisplayItem clip;
    clip.type = DisplayItem::kPushClip;
    clip.rect = content;
    out_->items.push_back(clip);
    float tx = content.x;
    if (centered) tx = content.x + (content.w - engine_->Measure(s, text)) / 2;
    Text(tx, textBaseline, text, s, color, alpha, false);
    DisplayItem pop;
    pop.type = DisplayItem::kPopClip;
    out_->items.push_back(pop);
    return;
  }
  if (tag == "select") {
    std::vector<Node*> opts;
    el->FindAll("option", opts);
    std::string text;
    int sel = el->selectedIndex;
    if (sel < 0) {
      for (size_t i = 0; i < opts.size(); ++i)
        if (opts[i]->HasAttr("selected")) sel = (int)i;
      if (sel < 0 && !opts.empty()) sel = 0;
    }
    if (sel >= 0 && sel < (int)opts.size()) text = CollapseWhitespace(opts[sel]->TextContent());
    float bw = std::min(16.0f, content.h);
    Rect btn(content.right() - bw, content.y, bw, content.h);
    DisplayItem clip;
    clip.type = DisplayItem::kPushClip;
    clip.rect = Rect(content.x, content.y, content.w - bw, content.h);
    out_->items.push_back(clip);
    Text(content.x + 1, textBaseline, text, s, s->color, alpha, false);
    DisplayItem pop;
    pop.type = DisplayItem::kPopClip;
    out_->items.push_back(pop);
    FillRect(btn, Color(212, 208, 200), alpha);
    ComputedStyle tmp;
    for (int k = 0; k < 4; ++k) {
      tmp.border[k].style = kBorderOutset;
      tmp.border[k].color = Color(212, 208, 200);
      tmp.border[k].colorIsCurrent = false;
    }
    float bwd[4] = {2, 2, 2, 2};
    PaintBorders(&tmp, btn, bwd, alpha, false, false);
    // Down arrow.
    float cx = std::floor(btn.x + btn.w / 2), cy = std::floor(btn.y + btn.h / 2) - 1;
    for (int k = 0; k < 4; ++k)
      FillRect(Rect(cx - 3 + k, cy + k, 7 - 2 * k, 1), Color(0, 0, 0), alpha);
    return;
  }
  if (tag == "textarea") {
    std::string text = FormValue(el);
    Color color = s->color;
    if (text.empty()) {
      text = el->Attr("placeholder");
      color = gray;
    }
    DisplayItem clip;
    clip.type = DisplayItem::kPushClip;
    clip.rect = content;
    out_->items.push_back(clip);
    std::vector<std::string> lines = Split(text, '\n');
    float lh = fm.ascent + fm.descent;
    for (size_t i = 0; i < lines.size(); ++i) {
      float base = content.y + fm.ascent + lh * i;
      if (base - fm.ascent > content.bottom()) break;
      Text(content.x, base, lines[i], s, color, alpha, false);
    }
    DisplayItem pop;
    pop.type = DisplayItem::kPopClip;
    out_->items.push_back(pop);
    return;
  }
  if (tag == "meter" || tag == "progress") {
    size_t used;
    double v = ParseDoublePrefix(el->Attr("value"), used);
    double mx = el->HasAttr("max") ? ParseDoublePrefix(el->Attr("max"), used) : 1.0;
    if (mx <= 0) mx = 1;
    float frac = (float)std::max(0.0, std::min(1.0, v / mx));
    FillRect(content, Color(220, 220, 220), alpha);
    FillRect(Rect(content.x, content.y, content.w * frac, content.h),
             tag == "meter" ? Color(0, 160, 0) : Color(10, 36, 106), alpha);
    return;
  }
  if (tag == "svg") {
    if (content.w < 1 || content.h < 1) return;
    DisplayItem it;
    it.type = DisplayItem::kSvg;
    it.rect = content;
    it.svgNode = el;
    it.color = s->color;
    it.alpha = alpha;
    out_->items.push_back(it);
    return;
  }
  if (tag == "iframe") {
    FillRect(content, Color(240, 240, 240), alpha);
    std::string src = el->Attr("src");
    DisplayItem clip;
    clip.type = DisplayItem::kPushClip;
    clip.rect = content;
    out_->items.push_back(clip);
    Text(content.x + 4, content.y + fm.ascent + 4, "[Frame] " + src, s, Color(0, 0, 238), alpha,
         false);
    DisplayItem pop;
    pop.type = DisplayItem::kPopClip;
    out_->items.push_back(pop);
    return;
  }
}

void Painter::PaintPositioned(LayoutBox* b, float ax, float ay, float alpha, bool negative) {
  if (b->positioned.empty()) return;
  std::vector<LayoutBox*> list(b->positioned.begin(), b->positioned.end());
  std::stable_sort(list.begin(), list.end(), [](LayoutBox* a, LayoutBox* c) {
    int za = a->style->zIndexAuto ? 0 : a->style->zIndex;
    int zc = c->style->zIndexAuto ? 0 : c->style->zIndex;
    return za < zc;
  });
  for (size_t i = 0; i < list.size(); ++i) {
    LayoutBox* c = list[i];
    int z = c->style->zIndexAuto ? 0 : c->style->zIndex;
    if ((z < 0) != negative) continue;
    if (!c->isPositionedChild) continue;
    PaintBox(c, ax, ay, alpha);
  }
}

void Painter::PaintBox(LayoutBox* b, float px, float py, float alpha) {
  const ComputedStyle* s = b->style;
  alpha *= s->opacity;
  if (s->clippedAway) return;
  // Heavy blur (glow effects) cannot be rendered: show only a faint tint.
  if (s->blur >= 8) alpha *= std::max(0.08f, 1.0f - s->blur / 40.0f) * 0.4f;
  if (alpha < 0.02f) return;
  float ax = px + b->x + b->relX, ay = py + b->y + b->relY;
  Rect r(ax, ay, b->w, b->h);
  if (s->visible && b->kind != LayoutBox::kTableRowGroup && b->kind != LayoutBox::kTableRow) {
    PaintBoxDecorations(b, r, alpha, false, false);
  } else if (s->visible) {
    FillRect(r, s->backgroundColor, alpha);
  }
  if (b->node && b->kind != LayoutBox::kTableRowGroup) {
    HitRegion hr = {r, b->node};
    out_->hits.push_back(hr);
  }
  Extend(r);
  if (s->display == kDisplayListItem && b->kind != LayoutBox::kReplaced) PaintMarker(b, ax, ay, alpha);

  bool clip = s->ClipsOverflow() && b->kind != LayoutBox::kReplaced && b != b->coordParent;
  if (clip && b->parent) {
    DisplayItem it;
    it.type = DisplayItem::kPushClip;
    it.rect = Rect(ax + b->border.left, ay + b->border.top,
                   b->w - b->border.left - b->border.right, b->h - b->border.top - b->border.bottom);
    out_->items.push_back(it);
    clipStack_.push_back(clipStack_.empty() ? it.rect : it.rect.intersect(clipStack_.back()));
  }
  PaintPositioned(b, ax, ay, alpha, true);
  if (b->kind == LayoutBox::kReplaced) {
    if (s->visible) PaintReplaced(b, ax, ay, alpha);
  } else if (b->inlineContent) {
    PaintLines(b, ax, ay, alpha);
    PaintFloatsInInline(b, b, ax, ay, alpha);
  } else {
    for (size_t i = 0; i < b->children.size(); ++i) {
      LayoutBox* c = b->children[i].get();
      if (c->isPositionedChild) continue;
      if (c->kind == LayoutBox::kText || c->kind == LayoutBox::kInline ||
          c->kind == LayoutBox::kLineBreak)
        continue;
      PaintBox(c, ax, ay, alpha);
    }
  }
  PaintPositioned(b, ax, ay, alpha, false);
  if (clip && b->parent) {
    DisplayItem it;
    it.type = DisplayItem::kPopClip;
    out_->items.push_back(it);
    clipStack_.pop_back();
  }
}

}  // namespace kite
