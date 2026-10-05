#include "css/properties.h"

#include <cmath>
#include <map>

#include "base/strings.h"
#include "net/url.h"

namespace kite {

namespace {

struct PropInfo {
  const char* name;
  bool inherited;
};

const PropInfo kProps[] = {
#define KITE_PROP_INFO(id, name, inh) {name, inh},
    KITE_PROPERTIES(KITE_PROP_INFO)
#undef KITE_PROP_INFO
};

const std::map<std::string, int>& PropMap() {
  static std::map<std::string, int>* m = 0;
  if (!m) {
    m = new std::map<std::string, int>;
    for (int i = 0; i < kPropCount; ++i) (*m)[kProps[i].name] = i;
  }
  return *m;
}

bool IsColorToken(const std::string& t) {
  Color c;
  return ParseColor(t, c, Color());
}

bool IsBorderStyleToken(const std::string& t) {
  static const char* const s[] = {"none", "hidden", "solid", "dashed", "dotted",
                                  "double", "groove", "ridge", "inset", "outset", 0};
  std::string l = AsciiLower(t);
  for (int i = 0; s[i]; ++i)
    if (l == s[i]) return true;
  return false;
}

bool IsGlobalKeyword(const std::string& v) {
  std::string l = AsciiLower(v);
  return l == "inherit" || l == "initial" || l == "unset" || l == "revert" ||
         l == "revert-layer";
}

void FourSides(const std::vector<std::string>& t, int top, int right, int bottom, int left,
               std::vector<std::pair<int, std::string> >& out) {
  if (t.empty()) return;
  std::string a = t[0];
  std::string b = t.size() > 1 ? t[1] : a;
  std::string c = t.size() > 2 ? t[2] : a;
  std::string d = t.size() > 3 ? t[3] : b;
  out.push_back(std::make_pair(top, a));
  out.push_back(std::make_pair(right, b));
  out.push_back(std::make_pair(bottom, c));
  out.push_back(std::make_pair(left, d));
}

void TwoSides(const std::vector<std::string>& t, int first, int second,
              std::vector<std::pair<int, std::string> >& out) {
  if (t.empty()) return;
  out.push_back(std::make_pair(first, t[0]));
  out.push_back(std::make_pair(second, t.size() > 1 ? t[1] : t[0]));
}

void ExpandBorderSide(const std::string& value, int widthId, int styleId, int colorId,
                      std::vector<std::pair<int, std::string> >& out) {
  std::string width = "medium", style = "none", color = "currentcolor";
  std::vector<std::string> t = SplitValueTokens(value);
  if (t.size() == 1 && IsGlobalKeyword(t[0])) {
    width = style = color = t[0];
  } else {
    for (size_t i = 0; i < t.size(); ++i) {
      if (IsBorderStyleToken(t[i])) style = t[i];
      else if (IsColorToken(t[i]) || StartsWithIgnoreCase(t[i], "var(")) color = t[i];
      else width = t[i];
    }
  }
  out.push_back(std::make_pair(widthId, width));
  out.push_back(std::make_pair(styleId, style));
  out.push_back(std::make_pair(colorId, color));
}

// Picks a representative color from a gradient: the first color stop.
bool GradientColor(const std::string& value, Color& out) {
  std::string lv = AsciiLower(value);
  size_t p = lv.find("gradient(");
  if (p == std::string::npos) return false;
  size_t start = p + 9;
  size_t end = lv.rfind(')');
  if (end == std::string::npos || end <= start) return false;
  std::vector<std::string> parts = SplitValueCommas(lv.substr(start, end - start));
  int count = 0;
  int r = 0, g = 0, b = 0, a = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    std::vector<std::string> toks = SplitValueTokens(parts[i]);
    for (size_t k = 0; k < toks.size(); ++k) {
      Color c;
      if (ParseColor(toks[k], c, Color())) {
        r += c.r; g += c.g; b += c.b; a += c.a;
        ++count;
        break;
      }
    }
  }
  if (!count) return false;
  out = Color(r / count, g / count, b / count, a / count);
  return true;
}

FlexAlign ParseFlexAlign(const std::string& raw, FlexAlign fallback) {
  std::string v = AsciiLower(Trim(raw));
  // Strip "safe"/"unsafe" prefixes.
  if (StartsWith(v, "safe ")) v = v.substr(5);
  if (StartsWith(v, "unsafe ")) v = v.substr(7);
  if (v == "flex-start" || v == "start" || v == "self-start" || v == "left") return kFlexStart;
  if (v == "flex-end" || v == "end" || v == "self-end" || v == "right") return kFlexEnd;
  if (v == "center") return kFlexCenter;
  if (v == "stretch") return kFlexStretch;
  if (v == "baseline" || v == "first baseline" || v == "last baseline") return kFlexBaseline;
  if (v == "space-between") return kFlexSpaceBetween;
  if (v == "space-around") return kFlexSpaceAround;
  if (v == "space-evenly") return kFlexSpaceEvenly;
  if (v == "auto") return kFlexAuto;
  if (v == "normal") return kFlexNormal;
  return fallback;
}

void ParseGridTemplate(const std::string& raw, const LengthContext& lc, ComputedStyle& s) {
  s.gridColumns.clear();
  s.gridAutoRepeat = 0;
  std::string v = AsciiLower(Trim(raw));
  if (v == "none" || v.empty()) return;
  std::vector<std::string> toks = SplitValueTokens(v);
  // Expand repeat() into a flat track list.
  std::vector<std::string> flat;
  for (size_t i = 0; i < toks.size(); ++i) {
    const std::string& t = toks[i];
    if (!t.empty() && t[0] == '[') continue;  // line names
    if (!StartsWith(t, "repeat(")) {
      flat.push_back(t);
      continue;
    }
    std::string inner = t.substr(7, t.size() - 8);
    size_t comma = inner.find(',');
    if (comma == std::string::npos) continue;
    std::string countStr = Trim(inner.substr(0, comma));
    std::vector<std::string> trackToks = SplitValueTokens(Trim(inner.substr(comma + 1)));
    std::vector<std::string> tracks;
    for (size_t j = 0; j < trackToks.size(); ++j)
      if (!trackToks[j].empty() && trackToks[j][0] != '[') tracks.push_back(trackToks[j]);
    if (tracks.empty()) continue;
    if (countStr == "auto-fill" || countStr == "auto-fit") {
      s.gridAutoRepeat = 1;
      std::string first = tracks[0];
      Length l;
      if (StartsWith(first, "minmax(")) {
        std::vector<std::string> a = SplitValueCommas(first.substr(7, first.size() - 8));
        if (!a.empty() && ParseLength(a[0], lc, l)) s.gridAutoMin = l;
      } else if (ParseLength(first, lc, l)) {
        s.gridAutoMin = l;
      }
      if (!s.gridAutoMin.IsFixed() || s.gridAutoMin.Resolve(1000) <= 0)
        s.gridAutoMin = Length::Px(200);
      flat.push_back("1fr");
      continue;
    }
    long long n = 1;
    ParseInt(countStr, n);
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    for (long long k = 0; k < n; ++k)
      for (size_t j = 0; j < tracks.size(); ++j) flat.push_back(tracks[j]);
  }
  for (size_t i = 0; i < flat.size(); ++i) {
    const std::string& t = flat[i];
    GridTrack tr;
    if (EndsWith(t, "fr")) {
      tr.kind = GridTrack::kFr;
      size_t used;
      tr.fr = (float)ParseDoublePrefix(t, used);
    } else if (StartsWith(t, "minmax(")) {
      std::vector<std::string> a = SplitValueCommas(t.substr(7, t.size() - 8));
      tr.kind = GridTrack::kMinMax;
      tr.fr = 0;
      if (!a.empty()) ParseLength(a[0], lc, tr.size);
      if (a.size() > 1 && EndsWith(Trim(a[1]), "fr")) {
        size_t used;
        tr.fr = (float)ParseDoublePrefix(Trim(a[1]), used);
      } else if (a.size() > 1) {
        Length mx;
        if (ParseLength(a[1], lc, mx) && mx.IsFixed()) {
          tr.kind = GridTrack::kFixedTrack;
          tr.size = mx;
        } else {
          tr.kind = GridTrack::kAutoTrack;
        }
      }
    } else if (t == "auto" || t == "min-content" || t == "max-content" ||
               StartsWith(t, "fit-content")) {
      tr.kind = GridTrack::kAutoTrack;
    } else {
      Length l;
      if (!ParseLength(t, lc, l) || !l.IsFixed()) continue;
      tr.kind = GridTrack::kFixedTrack;
      tr.size = l;
    }
    s.gridColumns.push_back(tr);
  }
}

void ParseGridLine(const std::string& raw, int& line, int& span) {
  std::string v = AsciiLower(Trim(raw));
  line = 0;
  if (v == "auto" || v.empty()) return;
  std::vector<std::string> t = SplitWhitespace(v);
  if (t[0] == "span") {
    long long n = 1;
    if (t.size() > 1) ParseInt(t[1], n);
    span = (int)(n < 1 ? 1 : n);
    return;
  }
  long long n;
  if (ParseInt(t[0], n)) line = (int)n;
}

ListStyle ParseListStyleType(const std::string& v) {
  if (v == "none") return kListNone;
  if (v == "circle") return kListCircle;
  if (v == "square") return kListSquare;
  if (v == "decimal") return kListDecimal;
  if (v == "decimal-leading-zero") return kListDecimalLeadingZero;
  if (v == "lower-alpha" || v == "lower-latin") return kListLowerAlpha;
  if (v == "upper-alpha" || v == "upper-latin") return kListUpperAlpha;
  if (v == "lower-roman") return kListLowerRoman;
  if (v == "upper-roman") return kListUpperRoman;
  if (v == "disc") return kListDisc;
  return kListDisc;
}

std::string UnquoteFamily(const std::string& raw) {
  std::string out;
  std::vector<std::string> fams = SplitValueCommas(raw);
  for (size_t i = 0; i < fams.size(); ++i) {
    std::string f = Trim(fams[i]);
    if (f.size() >= 2 && (f[0] == '"' || f[0] == '\'')) f = f.substr(1, f.size() - 2);
    f = ReplaceAll(f, "\\", "");
    f = AsciiLower(CollapseWhitespace(f));
    if (f.empty()) continue;
    if (!out.empty()) out += ",";
    out += f;
  }
  return out;
}

void CopyPropertyImpl(int id, ComputedStyle& d, const ComputedStyle& s) {
  switch (id) {
    case kPropFontSize: d.fontSize = s.fontSize; break;
    case kPropColor: d.color = s.color; break;
    case kPropFontFamily: d.fontFamily = s.fontFamily; break;
    case kPropFontWeight: d.fontWeight = s.fontWeight; break;
    case kPropFontStyle: d.italic = s.italic; break;
    case kPropLineHeight: d.lineHeight = s.lineHeight; d.lineHeightFactor = s.lineHeightFactor; break;
    case kPropTextAlign: d.textAlign = s.textAlign; break;
    case kPropWhiteSpace: d.whiteSpace = s.whiteSpace; break;
    case kPropTextTransform: d.textTransform = s.textTransform; break;
    case kPropTextIndent: d.textIndent = s.textIndent; break;
    case kPropLetterSpacing: d.letterSpacing = s.letterSpacing; break;
    case kPropWordSpacing: d.wordSpacing = s.wordSpacing; break;
    case kPropVisibility: d.visible = s.visible; break;
    case kPropListStyleType: d.listStyleType = s.listStyleType; break;
    case kPropListStylePosition: d.listStyleInside = s.listStyleInside; break;
    case kPropCursor: d.cursor = s.cursor; break;
    case kPropBorderCollapse: d.borderCollapse = s.borderCollapse; break;
    case kPropOverflowWrap: d.overflowWrap = s.overflowWrap; break;
    case kPropWordBreak: d.breakAll = s.breakAll; break;
    case kPropFill: d.fill = s.fill; d.fillDeclared = true; break;
    case kPropStroke: d.stroke = s.stroke; d.strokeDeclared = true; break;
    case kPropBorderSpacing: d.borderSpacingH = s.borderSpacingH; d.borderSpacingV = s.borderSpacingV; break;
    case kPropTextDecorationLine:
      d.underline = s.underline; d.lineThrough = s.lineThrough; d.overline = s.overline; break;
    case kPropTextDecorationColor: d.decorationColor = s.decorationColor; d.decorationColorSet = s.decorationColorSet; break;
    case kPropDisplay: d.display = s.display; break;
    case kPropPosition: d.position = s.position; break;
    case kPropFloat: d.floating = s.floating; break;
    case kPropClear: d.clear = s.clear; break;
    case kPropTop: d.inset[0] = s.inset[0]; break;
    case kPropRight: d.inset[1] = s.inset[1]; break;
    case kPropBottom: d.inset[2] = s.inset[2]; break;
    case kPropLeft: d.inset[3] = s.inset[3]; break;
    case kPropZIndex: d.zIndex = s.zIndex; d.zIndexAuto = s.zIndexAuto; break;
    case kPropWidth: d.width = s.width; break;
    case kPropHeight: d.height = s.height; break;
    case kPropMinWidth: d.minWidth = s.minWidth; break;
    case kPropMinHeight: d.minHeight = s.minHeight; break;
    case kPropMaxWidth: d.maxWidth = s.maxWidth; break;
    case kPropMaxHeight: d.maxHeight = s.maxHeight; break;
    case kPropMarginTop: case kPropMarginRight: case kPropMarginBottom: case kPropMarginLeft:
      d.margin[id - kPropMarginTop] = s.margin[id - kPropMarginTop]; break;
    case kPropPaddingTop: case kPropPaddingRight: case kPropPaddingBottom: case kPropPaddingLeft:
      d.padding[id - kPropPaddingTop] = s.padding[id - kPropPaddingTop]; break;
    case kPropBorderTopWidth: case kPropBorderRightWidth: case kPropBorderBottomWidth:
    case kPropBorderLeftWidth:
      d.border[id - kPropBorderTopWidth].width = s.border[id - kPropBorderTopWidth].width; break;
    case kPropBorderTopStyle: case kPropBorderRightStyle: case kPropBorderBottomStyle:
    case kPropBorderLeftStyle:
      d.border[id - kPropBorderTopStyle].style = s.border[id - kPropBorderTopStyle].style; break;
    case kPropBorderTopColor: case kPropBorderRightColor: case kPropBorderBottomColor:
    case kPropBorderLeftColor:
      d.border[id - kPropBorderTopColor].color = s.border[id - kPropBorderTopColor].color;
      d.border[id - kPropBorderTopColor].colorIsCurrent = s.border[id - kPropBorderTopColor].colorIsCurrent;
      break;
    case kPropBoxSizing: d.boxSizing = s.boxSizing; break;
    case kPropOverflowX: d.overflowX = s.overflowX; break;
    case kPropOverflowY: d.overflowY = s.overflowY; break;
    case kPropVerticalAlign: d.verticalAlign = s.verticalAlign; d.verticalAlignPx = s.verticalAlignPx; break;
    case kPropBackgroundColor: d.backgroundColor = s.backgroundColor; break;
    case kPropBackgroundImage: d.backgroundImage = s.backgroundImage; break;
    case kPropBackgroundRepeat: d.backgroundRepeat = s.backgroundRepeat; break;
    case kPropBackgroundPositionX: d.backgroundPosX = s.backgroundPosX; break;
    case kPropBackgroundPositionY: d.backgroundPosY = s.backgroundPosY; break;
    case kPropBackgroundSize:
      d.backgroundSizeMode = s.backgroundSizeMode; d.backgroundSizeW = s.backgroundSizeW;
      d.backgroundSizeH = s.backgroundSizeH; break;
    case kPropOpacity: d.opacity = s.opacity; break;
    case kPropAspectRatio: d.aspectRatio = s.aspectRatio; break;
    case kPropObjectFit: d.objectFit = s.objectFit; break;
    case kPropFlexDirection: d.flexDirection = s.flexDirection; break;
    case kPropFlexWrap: d.flexWrap = s.flexWrap; break;
    case kPropJustifyContent: d.justifyContent = s.justifyContent; break;
    case kPropAlignItems: d.alignItems = s.alignItems; break;
    case kPropAlignSelf: d.alignSelf = s.alignSelf; break;
    case kPropAlignContent: d.alignContent = s.alignContent; break;
    case kPropFlexGrow: d.flexGrow = s.flexGrow; break;
    case kPropFlexShrink: d.flexShrink = s.flexShrink; break;
    case kPropFlexBasis: d.flexBasis = s.flexBasis; break;
    case kPropOrder: d.order = s.order; break;
    case kPropRowGap: d.rowGap = s.rowGap; break;
    case kPropColumnGap: d.columnGap = s.columnGap; break;
    case kPropGridTemplateColumns:
      d.gridColumns = s.gridColumns; d.gridAutoRepeat = s.gridAutoRepeat; d.gridAutoMin = s.gridAutoMin; break;
    case kPropGridColumnStart: d.gridColumnStart = s.gridColumnStart; d.gridColumnSpan = s.gridColumnSpan; break;
    case kPropGridColumnEnd: d.gridColumnEnd = s.gridColumnEnd; break;
    case kPropContent: d.content = s.content; d.hasContent = s.hasContent; break;
    case kPropFilter: d.blur = s.blur; break;
    case kPropClip: case kPropClipPath: d.clippedAway = s.clippedAway; break;
    case kPropBackgroundClip: d.backgroundClipText = s.backgroundClipText; break;
    case kPropBorderTopLeftRadius: case kPropBorderTopRightRadius:
    case kPropBorderBottomRightRadius: case kPropBorderBottomLeftRadius:
      d.radius[id - kPropBorderTopLeftRadius] = s.radius[id - kPropBorderTopLeftRadius]; break;
    case kPropBoxShadow: d.shadows = s.shadows; break;
    case kPropTransform: case kPropTranslate:
      d.translateX = s.translateX; d.translateY = s.translateY; d.transformHidden = s.transformHidden;
      if (id == kPropTransform) d.transformOps = s.transformOps;
      break;
    case kPropRotate: d.rotateRad = s.rotateRad; break;
    case kPropScale: d.scaleX = s.scaleX; d.scaleY = s.scaleY; d.transformHidden = s.transformHidden; break;
    case kPropTransformOrigin: d.originX = s.originX; d.originY = s.originY; break;
    case kPropBackfaceVisibility: d.backfaceHidden = s.backfaceHidden; break;
    case kPropAnimationName: d.hasAnimation = s.hasAnimation; d.animName = s.animName; break;
    case kPropAnimationDuration: d.animDuration = s.animDuration; break;
    case kPropAnimationDelay: d.animDelay = s.animDelay; break;
    case kPropAnimationIterationCount: d.animIterations = s.animIterations; break;
    case kPropAnimationDirection: d.animDirection = s.animDirection; break;
    case kPropAnimationFillMode: d.animFillMode = s.animFillMode; break;
    case kPropAnimationTimingFunction: d.animTiming = s.animTiming; break;
    case kPropAnimationPlayState: d.animPlayState = s.animPlayState; break;
    case kPropTransitionProperty: d.transProperty = s.transProperty; break;
    case kPropTransitionDuration: d.transDuration = s.transDuration; break;
    case kPropTransitionDelay: d.transDelay = s.transDelay; break;
    case kPropTransitionTimingFunction: d.transTiming = s.transTiming; break;
    default: break;
  }
}

bool IsCssTime(const std::string& t) {
  if (t.size() < 2) return false;
  size_t used = 0;
  ParseDoublePrefix(t, used);
  if (used == 0) return false;
  std::string unit = t.substr(used);
  return unit == "s" || unit == "ms";
}

bool IsTimingFunction(const std::string& t) {
  return t == "linear" || t == "ease" || t == "ease-in" || t == "ease-out" || t == "ease-in-out" ||
         t == "step-start" || t == "step-end" || StartsWith(t, "cubic-bezier(") || StartsWith(t, "steps(") ||
         StartsWith(t, "linear(");
}

// Splits the animation shorthand into its longhand lists.
void ExpandAnimation(const std::string& value, std::vector<std::pair<int, std::string> >& out) {
  std::vector<std::string> parts = SplitValueCommas(value);
  std::string lists[8];
  for (size_t i = 0; i < parts.size(); ++i) {
    std::vector<std::string> tokens = SplitValueTokens(parts[i]);
    std::string name = "none", duration = "0s", delay = "0s", iter = "1", dir = "normal", fill = "none",
                timing = "ease", play = "running";
    bool haveDuration = false;
    for (size_t k = 0; k < tokens.size(); ++k) {
      std::string t = tokens[k], l = AsciiLower(t);
      if (IsCssTime(l)) {
        if (!haveDuration) duration = l;
        else delay = l;
        haveDuration = true;
      } else if (IsTimingFunction(l)) {
        timing = l;
      } else if (l == "infinite" || (!l.empty() && (IsAsciiDigit((unsigned char)l[0]) || l[0] == '.'))) {
        iter = l;
      } else if (l == "normal" || l == "reverse" || l == "alternate" || l == "alternate-reverse") {
        dir = l;
      } else if (l == "forwards" || l == "backwards" || l == "both") {
        fill = l;
      } else if (l == "running" || l == "paused") {
        play = l;
      } else if (l != "none" || tokens.size() == 1) {
        name = t;
      }
    }
    const std::string vals[8] = {name, duration, delay, iter, dir, fill, timing, play};
    for (int k = 0; k < 8; ++k) lists[k] += (i ? "," : "") + vals[k];
  }
  const int ids[8] = {kPropAnimationName, kPropAnimationDuration, kPropAnimationDelay,
                      kPropAnimationIterationCount, kPropAnimationDirection, kPropAnimationFillMode,
                      kPropAnimationTimingFunction, kPropAnimationPlayState};
  for (int k = 0; k < 8; ++k) out.push_back(std::make_pair(ids[k], lists[k]));
}

void ExpandTransition(const std::string& value, std::vector<std::pair<int, std::string> >& out) {
  std::vector<std::string> parts = SplitValueCommas(value);
  std::string lists[4];
  for (size_t i = 0; i < parts.size(); ++i) {
    std::vector<std::string> tokens = SplitValueTokens(parts[i]);
    std::string prop = "all", duration = "0s", delay = "0s", timing = "ease";
    bool haveDuration = false;
    for (size_t k = 0; k < tokens.size(); ++k) {
      std::string l = AsciiLower(tokens[k]);
      if (IsCssTime(l)) {
        if (!haveDuration) duration = l;
        else delay = l;
        haveDuration = true;
      } else if (IsTimingFunction(l)) {
        timing = l;
      } else if (l != "allow-discrete" && l != "normal") {
        prop = l;
      }
    }
    const std::string vals[4] = {prop, duration, delay, timing};
    for (int k = 0; k < 4; ++k) lists[k] += (i ? "," : "") + vals[k];
  }
  const int ids[4] = {kPropTransitionProperty, kPropTransitionDuration, kPropTransitionDelay,
                      kPropTransitionTimingFunction};
  for (int k = 0; k < 4; ++k) out.push_back(std::make_pair(ids[k], lists[k]));
}

}  // namespace

