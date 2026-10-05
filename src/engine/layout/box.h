// Kite Engine - layout (box) tree
#ifndef KITE_LAYOUT_BOX_H
#define KITE_LAYOUT_BOX_H

#include <memory>
#include <string>
#include <vector>

#include "base/geometry.h"
#include "css/style.h"
#include "dom/node.h"

namespace kite {

class LayoutBox;

struct Edges {
  float top, right, bottom, left;
  Edges() : top(0), right(0), bottom(0), left(0) {}
};

// A piece of a line box. Coordinates are relative to the border box of the
// block container that owns the line.
struct LineFragment {
  enum Kind { kText, kAtomic, kInlineBox, kMarker };
  Kind kind;
  LayoutBox* box;               // inline box / atomic box / text owner
  const ComputedStyle* style;
  Node* node;                   // DOM element for hit testing
  std::string text;
  float x, y, w, h;
  float baseline;               // absolute y of the baseline (text)
  bool openLeft, openRight;     // inline box continues on prev/next line
  LineFragment()
      : kind(kText), box(0), style(0), node(0), x(0), y(0), w(0), h(0), baseline(0),
        openLeft(false), openRight(false) {}
};

struct LineBox {
  float y, h, baseline;
  std::vector<LineFragment> fragments;  // inline box backgrounds first
  LineBox() : y(0), h(0), baseline(0) {}
};

class LayoutBox {
 public:
  enum Kind {
    kBlockFlow,     // block container (also inline-block, table cell, items)
    kInline,        // non-atomic inline box
    kText,
    kReplaced,      // img, form controls, svg, iframe, video ...
    kLineBreak,
    kTable,
    kTableRowGroup,
    kTableRow,
    kTableCell,
    kTableCaption,
    kFlex,
    kGrid,
  };

  LayoutBox(Kind k, Node* n, const ComputedStyle* s);

  Kind kind;
  Node* node;
  const ComputedStyle* style;
  std::unique_ptr<ComputedStyle> ownStyle;
  LayoutBox* parent;
  LayoutBox* coordParent;    // box that x/y are relative to
  std::vector<std::unique_ptr<LayoutBox> > children;

  std::string text;          // kText
  bool inlineContent;        // block container with inline-level children

  // Geometry of the border box, relative to the parent box's border box
  // (or to the containing block for absolutely positioned boxes).
  float x, y, w, h;
  float relX, relY;          // position:relative offsets (applied at paint)
  Edges margin, border, padding;
  std::vector<LineBox> lines;
  float firstBaseline;       // from border-box top; < 0 if none
  float lastBaseline;

  // Effective collapsed margins used during block layout.
  float collapsedMarginBottom;

  // Replaced element data.
  std::string imageUrl;
  float intrinsicW, intrinsicH;
  bool hasIntrinsic;

  // List items.
  std::string markerText;

  // Tables.
  int colspan, rowspan;

  // Absolutely positioned descendants (when this box is their containing
  // block) and the static position of this box (relative to its parent).
  std::vector<LayoutBox*> positioned;
  float staticX, staticY;
  bool isPositionedChild;     // laid out/painted through a containing block

  // Cached intrinsic widths (border box + margins).
  bool intrinsicValid;
  float minContent, maxContent;

  // Helpers.
  bool IsBlockContainer() const {
    return kind == kBlockFlow || kind == kTableCell || kind == kTableCaption;
  }
  bool IsAtomicInline() const;
  bool IsFloat() const { return style->IsFloating() && kind != kText && kind != kInline; }
  bool IsOutOfFlow() const { return style->IsOutOfFlow() && kind != kText; }
  bool EstablishesBfc() const;
  float ContentX() const { return border.left + padding.left; }
  float ContentY() const { return border.top + padding.top; }
  float ContentW() const { return w - border.left - border.right - padding.left - padding.right; }
  float ContentH() const { return h - border.top - border.bottom - padding.top - padding.bottom; }
  // Absolute position of the border box in document coordinates.
  void AbsolutePosition(float& ax, float& ay) const;
  LayoutBox* ContainingBlockForPositioned();

 private:
  LayoutBox(const LayoutBox&);
  LayoutBox& operator=(const LayoutBox&);
};

class FontProvider;
class ImageProvider;

// Builds the box tree for a styled document.
std::unique_ptr<LayoutBox> BuildLayoutTree(Document& doc, const std::string& baseUrl);

// Text used for list markers ("1.", "iv." ...); empty for bullet shapes.
std::string ListMarkerText(ListStyle type, int index);

}  // namespace kite

#endif
