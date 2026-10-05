// Kite Engine - display list produced by the painter and consumed by the
// platform rasterizer.
#ifndef KITE_PAINT_DISPLAY_LIST_H
#define KITE_PAINT_DISPLAY_LIST_H

#include <string>
#include <vector>

#include "base/geometry.h"
#include "dom/node.h"
#include "layout/resources.h"

namespace kite {

struct DisplayItem {
  enum Type { kRect, kText, kImage, kPushClip, kPopClip, kEllipse, kSvg, kRoundRect, kShadow,
              kBeginFixed, kEndFixed };
  Type type;
  Rect rect;        // rect / ellipse bounds / image destination area / clip
  Color color;
  // Text.
  std::string text;
  FontDesc font;
  float baseline;
  bool underline, lineThrough, overline;
  Color decorationColor;
  // Image (tiled into |rect| starting at tileX/tileY with tile size).
  std::string imageUrl;
  float tileX, tileY, tileW, tileH;
  bool repeatX, repeatY;
  float alpha;
  // Ellipse.
  bool hollow;
  // Inline <svg> element (rasterized by the platform at device size).
  const Node* svgNode;
  // Rounded rectangles / shadows / rounded images: corner radii (tl, tr,
  // br, bl), ring width for borders (0 = filled), shadow blur radius.
  float radii[4];
  float ring;
  float blur;
  bool HasRadii() const { return radii[0] > 0 || radii[1] > 0 || radii[2] > 0 || radii[3] > 0; }

  DisplayItem()
      : type(kRect), baseline(0), underline(false), lineThrough(false), overline(false),
        tileX(0), tileY(0), tileW(0), tileH(0), repeatX(false), repeatY(false), alpha(1),
        hollow(false), svgNode(0), ring(0), blur(0) {
    radii[0] = radii[1] = radii[2] = radii[3] = 0;
  }
};

struct HitRegion {
  Rect rect;
  Node* node;
  bool fixed;  // position: fixed (viewport coordinates)
};

struct DisplayList {
  std::vector<DisplayItem> items;
  std::vector<HitRegion> hits;
  Color background;
  float width, height;
  DisplayList() : background(255, 255, 255), width(0), height(0) {}
  void Clear() {
    items.clear();
    hits.clear();
    width = height = 0;
  }
};

class LayoutBox;
class LayoutEngine;

class Painter {
 public:
  Painter(LayoutEngine* engine, ImageProvider* images) : engine_(engine), images_(images) {}
  void Paint(LayoutBox* root, float viewportW, float viewportH, DisplayList& out);

 private:
  void PaintBox(LayoutBox* b, float px, float py, float alpha);
  void PaintBoxDecorations(LayoutBox* b, const Rect& r, float alpha, bool skipLeft,
                           bool skipRight);
  void PaintBackground(const ComputedStyle* s, const Rect& borderBox, const Rect& paddingBox,
                       float alpha);
  void PaintBorders(const ComputedStyle* s, const Rect& r, const float widths[4], float alpha,
                    bool skipLeft, bool skipRight);
  void PaintLines(LayoutBox* b, float ax, float ay, float alpha);
  void PaintFloatsInInline(LayoutBox* container, LayoutBox* b, float ax, float ay, float alpha);
  void PaintReplaced(LayoutBox* b, float ax, float ay, float alpha);
  void PaintMarker(LayoutBox* b, float ax, float ay, float alpha);
  void PaintPositioned(LayoutBox* b, float ax, float ay, float alpha, bool negative);
  void FillRect(const Rect& r, Color c, float alpha);
  void Text(float x, float baseline, const std::string& text, const ComputedStyle* s,
            Color color, float alpha, bool decorations);
  void Extend(const Rect& r);

  LayoutEngine* engine_;
  ImageProvider* images_;
  DisplayList* out_;
  LayoutBox* skipBackgroundOf_;
  std::vector<Rect> clipStack_;
  int fixedDepth_;
  void AddHit(const Rect& r, Node* n) {
    HitRegion h;
    h.rect = r;
    h.node = n;
    h.fixed = fixedDepth_ > 0;
    out_->hits.push_back(h);
  }
  void ResolveRadii(LayoutBox* b, const Rect& r, float out[4]);
};

}  // namespace kite

#endif
