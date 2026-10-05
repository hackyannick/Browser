#include "css/style.h"

#include <cmath>
#include <cstring>

#include "base/strings.h"

namespace kite {

ComputedStyle::ComputedStyle()
    : color(0, 0, 0),
      fontFamily("serif"),
      fontSize(16),
      fontWeight(400),
      italic(false),
      underline(false),
      lineThrough(false),
      overline(false),
      decorationColorSet(false),
      lineHeight(-1),
      lineHeightFactor(0),
      textAlign(kAlignLeft),
      whiteSpace(kWsNormal),
      textTransform(kTtNone),
      textIndent(Length::Px(0)),
      letterSpacing(0),
      wordSpacing(0),
      overflowWrap(false),
      breakAll(false),
      fillDeclared(false),
      strokeDeclared(false),
      visible(true),
      listStyleType(kListDisc),
      listStyleInside(false),
      cursor(kCursorAuto),
      borderCollapse(false),
      borderSpacingH(0),
      borderSpacingV(0),
      display(kDisplayInline),
      position(kPosStatic),
      floating(kFloatNone),
      clear(kClearNone),
      minWidth(Length::Auto()),
      minHeight(Length::Auto()),
      maxWidth(Length::None()),
      maxHeight(Length::None()),
      zIndex(0),
      zIndexAuto(true),
      boxSizing(kContentBox),
      overflowX(kOverflowVisible),
      overflowY(kOverflowVisible),
      verticalAlign(kVaBaseline),
      verticalAlignPx(0),
      backgroundColor(0, 0, 0, 0),
      backgroundRepeat(kRepeat),
      backgroundPosX(Length::Px(0)),
      backgroundPosY(Length::Px(0)),
      backgroundSizeMode(0),
      opacity(1),
      blur(0),
      clippedAway(false),
      backgroundClipText(false),
      translateX(Length::Px(0)),
      translateY(Length::Px(0)),
      transformHidden(false),
      rotateRad(0),
      scaleX(1),
      scaleY(1),
      originX(Length::Pct(50)),
      originY(Length::Pct(50)),
      backfaceHidden(false),
      hasAnimation(false),
      aspectRatio(0),
      objectFit(kFitFill),
      flexDirection(kFlexRow),
      flexWrap(false),
      justifyContent(kFlexNormal),
      alignItems(kFlexNormal),
      alignSelf(kFlexAuto),
      alignContent(kFlexNormal),
      flexGrow(0),
      flexShrink(1),
      order(0),
      rowGap(Length::Px(0)),
      columnGap(Length::Px(0)),
      gridAutoRepeat(0),
      gridColumnSpan(1),
      gridColumnStart(0),
      gridColumnEnd(0),
      hasContent(false) {
  for (int i = 0; i < 4; ++i) {
    margin[i] = Length::Px(0);
    padding[i] = Length::Px(0);
    radius[i] = Length::Px(0);
  }
}

bool ComputedStyle::IsBlockLevel() const {
  switch (display) {
    case kDisplayBlock:
    case kDisplayListItem:
    case kDisplayTable:
    case kDisplayFlex:
    case kDisplayGrid:
      return true;
    default:
      return false;
  }
}

bool ComputedStyle::IsInlineLevel() const {
  switch (display) {
    case kDisplayInline:
    case kDisplayInlineBlock:
    case kDisplayInlineTable:
    case kDisplayInlineFlex:
    case kDisplayInlineGrid:
      return true;
    default:
      return false;
  }
}

void ComputedStyle::InheritFrom(const ComputedStyle& p) {
  color = p.color;
  fontFamily = p.fontFamily;
  fontSize = p.fontSize;
  fontWeight = p.fontWeight;
  italic = p.italic;
  underline = p.underline;
  lineThrough = p.lineThrough;
  overline = p.overline;
  decorationColor = p.decorationColor;
  decorationColorSet = p.decorationColorSet;
  lineHeightFactor = p.lineHeightFactor;
  lineHeight = p.lineHeightFactor > 0 ? -1 : p.lineHeight;
  textAlign = p.textAlign;
  whiteSpace = p.whiteSpace;
  textTransform = p.textTransform;
  textIndent = p.textIndent;
  letterSpacing = p.letterSpacing;
  wordSpacing = p.wordSpacing;
  overflowWrap = p.overflowWrap;
  breakAll = p.breakAll;
  fill = p.fill;
  stroke = p.stroke;
  fillDeclared = strokeDeclared = false;
  visible = p.visible;
  listStyleType = p.listStyleType;
  listStyleInside = p.listStyleInside;
  cursor = p.cursor;
  borderCollapse = p.borderCollapse;
  borderSpacingH = p.borderSpacingH;
  borderSpacingV = p.borderSpacingV;
  customProperties = p.customProperties;
}

void ComputedStyle::CopyFrom(const ComputedStyle& o) {
  InheritFrom(o);
  lineHeight = o.lineHeight;
  display = o.display;
  position = o.position;
  floating = o.floating;
  clear = o.clear;
  width = o.width;
  height = o.height;
  minWidth = o.minWidth;
  minHeight = o.minHeight;
  maxWidth = o.maxWidth;
  maxHeight = o.maxHeight;
  for (int i = 0; i < 4; ++i) {
    margin[i] = o.margin[i];
    padding[i] = o.padding[i];
    border[i] = o.border[i];
    inset[i] = o.inset[i];
  }
  zIndex = o.zIndex;
  zIndexAuto = o.zIndexAuto;
  boxSizing = o.boxSizing;
  overflowX = o.overflowX;
  overflowY = o.overflowY;
  verticalAlign = o.verticalAlign;
  verticalAlignPx = o.verticalAlignPx;
  backgroundColor = o.backgroundColor;
  backgroundImage = o.backgroundImage;
  backgroundRepeat = o.backgroundRepeat;
  backgroundPosX = o.backgroundPosX;
  backgroundPosY = o.backgroundPosY;
  backgroundSizeMode = o.backgroundSizeMode;
  backgroundSizeW = o.backgroundSizeW;
  backgroundSizeH = o.backgroundSizeH;
  opacity = o.opacity;
  blur = o.blur;
  clippedAway = o.clippedAway;
  backgroundClipText = o.backgroundClipText;
  for (int i = 0; i < 4; ++i) radius[i] = o.radius[i];
  shadows = o.shadows;
  translateX = o.translateX;
  transformOps = o.transformOps;
  rotateRad = o.rotateRad;
  backfaceHidden = o.backfaceHidden;
  scaleX = o.scaleX;
  scaleY = o.scaleY;
  originX = o.originX;
  originY = o.originY;
  translateY = o.translateY;
  transformHidden = o.transformHidden;
  hasAnimation = o.hasAnimation;
  animName = o.animName;
  animDuration = o.animDuration;
  animDelay = o.animDelay;
  animIterations = o.animIterations;
  animDirection = o.animDirection;
  animFillMode = o.animFillMode;
  animTiming = o.animTiming;
  animPlayState = o.animPlayState;
  transProperty = o.transProperty;
  transDuration = o.transDuration;
  transDelay = o.transDelay;
  transTiming = o.transTiming;
  aspectRatio = o.aspectRatio;
  objectFit = o.objectFit;
  flexDirection = o.flexDirection;
  flexWrap = o.flexWrap;
  justifyContent = o.justifyContent;
  alignItems = o.alignItems;
  alignSelf = o.alignSelf;
  alignContent = o.alignContent;
  flexGrow = o.flexGrow;
  flexShrink = o.flexShrink;
  flexBasis = o.flexBasis;
  order = o.order;
  rowGap = o.rowGap;
  columnGap = o.columnGap;
  gridColumns = o.gridColumns;
  gridAutoRepeat = o.gridAutoRepeat;
  gridAutoMin = o.gridAutoMin;
  gridColumnSpan = o.gridColumnSpan;
  gridColumnStart = o.gridColumnStart;
  gridColumnEnd = o.gridColumnEnd;
  content = o.content;
  hasContent = o.hasContent;
}

// ---------------------------------------------------------------------------
// Lengths and calc()

namespace {

struct CalcValue {
  float px, pct, num;
  bool isNumber;  // unitless
  CalcValue() : px(0), pct(0), num(0), isNumber(false) {}
};

class CalcParser {
 public:
  CalcParser(const std::string& s, const LengthContext& ctx) : s_(s), p_(0), ctx_(ctx), ok_(true) {}