void CopyProperty(int id, ComputedStyle& dst, const ComputedStyle& src) { CopyPropertyImpl(id, dst, src); }

std::vector<std::string> SplitValueTokens(const std::string& value) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  char quote = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (quote) {
      cur += c;
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      cur += c;
      continue;
    }
    if (c == '(') ++depth;
    if (c == ')' && depth > 0) --depth;
    if (depth == 0 && IsAsciiSpace((unsigned char)c)) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      continue;
    }
    if (depth == 0 && c == '/' ) {
      if (!cur.empty()) out.push_back(cur);
      out.push_back("/");
      cur.clear();
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<std::string> SplitValueCommas(const std::string& value) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  char quote = 0;
  for (size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (quote) {
      cur += c;
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '"' || c == '\'') quote = c;
    if (c == '(') ++depth;
    if (c == ')' && depth > 0) --depth;
    if (depth == 0 && c == ',') {
      out.push_back(Trim(cur));
      cur.clear();
      continue;
    }
    cur += c;
  }
  out.push_back(Trim(cur));
  return out;
}

int LookupProperty(const std::string& name) {
  const std::map<std::string, int>& m = PropMap();
  std::map<std::string, int>::const_iterator it = m.find(name);
  return it == m.end() ? -1 : it->second;
}

const char* PropertyName(int id) { return kProps[id].name; }
bool IsInheritedProperty(int id) { return kProps[id].inherited; }

