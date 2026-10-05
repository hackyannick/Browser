// Kite Engine - CSS property table, shorthand expansion and value computation
#ifndef KITE_CSS_PROPERTIES_H
#define KITE_CSS_PROPERTIES_H

#include <string>
#include <utility>
#include <vector>

#include "css/style.h"

namespace kite {

#define KITE_PROPERTIES(X)                                 \
  X(FontSize, "font-size", true)                           \
  X(Color, "color", true)                                  \
  X(FontFamily, "font-family", true)                       \
  X(FontWeight, "font-weight", true)                       \
  X(FontStyle, "font-style", true)                         \
  X(LineHeight, "line-height", true)                       \
  X(TextAlign, "text-align", true)                         \
  X(WhiteSpace, "white-space", true)                       \
  X(TextTransform, "text-transform", true)                 \
  X(TextIndent, "text-indent", true)                       \
  X(LetterSpacing, "letter-spacing", true)                 \
  X(WordSpacing, "word-spacing", true)                     \
  X(Visibility, "visibility", true)                        \
  X(ListStyleType, "list-style-type", true)                \
  X(ListStylePosition, "list-style-position", true)        \
  X(Cursor, "cursor", true)                                \
  X(BorderCollapse, "border-collapse", true)               \
  X(BorderSpacing, "border-spacing", true)                 \
  X(OverflowWrap, "overflow-wrap", true)                   \
  X(WordBreak, "word-break", true)                         \
  X(Fill, "fill", true)                                    \
  X(Stroke, "stroke", true)                                \
  X(TextDecorationLine, "text-decoration-line", false)     \
  X(TextDecorationColor, "text-decoration-color", false)   \
  X(Display, "display", false)                             \
  X(Position, "position", false)                           \
  X(Float, "float", false)                                 \
  X(Clear, "clear", false)                                 \
  X(Top, "top", false)                                     \
  X(Right, "right", false)                                 \
  X(Bottom, "bottom", false)                               \
  X(Left, "left", false)                                   \
  X(ZIndex, "z-index", false)                              \
  X(Width, "width", false)                                 \
  X(Height, "height", false)                               \
  X(MinWidth, "min-width", false)                          \
  X(MinHeight, "min-height", false)                        \
  X(MaxWidth, "max-width", false)                          \
  X(MaxHeight, "max-height", false)                        \
  X(MarginTop, "margin-top", false)                        \
  X(MarginRight, "margin-right", false)                    \
  X(MarginBottom, "margin-bottom", false)                  \
  X(MarginLeft, "margin-left", false)                      \
  X(PaddingTop, "padding-top", false)                      \
  X(PaddingRight, "padding-right", false)                  \
  X(PaddingBottom, "padding-bottom", false)                \
  X(PaddingLeft, "padding-left", false)                    \
  X(BorderTopWidth, "border-top-width", false)             \
  X(BorderRightWidth, "border-right-width", false)         \
  X(BorderBottomWidth, "border-bottom-width", false)       \
  X(BorderLeftWidth, "border-left-width", false)           \
  X(BorderTopStyle, "border-top-style", false)             \
  X(BorderRightStyle, "border-right-style", false)         \
  X(BorderBottomStyle, "border-bottom-style", false)       \
  X(BorderLeftStyle, "border-left-style", false)           \
  X(BorderTopColor, "border-top-color", false)             \
  X(BorderRightColor, "border-right-color", false)         \
  X(BorderBottomColor, "border-bottom-color", false)       \
  X(BorderLeftColor, "border-left-color", false)           \
  X(BoxSizing, "box-sizing", false)                        \
  X(OverflowX, "overflow-x", false)                        \
  X(OverflowY, "overflow-y", false)                        \
  X(VerticalAlign, "vertical-align", false)                \
  X(BackgroundColor, "background-color", false)            \
  X(BackgroundImage, "background-image", false)            \
  X(BackgroundRepeat, "background-repeat", false)          \
  X(BackgroundPositionX, "background-position-x", false)   \
  X(BackgroundPositionY, "background-position-y", false)   \
  X(BackgroundSize, "background-size", false)              \
  X(Opacity, "opacity", false)                             \
  X(AspectRatio, "aspect-ratio", false)                    \
  X(ObjectFit, "object-fit", false)                        \
  X(FlexDirection, "flex-direction", false)                \
  X(FlexWrap, "flex-wrap", false)                          \
  X(JustifyContent, "justify-content", false)              \
  X(AlignItems, "align-items", false)                      \
  X(AlignSelf, "align-self", false)                        \
  X(AlignContent, "align-content", false)                  \
  X(FlexGrow, "flex-grow", false)                          \
  X(FlexShrink, "flex-shrink", false)                      \
  X(FlexBasis, "flex-basis", false)                        \
  X(Order, "order", false)                                 \
  X(RowGap, "row-gap", false)                              \
  X(ColumnGap, "column-gap", false)                        \
  X(GridTemplateColumns, "grid-template-columns", false)   \
  X(GridColumnStart, "grid-column-start", false)           \
  X(GridColumnEnd, "grid-column-end", false)               \
  X(Content, "content", false)                             \
  X(Filter, "filter", false)                               \
  X(Clip, "clip", false)                                   \
  X(ClipPath, "clip-path", false)                          \
  X(BackgroundClip, "background-clip", false)              \
  X(BorderTopLeftRadius, "border-top-left-radius", false)  \
  X(BorderTopRightRadius, "border-top-right-radius", false) \
  X(BorderBottomRightRadius, "border-bottom-right-radius", false) \
  X(BorderBottomLeftRadius, "border-bottom-left-radius", false) \
  X(BoxShadow, "box-shadow", false)                        \
  X(Transform, "transform", false)                         \
  X(Translate, "translate", false)                         \
  X(AnimationName, "animation-name", false)               \
  X(AnimationDuration, "animation-duration", false)       \
  X(AnimationDelay, "animation-delay", false)             \
  X(AnimationIterationCount, "animation-iteration-count", false) \
  X(AnimationDirection, "animation-direction", false)     \
  X(AnimationFillMode, "animation-fill-mode", false)      \
  X(AnimationTimingFunction, "animation-timing-function", false) \
  X(AnimationPlayState, "animation-play-state", false)    \
  X(TransitionProperty, "transition-property", false)     \
  X(TransitionDuration, "transition-duration", false)     \
  X(TransitionDelay, "transition-delay", false)           \
  X(TransitionTimingFunction, "transition-timing-function", false)

enum PropertyId {
#define KITE_PROP_ENUM(id, name, inh) kProp##id,
  KITE_PROPERTIES(KITE_PROP_ENUM)
#undef KITE_PROP_ENUM
  kPropCount
};

int LookupProperty(const std::string& name);
const char* PropertyName(int id);
bool IsInheritedProperty(int id);

// Expands |name: value| into longhand (id, value) pairs. Longhands map to
// themselves. Returns false for unknown properties.
bool ExpandProperty(const std::string& name, const std::string& value,
                    std::vector<std::pair<int, std::string> >& out);

struct ApplyContext {
  const ComputedStyle* parent;
  float rootFontSize;
  float viewportW, viewportH;
  const std::string* baseUrl;  // for url() resolution
};

// Computes |value| for property |id| into |style|. CSS-wide keywords
// (inherit/initial/unset) are handled here too.
void ApplyProperty(int id, const std::string& value, ComputedStyle& style,
                   const ApplyContext& ctx);

// Copies the computed value of property |id| from |src| to |dst|.
void CopyProperty(int id, ComputedStyle& dst, const ComputedStyle& src);

// Splits a value on top-level whitespace (keeping functions intact).
std::vector<std::string> SplitValueTokens(const std::string& value);
std::vector<std::string> SplitValueCommas(const std::string& value);

}  // namespace kite

#endif
