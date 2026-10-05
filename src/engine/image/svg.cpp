#include "image/svg.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "base/strings.h"
#include "css/resolver.h"
#include "css/style.h"
#include "css/stylesheet.h"
#include "html/parser.h"
#include "image/raster.h"

namespace kite {

namespace {

using namespace gfx;

void Composite(const Raster& r, Color c, float opacity, DecodedImage& out) {
  float a = c.a / 255.0f * opacity;
  for (int y = r.y0; y < r.y1; ++y) {
    const float* cov = r.Row(y);
    uint32_t* px = &out.pixels[(size_t)y * out.width];
    for (int x = r.x0; x < r.x1; ++x) {
      float cv = cov[x];
      if (cv <= 0) continue;
      float sa = std::min(1.0f, cv) * a;
      uint32_t d = px[x];
      float da = (d >> 24) / 255.0f;
      float dr = ((d >> 16) & 255), dg = ((d >> 8) & 255), db = (d & 255);
      float na = sa + da * (1 - sa);
      unsigned rr = (unsigned)(c.r * sa + dr * (1 - sa) + 0.5f);
      unsigned g = (unsigned)(c.g * sa + dg * (1 - sa) + 0.5f);
      unsigned b = (unsigned)(c.b * sa + db * (1 - sa) + 0.5f);
      unsigned aa = (unsigned)(na * 255 + 0.5f);
      px[x] = (std::min(aa, 255u) << 24) | (std::min(rr, 255u) << 16) | (std::min(g, 255u) << 8) |
              std::min(b, 255u);
    }
  }
}

// Number list parser tolerant of SVG's compact syntax ("1-2.5.5e3").
class NumReader {
 public:
  explicit NumReader(const std::string& s) : s_(s), p_(0) {}
  void SkipSep() {
    while (p_ < s_.size() && (IsAsciiSpace((unsigned char)s_[p_]) || s_[p_] == ',')) ++p_;
  }
  bool AtNumber() {
    SkipSep();
    if (p_ >= s_.size()) return false;
    char c = s_[p_];
    return IsAsciiDigit((unsigned char)c) || c == '-' || c == '+' || c == '.';
  }
  bool Number(float& out) {
    SkipSep();
    if (p_ >= s_.size()) return false;
    size_t start = p_;
    if (s_[p_] == '+' || s_[p_] == '-') ++p_;
    bool dot = false;
    while (p_ < s_.size() && (IsAsciiDigit((unsigned char)s_[p_]) || (s_[p_] == '.' && !dot))) {
      if (s_[p_] == '.') dot = true;
      ++p_;
    }
    if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
      size_t q = p_ + 1;
      if (q < s_.size() && (s_[q] == '+' || s_[q] == '-')) ++q;
      if (q < s_.size() && IsAsciiDigit((unsigned char)s_[q])) {
        p_ = q;
        while (p_ < s_.size() && IsAsciiDigit((unsigned char)s_[p_])) ++p_;
      }
    }
    if (p_ == start) return false;
    size_t used;
    out = (float)ParseDoublePrefix(s_.substr(start, p_ - start), used);
    return used > 0;
  }
  bool Flag(float& out) {  // arc flags may be written without separators
    SkipSep();
    if (p_ < s_.size() && (s_[p_] == '0' || s_[p_] == '1')) {
      out = (float)(s_[p_] - '0');
      ++p_;
      return true;
    }
    return false;
  }
  bool Command(char& c) {
    SkipSep();
    if (p_ < s_.size() && IsAsciiAlpha((unsigned char)s_[p_])) {
      c = s_[p_++];
      return true;
    }
    return false;
  }
  bool Done() {
    SkipSep();
    return p_ >= s_.size();
  }
  void Skip() { ++p_; }