bool ExpandProperty(const std::string& rawName, const std::string& value,
                    std::vector<std::pair<int, std::string> >& out) {
  std::string name = rawName;
  // Drop vendor prefixes for properties we understand unprefixed.
  if (StartsWith(name, "-webkit-") || StartsWith(name, "-moz-") || StartsWith(name, "-ms-")) {
    std::string un = name.substr(name.find('-', 1) + 1);
    if (un == "flex" || un == "flex-direction" || un == "flex-wrap" || un == "flex-flow" ||
        un == "flex-grow" || un == "flex-shrink" || un == "flex-basis" ||
        un == "justify-content" || un == "align-items" || un == "align-self" ||
        un == "box-sizing" || un == "order" || un == "background-clip" ||
        un == "border-radius" || un == "box-shadow" || StartsWith(un, "transform") ||
        StartsWith(un, "animation") || StartsWith(un, "transition"))
      name = un;
    else
      return false;
  }
  int id = LookupProperty(name);
  if (id >= 0) {
    out.push_back(std::make_pair(id, value));
    return true;
  }
  std::vector<std::string> t = SplitValueTokens(value);
  bool global = t.size() == 1 && IsGlobalKeyword(t[0]);
  if (name == "margin") {
    FourSides(t, kPropMarginTop, kPropMarginRight, kPropMarginBottom, kPropMarginLeft, out);
  } else if (name == "padding") {
    FourSides(t, kPropPaddingTop, kPropPaddingRight, kPropPaddingBottom, kPropPaddingLeft, out);
  } else if (name == "inset") {
    FourSides(t, kPropTop, kPropRight, kPropBottom, kPropLeft, out);
  } else if (name == "border-width") {
    FourSides(t, kPropBorderTopWidth, kPropBorderRightWidth, kPropBorderBottomWidth,
              kPropBorderLeftWidth, out);
  } else if (name == "border-style") {
    FourSides(t, kPropBorderTopStyle, kPropBorderRightStyle, kPropBorderBottomStyle,
              kPropBorderLeftStyle, out);
  } else if (name == "border-color") {
    FourSides(t, kPropBorderTopColor, kPropBorderRightColor, kPropBorderBottomColor,
              kPropBorderLeftColor, out);
  } else if (name == "border") {
    ExpandBorderSide(value, kPropBorderTopWidth, kPropBorderTopStyle, kPropBorderTopColor, out);
    ExpandBorderSide(value, kPropBorderRightWidth, kPropBorderRightStyle, kPropBorderRightColor, out);
    ExpandBorderSide(value, kPropBorderBottomWidth, kPropBorderBottomStyle, kPropBorderBottomColor, out);
    ExpandBorderSide(value, kPropBorderLeftWidth, kPropBorderLeftStyle, kPropBorderLeftColor, out);
  } else if (name == "border-top" || name == "border-block-start") {
    ExpandBorderSide(value, kPropBorderTopWidth, kPropBorderTopStyle, kPropBorderTopColor, out);
  } else if (name == "border-right" || name == "border-inline-end") {
    ExpandBorderSide(value, kPropBorderRightWidth, kPropBorderRightStyle, kPropBorderRightColor, out);
  } else if (name == "border-bottom" || name == "border-block-end") {
    ExpandBorderSide(value, kPropBorderBottomWidth, kPropBorderBottomStyle, kPropBorderBottomColor, out);
  } else if (name == "border-left" || name == "border-inline-start") {
    ExpandBorderSide(value, kPropBorderLeftWidth, kPropBorderLeftStyle, kPropBorderLeftColor, out);
  } else if (name == "border-block") {
    ExpandBorderSide(value, kPropBorderTopWidth, kPropBorderTopStyle, kPropBorderTopColor, out);
    ExpandBorderSide(value, kPropBorderBottomWidth, kPropBorderBottomStyle, kPropBorderBottomColor, out);
  } else if (name == "border-inline") {
    ExpandBorderSide(value, kPropBorderLeftWidth, kPropBorderLeftStyle, kPropBorderLeftColor, out);
    ExpandBorderSide(value, kPropBorderRightWidth, kPropBorderRightStyle, kPropBorderRightColor, out);
  } else if (StartsWith(name, "border-") && (EndsWith(name, "-width") || EndsWith(name, "-style") ||
                                              EndsWith(name, "-color"))) {
    // Logical longhands: border-block-start-width, border-inline-end-color ...
    std::string mid = name.substr(7, name.size() - 13);
    std::string kind = name.substr(name.size() - 5);
    int base = kind == "width" ? kPropBorderTopWidth
               : kind == "style" ? kPropBorderTopStyle : kPropBorderTopColor;
    if (mid == "block-start") out.push_back(std::make_pair(base + 0, value));
    else if (mid == "inline-end") out.push_back(std::make_pair(base + 1, value));
    else if (mid == "block-end") out.push_back(std::make_pair(base + 2, value));
    else if (mid == "inline-start") out.push_back(std::make_pair(base + 3, value));
    else if (mid == "block") TwoSides(t, base + 0, base + 2, out);
    else if (mid == "inline") TwoSides(t, base + 3, base + 1, out);
    else return false;
  } else if (name == "margin-block") {
    TwoSides(t, kPropMarginTop, kPropMarginBottom, out);
  } else if (name == "margin-inline") {
    TwoSides(t, kPropMarginLeft, kPropMarginRight, out);
  } else if (name == "padding-block") {
    TwoSides(t, kPropPaddingTop, kPropPaddingBottom, out);
  } else if (name == "padding-inline") {
    TwoSides(t, kPropPaddingLeft, kPropPaddingRight, out);
  } else if (name == "inset-block") {
    TwoSides(t, kPropTop, kPropBottom, out);
  } else if (name == "inset-inline") {
    TwoSides(t, kPropLeft, kPropRight, out);
  } else if (name == "margin-block-start") out.push_back(std::make_pair((int)kPropMarginTop, value));
  else if (name == "margin-block-end") out.push_back(std::make_pair((int)kPropMarginBottom, value));
  else if (name == "margin-inline-start") out.push_back(std::make_pair((int)kPropMarginLeft, value));
  else if (name == "margin-inline-end") out.push_back(std::make_pair((int)kPropMarginRight, value));
  else if (name == "padding-block-start") out.push_back(std::make_pair((int)kPropPaddingTop, value));
  else if (name == "padding-block-end") out.push_back(std::make_pair((int)kPropPaddingBottom, value));
  else if (name == "padding-inline-start") out.push_back(std::make_pair((int)kPropPaddingLeft, value));
  else if (name == "padding-inline-end") out.push_back(std::make_pair((int)kPropPaddingRight, value));
  else if (name == "inset-block-start") out.push_back(std::make_pair((int)kPropTop, value));
  else if (name == "inset-block-end") out.push_back(std::make_pair((int)kPropBottom, value));
  else if (name == "inset-inline-start") out.push_back(std::make_pair((int)kPropLeft, value));
  else if (name == "inset-inline-end") out.push_back(std::make_pair((int)kPropRight, value));
  else if (name == "inline-size") out.push_back(std::make_pair((int)kPropWidth, value));
  else if (name == "block-size") out.push_back(std::make_pair((int)kPropHeight, value));
  else if (name == "min-inline-size") out.push_back(std::make_pair((int)kPropMinWidth, value));
  else if (name == "max-inline-size") out.push_back(std::make_pair((int)kPropMaxWidth, value));
  else if (name == "min-block-size") out.push_back(std::make_pair((int)kPropMinHeight, value));
  else if (name == "max-block-size") out.push_back(std::make_pair((int)kPropMaxHeight, value));
  else if (name == "overflow") {
    TwoSides(t, kPropOverflowX, kPropOverflowY, out);
  } else if (name == "gap" || name == "grid-gap") {
    TwoSides(t, kPropRowGap, kPropColumnGap, out);
  } else if (name == "grid-row-gap") {
    out.push_back(std::make_pair((int)kPropRowGap, value));
  } else if (name == "grid-column-gap") {
    out.push_back(std::make_pair((int)kPropColumnGap, value));
  } else if (name == "background-position") {
    if (global) {
      out.push_back(std::make_pair((int)kPropBackgroundPositionX, value));
      out.push_back(std::make_pair((int)kPropBackgroundPositionY, value));
    } else {
      std::string first = SplitValueCommas(value)[0];
      std::vector<std::string> p = SplitValueTokens(first);
      std::string x = "50%", y = "50%";
      if (p.size() == 1) {
        std::string l = AsciiLower(p[0]);
        if (l == "top" || l == "bottom") y = p[0];
        else x = p[0];
      } else if (p.size() >= 2) {
        std::string a = AsciiLower(p[0]), b = AsciiLower(p[1]);
        if (a == "top" || a == "bottom" || b == "left" || b == "right") {
          x = p[1];
          y = p[0];
        } else {
          x = p[0];
          y = p[1];
        }
      }
      out.push_back(std::make_pair((int)kPropBackgroundPositionX, x));
      out.push_back(std::make_pair((int)kPropBackgroundPositionY, y));
    }
  } else if (name == "background") {
    std::string color = "transparent", image = "none", repeat = "repeat";
    std::string posX = "0%", posY = "0%", size = "auto";
    if (global) {
      color = image = repeat = posX = posY = size = value;
    } else {
      // Only the last layer may carry a color; the first layer's image wins.
      std::vector<std::string> layers = SplitValueCommas(value);
      std::vector<std::string> lt = SplitValueTokens(layers.back());
      std::vector<std::string> ft = SplitValueTokens(layers[0]);
      for (size_t i = 0; i < ft.size(); ++i) {
        std::string l = AsciiLower(ft[i]);
        if (StartsWith(l, "url(") || l.find("gradient(") != std::string::npos) image = ft[i];
        else if (l == "repeat" || l == "no-repeat" || l == "repeat-x" || l == "repeat-y" ||
                 l == "space" || l == "round")
          repeat = ft[i];
        else if (l == "/") {
          if (i + 1 < ft.size()) {
            size = ft[i + 1];
            if (i + 2 < ft.size()) {
              std::string n = AsciiLower(ft[i + 2]);
              Length dummy;
              LengthContext lc = {16, 16, 1000, 1000};
              if (ParseLength(n, lc, dummy) && n != "none") {
                size += " " + ft[i + 2];
                ++i;
              }
            }
            ++i;
          }
        } else if (l == "left" || l == "right" || l == "center" || l == "top" ||
                   l == "bottom" || IsAsciiDigit((unsigned char)l[0]) || l[0] == '-' ||
                   l[0] == '.') {
          // Position components.
          if (l == "top" || l == "bottom") posY = ft[i];
          else if (posX == "0%" && l != "center") posX = ft[i];
          else if (l == "center") {
            if (posX == "0%") posX = "50%";
            else posY = "50%";
          } else posY = ft[i];
        }
      }
      for (size_t i = 0; i < lt.size(); ++i) {
        if (IsColorToken(lt[i])) color = lt[i];
        else if (StartsWithIgnoreCase(lt[i], "var(") && lt.size() == 1) color = lt[i];
      }
    }
    out.push_back(std::make_pair((int)kPropBackgroundColor, color));
    out.push_back(std::make_pair((int)kPropBackgroundImage, image));
    out.push_back(std::make_pair((int)kPropBackgroundRepeat, repeat));
    out.push_back(std::make_pair((int)kPropBackgroundPositionX, posX));
    out.push_back(std::make_pair((int)kPropBackgroundPositionY, posY));
    out.push_back(std::make_pair((int)kPropBackgroundSize, size));
  } else if (name == "font") {
    if (global) {
      int ids[] = {kPropFontStyle, kPropFontWeight, kPropFontSize, kPropLineHeight, kPropFontFamily};
      for (int i = 0; i < 5; ++i) out.push_back(std::make_pair(ids[i], value));
      return true;
    }
    std::string l = AsciiLower(Trim(value));
    if (l == "caption" || l == "icon" || l == "menu" || l == "message-box" ||
        l == "small-caption" || l == "status-bar" || StartsWith(l, "-")) {
      out.push_back(std::make_pair((int)kPropFontStyle, std::string("normal")));
      out.push_back(std::make_pair((int)kPropFontWeight, std::string("normal")));
      out.push_back(std::make_pair((int)kPropFontSize, std::string("13px")));
      out.push_back(std::make_pair((int)kPropLineHeight, std::string("normal")));
      out.push_back(std::make_pair((int)kPropFontFamily, std::string("sans-serif")));
      return true;
    }
    std::string style = "normal", weight = "normal", size, lh = "normal", family;
    size_t i = 0;
    for (; i < t.size(); ++i) {
      std::string tl = AsciiLower(t[i]);
      if (tl == "italic" || tl == "oblique") style = tl;
      else if (tl == "bold" || tl == "bolder" || tl == "lighter" ||
               (tl.size() == 3 && IsAsciiDigit((unsigned char)tl[0]) && tl[1] == '0' && tl[2] == '0'))
        weight = tl;
      else if (tl == "normal" || tl == "small-caps" || tl == "condensed" || tl == "expanded" ||
               tl == "semi-condensed" || tl == "semi-expanded")
        continue;
      else break;
    }
    if (i < t.size()) size = t[i++];
    if (i < t.size() && t[i] == "/") {
      ++i;
      if (i < t.size()) lh = t[i++];
    } else if (!size.empty() && size.find('/') != std::string::npos) {
      size_t sl = size.find('/');
      lh = size.substr(sl + 1);
      size = size.substr(0, sl);
    }
    for (; i < t.size(); ++i) family += (family.empty() ? "" : " ") + t[i];
    if (size.empty() || family.empty()) return true;  // invalid: ignore
    out.push_back(std::make_pair((int)kPropFontStyle, style));
    out.push_back(std::make_pair((int)kPropFontWeight, weight));
    out.push_back(std::make_pair((int)kPropFontSize, size));
    out.push_back(std::make_pair((int)kPropLineHeight, lh));
    out.push_back(std::make_pair((int)kPropFontFamily, family));
  } else if (name == "flex") {
    std::string g = "0", s = "1", b = "auto";
    std::string l = AsciiLower(Trim(value));
    if (global) {
      g = s = b = value;
    } else if (l == "none") {
      g = "0"; s = "0"; b = "auto";
    } else if (l == "auto") {
      g = "1"; s = "1"; b = "auto";
    } else if (l == "initial") {
      g = "0"; s = "1"; b = "auto";
    } else {
      std::vector<std::string> nums, others;
      for (size_t i = 0; i < t.size(); ++i) {
        size_t used;
        ParseDoublePrefix(t[i], used);
        if (used == t[i].size() && used > 0) nums.push_back(t[i]);
        else others.push_back(t[i]);
      }
      b = "0%";
      if (!nums.empty()) g = nums[0];
      if (nums.size() > 1) s = nums[1];
      if (nums.size() > 2) b = nums[2];
      if (!others.empty()) b = others[0];
    }
    out.push_back(std::make_pair((int)kPropFlexGrow, g));
    out.push_back(std::make_pair((int)kPropFlexShrink, s));
    out.push_back(std::make_pair((int)kPropFlexBasis, b));
  } else if (name == "flex-flow") {
    for (size_t i = 0; i < t.size(); ++i) {
      std::string l = AsciiLower(t[i]);
      if (l == "wrap" || l == "nowrap" || l == "wrap-reverse")
        out.push_back(std::make_pair((int)kPropFlexWrap, t[i]));
      else
        out.push_back(std::make_pair((int)kPropFlexDirection, t[i]));
    }
  } else if (name == "text-decoration") {
    std::string line = "none", color = "currentcolor";
    for (size_t i = 0; i < t.size(); ++i) {
      std::string l = AsciiLower(t[i]);
      if (l == "underline" || l == "line-through" || l == "overline" || l == "none" ||
          IsGlobalKeyword(l)) {
        line = line == "none" ? l : line + " " + l;
      } else if (IsColorToken(t[i])) {
        color = t[i];
      }
    }
    out.push_back(std::make_pair((int)kPropTextDecorationLine, line));
    out.push_back(std::make_pair((int)kPropTextDecorationColor, color));
  } else if (name == "list-style") {
    std::string type = "disc", pos = "outside";
    for (size_t i = 0; i < t.size(); ++i) {
      std::string l = AsciiLower(t[i]);
      if (l == "inside" || l == "outside") pos = l;
      else if (!StartsWith(l, "url(")) type = l;
    }
    if (global) type = pos = value;
    out.push_back(std::make_pair((int)kPropListStyleType, type));
    out.push_back(std::make_pair((int)kPropListStylePosition, pos));
  } else if (name == "place-items") {
    if (!t.empty()) out.push_back(std::make_pair((int)kPropAlignItems, t[0]));
  } else if (name == "place-self") {
    if (!t.empty()) out.push_back(std::make_pair((int)kPropAlignSelf, t[0]));
  } else if (name == "place-content") {
    if (!t.empty()) {
      out.push_back(std::make_pair((int)kPropAlignContent, t[0]));
      out.push_back(std::make_pair((int)kPropJustifyContent, t.size() > 1 ? t[1] : t[0]));
    }
  } else if (name == "grid-column") {
    size_t slash = value.find('/');
    out.push_back(std::make_pair((int)kPropGridColumnStart,
                                 Trim(value.substr(0, slash))));
    out.push_back(std::make_pair((int)kPropGridColumnEnd,
                                 slash == std::string::npos ? std::string("auto")
                                                            : Trim(value.substr(slash + 1))));
  } else if (name == "grid-template") {
    size_t slash = value.find('/');
    if (slash != std::string::npos)
      out.push_back(std::make_pair((int)kPropGridTemplateColumns, Trim(value.substr(slash + 1))));
  } else if (name == "border-radius") {
    std::string first = value.substr(0, value.find('/'));
    std::vector<std::string> r = SplitValueTokens(first);
    if (global) r.assign(1, value);
    FourSides(r, kPropBorderTopLeftRadius, kPropBorderTopRightRadius,
              kPropBorderBottomRightRadius, kPropBorderBottomLeftRadius, out);
  } else if (name == "animation" || name == "-webkit-animation") {
    if (global) {
      const int ids[8] = {kPropAnimationName, kPropAnimationDuration, kPropAnimationDelay,
                          kPropAnimationIterationCount, kPropAnimationDirection, kPropAnimationFillMode,
                          kPropAnimationTimingFunction, kPropAnimationPlayState};
      for (int k = 0; k < 8; ++k) out.push_back(std::make_pair(ids[k], value));
    } else {
      ExpandAnimation(value, out);
    }
  } else if (name == "transition" || name == "-webkit-transition") {
    if (global) {
      const int ids[4] = {kPropTransitionProperty, kPropTransitionDuration, kPropTransitionDelay,
                          kPropTransitionTimingFunction};
      for (int k = 0; k < 4; ++k) out.push_back(std::make_pair(ids[k], value));
    } else {
      ExpandTransition(value, out);
    }
  } else if (name == "word-wrap") {
    out.push_back(std::make_pair((int)kPropOverflowWrap, value));
  } else if (name == "text-wrap" || name == "text-wrap-mode") {
    std::string l = AsciiLower(Trim(value));
    if (l == "nowrap") out.push_back(std::make_pair((int)kPropWhiteSpace, std::string("nowrap")));
  } else {
    return false;
  }
  return true;
}

