// Kite Engine - computed style
#ifndef KITE_CSS_STYLE_H
#define KITE_CSS_STYLE_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/geometry.h"
#include "css/stylesheet.h"

namespace kite {

// A computed length: px + pct% of a reference length, or a keyword.
struct Length {
  enum Kind { kAuto, kFixed, kNone, kMinContent, kMaxContent, kFitContent };
  Kind kind;
  float px;
  float pct;
  Length() : kind(kAuto), px(0), pct(0) {}
  static Length Px(float v) { Length l; l.kind = kFixed; l.px = v; return l; }
  static Length Pct(float v) { Length l; l.kind = kFixed; l.pct = v; return l; }
  static Length Auto() { return Length(); }
  static Length None() { Length l; l.kind = kNone; return l; }
  bool IsAuto() const { return kind == kAuto; }
  bool IsFixed() const { return kind == kFixed; }
  bool HasPercent() const { return kind == kFixed && pct != 0; }
  bool IsZero() const { return kind == kFixed && px == 0 && pct == 0; }
  // Resolves against |base|; if base is unknown (< 0) percentages count as 0.
  float Resolve(float base) const {
    if (kind != kFixed) return 0;
    return px + (base >= 0 ? pct * base / 100.0f : 0);
  }
};

enum Display {
  kDisplayNone,
  kDisplayInline,
  kDisplayBlock,
  kDisplayInlineBlock,
  kDisplayListItem,
  kDisplayTable,
  kDisplayInlineTable,
  kDisplayTableRowGroup,
  kDisplayTableHeaderGroup,
  kDisplayTableFooterGroup,
  kDisplayTableRow,
  kDisplayTableCell,
  kDisplayTableColumn,
  kDisplayTableColumnGroup,
  kDisplayTableCaption,
  kDisplayFlex,
  kDisplayInlineFlex,
  kDisplayGrid,
  kDisplayInlineGrid,
  kDisplayContents,
};

enum Position { kPosStatic, kPosRelative, kPosAbsolute, kPosFixed, kPosSticky };
enum Float { kFloatNone, kFloatLeft, kFloatRight };
enum Clear { kClearNone, kClearLeft, kClearRight, kClearBoth };
enum TextAlign { kAlignLeft, kAlignRight, kAlignCenter, kAlignJustify };
enum WhiteSpace { kWsNormal, kWsPre, kWsNowrap, kWsPreWrap, kWsPreLine };
enum Overflow { kOverflowVisible, kOverflowHidden, kOverflowScroll, kOverflowAuto };
enum BorderStyle { kBorderNone, kBorderSolid, kBorderDashed, kBorderDotted,
                   kBorderDouble, kBorderGroove, kBorderRidge, kBorderInset,
                   kBorderOutset, kBorderHidden };
enum VerticalAlign { kVaBaseline, kVaMiddle, kVaTop, kVaBottom, kVaSub, kVaSuper,
                     kVaTextTop, kVaTextBottom, kVaLength };
enum TextTransform { kTtNone, kTtUppercase, kTtLowercase, kTtCapitalize };
enum ListStyle { kListNone, kListDisc, kListCircle, kListSquare, kListDecimal,
                 kListLowerAlpha, kListUpperAlpha, kListLowerRoman, kListUpperRoman,
                 kListDecimalLeadingZero };
enum FlexDirection { kFlexRow, kFlexRowReverse, kFlexColumn, kFlexColumnReverse };
enum FlexAlign { kFlexStart, kFlexEnd, kFlexCenter, kFlexStretch, kFlexBaseline,
                 kFlexSpaceBetween, kFlexSpaceAround, kFlexSpaceEvenly, kFlexAuto,
                 kFlexNormal };
enum BoxSizing { kContentBox, kBorderBox };
enum BgRepeat { kRepeat, kRepeatX, kRepeatY, kNoRepeat };
enum ObjectFit { kFitFill, kFitContain, kFitCover, kFitNone, kFitScaleDown };
enum Cursor { kCursorAuto, kCursorPointer, kCursorText, kCursorDefault };

struct BorderSide {
  float width;
  BorderStyle style;
  Color color;
  bool colorIsCurrent;
  BorderSide() : width(3), style(kBorderNone), colorIsCurrent(true) {}
  float Used() const {
    return (style == kBorderNone || style == kBorderHidden) ? 0 : width;
  }
};

struct GridTrack {
  enum Kind { kFixedTrack, kFr, kAutoTrack, kMinMax };
  Kind kind;
  Length size;   // kFixedTrack / minmax min
  float fr;      // kFr / minmax max as fr
  GridTrack() : kind(kAutoTrack), fr(0) {}
};

struct BoxShadow {
  float x, y, blur, spread;
  Color color;
  bool inset;
  BoxShadow() : x(0), y(0), blur(0), spread(0), inset(false) {}
};

struct ComputedStyle {
  ComputedStyle();

