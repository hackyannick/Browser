#include "page/animation.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "base/strings.h"
#include "css/properties.h"

namespace kite {

static double (*g_testClock)() = 0;

void SetAnimationClockForTesting(double (*clock)()) { g_testClock = clock; }

double AnimationClockMs() {
  if (g_testClock) return g_testClock();
#ifdef _WIN32
  // GetTickCount wraps after 49 days; track the wrap-around.
  static DWORD last = 0;
  static double high = 0;
  DWORD t = GetTickCount();
  if (t < last) high += 4294967296.0;
  last = t;
  return high + t;
#else
  struct timeval tv;
  gettimeofday(&tv, 0);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
#endif
}

// ---------------------------------------------------------------------------
// Timing functions

TimingFunction TimingFunction::Parse(const std::string& raw) {
  std::string v = AsciiLower(Trim(raw));
  TimingFunction f;
  if (v == "linear" || StartsWith(v, "linear(")) {
    f.kind = kLinear;
  } else if (v == "ease-in") {
    f.x1 = 0.42f; f.y1 = 0; f.x2 = 1; f.y2 = 1;
  } else if (v == "ease-out") {
    f.x1 = 0; f.y1 = 0; f.x2 = 0.58f; f.y2 = 1;
  } else if (v == "ease-in-out") {
    f.x1 = 0.42f; f.y1 = 0; f.x2 = 0.58f; f.y2 = 1;
  } else if (v == "step-start") {
    f.kind = kSteps; f.steps = 1; f.stepPosition = 0;
  } else if (v == "step-end") {
    f.kind = kSteps; f.steps = 1; f.stepPosition = 1;
  } else if (StartsWith(v, "cubic-bezier(")) {
    std::vector<std::string> a = Split(v.substr(13, v.find(')') - 13), ',');
    if (a.size() == 4) {
      float n[4];
      bool ok = true;
      for (int i = 0; i < 4; ++i) {
        size_t used = 0;
        n[i] = (float)ParseDoublePrefix(Trim(a[i]), used);
        ok = ok && used > 0;
      }
      if (ok && n[0] >= 0 && n[0] <= 1 && n[2] >= 0 && n[2] <= 1) {
        f.x1 = n[0]; f.y1 = n[1]; f.x2 = n[2]; f.y2 = n[3];
      }
    }
  } else if (StartsWith(v, "steps(")) {
    std::vector<std::string> a = Split(v.substr(6, v.find(')') - 6), ',');
    long long n;
    if (!a.empty() && ParseInt(Trim(a[0]), n) && n > 0) {
      f.kind = kSteps;
      f.steps = (int)std::min(n, 10000LL);
      std::string pos = a.size() > 1 ? Trim(a[1]) : "end";
      f.stepPosition = pos == "start" || pos == "jump-start" ? 0 : pos == "jump-none" ? 2 : pos == "jump-both" ? 3 : 1;
    }
  }
  return f;
}

namespace {

float Bezier(float a, float b, float t) {  // 1D cubic with P0 = 0, P3 = 1
  float u = 1 - t;
  return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

}  // namespace

float TimingFunction::Apply(float t) const {
  if (kind == kLinear) return t;
  if (kind == kSteps) {
    int jumps = steps + (stepPosition == 3 ? 1 : 0) - (stepPosition == 2 ? 1 : 0);
    if (jumps < 1) jumps = 1;
    float cur = std::floor(t * steps);
    if (stepPosition == 0 || stepPosition == 3) cur += 1;
    if (t >= 0 && cur < 0) cur = 0;
    if (t <= 1 && cur > jumps) cur = (float)jumps;
    return cur / jumps;
  }
  if (t <= 0 || t >= 1) return t;
  // Solve x(s) = t, then return y(s).
  float lo = 0, hi = 1, s = t;
  for (int i = 0; i < 8; ++i) {  // Newton
    float x = Bezier(x1, x2, s) - t;
    if (std::fabs(x) < 1e-5f) return Bezier(y1, y2, s);
    float u = 1 - s;
    float dx = 3 * u * u * x1 + 6 * u * s * (x2 - x1) + 3 * s * s * (1 - x2);
    if (std::fabs(dx) < 1e-6f) break;
    s -= x / dx;
    if (s < 0 || s > 1) break;
  }
  s = t;
  for (int i = 0; i < 40; ++i) {  // bisection fallback
    float x = Bezier(x1, x2, s);
    if (std::fabs(x - t) < 1e-5f) break;
    if (x < t) lo = s;
    else hi = s;
    s = (lo + hi) / 2;
  }
  return Bezier(y1, y2, s);
}

// ---------------------------------------------------------------------------
// Interpolation of computed values

namespace {

float Mix(float a, float b, float t) { return a + (b - a) * t; }

Color MixColor(Color a, Color b, float t) {
  // Interpolate premultiplied, as browsers do.
  float aa = a.a / 255.0f, ba = b.a / 255.0f, oa = Mix(aa, ba, t);
  Color c;
  c.a = (uint8_t)std::max(0.0f, std::min(255.0f, oa * 255 + 0.5f));
  if (oa <= 0) return Color(0, 0, 0, 0);
  c.r = (uint8_t)std::max(0.0f, std::min(255.0f, Mix(a.r * aa, b.r * ba, t) / oa + 0.5f));
  c.g = (uint8_t)std::max(0.0f, std::min(255.0f, Mix(a.g * aa, b.g * ba, t) / oa + 0.5f));
  c.b = (uint8_t)std::max(0.0f, std::min(255.0f, Mix(a.b * aa, b.b * ba, t) / oa + 0.5f));
  return c;
}

// Returns false when the two lengths cannot be interpolated (auto ...).
bool MixLength(const Length& a, const Length& b, float t, Length& out) {
  if (a.kind != Length::kFixed || b.kind != Length::kFixed) return false;
  out = Length::Px(Mix(a.px, b.px, t));
  out.pct = Mix(a.pct, b.pct, t);
  return true;
}

bool SameLength(const Length& a, const Length& b) { return a.kind == b.kind && a.px == b.px && a.pct == b.pct; }

bool IsLayoutProperty(int id) {
  switch (id) {
    case kPropOpacity: case kPropColor: case kPropBackgroundColor: case kPropBorderTopColor:
    case kPropBorderRightColor: case kPropBorderBottomColor: case kPropBorderLeftColor:
    case kPropTransform: case kPropTranslate: case kPropBoxShadow: case kPropFilter:
    case kPropRotate: case kPropScale: case kPropTransformOrigin: case kPropPerspective:
    case kPropBorderTopLeftRadius: case kPropBorderTopRightRadius: case kPropBorderBottomRightRadius:
    case kPropBorderBottomLeftRadius: case kPropBackgroundPositionX: case kPropBackgroundPositionY:
    case kPropVisibility: case kPropTextDecorationColor: case kPropFill: case kPropStroke:
      return false;
    default:
      return true;
  }
}

Length* LengthField(int id, ComputedStyle& s) {
  switch (id) {
    case kPropWidth: return &s.width;
    case kPropHeight: return &s.height;
    case kPropMinWidth: return &s.minWidth;
    case kPropMinHeight: return &s.minHeight;
    case kPropMaxWidth: return &s.maxWidth;
    case kPropMaxHeight: return &s.maxHeight;
    case kPropMarginTop: case kPropMarginRight: case kPropMarginBottom: case kPropMarginLeft:
      return &s.margin[id - kPropMarginTop];
    case kPropPaddingTop: case kPropPaddingRight: case kPropPaddingBottom: case kPropPaddingLeft:
      return &s.padding[id - kPropPaddingTop];
    case kPropTop: return &s.inset[0];
    case kPropRight: return &s.inset[1];
    case kPropBottom: return &s.inset[2];
    case kPropLeft: return &s.inset[3];
    case kPropBorderTopLeftRadius: case kPropBorderTopRightRadius: case kPropBorderBottomRightRadius:
    case kPropBorderBottomLeftRadius:
      return &s.radius[id - kPropBorderTopLeftRadius];
    case kPropBackgroundPositionX: return &s.backgroundPosX;
    case kPropBackgroundPositionY: return &s.backgroundPosY;
    case kPropFlexBasis: return &s.flexBasis;
    case kPropRowGap: return &s.rowGap;
    case kPropColumnGap: return &s.columnGap;
    case kPropTextIndent: return &s.textIndent;
    default: return 0;
  }
}

Color* ColorField(int id, ComputedStyle& s) {
  switch (id) {
    case kPropColor: return &s.color;
    case kPropBackgroundColor: return &s.backgroundColor;
    case kPropBorderTopColor: case kPropBorderRightColor: case kPropBorderBottomColor:
    case kPropBorderLeftColor:
      return &s.border[id - kPropBorderTopColor].color;
    default: return 0;
  }
}

float* FloatField(int id, ComputedStyle& s) {
  switch (id) {
    case kPropOpacity: return &s.opacity;
    case kPropFontSize: return &s.fontSize;
    case kPropLetterSpacing: return &s.letterSpacing;
    case kPropWordSpacing: return &s.wordSpacing;
    case kPropFlexGrow: return &s.flexGrow;
    case kPropFlexShrink: return &s.flexShrink;
    case kPropFilter: return &s.blur;
    case kPropRotate: return &s.rotateRad;
    case kPropPerspective: return &s.perspective;
    case kPropBorderTopWidth: case kPropBorderRightWidth: case kPropBorderBottomWidth:
    case kPropBorderLeftWidth:
      return &s.border[id - kPropBorderTopWidth].width;
    default: return 0;
  }
}

// Effective border color (resolving currentColor).
Color BorderColor(const ComputedStyle& s, int side) {
  return s.border[side].colorIsCurrent ? s.color : s.border[side].color;
}

bool PropEqual(int id, const ComputedStyle& a, const ComputedStyle& b) {
  ComputedStyle& ma = const_cast<ComputedStyle&>(a);
  ComputedStyle& mb = const_cast<ComputedStyle&>(b);
  if (id >= kPropBorderTopColor && id <= kPropBorderLeftColor) {
    int side = id - kPropBorderTopColor;
    return BorderColor(a, side) == BorderColor(b, side);
  }
  if (Length* la = LengthField(id, ma)) return SameLength(*la, *LengthField(id, mb));
  if (Color* ca = ColorField(id, ma)) return *ca == *ColorField(id, mb);
  if (float* fa = FloatField(id, ma)) return *fa == *FloatField(id, mb);
  switch (id) {
    case kPropTransform: case kPropTranslate: {
      if (!(SameLength(a.translateX, b.translateX) && SameLength(a.translateY, b.translateY) &&
            a.transformHidden == b.transformHidden && a.transformOps.size() == b.transformOps.size()))
        return false;
      for (size_t i = 0; i < a.transformOps.size(); ++i) {
        const TransformOp &x = a.transformOps[i], &y = b.transformOps[i];
        if (x.kind != y.kind || !SameLength(x.tx, y.tx) || !SameLength(x.ty, y.ty)) return false;
        for (int k = 0; k < 6; ++k)
          if (x.v[k] != y.v[k]) return false;
      }
      return true;
    }
    case kPropScale: return a.scaleX == b.scaleX && a.scaleY == b.scaleY;
    case kPropTransformOrigin: return SameLength(a.originX, b.originX) && SameLength(a.originY, b.originY);
    case kPropVisibility: return a.visible == b.visible;
    case kPropBoxShadow: {
      if (a.shadows.size() != b.shadows.size()) return false;
      for (size_t i = 0; i < a.shadows.size(); ++i) {
        const BoxShadow &x = a.shadows[i], &y = b.shadows[i];
        if (x.x != y.x || x.y != y.y || x.blur != y.blur || x.spread != y.spread || x.color != y.color ||
            x.inset != y.inset)
          return false;
      }
      return true;
    }
  }
  return true;
}

// Properties a transition with "all" watches.
const int kTransitionable[] = {
    kPropOpacity, kPropColor, kPropBackgroundColor, kPropBorderTopColor, kPropBorderRightColor,
    kPropBorderBottomColor, kPropBorderLeftColor, kPropTransform, kPropBoxShadow, kPropFilter,
    kPropWidth, kPropHeight, kPropMinWidth, kPropMinHeight, kPropMaxWidth, kPropMaxHeight,
    kPropMarginTop, kPropMarginRight, kPropMarginBottom, kPropMarginLeft, kPropPaddingTop,
    kPropPaddingRight, kPropPaddingBottom, kPropPaddingLeft, kPropTop, kPropRight, kPropBottom,
    kPropLeft, kPropBorderTopLeftRadius, kPropBorderTopRightRadius, kPropBorderBottomRightRadius,
    kPropBorderBottomLeftRadius, kPropFontSize, kPropLetterSpacing, kPropVisibility,
    kPropBorderTopWidth, kPropBorderRightWidth, kPropBorderBottomWidth, kPropBorderLeftWidth,
    kPropBackgroundPositionX, kPropBackgroundPositionY, kPropRotate, kPropScale, kPropTransformOrigin, kPropPerspective};

const float kPiF = 3.14159265358979f;

std::vector<float> OpsMatrix(const std::vector<TransformOp>& ops) {
  ComputedStyle tmp;
  tmp.transformOps = ops;
  float m[6];
  TransformMatrix(tmp, 0, 0, m);
  return std::vector<float>(m, m + 6);
}

struct Decomposed {
  float tx = 0, ty = 0, sx = 1, sy = 1, angle = 0, skew = 0;
};

Decomposed Decompose(const std::vector<float>& m) {
  // Columns of the linear part: (a, b) and (c, d).
  Decomposed d;
  d.tx = m[4];
  d.ty = m[5];
  float r0x = m[0], r0y = m[1], r1x = m[2], r1y = m[3];
  d.sx = std::sqrt(r0x * r0x + r0y * r0y);
  if (d.sx != 0) {
    r0x /= d.sx;
    r0y /= d.sx;
  }
  d.skew = r0x * r1x + r0y * r1y;
  r1x -= r0x * d.skew;
  r1y -= r0y * d.skew;
  d.sy = std::sqrt(r1x * r1x + r1y * r1y);
  if (d.sy != 0) {
    r1x /= d.sy;
    r1y /= d.sy;
    d.skew /= d.sy;
  }
  if (r0x * r1y - r0y * r1x < 0) {  // mirrored
    d.sx = -d.sx;
    d.skew = -d.skew;
    r0x = -r0x;
    r0y = -r0y;
  }
  d.angle = std::atan2(r0y, r0x);
  return d;
}

TransformOp Recompose(const Decomposed& d) {
  // translate * rotate(angle) * skewX(atan(skew)) * scale(sx, sy)
  float c = std::cos(d.angle), sn = std::sin(d.angle);
  TransformOp op;
  op.kind = TransformOp::kMatrix;
  op.v[0] = c * d.sx;
  op.v[1] = sn * d.sx;
  op.v[2] = (c * d.skew - sn) * d.sy;
  op.v[3] = (sn * d.skew + c) * d.sy;
  op.v[4] = d.tx;
  op.v[5] = d.ty;
  return op;
}

// Transform list as written (translate-only lists live in translateX/Y).
std::vector<TransformOp> EffectiveOps(const ComputedStyle& s) {
  if (!s.transformOps.empty()) return s.transformOps;
  std::vector<TransformOp> ops;
  if (!s.translateX.IsZero() || !s.translateY.IsZero()) {
    TransformOp op;
    op.kind = TransformOp::kTranslate;
    op.tx = s.translateX;
    op.ty = s.translateY;
    ops.push_back(op);
  }
  return ops;
}

TransformOp IdentityLike(const TransformOp& o) {
  TransformOp id;
  id.kind = o.kind;
  id.tx = id.ty = Length::Px(0);
  if (o.kind == TransformOp::kScale) id.v[0] = id.v[1] = id.v[2] = 1;
  if (o.kind == TransformOp::kMatrix) id.v[0] = id.v[3] = 1;
  if (o.kind == TransformOp::kMatrix3d) id.v[0] = id.v[5] = id.v[10] = id.v[15] = 1;
  if (o.kind == TransformOp::kRotate3d)
    for (int k = 0; k < 3; ++k) id.v[k] = o.v[k];  // same axis, no rotation
  if (o.kind == TransformOp::kPerspective) id.v[0] = o.v[0];
  return id;
}

void MixTransform(const ComputedStyle& a, const ComputedStyle& b, float t, ComputedStyle& out) {
  std::vector<TransformOp> oa = EffectiveOps(a), ob = EffectiveOps(b);
  // A missing list counts as the identity of the other one's functions.
  if (oa.empty())
    for (size_t i = 0; i < ob.size(); ++i) oa.push_back(IdentityLike(ob[i]));
  if (ob.empty())
    for (size_t i = 0; i < oa.size(); ++i) ob.push_back(IdentityLike(oa[i]));
  bool same = oa.size() == ob.size();
  for (size_t i = 0; same && i < oa.size(); ++i) same = oa[i].kind == ob[i].kind;
  std::vector<TransformOp> res;
  if (!same) {
    // Different function lists: interpolate the decomposed matrices
    // (CSS Transforms "unmatrix" for 2D). Percent translations count as 0.
    Decomposed da = Decompose(OpsMatrix(oa)), db = Decompose(OpsMatrix(ob));
    if (std::fabs(da.angle - db.angle) > kPiF) {  // shortest way round
      if (da.angle > db.angle) da.angle -= 2 * kPiF;
      else db.angle -= 2 * kPiF;
    }
    Decomposed d;
    d.tx = Mix(da.tx, db.tx, t);
    d.ty = Mix(da.ty, db.ty, t);
    d.sx = Mix(da.sx, db.sx, t);
    d.sy = Mix(da.sy, db.sy, t);
    d.angle = Mix(da.angle, db.angle, t);
    d.skew = Mix(da.skew, db.skew, t);
    res.push_back(Recompose(d));
  } else {
    for (size_t i = 0; i < oa.size(); ++i) {
      TransformOp r = oa[i];
      if (r.kind == TransformOp::kTranslate) {
        Length l;
        if (MixLength(oa[i].tx, ob[i].tx, t, l)) r.tx = l;
        if (MixLength(oa[i].ty, ob[i].ty, t, l)) r.ty = l;
      } else {
        for (int k = 0; k < 16; ++k) r.v[k] = Mix(oa[i].v[k], ob[i].v[k], t);
      }
      res.push_back(r);
    }
  }
  out.transformOps.clear();
  out.translateX = out.translateY = Length::Px(0);
  bool onlyTranslate = true;
  for (size_t i = 0; i < res.size(); ++i)
    if (res[i].kind != TransformOp::kTranslate || res[i].v[2] != 0) onlyTranslate = false;
  if (onlyTranslate) {
    for (size_t i = 0; i < res.size(); ++i) {
      out.translateX.px += res[i].tx.px;
      out.translateX.pct += res[i].tx.pct;
      out.translateY.px += res[i].ty.px;
      out.translateY.pct += res[i].ty.pct;
    }
  } else {
    out.transformOps = res;
  }
}

// Writes the value of |id| interpolated between |a| and |b| into |out|.
// Non-interpolable values flip at t = 0.5.
void MixProperty(int id, const ComputedStyle& a, const ComputedStyle& b, float t, ComputedStyle& out) {
  ComputedStyle& ma = const_cast<ComputedStyle&>(a);
  ComputedStyle& mb = const_cast<ComputedStyle&>(b);
  if (id >= kPropBorderTopColor && id <= kPropBorderLeftColor) {
    int side = id - kPropBorderTopColor;
    out.border[side].color = MixColor(BorderColor(a, side), BorderColor(b, side), t);
    out.border[side].colorIsCurrent = false;
    return;
  }
  if (Length* la = LengthField(id, ma)) {
    Length r;
    if (MixLength(*la, *LengthField(id, mb), t, r)) *LengthField(id, out) = r;
    else *LengthField(id, out) = t < 0.5f ? *la : *LengthField(id, mb);
    return;
  }
  if (Color* ca = ColorField(id, ma)) {
    *ColorField(id, out) = MixColor(*ca, *ColorField(id, mb), t);
    return;
  }
  if (float* fa = FloatField(id, ma)) {
    *FloatField(id, out) = Mix(*fa, *FloatField(id, mb), t);
    if (id == kPropOpacity) out.opacity = std::max(0.0f, std::min(1.0f, out.opacity));
    return;
  }
  switch (id) {
    case kPropTransform:
    case kPropTranslate: {
      Length r;
      if (id == kPropTransform && (!a.transformOps.empty() || !b.transformOps.empty())) {
        MixTransform(a, b, t, out);
      } else {
        if (MixLength(a.translateX, b.translateX, t, r)) out.translateX = r;
        if (MixLength(a.translateY, b.translateY, t, r)) out.translateY = r;
      }
      // scale(0) <-> scale(1): visible as soon as the element starts growing.
      out.transformHidden = t <= 0 ? a.transformHidden : t >= 1 ? b.transformHidden
                                                                : (a.transformHidden && b.transformHidden);
      return;
    }
    case kPropScale:
      out.scaleX = Mix(a.scaleX, b.scaleX, t);
      out.scaleY = Mix(a.scaleY, b.scaleY, t);
      out.transformHidden = out.scaleX == 0 || out.scaleY == 0;
      return;
    case kPropTransformOrigin: {
      Length r;
      out.originX = MixLength(a.originX, b.originX, t, r) ? r : (t < 0.5f ? a.originX : b.originX);
      out.originY = MixLength(a.originY, b.originY, t, r) ? r : (t < 0.5f ? a.originY : b.originY);
      return;
    }
    case kPropVisibility:
      out.visible = t <= 0 ? a.visible : t >= 1 ? b.visible : (a.visible || b.visible);
      return;
    case kPropBoxShadow: {
      std::vector<BoxShadow> sa = a.shadows, sb = b.shadows;
      size_t n = std::max(sa.size(), sb.size());
      while (sa.size() < n) {
        BoxShadow z = sb[sa.size()];
        z.x = z.y = z.blur = z.spread = 0;
        z.color = Color(0, 0, 0, 0);
        sa.push_back(z);
      }
      while (sb.size() < n) {
        BoxShadow z = sa[sb.size()];
        z.x = z.y = z.blur = z.spread = 0;
        z.color = Color(0, 0, 0, 0);
        sb.push_back(z);
      }
      out.shadows.resize(n);
      for (size_t i = 0; i < n; ++i) {
        if (sa[i].inset != sb[i].inset) {
          out.shadows[i] = t < 0.5f ? sa[i] : sb[i];
          continue;
        }
        out.shadows[i].x = Mix(sa[i].x, sb[i].x, t);
        out.shadows[i].y = Mix(sa[i].y, sb[i].y, t);
        out.shadows[i].blur = std::max(0.0f, Mix(sa[i].blur, sb[i].blur, t));
        out.shadows[i].spread = Mix(sa[i].spread, sb[i].spread, t);
        out.shadows[i].color = MixColor(sa[i].color, sb[i].color, t);
        out.shadows[i].inset = sa[i].inset;
      }
      return;
    }
    default:
      CopyProperty(id, out, t < 0.5f ? a : b);
  }
}

std::vector<std::string> ListAt(const std::string& list) { return SplitValueCommas(list); }

std::string Item(const std::vector<std::string>& v, size_t i, const char* def) {
  if (v.empty()) return def;
  return Trim(v[i % v.size()]);
}

float TimeMs(const std::string& t) {
  size_t used = 0;
  double v = ParseDoublePrefix(t, used);
  if (used == 0) return 0;
  std::string unit = AsciiLower(t.substr(used));
  if (unit == "ms") return (float)v;
  if (unit == "s") return (float)(v * 1000);
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Controller

struct AnimationController::Animation {
  std::string name;
  double start = 0;
  float duration = 0, delay = 0;
  float iterations = 1;  // < 0: infinite
  int direction = 0;     // normal, reverse, alternate, alternate-reverse
  int fill = 0;          // none, forwards, backwards, both
  TimingFunction timing;
  bool paused = false;
  double pausedAt = 0;
  int lastIteration = -1;
  bool started = false, ended = false;
  struct Frame {
    float offset;
    std::unique_ptr<ComputedStyle> style;
    std::vector<int> props;
    TimingFunction timing;
  };
  std::vector<Frame> frames;
  std::vector<int> props;
};

struct AnimationController::Transition {
  int prop;
  double start;
  float duration, delay;
  TimingFunction timing;
  std::unique_ptr<ComputedStyle> from, to;
};

struct AnimationController::Entry {
  std::unique_ptr<ComputedStyle> base;
  std::vector<std::unique_ptr<Animation> > animations;
  std::vector<std::unique_ptr<Transition> > transitions;
};

AnimationController::AnimationController() {}
AnimationController::~AnimationController() {}

void AnimationController::Clear() {
  entries_.clear();
  before_.clear();
  events_.clear();
}

static void CollectElements(Node* n, std::vector<Node*>& out) {
  if (n->IsElement() && n->style) out.push_back(n);
  for (size_t i = 0; i < n->children.size(); ++i) CollectElements(n->children[i].get(), out);
}

void AnimationController::BeforeRestyle(Document* doc) {
  before_.clear();
  if (!doc) return;
  std::vector<Node*> els;
  CollectElements(doc->root.get(), els);
  for (size_t i = 0; i < els.size(); ++i) {
    const ComputedStyle* s = els[i]->style;
    if (s->transProperty.empty() && !entries_.count(els[i])) continue;
    std::unique_ptr<ComputedStyle> copy(new ComputedStyle);
    copy->CopyFrom(*s);
    before_[els[i]] = std::move(copy);
  }
}

static std::unique_ptr<ComputedStyle> CopyStyle(const ComputedStyle& s) {
  std::unique_ptr<ComputedStyle> c(new ComputedStyle);
  c->CopyFrom(s);
  return c;
}

void AnimationController::AfterRestyle(Document* doc, const StyleResolver& resolver, const Context& ctx,
                                       double now) {
  std::map<Node*, std::unique_ptr<Entry> > next;
  if (doc) {
    std::vector<Node*> els;
    CollectElements(doc->root.get(), els);
    for (size_t e = 0; e < els.size(); ++e) {
      Node* n = els[e];
      const ComputedStyle& s = *n->style;
      std::map<Node*, std::unique_ptr<Entry> >::iterator prevIt = entries_.find(n);
      Entry* prev = prevIt == entries_.end() ? 0 : prevIt->second.get();
      std::map<Node*, std::unique_ptr<ComputedStyle> >::iterator old = before_.find(n);
      bool wantsTransitions = !s.transProperty.empty() || (prev && !prev->transitions.empty());
      if (s.animName.empty() && !wantsTransitions) continue;
      std::unique_ptr<Entry> entry(new Entry);
      entry->base = CopyStyle(s);

      // Transitions.
      if (!s.transProperty.empty() || prev) {
        std::vector<std::string> props = ListAt(s.transProperty), durs = ListAt(s.transDuration),
                                 delays = ListAt(s.transDelay), timings = ListAt(s.transTiming);
        std::map<int, size_t> watched;  // property -> index in the lists
        for (size_t i = 0; i < props.size(); ++i) {
          std::string p = Trim(props[i]);
          if (p == "all") {
            for (size_t k = 0; k < sizeof kTransitionable / sizeof kTransitionable[0]; ++k)
              watched[kTransitionable[k]] = i;
          } else {
            std::vector<std::pair<int, std::string> > ids;
            if (ExpandProperty(p, "0", ids))
              for (size_t k = 0; k < ids.size(); ++k) watched[ids[k].first] = i;
          }
        }
        // Keep running transitions whose target did not change.
        if (prev) {
          for (size_t i = 0; i < prev->transitions.size(); ++i) {
            std::unique_ptr<Transition>& t = prev->transitions[i];
            if (watched.count(t->prop) && PropEqual(t->prop, *t->to, s)) {
              t->to = CopyStyle(s);
              entry->transitions.push_back(std::move(t));
            }
          }
        }
        if (old != before_.end()) {
          for (std::map<int, size_t>::iterator it = watched.begin(); it != watched.end(); ++it) {
            int prop = it->first;
            if (PropEqual(prop, *old->second, s)) continue;
            bool running = false;
            for (size_t k = 0; k < entry->transitions.size(); ++k)
              if (entry->transitions[k]->prop == prop) running = true;
            if (running) continue;
            float dur = TimeMs(Item(durs, it->second, "0s")), del = TimeMs(Item(delays, it->second, "0s"));
            if (dur <= 0 || dur + del <= 0) continue;
            std::unique_ptr<Transition> t(new Transition);
            t->prop = prop;
            t->start = now;
            t->duration = dur;
            t->delay = del;
            t->timing = TimingFunction::Parse(Item(timings, it->second, "ease"));
            t->from = CopyStyle(*old->second);
            t->to = CopyStyle(s);
            entry->transitions.push_back(std::move(t));
          }
        }
      }

      // Animations.
      if (!s.animName.empty()) {
        std::vector<std::string> names = ListAt(s.animName), durs = ListAt(s.animDuration),
                                 delays = ListAt(s.animDelay), iters = ListAt(s.animIterations),
                                 dirs = ListAt(s.animDirection), fills = ListAt(s.animFillMode),
                                 timings = ListAt(s.animTiming), states = ListAt(s.animPlayState);
        for (size_t i = 0; i < names.size() && i < 16; ++i) {
          std::string name = Trim(names[i]);
          if (name.size() >= 2 && (name[0] == '"' || name[0] == '\'')) name = name.substr(1, name.size() - 2);
          if (name.empty() || name == "none") continue;
          const KeyframesRule* kf = resolver.FindKeyframes(name);
          if (!kf) continue;
          std::unique_ptr<Animation> a;
          if (prev)
            for (size_t k = 0; k < prev->animations.size(); ++k)
              if (prev->animations[k] && prev->animations[k]->name == name) a = std::move(prev->animations[k]);
          if (!a) {
            a.reset(new Animation);
            a->name = name;
            a->start = now;
          }
          a->duration = TimeMs(Item(durs, i, "0s"));
          a->delay = TimeMs(Item(delays, i, "0s"));
          std::string it = Item(iters, i, "1");
          if (it == "infinite") a->iterations = -1;
          else {
            size_t used = 0;
            double v = ParseDoublePrefix(it, used);
            a->iterations = used > 0 && v >= 0 ? (float)v : 1;
          }
          std::string dir = Item(dirs, i, "normal");
          a->direction = dir == "reverse" ? 1 : dir == "alternate" ? 2 : dir == "alternate-reverse" ? 3 : 0;
          std::string fill = Item(fills, i, "none");
          a->fill = fill == "forwards" ? 1 : fill == "backwards" ? 2 : fill == "both" ? 3 : 0;
          a->timing = TimingFunction::Parse(Item(timings, i, "ease"));
          bool paused = Item(states, i, "running") == "paused";
          if (paused && !a->paused) a->pausedAt = now;
          if (!paused && a->paused) a->start += now - a->pausedAt;
          a->paused = paused;
          // Keyframe styles: base style plus the keyframe's declarations.
          a->frames.clear();
          a->props.clear();
          std::vector<const Keyframe*> sorted;
          for (size_t k = 0; k < kf->frames.size(); ++k) sorted.push_back(&kf->frames[k]);
          std::stable_sort(sorted.begin(), sorted.end(),
                           [](const Keyframe* x, const Keyframe* y) { return x->offset < y->offset; });
          ApplyContext actx;
          actx.parent = n->parent && n->parent->style ? n->parent->style : &s;
          actx.rootFontSize = ctx.rootFontSize;
          actx.viewportW = ctx.viewportW;
          actx.viewportH = ctx.viewportH;
          actx.baseUrl = &ctx.baseUrl;
          for (size_t k = 0; k < sorted.size(); ++k) {
            Animation::Frame f;
            f.offset = sorted[k]->offset;
            f.style = CopyStyle(s);
            f.timing = a->timing;
            for (size_t d = 0; d < sorted[k]->declarations.size(); ++d) {
              const Declaration& decl = sorted[k]->declarations[d];
              if (decl.important) continue;
              if (decl.property == "animation-timing-function") {
                f.timing = TimingFunction::Parse(decl.value);
                continue;
              }
              std::vector<std::pair<int, std::string> > longhands;
              if (!ExpandProperty(decl.property, decl.value, longhands)) continue;
              for (size_t q = 0; q < longhands.size(); ++q) {
                int id = longhands[q].first;
                if (id >= kPropAnimationName && id <= kPropTransitionTimingFunction) continue;
                if (id == kPropDisplay) continue;
                std::string value;
                if (!SubstituteCssVars(longhands[q].second, s, value)) continue;
                ApplyProperty(id, value, *f.style, actx);
                f.props.push_back(id);
                if (std::find(a->props.begin(), a->props.end(), id) == a->props.end()) a->props.push_back(id);
              }
            }
            a->frames.push_back(std::move(f));
          }
          entry->animations.push_back(std::move(a));
        }
      }
      if (entry->animations.empty() && entry->transitions.empty()) continue;
      next[n] = std::move(entry);
    }
  }
  entries_.swap(next);
  before_.clear();
  Apply(now);
}

AnimationController::TickResult AnimationController::Tick(double now) { return Apply(now); }

AnimationController::TickResult AnimationController::Apply(double now) {
  int result = kNothing;
  for (std::map<Node*, std::unique_ptr<Entry> >::iterator it = entries_.begin(); it != entries_.end(); ++it) {
    Node* n = it->first;
    Entry& e = *it->second;
    if (!n->style) continue;
    ComputedStyle& st = *n->style;
    // Reset the animated properties to their base values.
    for (size_t i = 0; i < e.transitions.size(); ++i) CopyProperty(e.transitions[i]->prop, st, *e.base);
    for (size_t i = 0; i < e.animations.size(); ++i)
      for (size_t k = 0; k < e.animations[i]->props.size(); ++k) CopyProperty(e.animations[i]->props[k], st, *e.base);
    // Transitions.
    for (size_t i = 0; i < e.transitions.size();) {
      Transition& t = *e.transitions[i];
      float p = instant_ ? 1 : (float)((now - t.start - t.delay) / t.duration);
      if (p >= 1) {
        CopyProperty(t.prop, st, *t.to);
        result |= IsLayoutProperty(t.prop) ? kRelayout : kRepaint;
        events_.push_back(std::make_pair(n, std::string("transitionend")));
        e.transitions.erase(e.transitions.begin() + i);
        continue;
      }
      MixProperty(t.prop, *t.from, *t.to, t.timing.Apply(std::max(0.0f, p)), st);
      result |= IsLayoutProperty(t.prop) ? kRelayout : kRepaint;
      ++i;
    }
    // Animations (later ones override earlier ones).
    for (size_t i = 0; i < e.animations.size(); ++i) {
      Animation& a = *e.animations[i];
      if (a.frames.empty()) continue;
      double t = (a.paused ? a.pausedAt : now) - a.start - a.delay;
      float iters = a.iterations;
      if (instant_ && iters >= 0) t = 1e18;
      double active = iters < 0 ? 1e300 : (double)a.duration * iters;
      int iteration;
      double local;  // 0..1 within the iteration
      bool after = false;
      if (t < 0) {
        if (!(a.fill & 2)) continue;  // backwards
        iteration = 0;
        local = 0;
      } else if (t >= active) {
        after = true;
        if (!a.ended && !a.paused) {
          a.ended = true;
          events_.push_back(std::make_pair(n, std::string("animationend")));
        }
        if (!(a.fill & 1)) continue;  // forwards
        if (iters <= 0) {
          iteration = 0;
          local = 0;
        } else {
          double whole = std::floor(iters);
          iteration = (int)(iters == whole ? whole - 1 : whole);
          local = iters == whole ? 1 : iters - whole;
        }
      } else {
        if (a.duration <= 0) {
          iteration = 0;
          local = 1;
        } else {
          iteration = (int)std::floor(t / a.duration);
          local = (t - (double)iteration * a.duration) / a.duration;
        }
        if (!a.started) {
          a.started = true;
          events_.push_back(std::make_pair(n, std::string("animationstart")));
        }
        if (a.lastIteration >= 0 && iteration != a.lastIteration)
          events_.push_back(std::make_pair(n, std::string("animationiteration")));
        a.lastIteration = iteration;
        a.ended = false;
      }
      (void)after;
      bool reverse = a.direction == 1 || (a.direction == 2 && iteration % 2 == 1) ||
                     (a.direction == 3 && iteration % 2 == 0);
      float p = (float)(reverse ? 1 - local : local);
      // Each property between its surrounding keyframes.
      for (size_t k = 0; k < a.props.size(); ++k) {
        int prop = a.props[k];
        const Animation::Frame* lo = 0;
        const Animation::Frame* hi = 0;
        for (size_t f = 0; f < a.frames.size(); ++f) {
          const Animation::Frame& fr = a.frames[f];
          if (std::find(fr.props.begin(), fr.props.end(), prop) == fr.props.end()) continue;
          if (fr.offset <= p) lo = &fr;
          else if (!hi) hi = &fr;
        }
        const ComputedStyle* from = lo ? lo->style.get() : e.base.get();
        const ComputedStyle* to = hi ? hi->style.get() : e.base.get();
        float o0 = lo ? lo->offset : 0, o1 = hi ? hi->offset : 1;
        const TimingFunction& tf = lo ? lo->timing : a.timing;
        float seg = o1 > o0 ? (p - o0) / (o1 - o0) : 1;
        MixProperty(prop, *from, *to, tf.Apply(std::max(0.0f, std::min(1.0f, seg))), st);
        result |= IsLayoutProperty(prop) ? kRelayout : kRepaint;
      }
    }
  }
  return (TickResult)(result & kRelayout ? kRelayout : result);
}

bool AnimationController::Active() const {
  if (instant_) return false;
  for (std::map<Node*, std::unique_ptr<Entry> >::const_iterator it = entries_.begin(); it != entries_.end(); ++it) {
    const Entry& e = *it->second;
    if (!e.transitions.empty()) return true;
    for (size_t i = 0; i < e.animations.size(); ++i) {
      const Animation& a = *e.animations[i];
      if (!a.paused && !a.ended && !a.frames.empty()) return true;
    }
  }
  return false;
}

std::vector<std::pair<Node*, std::string> > AnimationController::TakeEvents() {
  std::vector<std::pair<Node*, std::string> > out;
  out.swap(events_);
  return out;
}

}  // namespace kite