bool IsCssPropertySupported(const std::string& prop, const std::string& value) {
  if (StartsWith(prop, "--")) return true;
  std::vector<std::pair<int, std::string> > out;
  if (!ExpandProperty(prop, value, out)) return false;
  std::string v = AsciiLower(Trim(value));
  if (prop == "display") {
    return !(v.find("grid") != std::string::npos || v == "contents" || v == "subgrid" ||
             v == "ruby");
  }
  if (prop == "position") return v != "-webkit-sticky";
  if (StartsWith(prop, "grid")) return false;
  if (v.find("subgrid") != std::string::npos || v.find("color-mix") != std::string::npos ||
      v.find("oklch") != std::string::npos || v.find("lab(") != std::string::npos ||
      v.find("anchor") != std::string::npos || v.find("dvh") != std::string::npos ||
      v.find("svh") != std::string::npos)
    return false;
  return true;
}

bool ParseAngle(const std::string& raw, float& out) {
  std::string t = AsciiLower(Trim(raw));
  size_t used = 0;
  double v = ParseDoublePrefix(t, used);
  if (used == 0) return false;
  std::string unit = t.substr(used);
  const double pi = 3.14159265358979;
  if (unit == "deg") out = (float)(v * pi / 180);
  else if (unit == "rad") out = (float)v;
  else if (unit == "turn") out = (float)(v * 2 * pi);
  else if (unit == "grad") out = (float)(v * pi / 200);
  else if (unit.empty() && v == 0) out = 0;
  else return false;
  return true;
}