  bool Parse(CalcValue& out) {
    out = ParseSum();
    SkipWs();
    return ok_ && p_ == s_.size();
  }

  CalcValue ParseSum() {
    CalcValue v = ParseProduct();
    for (;;) {
      SkipWs();
      if (p_ >= s_.size()) break;
      char c = s_[p_];
      if (c != '+' && c != '-') break;
      ++p_;
      CalcValue r = ParseProduct();
      float sign = c == '+' ? 1.0f : -1.0f;
      if (v.isNumber != r.isNumber) {
        // number +- length: only valid if the number is zero (lenient).
        if (v.isNumber && v.num == 0) { v = r; v.px *= sign; v.pct *= sign; continue; }
        if (r.isNumber && r.num == 0) continue;
        ok_ = false;
        break;
      }
      v.px += sign * r.px;
      v.pct += sign * r.pct;
      v.num += sign * r.num;
    }
    return v;
  }

  CalcValue ParseProduct() {
    CalcValue v = ParseTerm();
    for (;;) {
      SkipWs();
      if (p_ >= s_.size()) break;
      char c = s_[p_];
      if (c != '*' && c != '/') break;
      ++p_;
      CalcValue r = ParseTerm();
      if (c == '*') {
        if (r.isNumber) Scale(v, r.num);
        else if (v.isNumber) { float n = v.num; v = r; Scale(v, n); }
        else ok_ = false;
      } else {
        if (!r.isNumber || r.num == 0) { ok_ = false; break; }
        Scale(v, 1.0f / r.num);
      }
    }
    return v;
  }

