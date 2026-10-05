// Kite Engine - minimal SVG renderer (shapes, paths, transforms, groups,
// <use>, solid fills/strokes; gradients approximated by their average color)
#ifndef KITE_IMAGE_SVG_H
#define KITE_IMAGE_SVG_H

#include <string>

#include "base/geometry.h"
#include "dom/node.h"
#include "image/image.h"

namespace kite {

// Intrinsic size from width/height attributes or the viewBox (CSS px).
bool SvgIntrinsicSize(const Node* svg, float& w, float& h);
// Rasterizes the <svg> element |svg| into a w x h image.
bool RenderSvg(const Node* svg, int w, int h, Color currentColor, DecodedImage& out);
// Parses and rasterizes a standalone SVG document. If w/h are <= 0 the
// intrinsic size is used (scaled by |scale|).
bool RenderSvgDocument(const std::string& text, int w, int h, float scale, DecodedImage& out);
bool LooksLikeSvg(const std::string& data);

}  // namespace kite

#endif
