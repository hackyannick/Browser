// Kite Engine - layout algorithms (block, inline, float, table, flex, grid,
// absolute positioning)
#ifndef KITE_LAYOUT_LAYOUT_H
#define KITE_LAYOUT_LAYOUT_H

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "layout/box.h"
#include "layout/resources.h"

namespace kite {

// Float placement inside one block formatting context. Coordinates are
// relative to the border box of the box that established the context.
class FloatContext {
 public:
  struct Placed {
    float x, y, w, h;  // margin box
    bool left;
  };
  // Available horizontal range [l, r) for a band [y, y + h).
  void Available(float y, float h, float& l, float& r) const;
  // Places a float and returns its margin-box position.
  void Place(float w, float h, bool left, float minY, float containerL, float containerR,
             float& outX, float& outY);
  float ClearY(Clear side) const;
  float MaxBottom() const;
  float NextBottomBelow(float y) const;
  bool Empty() const { return floats_.empty(); }

 private:
  std::vector<Placed> floats_;
  float lastTop_ = 0;
};

class LayoutEngine {
 public:
  LayoutEngine(FontProvider* fonts, ImageProvider* images);

  void Layout(LayoutBox* root, float viewportW, float viewportH);

  // Text measurement with caching (also used by the painter).
  float Measure(const ComputedStyle* style, const std::string& text);
  FontMetrics Metrics(const ComputedStyle* style);
  FontProvider* fonts() { return fonts_; }

 private:
  friend class TableLayout;
  friend class FlexLayout;

  // Lays out a block-level box whose x/y were set by the caller.
  // |forcedW|/|forcedH| (border box) override the computed size if >= 0.
  void LayoutBlockLevel(LayoutBox* b, float cbW, float cbH, FloatContext* fc,
                        float bfcX, float bfcY, float forcedW = -1, float forcedH = -1);
  void LayoutContents(LayoutBox* b, float cbH, FloatContext* fc, float bfcX, float bfcY,
                      float definiteContentH);
  float LayoutBlockChildren(LayoutBox* b, FloatContext* fc, float bfcX, float bfcY,
                            float definiteContentH);
  float LayoutInlineContent(LayoutBox* b, FloatContext* fc, float bfcX, float bfcY);
  void LayoutFloat(LayoutBox* f, LayoutBox* container, FloatContext* fc, float bfcX,
                   float bfcY, float localY);
  void LayoutReplaced(LayoutBox* b, float cbW, float cbH, float forcedW, float forcedH);
  void LayoutAtomicInline(LayoutBox* b, float cbW);
  void LayoutPositionedChildren(LayoutBox* cb);
  void LayoutPositioned(LayoutBox* cb, LayoutBox* child);
  void RegisterPositioned(LayoutBox* child, float staticX, float staticY);

  // Sizing helpers.
  void ResolveEdges(LayoutBox* b, float cbW);
  float SolveWidth(LayoutBox* b, float cbW, float available, bool shrinkToFit);
  float ClampWidth(LayoutBox* b, float borderBoxW, float cbW);
  float ClampHeight(LayoutBox* b, float borderBoxH, float cbH);
  float ResolveSpecifiedHeight(LayoutBox* b, float cbH);  // border box or -1
  float CollapsedTopMargin(LayoutBox* b, float cbW);
  void ComputeIntrinsic(LayoutBox* b, float& minW, float& maxW);
  void InlineIntrinsic(LayoutBox* b, float& minW, float& maxW);
  float ShrinkToFit(LayoutBox* b, float available);
  void ReplacedSize(LayoutBox* b, float cbW, float cbH, float& w, float& h);
  void ApplyRelativeOffset(LayoutBox* b, float cbW, float cbH);
  void ShiftContent(LayoutBox* b, float dy);

  FontProvider* fonts_;
  ImageProvider* images_;
  float viewportW_, viewportH_;
  LayoutBox* root_;

  std::map<FontDesc, int> fontIds_;
  std::unordered_map<std::string, float> measureCache_;
  std::map<FontDesc, FontMetrics> metricsCache_;
};

// Implemented in table.cpp / flex.cpp.
class TableLayout {
 public:
  explicit TableLayout(LayoutEngine* e) : e_(e) {}
  void Layout(LayoutBox* table, float cbW, float cbH, float forcedW);
  void Intrinsic(LayoutBox* table, float& minW, float& maxW);

 private:
  LayoutEngine* e_;
};

class FlexLayout {
 public:
  explicit FlexLayout(LayoutEngine* e) : e_(e) {}
  // Lays out children; returns the content height.
  float LayoutFlex(LayoutBox* box, float definiteContentH);
  float LayoutGrid(LayoutBox* box, float definiteContentH);
  void FlexIntrinsic(LayoutBox* box, float& minW, float& maxW);
  void GridIntrinsic(LayoutBox* box, float& minW, float& maxW);

 private:
  LayoutEngine* e_;
};

}  // namespace kite

#endif
