// Kite Engine - CSS grid layout.
//
// Explicit tracks (grid-template-columns/-rows with fixed, percentage, fr,
// auto, min-/max-content, minmax(), fit-content() and repeat() including
// auto-fill/auto-fit), named lines and areas, line-based and span placement,
// auto placement in row or column direction (sparse and dense), implicit
// tracks (grid-auto-rows/-columns), gaps, track sizing with intrinsic
// contributions, content distribution (justify-/align-content) and item
// alignment (justify-/align-items/-self, auto margins).
#include <algorithm>
#include <cmath>
#include <map>

#include "layout/layout.h"

namespace kite {

namespace {

const float kInf = 1e30f;

struct Item {
  LayoutBox* box;
  int r0, r1, c0, c1;  // 0-based tracks, end exclusive
  float minW, maxW;    // outer width contributions
  float h;             // outer height at the item's column width
};

// Lines of one axis after auto repetition was expanded.
struct Axis {
  std::vector<GridTrack> tracks;  // explicit tracks
  std::map<std::string, std::vector<int> > names;  // name -> 1-based lines
  int repeatStart = 0, repeatCount = 0;  // auto-repeated tracks (auto-fit collapse)
  bool autoFit = false;
};

void AddName(Axis& a, const std::string& n, int line) { a.names[n].push_back(line); }

float DefiniteSize(const GridSize& g, float avail) {
  if (g.type != GridSize::kLength) return -1;
  if (g.len.HasPercent() && avail < 0) return -1;
  return g.len.Resolve(avail < 0 ? 0 : avail);
}

// Expands repeat(auto-fill/auto-fit) for the available size |avail| (-1 = indefinite).
void BuildAxis(const GridTemplate& t, float avail, float gap, Axis& a) {
  int reps = 0;
  if (t.repeatAt >= 0 && !t.repeat.empty()) {
    reps = 1;
    if (avail >= 0) {
      float repSize = 0;
      bool ok = true;
      for (size_t i = 0; i < t.repeat.size(); ++i) {
        float mx = DefiniteSize(t.repeat[i].max, avail), mn = DefiniteSize(t.repeat[i].min, avail);
        float sz = mx >= 0 ? (mn >= 0 ? std::max(mn, mx) : mx) : mn;
        if (sz < 0) ok = false;
        else repSize += sz;
      }
      float other = 0;
      for (size_t i = 0; i < t.tracks.size(); ++i) {
        float mx = DefiniteSize(t.tracks[i].max, avail), mn = DefiniteSize(t.tracks[i].min, avail);
        other += std::max(0.0f, mx >= 0 ? mx : mn);
      }
      if (ok && repSize > 0) {
        int k = (int)t.repeat.size(), base = (int)t.tracks.size();
        while (reps < 1000) {
          int n = reps + 1;
          float total = other + n * repSize + (base + n * k - 1) * gap;
          if (total > avail + 0.01f) break;
          reps = n;
        }
      }
    }
  }
  a.tracks.clear();
  a.names.clear();
  int line = 1;
  auto addNames = [&](const std::vector<std::string>& n) {
    for (size_t i = 0; i < n.size(); ++i) AddName(a, n[i], line);
  };
  for (size_t i = 0; i <= t.tracks.size(); ++i) {
    if ((int)i == t.repeatAt && reps > 0) {
      a.repeatStart = (int)a.tracks.size();
      a.repeatCount = reps * (int)t.repeat.size();
      a.autoFit = t.autoFit;
      for (int r = 0; r < reps; ++r) {
        addNames(t.repeatNames[0]);
        for (size_t j = 0; j < t.repeat.size(); ++j) {
          a.tracks.push_back(t.repeat[j]);
          ++line;
          addNames(t.repeatNames[j + 1]);
        }
      }
    }
    if (i < t.names.size()) addNames(t.names[i]);
    if (i < t.tracks.size()) {
      a.tracks.push_back(t.tracks[i]);
      ++line;
    }
  }
}

// Resolves a grid-*-start / -end pair to a 0-based start (or -1 = auto)
// and span. |areaSuffix|: "-start" or "-end" lookups for area names.
void ResolveLines(const GridLine& s, const GridLine& e, const Axis& a, int explicitCount, int& start, int& span) {
  auto byName = [&](const GridLine& g, const char* suffix) -> int {
    std::map<std::string, std::vector<int> >::const_iterator it = a.names.find(g.name + suffix);
    if (it == a.names.end()) it = a.names.find(g.name);
    if (it == a.names.end() || it->second.empty()) return 0;
    std::vector<int> lines = it->second;
    std::sort(lines.begin(), lines.end());
    int n = g.line == 0 ? 1 : g.line;
    if (n > 0) return lines[std::min((size_t)n, lines.size()) - 1];
    return lines[lines.size() - std::min((size_t)-n, lines.size())];
  };
  auto lineOf = [&](const GridLine& g, const char* suffix) -> int {  // 1-based, 0 = auto
    if (g.span > 0) return 0;
    if (!g.name.empty()) return byName(g, suffix);
    if (g.line > 0) return g.line;
    if (g.line < 0) return std::max(1, explicitCount + 2 + g.line);
    return 0;
  };
  int ls = lineOf(s, "-start"), le = lineOf(e, "-end");
  start = -1;
  span = 1;
  if (ls && le) {
    if (le < ls) std::swap(ls, le);
    if (le == ls) le = ls + 1;
    start = ls - 1;
    span = le - ls;
  } else if (ls) {
    start = ls - 1;
    span = e.span > 0 ? e.span : 1;
  } else if (le) {
    span = s.span > 0 ? s.span : 1;
    start = std::max(0, le - 1 - span);
    span = le - 1 - start;
    if (span < 1) span = 1;
  } else {
    span = s.span > 0 ? s.span : (e.span > 0 ? e.span : 1);
  }
}

struct Occupancy {
  int cols;
  std::vector<std::vector<char> > cells;
  explicit Occupancy(int c) : cols(c) {}
  bool Free(int r, int c, int rs, int cs) {
    if (c + cs > cols) return false;
    for (int y = r; y < r + rs; ++y) {
      if (y >= (int)cells.size()) return true;
      for (int x = c; x < c + cs; ++x)
        if (cells[y][x]) return false;
    }
    return true;
  }
  void Mark(int r, int c, int rs, int cs) {
    while ((int)cells.size() < r + rs) cells.push_back(std::vector<char>(cols, 0));
    for (int y = r; y < r + rs; ++y)
      for (int x = c; x < c + cs; ++x) cells[y][x] = 1;
  }
};

// Places items in the row-major frame (|r*| = flow-crossing axis lines,
// |c*| = the axis the cursor moves along). Unresolved coordinates are -1.
void AutoPlace(std::vector<Item>& items, int cols, bool dense) {
  // The cursor axis never grows during placement: size it for every item
  // (c1 is the end line, or the span while c0 is unresolved).
  for (size_t i = 0; i < items.size(); ++i) cols = std::max(cols, items[i].c1);
  Occupancy occ(std::max(1, cols));
  // 1. Fully definite items.
  for (size_t i = 0; i < items.size(); ++i) {
    Item& it = items[i];
    if (it.r0 >= 0 && it.c0 >= 0) occ.Mark(it.r0, it.c0, it.r1 - it.r0, it.c1 - it.c0);
  }
  // 2. Items locked to a row.
  for (size_t i = 0; i < items.size(); ++i) {
    Item& it = items[i];
    if (it.r0 < 0 || it.c0 >= 0) continue;
    int cs = std::min(it.c1, occ.cols), rs = it.r1 - it.r0;
    int c = 0;
    while (c + cs <= occ.cols && !occ.Free(it.r0, c, rs, cs)) ++c;
    if (c + cs > occ.cols) c = 0;
    it.c0 = c;
    it.c1 = c + cs;
    occ.Mark(it.r0, it.c0, rs, cs);
  }
  // 3. The rest with the auto-placement cursor.
  int cr = 0, cc = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    Item& it = items[i];
    if (it.r0 >= 0) continue;
    int rs = it.r1;  // r1 holds the span while r0 is unresolved
    if (dense) cr = cc = 0;
    if (it.c0 >= 0) {
      int cs = it.c1 - it.c0;
      if (it.c0 < cc) ++cr;
      cc = it.c0;
      while (!occ.Free(cr, it.c0, rs, cs)) ++cr;
      it.r0 = cr;
      it.r1 = cr + rs;
      occ.Mark(it.r0, it.c0, rs, cs);
      cc = it.c1;
      continue;
    }
    int cs = std::min(it.c1, occ.cols);
    for (;;) {
      if (cc + cs > occ.cols) {
        cc = 0;
        ++cr;
        continue;
      }
      if (occ.Free(cr, cc, rs, cs)) break;
      ++cc;
    }
    it.r0 = cr;
    it.r1 = cr + rs;
    it.c0 = cc;
    it.c1 = cc + cs;
    occ.Mark(it.r0, it.c0, rs, cs);
    cc += cs;
  }
}

// Track sizing for one axis. |contrib(i, min)| gives item i's outer size
// contribution; |avail| < 0 means indefinite (max-content sizing).
struct TrackSizer {
  std::vector<const GridTrack*> defs;
  std::vector<float> base, limit;
  std::vector<bool> flex;