bool ParseScaleFactor(const std::string& raw, float& out) {
  std::string t = Trim(raw);
  if (!t.empty() && t[t.size() - 1] == '%') {
    size_t used = 0;
    double v = ParseDoublePrefix(t, used);
    if (used != t.size() - 1) return false;
    out = (float)(v / 100);
    return true;
  }
  return ParseNumber(t, out);
}

bool ParseTransformList(const std::string& v, const LengthContext& lc, std::vector<TransformOp>& ops) {
  size_t p = 0;
  while (p < v.size()) {
    size_t open = v.find('(', p);
    if (open == std::string::npos) break;
    // Matching parenthesis (arguments may contain calc()).
    int depth = 0;
    size_t close = open;
    for (; close < v.size(); ++close) {
      if (v[close] == '(') ++depth;
      else if (v[close] == ')' && --depth == 0) break;
    }
    if (close >= v.size()) return false;
    std::string fn = Trim(v.substr(p, open - p));
    std::vector<std::string> a = SplitValueCommas(v.substr(open + 1, close - open - 1));
    if (a.size() == 1) a = SplitValueTokens(a[0]);
    for (size_t k = 0; k < a.size(); ++k) a[k] = Trim(a[k]);
    TransformOp op;
    Length l;
    float f, g;
    if (fn == "translate" || fn == "translate3d" || fn == "translatex" || fn == "translatey") {
      op.kind = TransformOp::kTranslate;
      op.tx = op.ty = Length::Px(0);
      if (a.empty()) return false;
      if (fn == "translatey") {
        if (!ParseLength(a[0], lc, l) || !l.IsFixed()) return false;
        op.ty = l;
      } else {
        if (!ParseLength(a[0], lc, l) || !l.IsFixed()) return false;
        op.tx = l;
        if (fn != "translatex" && a.size() > 1) {
          if (!ParseLength(a[1], lc, l) || !l.IsFixed()) return false;
          op.ty = l;
        }
      }
    } else if (fn == "scale" || fn == "scale3d" || fn == "scalex" || fn == "scaley") {
      op.kind = TransformOp::kScale;
      if (a.empty() || !ParseScaleFactor(a[0], f)) return false;
      g = f;
      if ((fn == "scale" || fn == "scale3d") && a.size() > 1 && !ParseScaleFactor(a[1], g)) return false;
      op.v[0] = fn == "scaley" ? 1 : f;
      op.v[1] = fn == "scalex" ? 1 : g;
    } else if (fn == "rotate" || fn == "rotatez") {
      op.kind = TransformOp::kRotate;
      if (a.empty() || !ParseAngle(a[0], f)) return false;
      op.v[0] = f;
    } else if (fn == "skew" || fn == "skewx" || fn == "skewy") {
      op.kind = TransformOp::kSkew;
      if (a.empty() || !ParseAngle(a[0], f)) return false;
      g = 0;
      if (fn == "skew" && a.size() > 1 && !ParseAngle(a[1], g)) return false;
      op.v[0] = fn == "skewy" ? 0 : f;
      op.v[1] = fn == "skewy" ? f : g;
    } else if (fn == "matrix" && a.size() == 6) {
      op.kind = TransformOp::kMatrix;
      for (int k = 0; k < 6; ++k)
        if (!ParseNumber(a[k], op.v[k])) return false;
    } else if (fn == "matrix3d" && a.size() == 16) {
      op.kind = TransformOp::kMatrix;
      const int idx[6] = {0, 1, 4, 5, 12, 13};
      for (int k = 0; k < 6; ++k)
        if (!ParseNumber(a[idx[k]], op.v[k])) return false;
    } else if (fn == "rotatex" || fn == "rotatey") {
      op.kind = fn == "rotatex" ? TransformOp::kRotateX : TransformOp::kRotateY;
      if (a.empty() || !ParseAngle(a[0], f)) return false;
      op.v[0] = f;
    } else if (fn == "rotate3d" && a.size() == 4) {
      // Rotation about an arbitrary axis, projected onto the screen plane.
      float x, y, z, ang;
      if (!ParseNumber(a[0], x) || !ParseNumber(a[1], y) || !ParseNumber(a[2], z) || !ParseAngle(a[3], ang))
        return false;
      float len = std::sqrt(x * x + y * y + z * z);
      if (len <= 0) {
        p = close + 1;
        continue;
      }
      x /= len;
      y /= len;
      z /= len;
      float c = std::cos(ang), sn = std::sin(ang), t = 1 - c;
      op.kind = TransformOp::kMatrix;
      op.v[0] = t * x * x + c;      // m11
      op.v[1] = t * x * y + sn * z;  // m12
      op.v[2] = t * x * y - sn * z;  // m21
      op.v[3] = t * y * y + c;      // m22
    } else if (fn == "translatez" || fn == "perspective" || fn == "scalez") {
      // 3D: ignored (flat projection).
      p = close + 1;
      continue;
    } else {
      return false;
    }
    ops.push_back(op);
    p = close + 1;
    while (p < v.size() && (IsAsciiSpace((unsigned char)v[p]))) ++p;
  }
  return true;
}

