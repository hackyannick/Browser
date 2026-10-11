#include <algorithm>
#include <cmath>

#include "layout/layout.h"

namespace kite {

namespace {

struct FlexItem {
  LayoutBox* box;
  float base;      // flex base size (border box, main axis)
  float hypo;      // clamped
  float minMain, maxMain;
  float main;      // final main size
  float marginMain;  // sum of non-auto main margins
  int autoMargins;
  float cross;     // outer cross size after layout
};

FlexAlign ItemAlign(const LayoutBox* item, const ComputedStyle* container) {
  FlexAlign a = item->style->alignSelf;
  if (a == kFlexAuto || a == kFlexNormal) a = container->alignItems;
  if (a == kFlexNormal) a = kFlexStretch;
  return a;
}

std::vector<LayoutBox*> InFlowItems(LayoutBox* box, LayoutEngine* e,
                                    std::vector<LayoutBox*>& outOfFlow) {
  std::vector<LayoutBox*> items;
  for (size_t i = 0; i < box->children.size(); ++i) {
    LayoutBox* c = box->children[i].get();
    c->coordParent = box;
    if (c->IsOutOfFlow()) {
      outOfFlow.push_back(c);
      continue;
    }
    items.push_back(c);
  }
  std::stable_sort(items.begin(), items.end(), [](LayoutBox* a, LayoutBox* b) {
    return a->style->order < b->style->order;
  });
  return items;
}

}  // namespace

float FlexLayout::LayoutFlex(LayoutBox* box, float definiteH) {
  const ComputedStyle* s = box->style;
  float cw = box->ContentW();
  float cx = box->ContentX(), cy = box->ContentY();
  std::vector<LayoutBox*> abs;
  std::vector<LayoutBox*> items = InFlowItems(box, e_, abs);
  for (size_t i = 0; i < abs.size(); ++i) {
    e_->ResolveEdges(abs[i], cw);
    e_->RegisterPositioned(abs[i], cx, cy);
  }
  bool row = s->flexDirection == kFlexRow || s->flexDirection == kFlexRowReverse;
  bool reverse = s->flexDirection == kFlexRowReverse || s->flexDirection == kFlexColumnReverse;
  float colGap = s->columnGap.Resolve(cw);
  float rowGap = s->rowGap.Resolve(definiteH >= 0 ? definiteH : 0);

  if (row) {
    float gap = colGap;
    std::vector<FlexItem> fi(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
      LayoutBox* it = items[i];
      e_->ResolveEdges(it, cw);
      const ComputedStyle* is = it->style;
      FlexItem& f = fi[i];
      f.box = it;
      float pb = it->padding.left + it->padding.right + it->border.left + it->border.right;
      float mn, mx;
      e_->ComputeIntrinsic(it, mn, mx);
      float margins = it->margin.left + it->margin.right;
      if (is->flexBasis.IsFixed() && !(is->flexBasis.HasPercent() && cw < 0)) {
        f.base = is->flexBasis.Resolve(cw) + (is->boxSizing == kContentBox ? pb : 0);
      } else if (is->width.IsFixed()) {
        f.base = is->width.Resolve(cw) + (is->boxSizing == kContentBox ? pb : 0);
      } else if (it->kind == LayoutBox::kReplaced) {
        float w, h;
        e_->ReplacedSize(it, cw, -1, w, h);
        f.base = w + pb;
      } else {
        f.base = mx - margins;
      }
      // Automatic minimum size.
      if (is->minWidth.IsAuto()) {
        f.minMain = is->ClipsOverflow() ? pb : mn - margins;
        if (is->width.IsFixed()) {
          float w = is->width.Resolve(cw) + (is->boxSizing == kContentBox ? pb : 0);
          f.minMain = std::min(f.minMain, w);
        }
      } else {
        f.minMain = is->minWidth.Resolve(cw) + (is->boxSizing == kContentBox ? pb : 0);
      }
      f.maxMain = 1e9f;
      if (is->maxWidth.IsFixed())
        f.maxMain = is->maxWidth.Resolve(cw) + (is->boxSizing == kContentBox ? pb : 0);
      f.minMain = std::max(f.minMain, pb);
      f.hypo = std::max(f.minMain, std::min(f.base, f.maxMain));
      f.autoMargins = (is->margin[3].IsAuto() ? 1 : 0) + (is->margin[1].IsAuto() ? 1 : 0);
      f.marginMain = margins;
    }
    // Line breaking.
    std::vector<std::pair<size_t, size_t> > lines;
    size_t start = 0;
    float used = 0;
    for (size_t i = 0; i < fi.size(); ++i) {
      float outer = fi[i].hypo + fi[i].marginMain;
      if (s->flexWrap && i > start && used + gap + outer > cw + 0.01f) {
        lines.push_back(std::make_pair(start, i));
        start = i;
        used = 0;
      }
      used += (i > start ? gap : 0) + outer;
    }
    if (start < fi.size() || fi.empty()) lines.push_back(std::make_pair(start, fi.size()));

    float crossPos = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
      size_t a = lines[li].first, b = lines[li].second;
      if (a == b) continue;
      float sum = 0;
      for (size_t i = a; i < b; ++i) sum += fi[i].hypo + fi[i].marginMain;
      sum += gap * (b - a - 1);
      float free = cw - sum;
      for (size_t i = a; i < b; ++i) fi[i].main = fi[i].hypo;
      if (free > 0) {
        float grow = 0;
        for (size_t i = a; i < b; ++i) grow += fi[i].box->style->flexGrow;
        if (grow > 0) {
          // Distribute from the flex base sizes of the growing items, then
          // clamp to their min/max sizes.
          float baseFree = cw - gap * (b - a - 1);
          for (size_t i = a; i < b; ++i)
            baseFree -= fi[i].marginMain + (fi[i].box->style->flexGrow > 0 ? fi[i].base : fi[i].hypo);
          float share = grow < 1 ? baseFree * grow : baseFree;
          for (size_t i = a; i < b; ++i) {
            float g = fi[i].box->style->flexGrow;
            if (g <= 0) continue;
            fi[i].main = std::max(fi[i].minMain,
                                  std::min(fi[i].maxMain, fi[i].base + share * g / grow));
          }
        }
      } else if (free < 0) {
        // Shrink, iterating so that items clamped at their minimum give
        // their share to the others.
        std::vector<char> frozen(b - a, 0);
        float remaining = -free;
        for (int iter = 0; iter < 5 && remaining > 0.01f; ++iter) {
          float weights = 0;
          for (size_t i = a; i < b; ++i)
            if (!frozen[i - a]) weights += fi[i].box->style->flexShrink * fi[i].base;
          if (weights <= 0) break;
          float taken = 0;
          for (size_t i = a; i < b; ++i) {
            if (frozen[i - a]) continue;
            float want = remaining * fi[i].box->style->flexShrink * fi[i].base / weights;
            float nm = fi[i].main - want;
            if (nm < fi[i].minMain) {
              nm = fi[i].minMain;
              frozen[i - a] = 1;
            }
            taken += fi[i].main - nm;
            fi[i].main = nm;
          }
          remaining -= taken;
          if (taken <= 0.01f) break;
        }
      }
      // Layout items at their final width.
      float lineCross = 0;
      for (size_t i = a; i < b; ++i) {
        LayoutBox* it = fi[i].box;
        it->x = 0;
        it->y = 0;
        float cbH = definiteH;
        if (it->kind == LayoutBox::kReplaced) {
          e_->LayoutReplaced(it, cw, cbH, fi[i].main, -1);
        } else {
          e_->LayoutBlockLevel(it, cw, cbH, 0, 0, 0, fi[i].main, -1);
        }
        fi[i].cross = it->h + it->margin.top + it->margin.bottom;
        lineCross = std::max(lineCross, fi[i].cross);
      }
      if (lines.size() == 1 && definiteH >= 0) lineCross = std::max(lineCross, definiteH);
      // Stretch.
      for (size_t i = a; i < b; ++i) {
        LayoutBox* it = fi[i].box;
        if (ItemAlign(it, s) != kFlexStretch || !it->style->height.IsAuto()) continue;
        float target = lineCross - it->margin.top - it->margin.bottom;
        target = e_->ClampHeight(it, target, definiteH);
        if (target > it->h + 0.01f) {
          if (it->kind == LayoutBox::kReplaced) {
            it->h = target;
          } else {
            e_->LayoutBlockLevel(it, cw, definiteH, 0, 0, 0, fi[i].main, target);
          }
          fi[i].cross = it->h + it->margin.top + it->margin.bottom;
        }
      }
      // Main axis distribution.
      float total = 0;
      int autoCount = 0;
      for (size_t i = a; i < b; ++i) {
        total += fi[i].main + fi[i].marginMain;
        autoCount += fi[i].autoMargins;
      }
      total += gap * (b - a - 1);
      float remain = cw - total;
      float lead = 0, between = gap;
      float autoShare = 0;
      if (remain > 0 && autoCount > 0) {
        autoShare = remain / autoCount;
      } else if (remain > 0) {
        size_t n = b - a;
        switch (s->justifyContent) {
          case kFlexEnd: lead = remain; break;
          case kFlexCenter: lead = remain / 2; break;
          case kFlexSpaceBetween: if (n > 1) between += remain / (n - 1); break;
          case kFlexSpaceAround: lead = remain / n / 2; between += remain / n; break;
          case kFlexSpaceEvenly: lead = remain / (n + 1); between += remain / (n + 1); break;
          default: break;
        }
      } else if (remain < 0 && s->justifyContent == kFlexCenter && !s->flexWrap) {
        lead = remain / 2;
      }
      float pos = lead;
      for (size_t k = a; k < b; ++k) {
        size_t i = reverse ? (b - 1 - (k - a)) : k;
        LayoutBox* it = fi[i].box;
        float ml = it->margin.left, mr = it->margin.right;
        if (autoShare > 0) {
          if (it->style->margin[3].IsAuto()) ml += autoShare;
          if (it->style->margin[1].IsAuto()) mr += autoShare;
        }
        float x = reverse ? cw - pos - mr - it->w : pos + ml;
        it->x = cx + x;
        // Cross alignment.
        float outer = fi[i].cross;
        float off = 0;
        FlexAlign al = ItemAlign(it, s);
        bool mtAuto = it->style->margin[0].IsAuto(), mbAuto = it->style->margin[2].IsAuto();
        if (mtAuto || mbAuto) {
          float extra = lineCross - outer;
          if (extra > 0) off = (mtAuto && mbAuto) ? extra / 2 : (mtAuto ? extra : 0);
        } else if (al == kFlexCenter) {
          off = (lineCross - outer) / 2;
        } else if (al == kFlexEnd) {
          off = lineCross - outer;
        }
        it->y = cy + crossPos + off + it->margin.top;
        pos += ml + it->w + mr + between;
        if (box->firstBaseline < 0 && it->firstBaseline >= 0)
          box->firstBaseline = it->y + it->firstBaseline;
      }
      crossPos += lineCross + (li + 1 < lines.size() ? rowGap : 0);
    }
    box->lastBaseline = box->firstBaseline;
    return crossPos;
  }