  void Run(const std::vector<Item>& items, bool columns, float avail, float gap, bool minContentMode,
           bool stretchAuto) {
    int n = (int)defs.size();
    base.assign(n, 0);
    limit.assign(n, 0);
    flex.assign(n, false);
    float pctBasis = avail;
    for (int i = 0; i < n; ++i) {
      const GridTrack& t = *defs[i];
      float mn = DefiniteSize(t.min, pctBasis), mx = DefiniteSize(t.max, pctBasis);
      base[i] = mn >= 0 ? mn : 0;
      flex[i] = t.max.type == GridSize::kFr;
      limit[i] = flex[i] ? kInf : (mx >= 0 ? mx : -1);  // -1: intrinsic, set below
    }
    auto contrib = [&](const Item& it, bool wantMin) -> float {
      if (columns) return wantMin || minContentMode ? it.minW : it.maxW;
      return it.h;
    };
    auto first = [&](const Item& it) { return columns ? it.c0 : it.r0; };
    auto last = [&](const Item& it) { return columns ? it.c1 : it.r1; };
    auto intrinsicMin = [&](int i) {
      const GridTrack& t = *defs[i];
      return t.min.type == GridSize::kAuto || t.min.type == GridSize::kMinContent ||
             t.min.type == GridSize::kMaxContent || DefiniteSize(t.min, pctBasis) < 0;
    };
    // Span-1 items.
    std::vector<float> maxContrib(n, 0);
    for (size_t k = 0; k < items.size(); ++k) {
      const Item& it = items[k];
      if (last(it) - first(it) != 1) continue;
      int i = first(it);
      const GridTrack& t = *defs[i];
      if (intrinsicMin(i))
        base[i] = std::max(base[i], contrib(it, t.min.type != GridSize::kMaxContent));
      maxContrib[i] = std::max(maxContrib[i],
                               contrib(it, t.max.type == GridSize::kMinContent));
    }
    for (int i = 0; i < n; ++i) {
      if (limit[i] < 0) {
        limit[i] = maxContrib[i];
        if (defs[i]->fitContent) limit[i] = std::min(limit[i], defs[i]->fitLimit.Resolve(avail < 0 ? 0 : avail));
      }
      if (limit[i] < base[i]) limit[i] = base[i];
    }
    // Spanning items: grow the intrinsic tracks they cross.
    for (size_t k = 0; k < items.size(); ++k) {
      const Item& it = items[k];
      int a = first(it), b = last(it);
      if (b - a < 2) continue;
      bool hasFlex = false;
      for (int i = a; i < b; ++i)
        if (flex[i]) hasFlex = true;
      float gaps = gap * (b - a - 1);
      // Base sizes.
      float sum = gaps;
      std::vector<int> grow;
      for (int i = a; i < b; ++i) {
        sum += base[i];
        if (hasFlex ? flex[i] : intrinsicMin(i)) grow.push_back(i);
      }
      float extra = contrib(it, true) - sum;
      if (extra > 0 && !grow.empty())
        for (size_t g = 0; g < grow.size(); ++g) base[grow[g]] += extra / grow.size();
      if (hasFlex) continue;
      // Growth limits.
      sum = gaps;
      grow.clear();
      for (int i = a; i < b; ++i) {
        if (limit[i] < base[i]) limit[i] = base[i];
        sum += limit[i];
        if (DefiniteSize(defs[i]->max, pctBasis) < 0) grow.push_back(i);
      }
      extra = contrib(it, false) - sum;
      if (extra > 0 && !grow.empty())
        for (size_t g = 0; g < grow.size(); ++g) limit[grow[g]] += extra / grow.size();
    }
    for (int i = 0; i < n; ++i)
      if (!flex[i] && limit[i] < base[i]) limit[i] = base[i];
    float gaps = n > 1 ? gap * (n - 1) : 0;
    // Maximize non-flexible tracks.
    if (avail < 0) {
      for (int i = 0; i < n; ++i)
        if (!flex[i]) base[i] = limit[i];
    } else {
      float used = gaps;
      for (int i = 0; i < n; ++i) used += base[i];
      float free = avail - used;
      for (int round = 0; round < 8 && free > 0.01f; ++round) {
        int growable = 0;
        for (int i = 0; i < n; ++i)
          if (!flex[i] && limit[i] > base[i] + 0.01f) ++growable;
        if (!growable) break;
        float share = free / growable;
        for (int i = 0; i < n; ++i) {
          if (flex[i] || limit[i] <= base[i] + 0.01f) continue;
          float d = std::min(share, limit[i] - base[i]);
          base[i] += d;
          free -= d;
        }
      }
    }
    // Flexible tracks.
    bool anyFlex = false;
    for (int i = 0; i < n; ++i)
      if (flex[i]) anyFlex = true;
    if (anyFlex) {
      float frSize = 0;
      if (avail >= 0) {
        std::vector<bool> inflexible(n, false);
        for (int round = 0; round < n + 1; ++round) {
          float leftover = avail - gaps, frSum = 0;
          for (int i = 0; i < n; ++i) {
            if (flex[i] && !inflexible[i]) frSum += defs[i]->max.fr;
            else leftover -= base[i];
          }
          frSize = std::max(0.0f, leftover) / std::max(1.0f, frSum);
          bool changed = false;
          for (int i = 0; i < n; ++i)
            if (flex[i] && !inflexible[i] && frSize * defs[i]->max.fr < base[i]) {
              inflexible[i] = true;
              changed = true;
            }
          if (!changed) break;
        }
      } else {
        for (int i = 0; i < n; ++i) {
          if (!flex[i]) continue;
          float fr = defs[i]->max.fr;
          float need = std::max(base[i], maxContrib[i]);
          if (fr > 0) frSize = std::max(frSize, fr > 1 ? need / fr : need);
        }
        for (size_t k = 0; k < items.size(); ++k) {
          const Item& it = items[k];
          int a = first(it), b = last(it);
          if (b - a < 2) continue;
          float frSum = 0, fixed = gap * (b - a - 1);
          for (int i = a; i < b; ++i) {
            if (flex[i]) frSum += defs[i]->max.fr;
            else fixed += base[i];
          }
          if (frSum > 0) frSize = std::max(frSize, (contrib(it, false) - fixed) / std::max(1.0f, frSum));
        }
      }
      for (int i = 0; i < n; ++i)
        if (flex[i]) base[i] = std::max(base[i], frSize * defs[i]->max.fr);
    }
    // Stretch auto tracks into remaining space.
    if (stretchAuto && avail >= 0) {
      float used = gaps;
      int autos = 0;
      for (int i = 0; i < n; ++i) {
        used += base[i];
        if (!flex[i] && defs[i]->max.type == GridSize::kAuto) ++autos;
      }
      if (autos && avail > used)
        for (int i = 0; i < n; ++i)
          if (!flex[i] && defs[i]->max.type == GridSize::kAuto) base[i] += (avail - used) / autos;
    }
  }
};

// Track offsets including gaps and content distribution.
std::vector<float> TrackOffsets(const std::vector<float>& size, float gap, float avail, FlexAlign dist,
                                const std::vector<bool>& collapsed, float& total) {
  int n = (int)size.size();
  int visible = 0;
  float sum = 0;
  for (int i = 0; i < n; ++i) {
    sum += size[i];
    if (!collapsed[i]) ++visible;
  }
  float gaps = visible > 1 ? gap * (visible - 1) : 0;
  float free = avail >= 0 ? avail - sum - gaps : 0;
  float lead = 0, between = gap;
  if (free > 0) {
    switch (dist) {
      case kFlexEnd: lead = free; break;
      case kFlexCenter: lead = free / 2; break;
      case kFlexSpaceBetween: if (visible > 1) between += free / (visible - 1); break;
      case kFlexSpaceAround: if (visible) { lead = free / visible / 2; between += free / visible; } break;
      case kFlexSpaceEvenly: if (visible) { lead = free / (visible + 1); between += free / (visible + 1); } break;
      default: break;
    }
  }
  std::vector<float> off(n + 1, 0);
  float pos = lead;
  bool firstVisible = true;
  for (int i = 0; i < n; ++i) {
    if (!collapsed[i]) {
      if (!firstVisible) pos += between;
      firstVisible = false;
    }
    off[i] = pos;
    pos += size[i];
  }
  off[n] = pos;
  total = pos - lead;
  return off;
}

FlexAlign Resolve(FlexAlign self, FlexAlign items, bool replaced) {
  FlexAlign a = self;
  if (a == kFlexAuto || a == kFlexNormal) a = items;
  if (a == kFlexNormal || a == kFlexAuto) a = replaced ? kFlexStart : kFlexStretch;
  if (a == kFlexBaseline || a == kFlexSpaceBetween || a == kFlexSpaceAround || a == kFlexSpaceEvenly) a = kFlexStart;
  return a;
}

struct GridState {
  Axis cols, rows;
  std::vector<Item> items;
  std::vector<GridTrack> colDefs, rowDefs;  // explicit + implicit
  std::vector<bool> colCollapsed, rowCollapsed;
};

void Build(const ComputedStyle* s, std::vector<LayoutBox*>& boxes, float availW, float availH, float colGap,
           float rowGap, GridState& g) {
  BuildAxis(s->gridColumns, availW, colGap, g.cols);
  BuildAxis(s->gridRows, availH, rowGap, g.rows);
  // Areas name their lines.
  for (size_t i = 0; i < s->gridAreas.size(); ++i) {
    const GridArea& a = s->gridAreas[i];
    AddName(g.cols, a.name + "-start", a.c0 + 1);
    AddName(g.cols, a.name + "-end", a.c1 + 1);
    AddName(g.rows, a.name + "-start", a.r0 + 1);
    AddName(g.rows, a.name + "-end", a.r1 + 1);
  }
  int explicitCols = std::max((int)g.cols.tracks.size(), s->gridAreaCols);
  int explicitRows = std::max((int)g.rows.tracks.size(), s->gridAreaRows);
  bool colFlow = s->gridFlowColumn;
  g.items.clear();
  for (size_t i = 0; i < boxes.size(); ++i) {
    const ComputedStyle* is = boxes[i]->style;
    Item it;
    it.box = boxes[i];
    it.minW = it.maxW = it.h = 0;
    int cs, cspan, rs, rspan;
    ResolveLines(is->gridColumnStart, is->gridColumnEnd, g.cols, explicitCols, cs, cspan);
    ResolveLines(is->gridRowStart, is->gridRowEnd, g.rows, explicitRows, rs, rspan);
    // Into the placement frame: r = the axis that grows, c = the cursor axis.
    int pr = colFlow ? cs : rs, prSpan = colFlow ? cspan : rspan;
    int pc = colFlow ? rs : cs, pcSpan = colFlow ? rspan : cspan;
    it.r0 = pr;
    it.r1 = pr >= 0 ? pr + prSpan : prSpan;
    it.c0 = pc;
    it.c1 = pc >= 0 ? pc + pcSpan : pcSpan;
    g.items.push_back(it);
  }
  AutoPlace(g.items, colFlow ? explicitRows : explicitCols, s->gridFlowDense);
  int ncols = explicitCols, nrows = explicitRows;
  for (size_t i = 0; i < g.items.size(); ++i) {
    Item& it = g.items[i];
    if (colFlow) {
      std::swap(it.r0, it.c0);
      std::swap(it.r1, it.c1);
    }
    ncols = std::max(ncols, it.c1);
    nrows = std::max(nrows, it.r1);
  }
  ncols = std::max(ncols, 1);
  auto fill = [](const Axis& a, const std::vector<GridTrack>& autos, int count, std::vector<GridTrack>& out) {
    out = a.tracks;
    int explicitN = (int)a.tracks.size();
    GridTrack dflt;  // auto
    for (int i = explicitN; i < count; ++i)
      out.push_back(autos.empty() ? dflt : autos[(i - explicitN) % autos.size()]);
  };
  fill(g.cols, s->gridAutoColumns, ncols, g.colDefs);
  fill(g.rows, s->gridAutoRows, nrows, g.rowDefs);
  g.colCollapsed.assign(ncols, false);
  g.rowCollapsed.assign(nrows, false);
  // auto-fit: empty repeated tracks collapse.
  auto collapse = [&](const Axis& a, bool columns, std::vector<bool>& col, std::vector<GridTrack>& defs) {
    if (!a.autoFit) return;
    for (int t = a.repeatStart; t < a.repeatStart + a.repeatCount && t < (int)defs.size(); ++t) {
      bool used = false;
      for (size_t i = 0; i < g.items.size() && !used; ++i) {
        const Item& it = g.items[i];
        int s0 = columns ? it.c0 : it.r0, s1 = columns ? it.c1 : it.r1;
        if (t >= s0 && t < s1) used = true;
      }
      if (!used) {
        col[t] = true;
        GridTrack zero;
        zero.min.type = zero.max.type = GridSize::kLength;
        zero.min.len = zero.max.len = Length::Px(0);
        defs[t] = zero;
      }
    }
  };
  collapse(g.cols, true, g.colCollapsed, g.colDefs);
  collapse(g.rows, false, g.rowCollapsed, g.rowDefs);
}

}  // namespace