  static void Scale(CalcValue& v, float f) {
    v.px *= f;
    v.pct *= f;
    v.num *= f;
  }

  CalcValue ParseTerm() {
    SkipWs();
    CalcValue v;
    if (p_ >= s_.size()) { ok_ = false; return v; }
    if (s_[p_] == '(') {
      ++p_;
      v = ParseSum();
      SkipWs();
      if (p_ < s_.size() && s_[p_] == ')') ++p_;
      else ok_ = false;
      return v;
    }
    // Functions.
    size_t start = p_;
    while (p_ < s_.size() && (IsAsciiAlpha((unsigned char)s_[p_]) || s_[p_] == '-') &&
           !(s_[p_] == '-' && p_ + 1 < s_.size() &&
             (IsAsciiDigit((unsigned char)s_[p_ + 1]) || s_[p_ + 1] == '.')))
      ++p_;
    if (p_ > start && p_ < s_.size() && s_[p_] == '(') {
      std::string fn = AsciiLower(s_.substr(start, p_ - start));
      ++p_;
      std::vector<CalcValue> args;
      for (;;) {
        args.push_back(ParseSum());
        SkipWs();
        if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
        break;
      }
      if (p_ < s_.size() && s_[p_] == ')') ++p_;
      else ok_ = false;
      return ApplyFunction(fn, args);
    }
    p_ = start;
    // Number with optional unit.
    std::string rest = s_.substr(p_);
    size_t used = 0;
    double n = ParseDoublePrefix(rest, used);
    if (used == 0) { ok_ = false; return v; }
    p_ += used;
    size_t us = p_;
    while (p_ < s_.size() && (IsAsciiAlpha((unsigned char)s_[p_]) || s_[p_] == '%')) ++p_;
    std::string unit = AsciiLower(s_.substr(us, p_ - us));
    if (!ApplyUnit((float)n, unit, v)) ok_ = false;
    return v;
  }

