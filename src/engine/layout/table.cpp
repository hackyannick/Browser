#include <algorithm>

#include "layout/layout.h"

namespace kite {

namespace {

struct CellInfo {
  LayoutBox* cell;
  LayoutBox* row;
  int r, c, colspan, rowspan;
};

struct TableGrid {
  std::vector<LayoutBox*> captions;
  std::vector<LayoutBox*> groups;
  std::vector<LayoutBox*> rows;
  std::vector<int> rowGroup;  // index into groups
  std::vector<CellInfo> cells;
  int ncols;
};

void BuildGrid(LayoutBox* table, TableGrid& g) {
  g.ncols = 0;
  std::vector<LayoutBox*> head, body, foot;
  for (size_t i = 0; i < table->children.size(); ++i) {
    LayoutBox* c = table->children[i].get();
    c->coordParent = table;
    if (c->kind == LayoutBox::kTableCaption) g.captions.push_back(c);
    else if (c->kind == LayoutBox::kTableRowGroup) {
      Display d = c->style->display;
      if (d == kDisplayTableHeaderGroup && head.empty()) head.push_back(c);
      else if (d == kDisplayTableFooterGroup && foot.empty()) foot.push_back(c);
      else body.push_back(c);
    }
  }
  g.groups.insert(g.groups.end(), head.begin(), head.end());
  g.groups.insert(g.groups.end(), body.begin(), body.end());
  g.groups.insert(g.groups.end(), foot.begin(), foot.end());
  for (size_t gi = 0; gi < g.groups.size(); ++gi) {
    LayoutBox* grp = g.groups[gi];
    for (size_t i = 0; i < grp->children.size(); ++i) {
      LayoutBox* row = grp->children[i].get();
      if (row->kind != LayoutBox::kTableRow) continue;
      row->coordParent = grp;
      g.rows.push_back(row);
      g.rowGroup.push_back((int)gi);
    }
  }
  std::vector<std::vector<char> > occ(g.rows.size());
  for (size_t r = 0; r < g.rows.size(); ++r) {
    LayoutBox* row = g.rows[r];
    int col = 0;
    for (size_t i = 0; i < row->children.size(); ++i) {
      LayoutBox* cell = row->children[i].get();
      if (cell->kind != LayoutBox::kTableCell) continue;
      cell->coordParent = row;
      while (col < (int)occ[r].size() && occ[r][col]) ++col;
      int cs = std::max(1, std::min(cell->colspan, 1000));
      int rs = std::max(1, std::min(cell->rowspan, (int)(g.rows.size() - r)));
      for (int rr = 0; rr < rs; ++rr) {
        std::vector<char>& o = occ[r + rr];
        if ((int)o.size() < col + cs) o.resize(col + cs, 0);
        for (int cc = 0; cc < cs; ++cc) o[col + cc] = 1;
      }
      CellInfo ci;
      ci.cell = cell;
      ci.row = row;
      ci.r = (int)r;
      ci.c = col;
      ci.colspan = cs;
      ci.rowspan = rs;
      g.cells.push_back(ci);
      col += cs;
      g.ncols = std::max(g.ncols, col);
    }
  }
}

}  // namespace

void TableLayout::Intrinsic(LayoutBox* table, float& minW, float& maxW) {
  TableGrid g;
  BuildGrid(table, g);
  const ComputedStyle* s = table->style;
  float spacing = s->borderCollapse ? 0 : s->borderSpacingH;
  std::vector<float> colMin(g.ncols, 0), colMax(g.ncols, 0), colPct(g.ncols, 0);
  std::vector<char> colFixed(g.ncols, 0);
  for (size_t i = 0; i < g.cells.size(); ++i) {
    const CellInfo& ci = g.cells[i];
    if (ci.colspan != 1) continue;
    float mn, mx;
    e_->ComputeIntrinsic(ci.cell, mn, mx);
    colMin[ci.c] = std::max(colMin[ci.c], mn);
    colMax[ci.c] = std::max(colMax[ci.c], mx);
  }
  for (size_t i = 0; i < g.cells.size(); ++i) {
    const CellInfo& ci = g.cells[i];
    if (ci.colspan == 1) continue;
    float mn, mx;
    e_->ComputeIntrinsic(ci.cell, mn, mx);
    float smin = 0, smax = 0;
    for (int c = ci.c; c < ci.c + ci.colspan; ++c) {
      smin += colMin[c];
      smax += colMax[c];
    }
    smin += spacing * (ci.colspan - 1);
    smax += spacing * (ci.colspan - 1);
    if (mn > smin)
      for (int c = ci.c; c < ci.c + ci.colspan; ++c) colMin[c] += (mn - smin) / ci.colspan;
    if (mx > smax)
      for (int c = ci.c; c < ci.c + ci.colspan; ++c) colMax[c] += (mx - smax) / ci.colspan;
  }
  e_->ResolveEdges(table, -1);
  float overhead = table->padding.left + table->padding.right + table->border.left +
                   table->border.right + spacing * (g.ncols + 1);
  float sumMin = 0, sumMax = 0;
  for (int c = 0; c < g.ncols; ++c) {
    colMax[c] = std::max(colMax[c], colMin[c]);
    sumMin += colMin[c];
    sumMax += colMax[c];
  }
  for (size_t i = 0; i < g.captions.size(); ++i) {
    float mn, mx;
    e_->ComputeIntrinsic(g.captions[i], mn, mx);
    sumMin = std::max(sumMin, mn - overhead);
  }
  minW = sumMin + overhead;
  maxW = std::max(minW, sumMax + overhead);
  if (s->width.IsFixed() && !s->width.HasPercent()) {
    float w = s->width.Resolve(0);
    minW = maxW = std::max(minW, w);
  }
  minW += table->margin.left + table->margin.right;
  maxW += table->margin.left + table->margin.right;
}

void TableLayout::Layout(LayoutBox* table, float cbW, float cbH, float forcedW) {
  TableGrid g;
  BuildGrid(table, g);
  const ComputedStyle* s = table->style;
  float spacingH = s->borderCollapse ? 0 : s->borderSpacingH;
  float spacingV = s->borderCollapse ? 0 : s->borderSpacingV;
  e_->ResolveEdges(table, cbW);
  int ncols = g.ncols;

  // Column constraints.
  std::vector<float> colMin(ncols, 0), colMax(ncols, 0), colPct(ncols, 0), colSpec(ncols, 0);
  std::vector<char> colFixed(ncols, 0);
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t i = 0; i < g.cells.size(); ++i) {
      const CellInfo& ci = g.cells[i];
      if ((pass == 0) != (ci.colspan == 1)) continue;
      LayoutBox* cell = ci.cell;
      e_->ResolveEdges(cell, -1);
      float mn, mx;
      cell->intrinsicValid = false;
      e_->ComputeIntrinsic(cell, mn, mx);
      const ComputedStyle* cs = cell->style;
      float pb = cell->padding.left + cell->padding.right + cell->border.left + cell->border.right;
      if (pass == 0) {
        int c = ci.c;
        if (cs->width.IsFixed() && cs->width.HasPercent() && cs->width.px == 0) {
          colPct[c] = std::max(colPct[c], cs->width.pct);
        } else if (cs->width.IsFixed() && !cs->width.HasPercent()) {
          float w = cs->width.Resolve(0) + (cs->boxSizing == kContentBox ? pb : 0);
          colFixed[c] = 1;
          colSpec[c] = std::max(colSpec[c], w);
        }
        colMin[c] = std::max(colMin[c], mn);
        colMax[c] = std::max(colMax[c], mx);
      } else {
        float smin = 0, smax = 0;
        for (int c = ci.c; c < ci.c + ci.colspan; ++c) {
          smin += colMin[c];
          smax += colMax[c];
        }
        smin += spacingH * (ci.colspan - 1);
        smax += spacingH * (ci.colspan - 1);
        if (mn > smin)
          for (int c = ci.c; c < ci.c + ci.colspan; ++c) colMin[c] += (mn - smin) / ci.colspan;
        if (mx > smax) {
          float base = smax - spacingH * (ci.colspan - 1);
          for (int c = ci.c; c < ci.c + ci.colspan; ++c) {
            float share = base > 0 ? colMax[c] / base : 1.0f / ci.colspan;
            colMax[c] += (mx - smax) * share;
          }
        }
      }
    }
  }
  for (int c = 0; c < ncols; ++c) {
    if (colFixed[c]) colMax[c] = std::max(colMin[c], colSpec[c]);
    colMax[c] = std::max(colMax[c], colMin[c]);
  }

  float pbH = table->padding.left + table->padding.right + table->border.left + table->border.right;
  float overhead = pbH + (ncols > 0 ? spacingH * (ncols + 1) : 0);
  float sumMin = 0, sumMax = 0;
  for (int c = 0; c < ncols; ++c) {
    sumMin += colMin[c];
    sumMax += colMax[c];
  }
  float captionMin = 0;
  for (size_t i = 0; i < g.captions.size(); ++i) {
    float mn, mx;
    e_->ComputeIntrinsic(g.captions[i], mn, mx);
    captionMin = std::max(captionMin, mn);
  }

  float W;
  bool autoWidth = false;
  if (forcedW >= 0) {
    W = forcedW;
  } else if (s->width.IsFixed() && !(s->width.HasPercent() && cbW < 0)) {
    W = s->width.Resolve(cbW);
    if (s->boxSizing == kContentBox) W += pbH;
  } else {
    float avail = (cbW >= 0 ? cbW : 1e6f) - table->margin.left - table->margin.right;
    W = std::min(avail, sumMax + overhead);
    autoWidth = true;
  }
  W = std::max(W, sumMin + overhead);
  W = std::max(W, captionMin);
  W = e_->ClampWidth(table, W, cbW);
  W = std::max(W, sumMin + overhead);

  // Distribute column widths.
  float avail = W - overhead;
  std::vector<float> colW(ncols, 0);
  if (ncols > 0) {
    // Percentage columns first.
    float used = 0;
    std::vector<char> done(ncols, 0);
    for (int c = 0; c < ncols; ++c) {
      if (colPct[c] > 0) {
        colW[c] = std::max(colMin[c], colPct[c] * avail / 100);
        done[c] = 1;
        used += colW[c];
      }
    }
    float rest = avail - used;
    float rMin = 0, rMax = 0;
    int nAuto = 0;
    float autoMax = 0;
    for (int c = 0; c < ncols; ++c) {
      if (done[c]) continue;
      rMin += colMin[c];
      rMax += colMax[c];
      if (!colFixed[c]) {
        ++nAuto;
        autoMax += colMax[c];
      }
    }
    for (int c = 0; c < ncols; ++c) {
      if (done[c]) continue;
      if (rest >= rMax) {
        float extra = rest - rMax;
        float share;
        if (nAuto > 0) share = colFixed[c] ? 0 : (autoMax > 0 ? colMax[c] / autoMax : 1.0f / nAuto);
        else share = rMax > 0 ? colMax[c] / rMax : 1.0f / ncols;
        colW[c] = colMax[c] + extra * share;
      } else if (rest >= rMin && rMax > rMin) {
        colW[c] = colMin[c] + (colMax[c] - colMin[c]) * (rest - rMin) / (rMax - rMin);
      } else {
        colW[c] = colMin[c];
      }
    }
  }
  (void)autoWidth;

  table->w = W;
  float cx = table->border.left + table->padding.left;
  float innerW = W - pbH;
  float y = table->border.top + table->padding.top;

  // Captions.
  for (size_t i = 0; i < g.captions.size(); ++i) {
    LayoutBox* cap = g.captions[i];
    cap->x = cx;
    cap->y = y;
    e_->LayoutBlockLevel(cap, innerW, -1, 0, 0, 0, innerW, -1);
    y += cap->h;
  }

  // Cell layout and row heights.
  size_t nrows = g.rows.size();
  std::vector<float> rowH(nrows, 0);
  for (size_t r = 0; r < nrows; ++r) {
    float spec = e_->ResolveSpecifiedHeight(g.rows[r], -1);
    if (spec > 0) rowH[r] = spec;
  }
  for (size_t i = 0; i < g.cells.size(); ++i) {
    CellInfo& ci = g.cells[i];
    float w = spacingH * (ci.colspan - 1);
    for (int c = ci.c; c < ci.c + ci.colspan; ++c) w += colW[c];
    ci.cell->x = 0;
    ci.cell->y = 0;
    e_->LayoutBlockLevel(ci.cell, w, -1, 0, 0, 0, w, -1);
    if (ci.rowspan == 1) rowH[ci.r] = std::max(rowH[ci.r], ci.cell->h);
  }
  for (size_t i = 0; i < g.cells.size(); ++i) {
    CellInfo& ci = g.cells[i];
    if (ci.rowspan == 1) continue;
    float have = spacingV * (ci.rowspan - 1);
    for (int r = ci.r; r < ci.r + ci.rowspan; ++r) have += rowH[r];
    if (ci.cell->h > have) rowH[ci.r + ci.rowspan - 1] += ci.cell->h - have;
  }
  // Specified table height: grow rows proportionally.
  float specTableH = e_->ResolveSpecifiedHeight(table, cbH);
  if (specTableH > 0 && nrows > 0) {
    float need = table->padding.top + table->padding.bottom + table->border.top +
                 table->border.bottom + spacingV * (nrows + 1) + (y - table->border.top - table->padding.top);
    float sum = 0;
    for (size_t r = 0; r < nrows; ++r) sum += rowH[r];
    float extra = specTableH - need - sum;
    if (extra > 0)
      for (size_t r = 0; r < nrows; ++r) rowH[r] += sum > 0 ? extra * rowH[r] / sum : extra / nrows;
  }

  // Position groups, rows and cells.
  std::vector<float> rowY(nrows, 0);  // relative to the row's group
  y += spacingV;
  int curGroup = -1;
  LayoutBox* group = 0;
  float groupStart = y;
  for (size_t r = 0; r < nrows; ++r) {
    if (g.rowGroup[r] != curGroup) {
      if (group) group->h = y - spacingV - groupStart;
      curGroup = g.rowGroup[r];
      group = g.groups[curGroup];
      group->x = cx;
      group->y = y;
      group->w = innerW;
      groupStart = y;
    }
    LayoutBox* row = g.rows[r];
    row->x = 0;
    row->y = y - groupStart;
    row->w = innerW;
    row->h = rowH[r];
    y += rowH[r] + spacingV;
  }
  if (group) group->h = y - spacingV - groupStart;
  // Empty groups.
  for (size_t gi = 0; gi < g.groups.size(); ++gi) {
    bool hasRow = false;
    for (size_t r = 0; r < nrows; ++r)
      if (g.rowGroup[r] == (int)gi) hasRow = true;
    if (!hasRow) {
      g.groups[gi]->x = cx;
      g.groups[gi]->y = y;
      g.groups[gi]->w = innerW;
      g.groups[gi]->h = 0;
    }
  }
  std::vector<float> colX(ncols + 1, 0);
  float xacc = spacingH;
  for (int c = 0; c < ncols; ++c) {
    colX[c] = xacc;
    xacc += colW[c] + spacingH;
  }
  colX[ncols] = xacc;
  for (size_t i = 0; i < g.cells.size(); ++i) {
    CellInfo& ci = g.cells[i];
    LayoutBox* cell = ci.cell;
    float h = spacingV * (ci.rowspan - 1);
    for (int r = ci.r; r < ci.r + ci.rowspan; ++r) h += rowH[r];
    float contentH = cell->h;
    cell->x = colX[ci.c];
    cell->y = 0;
    cell->h = h;
    VerticalAlign va = cell->style->verticalAlign;
    float dy = 0;
    if (va == kVaMiddle) dy = (h - contentH) / 2;
    else if (va == kVaBottom) dy = h - contentH;
    if (dy > 0) e_->ShiftContent(cell, dy);
  }
  if (!g.rows.empty() && table->firstBaseline < 0) {
    for (size_t i = 0; i < g.cells.size(); ++i) {
      if (g.cells[i].r == 0 && g.cells[i].cell->firstBaseline >= 0) {
        LayoutBox* grp0 = g.groups[g.rowGroup[0]];
        table->firstBaseline = grp0->y + g.rows[0]->y + g.cells[i].cell->firstBaseline;
        break;
      }
    }
  }
  table->lastBaseline = table->firstBaseline;
  float h = y + table->padding.bottom + table->border.bottom;
  if (nrows == 0) h = y - spacingV + table->padding.bottom + table->border.bottom + (ncols ? spacingV : 0);
  if (specTableH > h) h = specTableH;
  table->h = h;

  // Centering via auto margins.
  if (cbW >= 0 && !table->IsFloat()) {
    bool ml = s->margin[3].IsAuto(), mr = s->margin[1].IsAuto();
    float free = cbW - W - (ml ? 0 : table->margin.left) - (mr ? 0 : table->margin.right);
    if (ml && mr) table->margin.left = table->margin.right = std::max(0.0f, free / 2);
    else if (ml) table->margin.left = std::max(0.0f, free);
  }
}

}  // namespace kite