float FlexLayout::LayoutGrid(LayoutBox* box, float definiteH) {
  const ComputedStyle* s = box->style;
  float cw = box->ContentW();
  float cx = box->ContentX(), cy = box->ContentY();
  std::vector<LayoutBox*> abs, boxes;
  for (size_t i = 0; i < box->children.size(); ++i) {
    LayoutBox* c = box->children[i].get();
    c->coordParent = box;
    if (c->IsOutOfFlow()) abs.push_back(c);
    else boxes.push_back(c);
  }
  std::stable_sort(boxes.begin(), boxes.end(),
                   [](LayoutBox* a, LayoutBox* b) { return a->style->order < b->style->order; });
  for (size_t i = 0; i < abs.size(); ++i) {
    e_->ResolveEdges(abs[i], cw);
    e_->RegisterPositioned(abs[i], cx, cy);
  }
  float colGap = s->columnGap.Resolve(cw);
  float rowGap = s->rowGap.Resolve(definiteH >= 0 ? definiteH : 0);
  GridState g;
  Build(s, boxes, cw, definiteH, colGap, rowGap, g);

  // Columns.
  for (size_t i = 0; i < g.items.size(); ++i) {
    Item& it = g.items[i];
    e_->ResolveEdges(it.box, cw);
    e_->ComputeIntrinsic(it.box, it.minW, it.maxW);
  }
  TrackSizer colSizer;
  for (size_t i = 0; i < g.colDefs.size(); ++i) colSizer.defs.push_back(&g.colDefs[i]);
  bool stretchCols = s->justifyContent == kFlexNormal || s->justifyContent == kFlexStretch;
  colSizer.Run(g.items, true, cw, colGap, false, stretchCols);
  float colTotal;
  std::vector<float> colX = TrackOffsets(colSizer.base, colGap, cw, s->justifyContent, g.colCollapsed, colTotal);

  // Lay out items at their column widths to get the row contributions.
  std::vector<float> itemW(g.items.size(), 0);
  std::vector<FlexAlign> justify(g.items.size());
  for (size_t i = 0; i < g.items.size(); ++i) {
    Item& it = g.items[i];
    LayoutBox* b = it.box;
    float areaW = colX[it.c1 - 1] + colSizer.base[it.c1 - 1] - colX[it.c0];
    itemW[i] = areaW;
    bool replaced = b->kind == LayoutBox::kReplaced;
    justify[i] = Resolve(b->style->justifySelf, s->justifyItems, replaced);
    e_->ResolveEdges(b, areaW);
    b->x = 0;
    b->y = 0;
    if (replaced) {
      e_->LayoutReplaced(b, areaW, -1, -1, -1);
    } else {
      bool autoW = b->style->width.IsAuto();
      bool marginAuto = b->style->margin[1].IsAuto() || b->style->margin[3].IsAuto();
      float bw = autoW && justify[i] == kFlexStretch && !marginAuto
                     ? areaW - b->margin.left - b->margin.right
                     : e_->SolveWidth(b, areaW, areaW, true);
      e_->LayoutBlockLevel(b, areaW, -1, 0, 0, 0, std::max(0.0f, bw), -1);
    }
    it.h = b->h + b->margin.top + b->margin.bottom;
  }

  // Rows.
  TrackSizer rowSizer;
  for (size_t i = 0; i < g.rowDefs.size(); ++i) rowSizer.defs.push_back(&g.rowDefs[i]);
  bool stretchRows = s->alignContent == kFlexNormal || s->alignContent == kFlexStretch;
  rowSizer.Run(g.items, false, definiteH, rowGap, false, stretchRows);
  if (definiteH < 0 && s->minHeight.IsFixed() && !s->minHeight.HasPercent()) {
    // An auto-height grid with a min-height: size the rows again for that
    // height when they come out smaller (fr rows then fill it).
    float minH = s->minHeight.Resolve(0);
    if (s->boxSizing == kBorderBox)
      minH -= box->padding.top + box->padding.bottom + box->border.top + box->border.bottom;
    float total = 0;
    for (size_t i = 0; i < rowSizer.base.size(); ++i) total += rowSizer.base[i];
    if (rowSizer.base.size() > 1) total += rowGap * (rowSizer.base.size() - 1);
    if (minH > total + 0.5f) {
      definiteH = minH;
      rowSizer.Run(g.items, false, definiteH, rowGap, false, stretchRows);
    }
  }
  float rowTotal;
  std::vector<float> rowY = TrackOffsets(rowSizer.base, rowGap, definiteH, s->alignContent, g.rowCollapsed, rowTotal);

  // Final sizes and positions.
  for (size_t i = 0; i < g.items.size(); ++i) {
    Item& it = g.items[i];
    LayoutBox* b = it.box;
    const ComputedStyle* is = b->style;
    bool replaced = b->kind == LayoutBox::kReplaced;
    float areaW = itemW[i];
    float areaH = rowY[it.r1 - 1] + rowSizer.base[it.r1 - 1] - rowY[it.r0];
    FlexAlign al = Resolve(is->alignSelf, s->alignItems, replaced);
    bool mTop = is->margin[0].IsAuto(), mBottom = is->margin[2].IsAuto();
    float outerH = b->h + b->margin.top + b->margin.bottom;
    if (al == kFlexStretch && is->height.IsAuto() && !replaced && !mTop && !mBottom &&
        std::fabs(outerH - areaH) > 0.01f) {
      e_->LayoutBlockLevel(b, areaW, areaH, 0, 0, 0, b->w, std::max(0.0f, areaH - b->margin.top - b->margin.bottom));
      outerH = b->h + b->margin.top + b->margin.bottom;
    } else if (is->height.HasPercent() && !replaced) {
      // Percentage heights resolve against the grid area.
      e_->LayoutBlockLevel(b, areaW, areaH, 0, 0, 0, b->w, -1);
      outerH = b->h + b->margin.top + b->margin.bottom;
    }
    float offY = 0, freeY = areaH - outerH;
    if (mTop && mBottom) offY = std::max(0.0f, freeY / 2);
    else if (mTop) offY = std::max(0.0f, freeY);
    else if (!mBottom) {
      if (al == kFlexCenter) offY = freeY / 2;
      else if (al == kFlexEnd) offY = freeY;
    }
    float outerW = b->w + b->margin.left + b->margin.right;
    bool mLeft = is->margin[3].IsAuto(), mRight = is->margin[1].IsAuto();
    float offX = 0, freeX = areaW - outerW;
    if (mLeft && mRight) offX = std::max(0.0f, freeX / 2);
    else if (mLeft) offX = std::max(0.0f, freeX);
    else if (!mRight) {
      if (justify[i] == kFlexCenter) offX = freeX / 2;
      else if (justify[i] == kFlexEnd) offX = freeX;
    }
    b->x = cx + colX[it.c0] + offX + b->margin.left;
    b->y = cy + rowY[it.r0] + offY + b->margin.top;
  }
  // Baseline: the first item in the first row.
  for (size_t i = 0; i < g.items.size() && box->firstBaseline < 0; ++i) {
    LayoutBox* b = g.items[i].box;
    if (g.items[i].r0 == 0 && b->firstBaseline >= 0) box->firstBaseline = b->y + b->firstBaseline;
  }
  box->lastBaseline = box->firstBaseline;
  float h = rowY.empty() ? 0 : rowY.back();
  return definiteH >= 0 ? std::max(h, definiteH) : h;
}