  // Inherited properties.
  Color color;
  std::string fontFamily;  // comma separated, lower-case, quotes removed
  float fontSize;          // px
  int fontWeight;          // 100..900
  bool italic;
  bool underline, lineThrough, overline;  // propagated text decorations
  Color decorationColor;
  bool decorationColorSet;
  float lineHeight;        // px; < 0 means 'normal'
  float lineHeightFactor;  // > 0 when specified as a number (inherited as such)
  TextAlign textAlign;
  WhiteSpace whiteSpace;
  TextTransform textTransform;
  Length textIndent;
  float letterSpacing;
  float wordSpacing;
  bool overflowWrap;  // overflow-wrap: break-word | anywhere
  bool breakAll;      // word-break: break-all | break-word
  std::string fill, stroke;          // SVG paint (raw CSS values)
  bool fillDeclared, strokeDeclared;  // set on this element (not inherited)
  bool visible;
  ListStyle listStyleType;
  bool listStyleInside;
  Cursor cursor;
  bool borderCollapse;
  float borderSpacingH, borderSpacingV;
  // Custom properties (--foo), shared copy-on-write between elements.
  std::shared_ptr<const std::map<std::string, std::string> > customProperties;

  // Non-inherited properties.
  Display display;
  Position position;
  Float floating;
  Clear clear;
  Length width, height, minWidth, minHeight, maxWidth, maxHeight;
  Length margin[4];   // top, right, bottom, left
  Length padding[4];
  BorderSide border[4];
  Length inset[4];    // top, right, bottom, left
  int zIndex;
  bool zIndexAuto;
  BoxSizing boxSizing;
  Overflow overflowX, overflowY;
  VerticalAlign verticalAlign;
  float verticalAlignPx;
  Color backgroundColor;
  std::string backgroundImage;  // absolute URL (resolved by resolver)
  BgRepeat backgroundRepeat;
  Length backgroundPosX, backgroundPosY;
  int backgroundSizeMode;  // 0 auto, 1 cover, 2 contain, 3 explicit
  Length backgroundSizeW, backgroundSizeH;
  float opacity;
  float blur;
  bool clippedAway;
  bool backgroundClipText;
  Length radius[4];  // top-left, top-right, bottom-right, bottom-left
  std::vector<BoxShadow> shadows;
  Length translateX, translateY;  // transform: translate(...)
  bool transformHidden;           // scale(0)
  bool hasAnimation;  // clip: rect(0 0 0 0) / clip-path: inset(50%)  // filter: blur() radius in px (approximated)
  float aspectRatio;  // 0 = none
  ObjectFit objectFit;

  // Flexbox / grid.
  FlexDirection flexDirection;
  bool flexWrap;
  FlexAlign justifyContent;
  FlexAlign alignItems;
  FlexAlign alignSelf;
  FlexAlign alignContent;
  float flexGrow, flexShrink;
  Length flexBasis;  // auto = use width
  int order;
  Length rowGap, columnGap;
  std::vector<GridTrack> gridColumns;
  int gridAutoRepeat;   // >0 when repeat(auto-fill/auto-fit, ...) was used
  Length gridAutoMin;   // minimum track size for auto-fill
  int gridColumnSpan;   // grid-column: span N
  int gridColumnStart;  // explicit start line (1-based), 0 = auto
  int gridColumnEnd;    // explicit end line (1-based, may be -1), 0 = auto

  // Generated content (for ::before / ::after styles).
  std::string content;
  bool hasContent;
  std::unique_ptr<ComputedStyle> before;
  std::unique_ptr<ComputedStyle> after;

  bool IsBlockLevel() const;
  bool IsInlineLevel() const;
  bool IsFloating() const { return floating != kFloatNone; }
  bool IsOutOfFlow() const {
    return position == kPosAbsolute || position == kPosFixed;
  }
  bool ClipsOverflow() const {
    return overflowX != kOverflowVisible || overflowY != kOverflowVisible;
  }
  void InheritFrom(const ComputedStyle& parent);
  // Copies all non-pseudo fields.
  void CopyFrom(const ComputedStyle& other);

 private:
  ComputedStyle(const ComputedStyle&);
  ComputedStyle& operator=(const ComputedStyle&);
};

// Context used to compute lengths (em, rem, vw ...).
struct LengthContext {
  float fontSize;
  float rootFontSize;
  float viewportW, viewportH;
};

// Parses a CSS length/percentage/calc() value. Returns false if invalid.
bool ParseLength(const std::string& value, const LengthContext& ctx, Length& out,
                 bool allowNegative = true);
// Parses a unitless number, also inside calc()/min()/max(). False if the
// value has units.
bool ParseNumber(const std::string& value, float& out);
bool ParseColor(const std::string& value, Color& out, const Color& currentColor);
// Returns the value of a url(...) token, or "" if none.
std::string ExtractUrl(const std::string& value);
bool IsCssPropertySupported(const std::string& prop, const std::string& value);

}  // namespace kite

#endif