void ApplyProperty(int id, const std::string& rawValue, ComputedStyle& s,
                   const ApplyContext& ctx) {
  const ComputedStyle& parent = *ctx.parent;
  std::string value = Trim(rawValue);
  std::string v = AsciiLower(value);
  if (v == "inherit") {
    CopyPropertyImpl(id, s, parent);
    return;
  }
  if (v == "initial" || v == "unset" || v == "revert" || v == "revert-layer") {
    if (v != "initial" && IsInheritedProperty(id)) {
      CopyPropertyImpl(id, s, parent);
    } else {
      static ComputedStyle* initial = new ComputedStyle;
      CopyPropertyImpl(id, s, *initial);
    }
    return;
  }

  LengthContext lc;
  lc.fontSize = s.fontSize;
  lc.rootFontSize = ctx.rootFontSize;
  lc.viewportW = ctx.viewportW;
  lc.viewportH = ctx.viewportH;
  Length len;
  Color col;

  switch (id) {
    case kPropFontSize: {
      float ps = parent.fontSize;
      static const struct { const char* n; float px; } kSizes[] = {
          {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16},
          {"large", 18}, {"x-large", 24}, {"xx-large", 32}, {"xxx-large", 48}, {0, 0}};
      for (int i = 0; kSizes[i].n; ++i)
        if (v == kSizes[i].n) { s.fontSize = kSizes[i].px; return; }
      if (v == "smaller") { s.fontSize = ps / 1.2f; return; }
      if (v == "larger") { s.fontSize = ps * 1.2f; return; }
      lc.fontSize = ps;
      if (ParseLength(v, lc, len) && len.IsFixed()) {
        float px = len.Resolve(ps);
        if (px >= 0) s.fontSize = px;
      }
      return;
    }
    case kPropColor:
      if (ParseColor(v, col, parent.color)) s.color = col;
      return;
    case kPropFontFamily: {
      std::string f = UnquoteFamily(value);
      if (!f.empty()) s.fontFamily = f;
      return;
    }
    case kPropFontWeight:
      if (v == "normal") s.fontWeight = 400;
      else if (v == "bold") s.fontWeight = 700;
      else if (v == "bolder") s.fontWeight = parent.fontWeight < 600 ? 700 : 900;
      else if (v == "lighter") s.fontWeight = parent.fontWeight > 500 ? 400 : 100;
      else {
        long long n;
        if (ParseInt(v, n) && n >= 1 && n <= 1000) s.fontWeight = (int)n;
      }
      return;
    case kPropFontStyle:
      s.italic = v == "italic" || StartsWith(v, "oblique");
      return;
    case kPropLineHeight: {
      if (v == "normal") { s.lineHeight = -1; s.lineHeightFactor = 0; return; }
      float n;
      if (ParseNumber(v, n)) {
        s.lineHeightFactor = n;
        s.lineHeight = n * s.fontSize;
        return;
      }
      if (ParseLength(v, lc, len) && len.IsFixed()) {
        s.lineHeight = len.Resolve(s.fontSize);
        s.lineHeightFactor = 0;
      }
      return;
    }
    case kPropTextAlign:
      if (v == "left" || v == "start" || v == "-webkit-left") s.textAlign = kAlignLeft;
      else if (v == "right" || v == "end" || v == "-webkit-right") s.textAlign = kAlignRight;
      else if (v == "center" || v == "-webkit-center" || v == "-moz-center") s.textAlign = kAlignCenter;
      else if (v == "justify") s.textAlign = kAlignJustify;
      return;
    case kPropWhiteSpace:
      if (v == "normal") s.whiteSpace = kWsNormal;
      else if (v == "pre") s.whiteSpace = kWsPre;
      else if (v == "nowrap") s.whiteSpace = kWsNowrap;
      else if (v == "pre-wrap" || v == "break-spaces") s.whiteSpace = kWsPreWrap;
      else if (v == "pre-line") s.whiteSpace = kWsPreLine;
      return;
    case kPropTextTransform:
      if (v == "uppercase") s.textTransform = kTtUppercase;
      else if (v == "lowercase") s.textTransform = kTtLowercase;
      else if (v == "capitalize") s.textTransform = kTtCapitalize;
      else if (v == "none") s.textTransform = kTtNone;
      return;
    case kPropTextIndent:
      if (ParseLength(v, lc, len) && len.IsFixed()) s.textIndent = len;
      return;
    case kPropLetterSpacing:
      if (v == "normal") s.letterSpacing = 0;
      else if (ParseLength(v, lc, len) && len.IsFixed()) s.letterSpacing = len.Resolve(0);
      return;
    case kPropWordSpacing:
      if (v == "normal") s.wordSpacing = 0;
      else if (ParseLength(v, lc, len) && len.IsFixed()) s.wordSpacing = len.Resolve(0);
      return;
    case kPropVisibility:
      s.visible = !(v == "hidden" || v == "collapse");
      return;
    case kPropListStyleType:
      s.listStyleType = ParseListStyleType(v);
      return;
    case kPropListStylePosition:
      s.listStyleInside = v == "inside";
      return;
    case kPropCursor:
      if (v == "pointer" || v == "hand") s.cursor = kCursorPointer;
      else if (v == "text") s.cursor = kCursorText;
      else if (v == "default") s.cursor = kCursorDefault;
      else s.cursor = kCursorAuto;
      return;
    case kPropBorderCollapse:
      s.borderCollapse = v == "collapse";
      return;
    case kPropFill:
      s.fill = value;
      s.fillDeclared = true;
      return;
    case kPropStroke:
      s.stroke = value;
      s.strokeDeclared = true;
      return;
    case kPropOverflowWrap:
      s.overflowWrap = v == "break-word" || v == "anywhere";
      return;
    case kPropWordBreak:
      s.breakAll = v == "break-all" || v == "break-word";
      return;
    case kPropBorderSpacing: {
      std::vector<std::string> t = SplitValueTokens(v);
      if (t.empty()) return;
      Length a, b;
      if (ParseLength(t[0], lc, a) && a.IsFixed()) {
        s.borderSpacingH = a.Resolve(0);
        s.borderSpacingV = s.borderSpacingH;
        if (t.size() > 1 && ParseLength(t[1], lc, b) && b.IsFixed())
          s.borderSpacingV = b.Resolve(0);
      }
      return;
    }
    case kPropTextDecorationLine: {
      if (v == "none") return;  // cannot remove decorations from ancestors
      std::vector<std::string> t = SplitWhitespace(v);
      for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == "underline") s.underline = true;
        else if (t[i] == "line-through") s.lineThrough = true;
        else if (t[i] == "overline") s.overline = true;
      }
      return;
    }
    case kPropTextDecorationColor:
      if (v == "currentcolor") {
        s.decorationColorSet = false;
      } else if (ParseColor(v, col, s.color)) {
        s.decorationColor = col;
        s.decorationColorSet = true;
      }
      return;
    case kPropDisplay: {
      std::vector<std::string> t = SplitWhitespace(v);
      bool inl = false;
      std::string inner;
      for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == "inline") inl = true;
        else if (t[i] == "block" || t[i] == "flow" || t[i] == "flow-root") {}
        else inner = t[i];
      }
      if (t.size() == 1) {
        inl = false;
        inner = t[0];
      }
      if (inner == "none") s.display = kDisplayNone;
      else if (inner == "inline") s.display = kDisplayInline;
      else if (inner == "block" || inner == "flow-root" || inner == "flow" || inner == "run-in" ||
               inner == "-webkit-box" || inner == "-moz-box")
        s.display = inl ? kDisplayInlineBlock : kDisplayBlock;
      else if (inner == "inline-block" || inner == "-webkit-inline-box") s.display = kDisplayInlineBlock;
      else if (inner == "list-item") s.display = kDisplayListItem;
      else if (inner == "table") s.display = inl ? kDisplayInlineTable : kDisplayTable;
      else if (inner == "inline-table") s.display = kDisplayInlineTable;
      else if (inner == "table-row-group") s.display = kDisplayTableRowGroup;
      else if (inner == "table-header-group") s.display = kDisplayTableHeaderGroup;
      else if (inner == "table-footer-group") s.display = kDisplayTableFooterGroup;
      else if (inner == "table-row") s.display = kDisplayTableRow;
      else if (inner == "table-cell") s.display = kDisplayTableCell;
      else if (inner == "table-column") s.display = kDisplayTableColumn;
      else if (inner == "table-column-group") s.display = kDisplayTableColumnGroup;
      else if (inner == "table-caption") s.display = kDisplayTableCaption;
      else if (inner == "flex" || inner == "-webkit-flex" || inner == "-ms-flexbox")
        s.display = inl ? kDisplayInlineFlex : kDisplayFlex;
      else if (inner == "inline-flex" || inner == "-webkit-inline-flex" || inner == "-ms-inline-flexbox")
        s.display = kDisplayInlineFlex;
      else if (inner == "grid" || inner == "-ms-grid") s.display = inl ? kDisplayInlineGrid : kDisplayGrid;
      else if (inner == "inline-grid") s.display = kDisplayInlineGrid;
      else if (inner == "contents") s.display = kDisplayContents;
      else if (inner == "ruby" || inner == "ruby-text" || inner == "ruby-base") s.display = kDisplayInline;
      return;
    }
    case kPropPosition:
      if (v == "static") s.position = kPosStatic;
      else if (v == "relative") s.position = kPosRelative;
      else if (v == "absolute") s.position = kPosAbsolute;
      else if (v == "fixed") s.position = kPosFixed;
      else if (v == "sticky" || v == "-webkit-sticky") s.position = kPosSticky;
      return;
    case kPropFloat:
      if (v == "left" || v == "inline-start") s.floating = kFloatLeft;
      else if (v == "right" || v == "inline-end") s.floating = kFloatRight;
      else if (v == "none") s.floating = kFloatNone;
      return;
    case kPropClear:
      if (v == "left" || v == "inline-start") s.clear = kClearLeft;
      else if (v == "right" || v == "inline-end") s.clear = kClearRight;
      else if (v == "both") s.clear = kClearBoth;
      else if (v == "none") s.clear = kClearNone;
      return;
    case kPropTop: case kPropRight: case kPropBottom: case kPropLeft:
      if (ParseLength(v, lc, len)) s.inset[id - kPropTop] = len;
      return;
    case kPropZIndex: {
      long long n;
      float f;
      if (v == "auto") s.zIndexAuto = true;
      else if (ParseInt(v, n)) {
        s.zIndexAuto = false;
        s.zIndex = (int)std::max(-100000LL, std::min(100000LL, n));
      } else if (ParseNumber(v, f)) {  // calc(-10)
        s.zIndexAuto = false;
        s.zIndex = (int)std::max(-100000.0f, std::min(100000.0f, std::floor(f + 0.5f)));
      }
      return;
    }
    case kPropWidth:
      if (ParseLength(v, lc, len, false) && len.kind != Length::kNone) s.width = len;
      return;
    case kPropHeight:
      if (ParseLength(v, lc, len, false) && len.kind != Length::kNone) s.height = len;
      return;
    case kPropMinWidth:
      if (ParseLength(v, lc, len, false)) s.minWidth = len.kind == Length::kNone ? Length::Auto() : len;
      return;
    case kPropMinHeight:
      if (ParseLength(v, lc, len, false)) s.minHeight = len.kind == Length::kNone ? Length::Auto() : len;
      return;
    case kPropMaxWidth:
      if (ParseLength(v, lc, len, false)) s.maxWidth = len.IsAuto() ? Length::None() : len;
      return;
    case kPropMaxHeight:
      if (ParseLength(v, lc, len, false)) s.maxHeight = len.IsAuto() ? Length::None() : len;
      return;
    case kPropMarginTop: case kPropMarginRight: case kPropMarginBottom: case kPropMarginLeft:
      if (ParseLength(v, lc, len) && (len.IsFixed() || len.IsAuto()))
        s.margin[id - kPropMarginTop] = len;
      return;
    case kPropPaddingTop: case kPropPaddingRight: case kPropPaddingBottom: case kPropPaddingLeft:
      if (ParseLength(v, lc, len, false) && len.IsFixed()) s.padding[id - kPropPaddingTop] = len;
      return;
    case kPropBorderTopWidth: case kPropBorderRightWidth: case kPropBorderBottomWidth:
    case kPropBorderLeftWidth: {
      BorderSide& b = s.border[id - kPropBorderTopWidth];
      if (v == "thin") b.width = 1;
      else if (v == "medium") b.width = 3;
      else if (v == "thick") b.width = 5;
      else if (ParseLength(v, lc, len, false) && len.IsFixed()) {
        float w = len.Resolve(0);
        b.width = w > 0 && w < 1 ? 1 : w;
      }
      return;
    }
    case kPropBorderTopStyle: case kPropBorderRightStyle: case kPropBorderBottomStyle:
    case kPropBorderLeftStyle: {
      BorderSide& b = s.border[id - kPropBorderTopStyle];
      if (v == "none") b.style = kBorderNone;
      else if (v == "hidden") b.style = kBorderHidden;
      else if (v == "solid") b.style = kBorderSolid;
      else if (v == "dashed") b.style = kBorderDashed;
      else if (v == "dotted") b.style = kBorderDotted;
      else if (v == "double") b.style = kBorderDouble;
      else if (v == "groove") b.style = kBorderGroove;
      else if (v == "ridge") b.style = kBorderRidge;
      else if (v == "inset") b.style = kBorderInset;
      else if (v == "outset") b.style = kBorderOutset;
      return;
    }
    case kPropBorderTopColor: case kPropBorderRightColor: case kPropBorderBottomColor:
    case kPropBorderLeftColor: {
      BorderSide& b = s.border[id - kPropBorderTopColor];
      if (v == "currentcolor") b.colorIsCurrent = true;
      else if (ParseColor(v, col, s.color)) {
        b.color = col;
        b.colorIsCurrent = false;
      }
      return;
    }
    case kPropBoxSizing:
      s.boxSizing = v == "border-box" ? kBorderBox : kContentBox;
      return;
    case kPropOverflowX: case kPropOverflowY: {
      Overflow o = kOverflowVisible;
      if (v == "hidden" || v == "clip") o = kOverflowHidden;
      else if (v == "scroll") o = kOverflowScroll;
      else if (v == "auto" || v == "overlay") o = kOverflowAuto;
      if (id == kPropOverflowX) s.overflowX = o;
      else s.overflowY = o;
      return;
    }
    case kPropVerticalAlign:
      if (v == "baseline") s.verticalAlign = kVaBaseline;
      else if (v == "middle") s.verticalAlign = kVaMiddle;
      else if (v == "top") s.verticalAlign = kVaTop;
      else if (v == "bottom") s.verticalAlign = kVaBottom;
      else if (v == "sub") s.verticalAlign = kVaSub;
      else if (v == "super") s.verticalAlign = kVaSuper;
      else if (v == "text-top") s.verticalAlign = kVaTextTop;
      else if (v == "text-bottom") s.verticalAlign = kVaTextBottom;
      else if (ParseLength(v, lc, len) && len.IsFixed()) {
        s.verticalAlign = kVaLength;
        s.verticalAlignPx = len.Resolve(s.lineHeight > 0 ? s.lineHeight : s.fontSize * 1.2f);
      }
      return;
    case kPropBackgroundColor:
      if (ParseColor(v, col, s.color)) s.backgroundColor = col;
      return;
    case kPropBackgroundImage: {
      if (v == "none") {
        s.backgroundImage.clear();
        return;
      }
      std::vector<std::string> layers = SplitValueCommas(value);
      // Use the first layer that is a url().
      for (size_t i = 0; i < layers.size(); ++i) {
        std::string u = ExtractUrl(layers[i]);
        if (!u.empty() && AsciiLower(layers[i]).find("gradient(") == std::string::npos) {
          if (ctx.baseUrl && !ctx.baseUrl->empty()) {
            Url base = Url::Parse(*ctx.baseUrl);
            Url r = base.Resolve(u);
            s.backgroundImage = r.valid() ? r.Spec() : std::string();
          } else {
            s.backgroundImage = u;
          }
          return;
        }
      }
      if (s.backgroundColor.transparent() && GradientColor(value, col)) s.backgroundColor = col;
      return;
    }
    case kPropBackgroundRepeat: {
      std::vector<std::string> t = SplitWhitespace(SplitValueCommas(v)[0]);
      if (t.empty()) return;
      if (t.size() == 2) {
        bool rx = t[0] != "no-repeat", ry = t[1] != "no-repeat";
        s.backgroundRepeat = rx && ry ? kRepeat : rx ? kRepeatX : ry ? kRepeatY : kNoRepeat;
      } else if (t[0] == "repeat-x") s.backgroundRepeat = kRepeatX;
      else if (t[0] == "repeat-y") s.backgroundRepeat = kRepeatY;
      else if (t[0] == "no-repeat") s.backgroundRepeat = kNoRepeat;
      else s.backgroundRepeat = kRepeat;
      return;
    }
    case kPropBackgroundPositionX: case kPropBackgroundPositionY: {
      std::string first = SplitValueCommas(v)[0];
      Length* target = id == kPropBackgroundPositionX ? &s.backgroundPosX : &s.backgroundPosY;
      if (first == "left" || first == "top") *target = Length::Pct(0);
      else if (first == "center") *target = Length::Pct(50);
      else if (first == "right" || first == "bottom") *target = Length::Pct(100);
      else if (ParseLength(first, lc, len) && len.IsFixed()) *target = len;
      return;
    }
    case kPropBackgroundSize: {
      std::string first = SplitValueCommas(v)[0];
      if (first == "cover") s.backgroundSizeMode = 1;
      else if (first == "contain") s.backgroundSizeMode = 2;
      else if (first == "auto" || first == "auto auto") s.backgroundSizeMode = 0;
      else {
        std::vector<std::string> t = SplitWhitespace(first);
        Length w, h;
        if (!t.empty() && ParseLength(t[0], lc, w)) {
          s.backgroundSizeMode = 3;
          s.backgroundSizeW = w;
          s.backgroundSizeH = Length::Auto();
          if (t.size() > 1 && ParseLength(t[1], lc, h)) s.backgroundSizeH = h;
        }
      }
      return;
    }
    case kPropOpacity: {
      size_t used;
      double n = ParseDoublePrefix(v, used);
      if (used > 0) {
        if (used < v.size() && v[used] == '%') n /= 100;
        s.opacity = (float)std::max(0.0, std::min(1.0, n));
      }
      return;
    }
    case kPropAspectRatio: {
      if (v == "auto") { s.aspectRatio = 0; return; }
      std::string r = v;
      if (StartsWith(r, "auto ")) r = r.substr(5);
      size_t slash = r.find('/');
      size_t used;
      double a = ParseDoublePrefix(Trim(r.substr(0, slash)), used);
      double b = 1;
      if (slash != std::string::npos) b = ParseDoublePrefix(Trim(r.substr(slash + 1)), used);
      if (a > 0 && b > 0) s.aspectRatio = (float)(a / b);
      return;
    }
    case kPropObjectFit:
      if (v == "contain") s.objectFit = kFitContain;
      else if (v == "cover") s.objectFit = kFitCover;
      else if (v == "none") s.objectFit = kFitNone;
      else if (v == "scale-down") s.objectFit = kFitScaleDown;
      else s.objectFit = kFitFill;
      return;
    case kPropFlexDirection:
      if (v == "row") s.flexDirection = kFlexRow;
      else if (v == "row-reverse") s.flexDirection = kFlexRowReverse;
      else if (v == "column") s.flexDirection = kFlexColumn;
      else if (v == "column-reverse") s.flexDirection = kFlexColumnReverse;
      return;
    case kPropFlexWrap:
      s.flexWrap = v == "wrap" || v == "wrap-reverse";
      return;
    case kPropJustifyContent:
      s.justifyContent = ParseFlexAlign(v, s.justifyContent);
      return;
    case kPropAlignItems:
      s.alignItems = ParseFlexAlign(v, s.alignItems);
      return;
    case kPropAlignSelf:
      s.alignSelf = ParseFlexAlign(v, s.alignSelf);
      return;
    case kPropAlignContent:
      s.alignContent = ParseFlexAlign(v, s.alignContent);
      return;
    case kPropFlexGrow: case kPropFlexShrink: {
      size_t used;
      double n = ParseDoublePrefix(v, used);
      if (used > 0 && n >= 0) {
        if (id == kPropFlexGrow) s.flexGrow = (float)n;
        else s.flexShrink = (float)n;
      }
      return;
    }
    case kPropFlexBasis:
      if (v == "content") s.flexBasis = Length::Auto();
      else if (ParseLength(v, lc, len, false)) s.flexBasis = len;
      return;
    case kPropOrder: {
      long long n;
      if (ParseInt(v, n)) s.order = (int)n;
      return;
    }
    case kPropRowGap: case kPropColumnGap:
      if (v == "normal") len = Length::Px(0);
      else if (!ParseLength(v, lc, len, false) || !len.IsFixed()) return;
      if (id == kPropRowGap) s.rowGap = len;
      else s.columnGap = len;
      return;
    case kPropGridTemplateColumns:
      ParseGridTemplate(value, lc, s);
      return;
    case kPropGridColumnStart: {
      int span = 1;
      ParseGridLine(v, s.gridColumnStart, span);
      s.gridColumnSpan = span;
      return;
    }
    case kPropGridColumnEnd: {
      int span = s.gridColumnSpan;
      int line = 0;
      ParseGridLine(v, line, span);
      s.gridColumnEnd = line;
      s.gridColumnSpan = span;
      return;
    }
    case kPropClip: {
      s.clippedAway = false;
      if (StartsWith(v, "rect(")) {
        std::string inner = v.substr(5, v.find(')') - 5);
        for (size_t i = 0; i < inner.size(); ++i)
          if (inner[i] == ',') inner[i] = ' ';
        std::vector<std::string> t = SplitWhitespace(inner);
        if (t.size() == 4) {
          Length a, b, c, d;
          bool ok = ParseLength(t[0], lc, a) && ParseLength(t[1], lc, b) &&
                    ParseLength(t[2], lc, c) && ParseLength(t[3], lc, d);
          if (ok && a.IsFixed() && b.IsFixed() && c.IsFixed() && d.IsFixed() &&
              (c.Resolve(0) <= a.Resolve(0) || b.Resolve(0) <= d.Resolve(0)))
            s.clippedAway = true;
        }
      }
      return;
    }
    case kPropBackgroundClip:
      s.backgroundClipText = v == "text";
      return;
    case kPropBorderTopLeftRadius: case kPropBorderTopRightRadius:
    case kPropBorderBottomRightRadius: case kPropBorderBottomLeftRadius: {
      std::vector<std::string> t = SplitValueTokens(v);
      if (!t.empty() && ParseLength(t[0], lc, len, false) && len.IsFixed())
        s.radius[id - kPropBorderTopLeftRadius] = len;
      return;
    }
    case kPropBoxShadow: {
      s.shadows.clear();
      if (v == "none") return;
      std::vector<std::string> layers = SplitValueCommas(value);
      for (size_t i = 0; i < layers.size(); ++i) {
        std::vector<std::string> t = SplitValueTokens(layers[i]);
        BoxShadow sh;
        sh.color = Color(0, 0, 0, 128);
        float nums[4] = {0, 0, 0, 0};
        int n = 0;
        for (size_t k = 0; k < t.size(); ++k) {
          std::string tk = AsciiLower(t[k]);
          if (tk == "inset") { sh.inset = true; continue; }
          Length l;
          if (n < 4 && ParseLength(tk, lc, l) && l.IsFixed() && !l.HasPercent()) {
            nums[n++] = l.Resolve(0);
            continue;
          }
          Color c;
          if (ParseColor(tk, c, s.color)) sh.color = c;
        }
        if (n < 2) continue;
        sh.x = nums[0];
        sh.y = nums[1];
        sh.blur = std::max(0.0f, nums[2]);
        sh.spread = nums[3];
        if (!sh.inset && sh.color.a > 0) s.shadows.push_back(sh);
      }
      return;
    }
    case kPropTranslate: {
      s.translateX = Length::Px(0);
      s.translateY = Length::Px(0);
      if (v == "none") return;
      std::vector<std::string> a = SplitValueTokens(v);
      Length l;
      if (!a.empty() && ParseLength(a[0], lc, l) && l.IsFixed()) s.translateX = l;
      if (a.size() > 1 && ParseLength(a[1], lc, l) && l.IsFixed()) s.translateY = l;
      return;
    }
    case kPropRotate: {
      s.rotateRad = 0;
      std::vector<std::string> a = SplitValueTokens(v);
      if (a.empty() || v == "none") return;
      float ang;
      // "rotate: z 45deg" / "rotate: 0 0 1 45deg": only rotation about z counts.
      if (ParseAngle(a.back(), ang)) s.rotateRad = ang;
      return;
    }
    case kPropScale: {
      s.scaleX = s.scaleY = 1;
      if (v == "none") return;
      std::vector<std::string> a = SplitValueTokens(v);
      float f;
      if (!a.empty() && ParseScaleFactor(a[0], f)) s.scaleX = s.scaleY = f;
      if (a.size() > 1 && ParseScaleFactor(a[1], f)) s.scaleY = f;
      s.transformHidden = s.scaleX == 0 || s.scaleY == 0;
      return;
    }
    case kPropBackfaceVisibility:
      s.backfaceHidden = v == "hidden";
      return;
    case kPropTransformOrigin: {
      std::vector<std::string> a = SplitValueTokens(v);
      Length x = Length::Pct(50), y = Length::Pct(50);
      for (size_t i = 0; i < a.size() && i < 2; ++i) {
        const std::string& t = a[i];
        Length l;
        if (t == "left") x = Length::Pct(0);
        else if (t == "right") x = Length::Pct(100);
        else if (t == "top") y = Length::Pct(0);
        else if (t == "bottom") y = Length::Pct(100);
        else if (t == "center") continue;
        else if (ParseLength(t, lc, l) && l.IsFixed()) {
          if (i == 0) x = l;
          else y = l;
        }
      }
      s.originX = x;
      s.originY = y;
      return;
    }
    case kPropTransform: {
      s.translateX = Length::Px(0);
      s.translateY = Length::Px(0);
      s.transformHidden = false;
      s.transformOps.clear();
      if (v == "none") return;
      std::vector<TransformOp> ops;
      if (!ParseTransformList(v, lc, ops)) return;
      bool onlyTranslate = true;
      for (size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].kind != TransformOp::kTranslate) onlyTranslate = false;
        if (ops[i].kind == TransformOp::kScale && (ops[i].v[0] == 0 || ops[i].v[1] == 0)) s.transformHidden = true;
      }
      if (onlyTranslate) {
        for (size_t i = 0; i < ops.size(); ++i) {
          s.translateX.px += ops[i].tx.px;
          s.translateX.pct += ops[i].tx.pct;
          s.translateY.px += ops[i].ty.px;
          s.translateY.pct += ops[i].ty.pct;
        }
      } else {
        s.transformOps = ops;
      }
      return;
    }
    case kPropAnimationName:
      s.hasAnimation = v != "none" && !v.empty();
      s.animName = s.hasAnimation ? value : std::string();
      return;
    case kPropAnimationDuration: s.animDuration = v; return;
    case kPropAnimationDelay: s.animDelay = v; return;
    case kPropAnimationIterationCount: s.animIterations = v; return;
    case kPropAnimationDirection: s.animDirection = v; return;
    case kPropAnimationFillMode: s.animFillMode = v; return;
    case kPropAnimationTimingFunction: s.animTiming = v; return;
    case kPropAnimationPlayState: s.animPlayState = v; return;
    case kPropTransitionProperty: s.transProperty = v == "none" ? std::string() : v; return;
    case kPropTransitionDuration: s.transDuration = v; return;
    case kPropTransitionDelay: s.transDelay = v; return;
    case kPropTransitionTimingFunction: s.transTiming = v; return;
    case kPropClipPath:
      s.clippedAway = v == "inset(50%)" || v == "inset(100%)" || StartsWith(v, "circle(0") ||
                      v == "polygon(0 0,0 0,0 0)" || v == "polygon(0 0, 0 0, 0 0)";
      return;
    case kPropFilter: {
      s.blur = 0;
      size_t p = v.find("blur(");
      if (p != std::string::npos) {
        Length l;
        size_t e = v.find(')', p);
        if (e != std::string::npos && ParseLength(v.substr(p + 5, e - p - 5), lc, l) && l.IsFixed())
          s.blur = l.Resolve(0);
      }
      return;
    }
    case kPropContent: {
      if (v == "none" || v == "normal") {
        s.hasContent = false;
        s.content.clear();
        return;
      }
      // Only string literals are supported (counters/attr() are dropped,
      // except attr() which the resolver substitutes beforehand).
      std::string out;
      bool any = false;
      for (size_t i = 0; i < value.size(); ++i) {
        char q = value[i];
        if (q == '"' || q == '\'') {
          size_t j = i + 1;
          while (j < value.size() && value[j] != q) {
            if (value[j] == '\\' && j + 1 < value.size()) {
              ++j;
              if (IsAsciiHex((unsigned char)value[j])) {
                unsigned long cp = 0;
                int n = 0;
                while (j < value.size() && n < 6 && IsAsciiHex((unsigned char)value[j])) {
                  cp = cp * 16 + HexValue(value[j]);
                  ++j;
                  ++n;
                }
                if (j < value.size() && value[j] == ' ') ++j;
                Utf8Append(out, (Codepoint)cp);
                continue;
              }
            }
            out += value[j];
            ++j;
          }
          i = j;
          any = true;
        }
      }
      s.content = out;
      s.hasContent = true;
      (void)any;
      return;
    }
    default:
      return;
  }
}

}  // namespace kite