void FlexLayout::GridIntrinsic(LayoutBox* box, float& minW, float& maxW) {
  const ComputedStyle* s = box->style;
  std::vector<LayoutBox*> boxes;
  for (size_t i = 0; i < box->children.size(); ++i) {
    LayoutBox* c = box->children[i].get();
    if (!c->IsOutOfFlow()) boxes.push_back(c);
  }
  std::stable_sort(boxes.begin(), boxes.end(),
                   [](LayoutBox* a, LayoutBox* b) { return a->style->order < b->style->order; });
  float gap = s->columnGap.HasPercent() ? 0 : s->columnGap.Resolve(0);
  GridState g;
  Build(s, boxes, -1, -1, gap, 0, g);
  for (size_t i = 0; i < g.items.size(); ++i) e_->ComputeIntrinsic(g.items[i].box, g.items[i].minW, g.items[i].maxW);
  for (int pass = 0; pass < 2; ++pass) {
    TrackSizer t;
    for (size_t i = 0; i < g.colDefs.size(); ++i) t.defs.push_back(&g.colDefs[i]);
    t.Run(g.items, true, -1, gap, pass == 0, false);
    float total;
    TrackOffsets(t.base, gap, -1, kFlexStart, g.colCollapsed, total);
    (pass == 0 ? minW : maxW) = total;
  }
  if (minW > maxW) maxW = minW;
}

}  // namespace kite