  bool ApplyUnit(float n, const std::string& unit, CalcValue& v) {
    if (unit.empty()) { v.isNumber = true; v.num = n; return true; }
    if (unit == "%") { v.pct = n; return true; }
    float f;
    if (unit == "px") f = 1;
    else if (unit == "em") f = ctx_.fontSize;
    else if (unit == "rem") f = ctx_.rootFontSize;
    else if (unit == "ex" || unit == "ch") f = ctx_.fontSize * 0.5f;
    else if (unit == "cap" || unit == "ic") f = ctx_.fontSize;
    else if (unit == "lh") f = ctx_.fontSize * 1.2f;
    else if (unit == "vw" || unit == "svw" || unit == "lvw" || unit == "dvw") f = ctx_.viewportW / 100;
    else if (unit == "vh" || unit == "svh" || unit == "lvh" || unit == "dvh") f = ctx_.viewportH / 100;
    else if (unit == "vmin") f = std::min(ctx_.viewportW, ctx_.viewportH) / 100;
    else if (unit == "vmax") f = std::max(ctx_.viewportW, ctx_.viewportH) / 100;
    else if (unit == "pt") f = 96.0f / 72;
    else if (unit == "pc") f = 16;
    else if (unit == "in") f = 96;
    else if (unit == "cm") f = 96 / 2.54f;
    else if (unit == "mm") f = 96 / 25.4f;
    else if (unit == "q") f = 96 / 101.6f;
    else return false;
    v.px = n * f;
    return true;
  }

  CalcValue ApplyFunction(const std::string& fn, const std::vector<CalcValue>& a) {
    if (fn == "calc" || fn == "-webkit-calc" || fn == "-moz-calc") {
      return a.empty() ? CalcValue() : a[0];
    }
    if ((fn == "min" || fn == "max") && !a.empty()) {
      bool anyPct = false;
      for (size_t i = 0; i < a.size(); ++i)
        if (a[i].pct != 0) anyPct = true;
      if (!anyPct) {
        CalcValue best = a[0];
        for (size_t i = 1; i < a.size(); ++i) {
          float x = a[i].isNumber ? a[i].num : a[i].px;
          float b = best.isNumber ? best.num : best.px;
          if (fn == "min" ? x < b : x > b) best = a[i];
        }
        return best;
      }
      // Mixed: min() prefers the relative value (never overflows), max()
      // prefers the fixed one.
      for (size_t i = 0; i < a.size(); ++i)
        if ((a[i].pct != 0) == (fn == "min")) return a[i];
      return a[0];
    }
    if (fn == "clamp" && a.size() == 3) {
      if (a[0].pct == 0 && a[1].pct == 0 && a[2].pct == 0) {
        CalcValue v = a[1];
        if (v.px < a[0].px) v = a[0];
        if (v.px > a[2].px) v = a[2];
        return v;
      }
      return a[1];
    }
    if (fn == "env" || fn == "var") {
      ok_ = false;
      return CalcValue();
    }
    if (!a.empty()) return a[0];
    ok_ = false;
    return CalcValue();
  }

  void SkipWs() {
    while (p_ < s_.size() && IsAsciiSpace((unsigned char)s_[p_])) ++p_;
  }