  // Column direction.
  float gap = rowGap;
  std::vector<FlexItem> fi(items.size());
  float sum = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    LayoutBox* it = items[i];
    e_->ResolveEdges(it, cw);
    FlexItem& f = fi[i];
    f.box = it;
    FlexAlign al = ItemAlign(it, s);
    float w;
    if (it->kind == LayoutBox::kReplaced) {
      e_->LayoutReplaced(it, cw, definiteH, -1, -1);
      w = it->w;
    } else if (al == kFlexStretch && it->style->width.IsAuto()) {
      w = e_->SolveWidth(it, cw, cw, false);
    } else {
      w = e_->SolveWidth(it, cw, cw, true);
    }
    it->x = 0;
    it->y = 0;
    if (it->kind != LayoutBox::kReplaced) e_->LayoutBlockLevel(it, cw, -1, 0, 0, 0, w, -1);
    const ComputedStyle* is = it->style;
    float pbV = it->padding.top + it->padding.bottom + it->border.top + it->border.bottom;
    f.base = it->h;
    if (is->flexBasis.IsFixed() && !(is->flexBasis.HasPercent() && definiteH < 0)) {
      f.base = is->flexBasis.Resolve(definiteH) + (is->boxSizing == kContentBox ? pbV : 0);
    }
    f.minMain = is->minHeight.IsAuto() ? (is->ClipsOverflow() ? pbV : it->h)
                                      : is->minHeight.Resolve(definiteH);
    f.maxMain = is->maxHeight.IsFixed() ? is->maxHeight.Resolve(definiteH) : 1e9f;
    f.hypo = std::max(std::min(f.base, f.maxMain), std::min(f.minMain, f.base));
    f.main = f.hypo;
    f.marginMain = it->margin.top + it->margin.bottom;
    sum += f.hypo + f.marginMain;
  }
  if (!fi.empty()) sum += gap * (fi.size() - 1);
  if (definiteH >= 0 && !fi.empty()) {
    float free = definiteH - sum;
    if (free > 0) {
      float grow = 0;
      for (size_t i = 0; i < fi.size(); ++i) grow += fi[i].box->style->flexGrow;
      if (grow > 0) {
        for (size_t i = 0; i < fi.size(); ++i)
          fi[i].main = std::min(fi[i].maxMain,
                                fi[i].hypo + free * fi[i].box->style->flexGrow / grow);
      }
    } else if (free < 0) {
      float weights = 0;
      for (size_t i = 0; i < fi.size(); ++i) weights += fi[i].box->style->flexShrink * fi[i].hypo;
      if (weights > 0)
        for (size_t i = 0; i < fi.size(); ++i)
          fi[i].main = std::max(fi[i].minMain, fi[i].hypo + free * fi[i].box->style->flexShrink *
                                                                 fi[i].hypo / weights);
    }
  }
  float total = 0;
  for (size_t i = 0; i < fi.size(); ++i) {
    LayoutBox* it = fi[i].box;
    if (std::fabs(fi[i].main - it->h) > 0.01f) {
      if (it->kind == LayoutBox::kReplaced) it->h = fi[i].main;
      else e_->LayoutBlockLevel(it, cw, definiteH, 0, 0, 0, it->w, fi[i].main);
    }
    total += it->h + fi[i].marginMain;
  }
  if (!fi.empty()) total += gap * (fi.size() - 1);
  float lead = 0, between = gap;
  float remain = definiteH >= 0 ? definiteH - total : 0;
  if (remain > 0) {
    size_t n = fi.size();
    switch (s->justifyContent) {
      case kFlexEnd: lead = remain; break;
      case kFlexCenter: lead = remain / 2; break;
      case kFlexSpaceBetween: if (n > 1) between += remain / (n - 1); break;
      case kFlexSpaceAround: if (n) { lead = remain / n / 2; between += remain / n; } break;
      case kFlexSpaceEvenly: lead = remain / (n + 1); between += remain / (n + 1); break;
      default: break;
    }
  }
  float pos = lead;
  for (size_t k = 0; k < fi.size(); ++k) {
    size_t i = reverse ? fi.size() - 1 - k : k;
    LayoutBox* it = fi[i].box;
    float outerW = it->w + it->margin.left + it->margin.right;
    float off = 0;
    FlexAlign al = ItemAlign(it, s);
    bool mlAuto = it->style->margin[3].IsAuto(), mrAuto = it->style->margin[1].IsAuto();
    float extra = cw - outerW;
    if ((mlAuto || mrAuto) && extra > 0) off = (mlAuto && mrAuto) ? extra / 2 : (mlAuto ? extra : 0);
    else if (al == kFlexCenter) off = extra / 2;
    else if (al == kFlexEnd) off = extra;
    it->x = cx + off + it->margin.left;
    it->y = cy + pos + it->margin.top;
    pos += it->h + fi[i].marginMain + between;
    if (box->firstBaseline < 0 && it->firstBaseline >= 0) box->firstBaseline = it->y + it->firstBaseline;
    if (it->lastBaseline >= 0) box->lastBaseline = it->y + it->lastBaseline;
  }
  float content = fi.empty() ? 0 : pos - between;
  return definiteH >= 0 ? std::max(definiteH, content) : content;
}

void FlexLayout::FlexIntrinsic(LayoutBox* box, float& minW, float& maxW) {
  const ComputedStyle* s = box->style;
  bool row = s->flexDirection == kFlexRow || s->flexDirection == kFlexRowReverse;
  float gap = s->columnGap.Resolve(0);
  minW = maxW = 0;
  int n = 0;
  for (size_t i = 0; i < box->children.size(); ++i) {
    LayoutBox* c = box->children[i].get();
    if (c->IsOutOfFlow()) continue;
    float mn, mx;
    e_->ComputeIntrinsic(c, mn, mx);
    if (row) {
      maxW += mx + (n ? gap : 0);
      if (s->flexWrap) minW = std::max(minW, mn);
      else minW += mn + (n ? gap : 0);
    } else {
      maxW = std::max(maxW, mx);
      minW = std::max(minW, mn);
    }
    ++n;
  }
}

}  // namespace kite