 private:
  std::string s_;
  size_t p_;
};

Matrix ParseTransform(const std::string& t) {
  Matrix m;
  size_t p = 0;
  while (p < t.size()) {
    size_t open = t.find('(', p);
    if (open == std::string::npos) break;
    size_t close = t.find(')', open);
    if (close == std::string::npos) break;
    std::string name = Trim(t.substr(p, open - p));
    while (!name.empty() && (name[0] == ',' || IsAsciiSpace((unsigned char)name[0]))) name = name.substr(1);
    std::string args = t.substr(open + 1, close - open - 1);
    NumReader r(args);
    std::vector<float> v;
    float x;
    while (r.Number(x)) v.push_back(x);
    Matrix n;
    if (name == "matrix" && v.size() == 6) n = Matrix(v[0], v[1], v[2], v[3], v[4], v[5]);
    else if (name == "translate" && !v.empty()) n = Matrix(1, 0, 0, 1, v[0], v.size() > 1 ? v[1] : 0);
    else if (name == "scale" && !v.empty()) n = Matrix(v[0], 0, 0, v.size() > 1 ? v[1] : v[0], 0, 0);
    else if (name == "rotate" && !v.empty()) {
      float a = v[0] * kPi / 180, cs = std::cos(a), sn = std::sin(a);
      Matrix rot(cs, sn, -sn, cs, 0, 0);
      if (v.size() == 3) n = Matrix(1, 0, 0, 1, v[1], v[2]) * rot * Matrix(1, 0, 0, 1, -v[1], -v[2]);
      else n = rot;
    } else if (name == "skewx" || name == "skewX") {
      if (!v.empty()) n = Matrix(1, 0, std::tan(v[0] * kPi / 180), 1, 0, 0);
    } else if (name == "skewy" || name == "skewY") {
      if (!v.empty()) n = Matrix(1, std::tan(v[0] * kPi / 180), 0, 1, 0, 0);
    }
    m = m * n;
    p = close + 1;
  }
  return m;
}

void ParsePathData(const std::string& d, PathBuilder& pb) {
  NumReader r(d);
  char cmd = 0;
  float cx = 0, cy = 0;     // current point
  float lcx = 0, lcy = 0;   // last control point
  char prev = 0;
  float startX = 0, startY = 0;
  for (int guard = 0; guard < 200000; ++guard) {
    char c;
    if (r.Command(c)) cmd = c;
    else if (!r.AtNumber()) {
      if (r.Done()) break;
      r.Skip();
      continue;
    } else if (cmd == 'M') cmd = 'L';
    else if (cmd == 'm') cmd = 'l';
    bool rel = cmd >= 'a' && cmd <= 'z';
    char up = (char)(rel ? cmd - 32 : cmd);
    float ox = rel ? cx : 0, oy = rel ? cy : 0;
    float a[7];
    switch (up) {
      case 'M':
        if (!r.Number(a[0]) || !r.Number(a[1])) return;
        cx = a[0] + ox;
        cy = a[1] + oy;
        pb.MoveTo(cx, cy);
        startX = cx;
        startY = cy;
        break;
      case 'L':
        if (!r.Number(a[0]) || !r.Number(a[1])) return;
        cx = a[0] + ox;
        cy = a[1] + oy;
        pb.LineTo(cx, cy);
        break;
      case 'H':
        if (!r.Number(a[0])) return;
        cx = a[0] + ox;
        pb.LineTo(cx, cy);
        break;
      case 'V':
        if (!r.Number(a[0])) return;
        cy = a[0] + oy;
        pb.LineTo(cx, cy);
        break;
      case 'C':
        for (int i = 0; i < 6; ++i)
          if (!r.Number(a[i])) return;
        pb.CubicTo(a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy, a[4] + ox, a[5] + oy);
        lcx = a[2] + ox;
        lcy = a[3] + oy;
        cx = a[4] + ox;
        cy = a[5] + oy;
        break;
      case 'S': {
        for (int i = 0; i < 4; ++i)
          if (!r.Number(a[i])) return;
        float x1 = cx, y1 = cy;
        if (prev == 'C' || prev == 'S') {
          x1 = 2 * cx - lcx;
          y1 = 2 * cy - lcy;
        }
        pb.CubicTo(x1, y1, a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
        lcx = a[0] + ox;
        lcy = a[1] + oy;
        cx = a[2] + ox;
        cy = a[3] + oy;
        break;
      }
      case 'Q':
        for (int i = 0; i < 4; ++i)
          if (!r.Number(a[i])) return;
        pb.QuadTo(a[0] + ox, a[1] + oy, a[2] + ox, a[3] + oy);
        lcx = a[0] + ox;
        lcy = a[1] + oy;
        cx = a[2] + ox;
        cy = a[3] + oy;
        break;
      case 'T': {
        if (!r.Number(a[0]) || !r.Number(a[1])) return;
        float x1 = cx, y1 = cy;
        if (prev == 'Q' || prev == 'T') {
          x1 = 2 * cx - lcx;
          y1 = 2 * cy - lcy;
        }
        pb.QuadTo(x1, y1, a[0] + ox, a[1] + oy);
        lcx = x1;
        lcy = y1;
        cx = a[0] + ox;
        cy = a[1] + oy;
        break;
      }
      case 'A':
        if (!r.Number(a[0]) || !r.Number(a[1]) || !r.Number(a[2]) || !r.Flag(a[3]) || !r.Flag(a[4]) ||
            !r.Number(a[5]) || !r.Number(a[6]))
          return;
        pb.ArcTo(a[0], a[1], a[2], a[3] != 0, a[4] != 0, a[5] + ox, a[6] + oy);
        cx = a[5] + ox;
        cy = a[6] + oy;
        break;
      case 'Z':
        pb.Close();
        cx = startX;
        cy = startY;
        break;
      default:
        return;
    }
    prev = up;
  }
  pb.Finish();
}

// ---------------------------------------------------------------------------
// Styling

struct PaintState {
  bool hasFill;
  Color fill;
  bool hasStroke;
  Color stroke;
  float strokeWidth;
  float opacity;
  float fillOpacity;
  float strokeOpacity;
  bool evenOdd;
  Color current;
  LineJoin join = kJoinMiter;
  LineCap cap = kCapButt;
  float miterLimit = 4;
  std::vector<float> dash;
  PaintState()
      : hasFill(true), fill(0, 0, 0), hasStroke(false), strokeWidth(1), opacity(1),
        fillOpacity(1), strokeOpacity(1), evenOdd(false), current(0, 0, 0) {}
};

class SvgRenderer {
 public:
  SvgRenderer(const Node* root, Color current) : root_(root), current_(current) {
    CollectIds(root);
    // <style> elements inside the SVG.
    std::vector<Node*> styles;
    const_cast<Node*>(root)->FindAll("style", styles);
    for (size_t i = 0; i < styles.size(); ++i) {
      std::shared_ptr<Stylesheet> sheet(new Stylesheet);
      ParseStylesheet(styles[i]->TextContent(), *sheet);
      sheets_.push_back(sheet);
    }
  }

  bool Render(int w, int h, DecodedImage& out) {
    out.width = w;
    out.height = h;
    out.hasAlpha = true;
    out.pixels.assign((size_t)w * h, 0);
    raster_.reset(new Raster(w, h));
    float iw, ih;
    SvgIntrinsicSize(root_, iw, ih);
    Matrix m;
    std::vector<std::string> vb = SplitWhitespace(ReplaceAll(root_->Attr("viewbox"), ",", " "));
    if (vb.size() == 4) {
      size_t u;
      float vx = (float)ParseDoublePrefix(vb[0], u), vy = (float)ParseDoublePrefix(vb[1], u);
      float vw = (float)ParseDoublePrefix(vb[2], u), vh = (float)ParseDoublePrefix(vb[3], u);
      if (vw > 0 && vh > 0) {
        std::string par = AsciiLower(root_->Attr("preserveaspectratio"));
        float sx = w / vw, sy = h / vh;
        if (StartsWith(par, "none")) {
          m = Matrix(sx, 0, 0, sy, -vx * sx, -vy * sy);
        } else {
          bool slice = par.find("slice") != std::string::npos;
          float s = slice ? std::max(sx, sy) : std::min(sx, sy);
          float tx = (w - vw * s) / 2, ty = (h - vh * s) / 2;
          if (par.find("xmin") != std::string::npos) tx = 0;
          if (par.find("xmax") != std::string::npos) tx = w - vw * s;
          if (par.find("ymin") != std::string::npos) ty = 0;
          if (par.find("ymax") != std::string::npos) ty = h - vh * s;
          m = Matrix(s, 0, 0, s, tx - vx * s, ty - vy * s);
        }
      }
    } else if (iw > 0 && ih > 0) {
      m = Matrix(w / iw, 0, 0, h / ih, 0, 0);
    }
    PaintState ps;
    ps.current = CurrentColorOf(root_, current_);
    ApplyStyle(root_, ps);
    for (size_t i = 0; i < root_->children.size(); ++i) Draw(root_->children[i].get(), m, ps, 0, out);
    return true;
  }

 private:
  void CollectIds(const Node* n) {
    if (n->IsElement() && !n->id.empty()) ids_[n->id] = n;
    for (size_t i = 0; i < n->children.size(); ++i) CollectIds(n->children[i].get());
  }

  static Color CurrentColorOf(const Node* n, Color fallback) {
    if (n->style) return n->style->color;
    return fallback;
  }

  // Property lookup: style="" > CSS rules in the SVG's <style> > page CSS
  // declared on the element > presentation attribute.
  bool Prop(const Node* n, const char* name, std::string& out) {
    const std::string* st = n->GetAttr("style");
    if (st) {
      std::vector<Declaration> decls = ParseDeclarations(*st);
      for (size_t i = decls.size(); i-- > 0;)
        if (decls[i].property == name) {
          out = decls[i].value;
          return true;
        }
    }
    if (!sheets_.empty()) {
      int bestSpec = -1;
      size_t bestOrder = 0;
      ElementState es;
      for (size_t s = 0; s < sheets_.size(); ++s) {
        const Stylesheet& sh = *sheets_[s];
        for (size_t r = 0; r < sh.rules.size(); ++r) {
          const StyleRule& rule = sh.rules[r];
          for (size_t d = 0; d < rule.declarations.size(); ++d) {
            if (rule.declarations[d].property != name) continue;
            for (size_t k = 0; k < rule.selectors.size(); ++k) {
              const ComplexSelector& sel = rule.selectors[k];
              if (sel.specificity < bestSpec) continue;
              if (!MatchesSelector(sel, n, es)) continue;
              if (sel.specificity > bestSpec || rule.order >= bestOrder) {
                bestSpec = sel.specificity;
                bestOrder = rule.order;
                out = rule.declarations[d].value;
              }
            }
          }
        }
      }
      if (bestSpec >= 0) return true;
    }
    if (n->style) {
      std::string css;
      if (std::string(name) == "fill" && n->style->fillDeclared) {
        out = n->style->fill;
        return true;
      }
      if (std::string(name) == "stroke" && n->style->strokeDeclared) {
        out = n->style->stroke;
        return true;
      }
    }
    const std::string* a = n->GetAttr(name);
    if (a) {
      out = *a;
      return true;
    }
    return false;
  }

  bool ParsePaint(const std::string& raw, const PaintState& ps, Color& out) {
    std::string v = AsciiLower(Trim(raw));
    if (v == "none" || v == "transparent") return false;
    if (v == "currentcolor") {
      out = ps.current;
      return true;
    }
    if (StartsWith(v, "url(")) {
      size_t hash = raw.find('#');
      size_t close = raw.find(')');
      std::string id = hash == std::string::npos ? "" : raw.substr(hash + 1, close - hash - 1);
      id = Trim(ReplaceAll(ReplaceAll(id, "\"", ""), "'", ""));
      std::map<std::string, const Node*>::iterator it = ids_.find(id);
      if (it != ids_.end()) {
        // Average of the gradient stops.
        std::vector<Node*> stops;
        const_cast<Node*>(it->second)->FindAll("stop", stops);
        const Node* g = it->second;
        while (stops.empty()) {
          std::string href = g->Attr("href");
          if (href.empty()) href = g->Attr("xlink:href");
          if (href.empty() || href[0] != '#') break;
          std::map<std::string, const Node*>::iterator jt = ids_.find(href.substr(1));
          if (jt == ids_.end() || jt->second == g) break;
          g = jt->second;
          const_cast<Node*>(g)->FindAll("stop", stops);
        }
        int r = 0, gg = 0, b = 0, a = 0, n = 0;
        for (size_t i = 0; i < stops.size(); ++i) {
          std::string sc;
          Color c(0, 0, 0);
          if (Prop(stops[i], "stop-color", sc)) ParseColor(sc, c, ps.current);
          std::string so;
          float op = 1;
          if (Prop(stops[i], "stop-opacity", so)) {
            size_t u;
            op = (float)ParseDoublePrefix(so, u);
          }
          r += c.r;
          gg += c.g;
          b += c.b;
          a += (int)(c.a * op);
          ++n;
        }
        if (n) {
          out = Color(r / n, gg / n, b / n, a / n);
          return true;
        }
      }
      // Fallback color after the url().
      std::string rest = Trim(raw.substr(close == std::string::npos ? raw.size() : close + 1));
      if (!rest.empty()) return ParseColor(rest, out, ps.current);
      return false;
    }
    return ParseColor(v, out, ps.current);
  }

  static float Num(const std::string& s, float def) {
    size_t u;
    std::string t = Trim(s);
    float v = (float)ParseDoublePrefix(t, u);
    return u ? v : def;
  }

  void ApplyStyle(const Node* n, PaintState& ps) {
    std::string v;
    if (n->style) ps.current = n->style->color;
    if (Prop(n, "color", v)) ParseColor(v, ps.current, ps.current);
    if (Prop(n, "fill", v)) {
      Color c;
      ps.hasFill = ParsePaint(v, ps, c);
      if (ps.hasFill) ps.fill = c;
    }
    if (Prop(n, "stroke", v)) {
      Color c;
      ps.hasStroke = ParsePaint(v, ps, c);
      if (ps.hasStroke) ps.stroke = c;
    }
    if (Prop(n, "stroke-width", v)) ps.strokeWidth = Num(v, 1);
    if (Prop(n, "stroke-linejoin", v)) ps.join = v == "round" ? kJoinRound : v == "bevel" ? kJoinBevel : kJoinMiter;
    if (Prop(n, "stroke-linecap", v)) ps.cap = v == "round" ? kCapRound : v == "square" ? kCapSquare : kCapButt;
    if (Prop(n, "stroke-miterlimit", v)) ps.miterLimit = Num(v, 4);
    if (Prop(n, "stroke-dasharray", v)) {
      ps.dash.clear();
      if (v != "none") {
        NumReader r(v);
        float x;
        while (r.Number(x)) ps.dash.push_back(std::max(0.0f, x));
        if (ps.dash.size() % 2) ps.dash.insert(ps.dash.end(), ps.dash.begin(), ps.dash.end());
        float sum = 0;
        for (size_t i = 0; i < ps.dash.size(); ++i) sum += ps.dash[i];
        if (sum <= 0) ps.dash.clear();
      }
    }
    if (Prop(n, "fill-opacity", v)) ps.fillOpacity = Num(v, 1);
    if (Prop(n, "stroke-opacity", v)) ps.strokeOpacity = Num(v, 1);
    if (Prop(n, "fill-rule", v)) ps.evenOdd = AsciiLower(Trim(v)) == "evenodd";
  }

  void Shape(const std::vector<Poly>& polys, const std::vector<bool>& closed, const Matrix& m,
             const PaintState& ps, float opacity, DecodedImage& out) {
    if (ps.hasFill && ps.fillOpacity > 0) {
      raster_->Clear();
      raster_->Fill(polys, ps.evenOdd);
      Composite(*raster_, ps.fill, opacity * ps.fillOpacity, out);
    }
    if (ps.hasStroke && ps.strokeWidth > 0 && ps.strokeOpacity > 0) {
      StrokeStyle st;
      st.width = std::max(0.7f, ps.strokeWidth * m.Scale());
      st.join = ps.join;
      st.cap = ps.cap;
      st.miterLimit = ps.miterLimit;
      for (size_t i = 0; i < ps.dash.size(); ++i) st.dash.push_back(ps.dash[i] * m.Scale());
      std::vector<Poly> outline = StrokePolys(polys, closed, st);
      raster_->Clear();
      raster_->Fill(outline, false);
      Composite(*raster_, ps.stroke, opacity * ps.strokeOpacity, out);
    }
  }

  float Len(const Node* n, const char* name, float def) {
    std::string v = n->Attr(name);
    if (v.empty()) return def;
    return Num(v, def);
  }

  void Draw(const Node* n, const Matrix& parentM, const PaintState& parentPs, int depth,
            DecodedImage& out) {
    if (!n->IsElement() || depth > 40) return;
    const std::string& t = n->tag;
    if (t == "defs" || t == "lineargradient" || t == "radialgradient" || t == "clippath" ||
        t == "mask" || t == "pattern" || t == "symbol" || t == "style" || t == "title" ||
        t == "desc" || t == "metadata" || t == "filter" || t == "marker" || t == "text" ||
        t == "foreignobject" || t == "script")
      return;
    std::string v;
    if (Prop(n, "display", v) && AsciiLower(Trim(v)) == "none") return;
    if (n->style && n->style->display == kDisplayNone && n != root_) return;
    if (Prop(n, "visibility", v) && AsciiLower(Trim(v)) == "hidden") return;
    Matrix m = parentM;
    if (n->HasAttr("transform")) m = m * ParseTransform(n->Attr("transform"));
    PaintState ps = parentPs;
    ApplyStyle(n, ps);
    float opacity = 1;
    if (Prop(n, "opacity", v)) opacity = Num(v, 1);
    if (n->style && n->style->opacity < 1) opacity *= n->style->opacity;
    PaintState local = ps;
    local.opacity *= opacity;
    float op = local.opacity;

    PathBuilder pb(m, 0.2f);
    if (t == "g" || t == "a" || t == "svg" || t == "switch") {
      Matrix cm = m;
      if (t == "svg" && n != root_) cm = m * Matrix(1, 0, 0, 1, Len(n, "x", 0), Len(n, "y", 0));
      for (size_t i = 0; i < n->children.size(); ++i) Draw(n->children[i].get(), cm, local, depth + 1, out);
      return;
    }
    if (t == "use") {
      std::string href = n->Attr("href");
      if (href.empty()) href = n->Attr("xlink:href");
      if (href.size() > 1 && href[0] == '#') {
        std::map<std::string, const Node*>::iterator it = ids_.find(href.substr(1));
        if (it != ids_.end()) {
          Matrix um = m * Matrix(1, 0, 0, 1, Len(n, "x", 0), Len(n, "y", 0));
          const Node* ref = it->second;
          if (ref->tag == "symbol") {
            std::vector<std::string> vb = SplitWhitespace(ReplaceAll(ref->Attr("viewbox"), ",", " "));
            float uw = Len(n, "width", 0), uh = Len(n, "height", 0);
            if (vb.size() == 4 && uw > 0 && uh > 0) {
              float vw = Num(vb[2], 1), vh = Num(vb[3], 1);
              float s = std::min(uw / vw, uh / vh);
              um = um * Matrix(s, 0, 0, s, -Num(vb[0], 0) * s, -Num(vb[1], 0) * s);
            }
            PaintState sps = local;
            ApplyStyle(ref, sps);
            for (size_t i = 0; i < ref->children.size(); ++i)
              Draw(ref->children[i].get(), um, sps, depth + 1, out);
          } else if (ref != n) {
            Draw(ref, um, local, depth + 1, out);
          }
        }
      }
      return;
    }
    if (t == "path") {
      ParsePathData(n->Attr("d"), pb);
    } else if (t == "rect") {
      float x = Len(n, "x", 0), y = Len(n, "y", 0), w = Len(n, "width", 0), h = Len(n, "height", 0);
      float rx = Len(n, "rx", -1), ry = Len(n, "ry", -1);
      if (rx < 0) rx = ry;
      if (ry < 0) ry = rx;
      rx = std::max(0.0f, std::min(rx, w / 2));
      ry = std::max(0.0f, std::min(ry, h / 2));
      if (w <= 0 || h <= 0) return;
      if (rx > 0 && ry > 0) {
        pb.MoveTo(x + rx, y);
        pb.LineTo(x + w - rx, y);
        pb.ArcTo(rx, ry, 0, false, true, x + w, y + ry);
        pb.LineTo(x + w, y + h - ry);
        pb.ArcTo(rx, ry, 0, false, true, x + w - rx, y + h);
        pb.LineTo(x + rx, y + h);
        pb.ArcTo(rx, ry, 0, false, true, x, y + h - ry);
        pb.LineTo(x, y + ry);
        pb.ArcTo(rx, ry, 0, false, true, x + rx, y);
      } else {
        pb.MoveTo(x, y);
        pb.LineTo(x + w, y);
        pb.LineTo(x + w, y + h);
        pb.LineTo(x, y + h);
      }
      pb.Close();
    } else if (t == "circle" || t == "ellipse") {
      float cx = Len(n, "cx", 0), cy = Len(n, "cy", 0);
      float rx = t == "circle" ? Len(n, "r", 0) : Len(n, "rx", 0);
      float ry = t == "circle" ? rx : Len(n, "ry", 0);
      if (rx <= 0 || ry <= 0) return;
      pb.MoveTo(cx + rx, cy);
      pb.ArcTo(rx, ry, 0, false, true, cx - rx, cy);
      pb.ArcTo(rx, ry, 0, false, true, cx + rx, cy);
      pb.Close();
    } else if (t == "line") {
      pb.MoveTo(Len(n, "x1", 0), Len(n, "y1", 0));
      pb.LineTo(Len(n, "x2", 0), Len(n, "y2", 0));
      pb.Finish();
      local.hasFill = false;
    } else if (t == "polyline" || t == "polygon") {
      NumReader r(n->Attr("points"));
      float x, y;
      bool first = true;
      while (r.Number(x) && r.Number(y)) {
        if (first) pb.MoveTo(x, y);
        else pb.LineTo(x, y);
        first = false;
      }
      if (t == "polygon") pb.Close();
      else pb.Finish();
    } else {
      return;
    }
    if (pb.polys.empty()) return;
    Shape(pb.polys, pb.closed, m, local, op, out);
  }

  const Node* root_;
  Color current_;
  std::map<std::string, const Node*> ids_;
  std::vector<std::shared_ptr<Stylesheet> > sheets_;
  std::unique_ptr<Raster> raster_;
};

}  // namespace

namespace gfx {
void ParseSvgPath(const std::string& d, PathBuilder& pb) { ParsePathData(d, pb); }
}  // namespace gfx

bool SvgIntrinsicSize(const Node* svg, float& w, float& h) {
  w = h = 0;
  size_t u;
  std::string ws = Trim(svg->Attr("width")), hs = Trim(svg->Attr("height"));
  if (!ws.empty() && ws.find('%') == std::string::npos) w = (float)ParseDoublePrefix(ws, u);
  if (!hs.empty() && hs.find('%') == std::string::npos) h = (float)ParseDoublePrefix(hs, u);
  std::vector<std::string> vb = SplitWhitespace(ReplaceAll(svg->Attr("viewbox"), ",", " "));
  float vw = 0, vh = 0;
  if (vb.size() == 4) {
    vw = (float)ParseDoublePrefix(vb[2], u);
    vh = (float)ParseDoublePrefix(vb[3], u);
  }
  if (w <= 0 && h <= 0) {
    w = vw;
    h = vh;
  } else if (w <= 0) {
    w = vh > 0 ? h * vw / vh : h;
  } else if (h <= 0) {
    h = vw > 0 ? w * vh / vw : w;
  }
  if (w <= 0 || h <= 0) {
    w = 300;
    h = 150;
    return false;
  }
  return true;
}

bool RenderSvg(const Node* svg, int w, int h, Color currentColor, DecodedImage& out) {
  if (!svg || w <= 0 || h <= 0 || (long long)w * h > 4096LL * 4096) return false;
  SvgRenderer r(svg, currentColor);
  return r.Render(w, h, out);
}

bool LooksLikeSvg(const std::string& data) {
  std::string head = AsciiLower(data.substr(0, 1024));
  return head.find("<svg") != std::string::npos;
}

bool RenderSvgDocument(const std::string& text, int w, int h, float scale, DecodedImage& out) {
  std::unique_ptr<Document> doc = ParseHtml(text);
  Node* svg = doc->root->FindFirst("svg");
  if (!svg) return false;
  if (w <= 0 || h <= 0) {
    float iw, ih;
    SvgIntrinsicSize(svg, iw, ih);
    w = std::max(1, (int)(iw * scale + 0.5f));
    h = std::max(1, (int)(ih * scale + 0.5f));
    if ((long long)w * h > 2048LL * 2048) {
      float f = std::sqrt(2048.0f * 2048.0f / ((float)w * h));
      w = std::max(1, (int)(w * f));
      h = std::max(1, (int)(h * f));
    }
  }
  return RenderSvg(svg, w, h, Color(0, 0, 0), out);
}

}  // namespace kite