 private:
  const std::string& s_;
  size_t p_;
  const LengthContext& ctx_;
  bool ok_;
};

struct NamedColor {
  const char* name;
  uint32_t rgb;
};

const NamedColor kNamedColors[] = {
    {"aliceblue", 0xF0F8FF}, {"antiquewhite", 0xFAEBD7}, {"aqua", 0x00FFFF},
    {"aquamarine", 0x7FFFD4}, {"azure", 0xF0FFFF}, {"beige", 0xF5F5DC},
    {"bisque", 0xFFE4C4}, {"black", 0x000000}, {"blanchedalmond", 0xFFEBCD},
    {"blue", 0x0000FF}, {"blueviolet", 0x8A2BE2}, {"brown", 0xA52A2A},
    {"burlywood", 0xDEB887}, {"cadetblue", 0x5F9EA0}, {"chartreuse", 0x7FFF00},
    {"chocolate", 0xD2691E}, {"coral", 0xFF7F50}, {"cornflowerblue", 0x6495ED},
    {"cornsilk", 0xFFF8DC}, {"crimson", 0xDC143C}, {"cyan", 0x00FFFF},
    {"darkblue", 0x00008B}, {"darkcyan", 0x008B8B}, {"darkgoldenrod", 0xB8860B},
    {"darkgray", 0xA9A9A9}, {"darkgreen", 0x006400}, {"darkgrey", 0xA9A9A9},
    {"darkkhaki", 0xBDB76B}, {"darkmagenta", 0x8B008B}, {"darkolivegreen", 0x556B2F},
    {"darkorange", 0xFF8C00}, {"darkorchid", 0x9932CC}, {"darkred", 0x8B0000},
    {"darksalmon", 0xE9967A}, {"darkseagreen", 0x8FBC8F}, {"darkslateblue", 0x483D8B},
    {"darkslategray", 0x2F4F4F}, {"darkslategrey", 0x2F4F4F}, {"darkturquoise", 0x00CED1},
    {"darkviolet", 0x9400D3}, {"deeppink", 0xFF1493}, {"deepskyblue", 0x00BFFF},
    {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"dodgerblue", 0x1E90FF},
    {"firebrick", 0xB22222}, {"floralwhite", 0xFFFAF0}, {"forestgreen", 0x228B22},
    {"fuchsia", 0xFF00FF}, {"gainsboro", 0xDCDCDC}, {"ghostwhite", 0xF8F8FF},
    {"gold", 0xFFD700}, {"goldenrod", 0xDAA520}, {"gray", 0x808080},
    {"green", 0x008000}, {"greenyellow", 0xADFF2F}, {"grey", 0x808080},
    {"honeydew", 0xF0FFF0}, {"hotpink", 0xFF69B4}, {"indianred", 0xCD5C5C},
    {"indigo", 0x4B0082}, {"ivory", 0xFFFFF0}, {"khaki", 0xF0E68C},
    {"lavender", 0xE6E6FA}, {"lavenderblush", 0xFFF0F5}, {"lawngreen", 0x7CFC00},
    {"lemonchiffon", 0xFFFACD}, {"lightblue", 0xADD8E6}, {"lightcoral", 0xF08080},
    {"lightcyan", 0xE0FFFF}, {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray", 0xD3D3D3},
    {"lightgreen", 0x90EE90}, {"lightgrey", 0xD3D3D3}, {"lightpink", 0xFFB6C1},
    {"lightsalmon", 0xFFA07A}, {"lightseagreen", 0x20B2AA}, {"lightskyblue", 0x87CEFA},
    {"lightslategray", 0x778899}, {"lightslategrey", 0x778899}, {"lightsteelblue", 0xB0C4DE},
    {"lightyellow", 0xFFFFE0}, {"lime", 0x00FF00}, {"limegreen", 0x32CD32},
    {"linen", 0xFAF0E6}, {"magenta", 0xFF00FF}, {"maroon", 0x800000},
    {"mediumaquamarine", 0x66CDAA}, {"mediumblue", 0x0000CD}, {"mediumorchid", 0xBA55D3},
    {"mediumpurple", 0x9370DB}, {"mediumseagreen", 0x3CB371}, {"mediumslateblue", 0x7B68EE},
    {"mediumspringgreen", 0x00FA9A}, {"mediumturquoise", 0x48D1CC},
    {"mediumvioletred", 0xC71585}, {"midnightblue", 0x191970}, {"mintcream", 0xF5FFFA},
    {"mistyrose", 0xFFE4E1}, {"moccasin", 0xFFE4B5}, {"navajowhite", 0xFFDEAD},
    {"navy", 0x000080}, {"oldlace", 0xFDF5E6}, {"olive", 0x808000},
    {"olivedrab", 0x6B8E23}, {"orange", 0xFFA500}, {"orangered", 0xFF4500},
    {"orchid", 0xDA70D6}, {"palegoldenrod", 0xEEE8AA}, {"palegreen", 0x98FB98},
    {"paleturquoise", 0xAFEEEE}, {"palevioletred", 0xDB7093}, {"papayawhip", 0xFFEFD5},
    {"peachpuff", 0xFFDAB9}, {"peru", 0xCD853F}, {"pink", 0xFFC0CB},
    {"plum", 0xDDA0DD}, {"powderblue", 0xB0E0E6}, {"purple", 0x800080},
    {"rebeccapurple", 0x663399}, {"red", 0xFF0000}, {"rosybrown", 0xBC8F8F},
    {"royalblue", 0x4169E1}, {"saddlebrown", 0x8B4513}, {"salmon", 0xFA8072},
    {"sandybrown", 0xF4A460}, {"seagreen", 0x2E8B57}, {"seashell", 0xFFF5EE},
    {"sienna", 0xA0522D}, {"silver", 0xC0C0C0}, {"skyblue", 0x87CEEB},
    {"slateblue", 0x6A5ACD}, {"slategray", 0x708090}, {"slategrey", 0x708090},
    {"snow", 0xFFFAFA}, {"springgreen", 0x00FF7F}, {"steelblue", 0x4682B4},
    {"tan", 0xD2B48C}, {"teal", 0x008080}, {"thistle", 0xD8BFD8},
    {"tomato", 0xFF6347}, {"turquoise", 0x40E0D0}, {"violet", 0xEE82EE},
    {"wheat", 0xF5DEB3}, {"white", 0xFFFFFF}, {"whitesmoke", 0xF5F5F5},
    {"yellow", 0xFFFF00}, {"yellowgreen", 0x9ACD32},
    // CSS2 system colors (approximated with Windows 2000 defaults).
    {"buttonface", 0xD4D0C8}, {"buttontext", 0x000000}, {"canvas", 0xFFFFFF},
    {"canvastext", 0x000000}, {"graytext", 0x808080}, {"highlight", 0x0A246A},
    {"highlighttext", 0xFFFFFF}, {"linktext", 0x0000EE}, {"window", 0xFFFFFF},
    {"windowtext", 0x000000}, {"field", 0xFFFFFF}, {"fieldtext", 0x000000},
    {"buttonborder", 0x808080}, {"visitedtext", 0x551A8B}, {"activetext", 0xFF0000},
    {"mark", 0xFFFF00}, {"marktext", 0x000000}, {"accentcolor", 0x0A246A},
    {"accentcolortext", 0xFFFFFF},
};

float ParseComponent(const std::string& s, float scaleForPercent, bool& ok) {
  size_t used = 0;
  double v = ParseDoublePrefix(s, used);
  if (used == 0) {
    if (s == "none") return 0;
    ok = false;
    return 0;
  }
  if (used < s.size() && s[used] == '%') return (float)(v * scaleForPercent / 100.0);
  return (float)v;
}

uint8_t Clamp255(float v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)(v + 0.5f);
}

float HueToRgb(float p, float q, float t) {
  if (t < 0) t += 1;
  if (t > 1) t -= 1;
  if (t < 1.0f / 6) return p + (q - p) * 6 * t;
  if (t < 0.5f) return q;
  if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
  return p;
}

}  // namespace

bool ParseLength(const std::string& raw, const LengthContext& ctx, Length& out,
                 bool allowNegative) {
  std::string v = Trim(raw);
  if (v.empty()) return false;
  std::string lv = AsciiLower(v);
  if (lv == "auto") { out = Length::Auto(); return true; }
  if (lv == "none") { out = Length::None(); return true; }
  if (lv == "min-content" || lv == "-webkit-min-content") {
    out = Length(); out.kind = Length::kMinContent; return true;
  }
  if (lv == "max-content" || lv == "-webkit-max-content" || lv == "-moz-max-content") {
    out = Length(); out.kind = Length::kMaxContent; return true;
  }
  if (lv == "fit-content" || lv == "-webkit-fit-content" || lv == "-moz-fit-content" ||
      StartsWith(lv, "fit-content(")) {
    out = Length(); out.kind = Length::kFitContent; return true;
  }
  if (lv == "stretch" || lv == "-webkit-fill-available" || lv == "-moz-available") {
    out = Length::Auto();
    return true;
  }
  CalcParser p(lv, ctx);
  CalcValue cv;
  if (!p.Parse(cv)) return false;
  if (cv.isNumber) {
    // Unitless numbers: 0 always; other values accepted as px for legacy
    // content (quirks-mode behaviour).
    cv.px = cv.num;
  }
  out = Length();
  out.kind = Length::kFixed;
  out.px = cv.px;
  out.pct = cv.pct;
  if (!allowNegative && (out.px < 0 || out.pct < 0) && !(out.px > 0 || out.pct > 0)) return false;
  return true;
}

bool ParseNumber(const std::string& raw, float& out) {
  std::string v = AsciiLower(Trim(raw));
  LengthContext ctx = {16, 16, 1000, 1000};
  CalcParser p(v, ctx);
  CalcValue cv;
  if (!p.Parse(cv) || !cv.isNumber) return false;
  out = cv.num;
  return true;
}

bool ParseColor(const std::string& raw, Color& out, const Color& currentColor) {
  std::string v = AsciiLower(Trim(raw));
  if (v.empty()) return false;
  if (v == "transparent") { out = Color(0, 0, 0, 0); return true; }
  if (v == "currentcolor") { out = currentColor; return true; }
  if (v[0] == '#') {
    std::string h = v.substr(1);
    for (size_t i = 0; i < h.size(); ++i)
      if (!IsAsciiHex((unsigned char)h[i])) return false;
    if (h.size() == 3 || h.size() == 4) {
      out = Color(HexValue(h[0]) * 17, HexValue(h[1]) * 17, HexValue(h[2]) * 17,
                  h.size() == 4 ? HexValue(h[3]) * 17 : 255);
      return true;
    }
    if (h.size() == 6 || h.size() == 8) {
      out = Color(HexValue(h[0]) * 16 + HexValue(h[1]), HexValue(h[2]) * 16 + HexValue(h[3]),
                  HexValue(h[4]) * 16 + HexValue(h[5]),
                  h.size() == 8 ? HexValue(h[6]) * 16 + HexValue(h[7]) : 255);
      return true;
    }
    return false;
  }
  size_t paren = v.find('(');
  if (paren != std::string::npos && v[v.size() - 1] == ')') {
    std::string fn = v.substr(0, paren);
    std::string args = v.substr(paren + 1, v.size() - paren - 2);
    // Split alpha after '/'.
    std::string alphaPart;
    size_t slash = args.find('/');
    if (slash != std::string::npos) {
      alphaPart = Trim(args.substr(slash + 1));
      args = args.substr(0, slash);
    }
    for (size_t i = 0; i < args.size(); ++i)
      if (args[i] == ',') args[i] = ' ';
    std::vector<std::string> parts = SplitWhitespace(args);
    if (parts.size() == 4 && alphaPart.empty()) {
      alphaPart = parts[3];
      parts.pop_back();
    }
    if (parts.size() != 3) return false;
    bool ok = true;
    float alpha = 1;
    if (!alphaPart.empty()) alpha = ParseComponent(alphaPart, 1, ok);
    if (fn == "rgb" || fn == "rgba") {
      float r = ParseComponent(parts[0], 255, ok);
      float g = ParseComponent(parts[1], 255, ok);
      float b = ParseComponent(parts[2], 255, ok);
      if (!ok) return false;
      out = Color(Clamp255(r), Clamp255(g), Clamp255(b), Clamp255(alpha * 255));
      return true;
    }
    if (fn == "hsl" || fn == "hsla") {
      size_t used;
      float h = (float)ParseDoublePrefix(parts[0], used);
      if (used == 0) return false;
      std::string unit = parts[0].substr(used);
      if (unit == "rad") h = h * 180 / 3.14159265f;
      else if (unit == "turn") h *= 360;
      float s = ParseComponent(parts[1], 1, ok);
      float l = ParseComponent(parts[2], 1, ok);
      if (!ok) return false;
      if (parts[1].find('%') == std::string::npos) s /= 100;
      if (parts[2].find('%') == std::string::npos) l /= 100;
      h = std::fmod(h, 360.0f);
      if (h < 0) h += 360;
      h /= 360;
      float r, g, b;
      if (s == 0) {
        r = g = b = l;
      } else {
        float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
        float p = 2 * l - q;
        r = HueToRgb(p, q, h + 1.0f / 3);
        g = HueToRgb(p, q, h);
        b = HueToRgb(p, q, h - 1.0f / 3);
      }
      out = Color(Clamp255(r * 255), Clamp255(g * 255), Clamp255(b * 255),
                  Clamp255(alpha * 255));
      return true;
    }
    // oklch()/lab()/color() etc: unsupported -> approximate grey by lightness
    if (fn == "oklch" || fn == "oklab" || fn == "lab" || fn == "lch") {
      float l = ParseComponent(parts[0], 1, ok);
      if (!ok) return false;
      if (parts[0].find('%') == std::string::npos && (fn == "lab" || fn == "lch")) l /= 100;
      if (parts[0].find('%') != std::string::npos && (fn == "oklch" || fn == "oklab")) {}
      uint8_t g = Clamp255(l * 255);
      out = Color(g, g, g, Clamp255(alpha * 255));
      return true;
    }
    return false;
  }
  size_t lo = 0, hi = sizeof(kNamedColors) / sizeof(kNamedColors[0]);
  for (size_t i = lo; i < hi; ++i) {
    if (v == kNamedColors[i].name) {
      uint32_t c = kNamedColors[i].rgb;
      out = Color((c >> 16) & 255, (c >> 8) & 255, c & 255);
      return true;
    }
  }
  return false;
}

std::string ExtractUrl(const std::string& value) {
  std::string lv = AsciiLower(value);
  size_t p = lv.find("url(");
  if (p == std::string::npos) return std::string();
  size_t start = p + 4;
  size_t end = value.find(')', start);
  if (end == std::string::npos) return std::string();
  std::string u = Trim(value.substr(start, end - start));
  if (u.size() >= 2 && (u[0] == '"' || u[0] == '\'')) {
    char q = u[0];
    size_t close = value.find(q, value.find(q, start) + 1);
    if (close == std::string::npos) return std::string();
    u = value.substr(value.find(q, start) + 1, close - value.find(q, start) - 1);
  }
  return u;
}

static void MulMatrix(float m[6], const float n[6]) {  // m = m * n
  float r[6] = {m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3],
                m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
  for (int i = 0; i < 6; ++i) m[i] = r[i];
}

void TransformMatrix(const ComputedStyle& s, float w, float h, float out[6]) {
  float m[6] = {1, 0, 0, 1, 0, 0};
  if (s.rotateRad != 0) {
    float c = std::cos(s.rotateRad), sn = std::sin(s.rotateRad);
    float r[6] = {c, sn, -sn, c, 0, 0};
    MulMatrix(m, r);
  }
  if (s.scaleX != 1 || s.scaleY != 1) {
    float r[6] = {s.scaleX, 0, 0, s.scaleY, 0, 0};
    MulMatrix(m, r);
  }
  for (size_t i = 0; i < s.transformOps.size(); ++i) {
    const TransformOp& op = s.transformOps[i];
    float r[6] = {1, 0, 0, 1, 0, 0};
    switch (op.kind) {
      case TransformOp::kTranslate:
        r[4] = op.tx.Resolve(w);
        r[5] = op.ty.Resolve(h);
        break;
      case TransformOp::kScale:
        r[0] = op.v[0];
        r[3] = op.v[1];
        break;
      case TransformOp::kRotate:
        r[0] = r[3] = std::cos(op.v[0]);
        r[1] = std::sin(op.v[0]);
        r[2] = -r[1];
        break;
      case TransformOp::kRotateX:
        r[3] = std::cos(op.v[0]);
        break;
      case TransformOp::kRotateY:
        r[0] = std::cos(op.v[0]);
        break;
      case TransformOp::kSkew:
        r[2] = std::tan(op.v[0]);
        r[1] = std::tan(op.v[1]);
        break;
      case TransformOp::kMatrix:
        for (int k = 0; k < 6; ++k) r[k] = op.v[k];
        break;
    }
    MulMatrix(m, r);
  }
  for (int i = 0; i < 6; ++i) out[i] = m[i];
}

}  // namespace kite
