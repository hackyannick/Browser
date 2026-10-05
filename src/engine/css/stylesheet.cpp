#include "css/stylesheet.h"

#include <cstdlib>

#include "base/strings.h"

namespace kite {

// Implemented by the style system (css/style.cpp); used for @supports.
bool IsCssPropertySupported(const std::string& prop, const std::string& value);

std::string StripCssComments(const std::string& css) {
  std::string out;
  out.reserve(css.size());
  size_t i = 0;
  while (i < css.size()) {
    char c = css[i];
    if (c == '"' || c == '\'') {
      size_t j = i + 1;
      while (j < css.size() && css[j] != c && css[j] != '\n') {
        if (css[j] == '\\') ++j;
        ++j;
      }
      if (j < css.size() && css[j] == c) ++j;
      out.append(css, i, j - i);
      i = j;
    } else if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      size_t end = css.find("*/", i + 2);
      i = end == std::string::npos ? css.size() : end + 2;
      out += ' ';
    } else if (c == '\\' && i + 1 < css.size()) {
      out += c;
      out += css[i + 1];
      i += 2;
    } else {
      out += c;
      ++i;
    }
  }
  return out;
}

namespace {

// Finds the first occurrence of any character in |stops| at nesting depth 0,
// skipping strings and (), [], {} groups that do not contain the stop.
size_t FindTopLevel(const std::string& s, size_t start, const char* stops) {
  int depth = 0;
  for (size_t i = start; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\') {
      ++i;
      continue;
    }
    if (c == '"' || c == '\'') {
      size_t j = i + 1;
      while (j < s.size() && s[j] != c) {
        if (s[j] == '\\') ++j;
        ++j;
      }
      i = j;
      continue;
    }
    if (depth == 0) {
      for (const char* p = stops; *p; ++p)
        if (c == *p) return i;
    }
    if (c == '(' || c == '[' || c == '{') ++depth;
    else if ((c == ')' || c == ']' || c == '}') && depth > 0) --depth;
  }
  return std::string::npos;
}

// s[open] == '{'. Returns index of the matching '}' (or s.size()).
size_t MatchBrace(const std::string& s, size_t open) {
  int depth = 0;
  for (size_t i = open; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\') {
      ++i;
      continue;
    }
    if (c == '"' || c == '\'') {
      size_t j = i + 1;
      while (j < s.size() && s[j] != c && s[j] != '\n') {
        if (s[j] == '\\') ++j;
        ++j;
      }
      i = j;
      continue;
    }
    if (c == '{') ++depth;
    else if (c == '}') {
      if (--depth == 0) return i;
    }
  }
  return s.size();
}

std::vector<std::string> SplitTopLevel(const std::string& s, char sep) {
  std::vector<std::string> out;
  char stops[2] = {sep, 0};
  size_t start = 0;
  for (;;) {
    size_t p = FindTopLevel(s, start, stops);
    if (p == std::string::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Selector parsing

bool IsIdentChar(unsigned char c) {
  return IsAsciiAlpha(c) || IsAsciiDigit(c) || c == '-' || c == '_' || c >= 0x80;
}

class SelectorParser {
 public:
  SelectorParser(const std::string& s) : s_(s), p_(0) {}

  bool ParseList(std::vector<ComplexSelector>& out, bool forgiving) {
    std::vector<std::string> parts = SplitTopLevel(s_, ',');
    bool allOk = true;
    for (size_t i = 0; i < parts.size(); ++i) {
      SelectorParser sub(parts[i]);
      ComplexSelector cs;
      if (sub.ParseComplex(cs)) {
        out.push_back(cs);
      } else {
        allOk = false;
        if (!forgiving) return false;
      }
    }
    return allOk || forgiving;
  }

  bool ParseComplex(ComplexSelector& out) {
    SkipWs();
    for (;;) {
      bool ws = SkipWs();
      if (p_ >= s_.size()) break;
      char c = s_[p_];
      char comb = 0;
      if (c == '>' || c == '+' || c == '~') {
        comb = c;
        ++p_;
        SkipWs();
      } else if (!out.compounds.empty()) {
        if (!ws) return false;
        comb = ' ';
      }
      if (comb) {
        if (out.compounds.empty()) {
          // Relative selector like "> a" (only valid when nesting).
          return false;
        }
        if (out.pseudo != kPseudoNone) return false;  // nothing may follow
        out.combinators.push_back(comb);
      }
      CompoundSelector comp;
      if (!ParseCompound(comp, out)) return false;
      out.compounds.push_back(comp);
    }
    if (out.compounds.empty()) return false;
    out.specificity = ComputeSpecificity(out);
    return true;
  }

  static int ComputeSpecificity(const ComplexSelector& cs) {
    int a = 0, b = 0, c = 0;
    for (size_t i = 0; i < cs.compounds.size(); ++i) {
      const std::vector<SimpleSelector>& parts = cs.compounds[i].parts;
      for (size_t k = 0; k < parts.size(); ++k) {
        const SimpleSelector& s = parts[k];
        switch (s.kind) {
          case SimpleSelector::kId: ++a; break;
          case SimpleSelector::kClass:
          case SimpleSelector::kAttr: ++b; break;
          case SimpleSelector::kType: ++c; break;
          case SimpleSelector::kPseudoClass:
            if (s.name == "where") break;
            if (s.name == "not" || s.name == "is" || s.name == "matches" ||
                s.name == "-webkit-any") {
              int best = 0;
              for (size_t j = 0; j < s.args.size(); ++j)
                if (s.args[j].specificity > best) best = s.args[j].specificity;
              a += best / 10000;
              b += (best / 100) % 100;
              c += best % 100;
            } else {
              ++b;
            }
            break;
          default: break;
        }
      }
    }
    if (cs.pseudo != kPseudoNone) ++c;
    if (a > 99) a = 99;
    if (b > 99) b = 99;
    if (c > 99) c = 99;
    return a * 10000 + b * 100 + c;
  }

 private:
  bool SkipWs() {
    bool any = false;
    while (p_ < s_.size() && IsAsciiSpace((unsigned char)s_[p_])) {
      ++p_;
      any = true;
    }
    return any;
  }

  bool ParseIdent(std::string& out) {
    out.clear();
    size_t start = p_;
    if (p_ < s_.size() && s_[p_] == '-') {
      out += '-';
      ++p_;
    }
    while (p_ < s_.size()) {
      unsigned char c = s_[p_];
      if (c == '\\') {
        ++p_;
        if (p_ >= s_.size()) break;
        if (IsAsciiHex(s_[p_])) {
          unsigned long cp = 0;
          int n = 0;
          while (p_ < s_.size() && n < 6 && IsAsciiHex(s_[p_])) {
            cp = cp * 16 + HexValue(s_[p_]);
            ++p_;
            ++n;
          }
          if (p_ < s_.size() && IsAsciiSpace((unsigned char)s_[p_])) ++p_;
          Utf8Append(out, (Codepoint)cp);
        } else {
          out += s_[p_++];
        }
      } else if (IsIdentChar(c)) {
        out += char(c);
        ++p_;
      } else {
        break;
      }
    }
    return p_ > start && !(out == "-");
  }

  bool ParseCompound(CompoundSelector& comp, ComplexSelector& owner) {
    size_t start = p_;
    while (p_ < s_.size()) {
      char c = s_[p_];
      SimpleSelector sel;
      if (c == '*') {
        ++p_;
        sel.kind = SimpleSelector::kUniversal;
        if (p_ < s_.size() && s_[p_] == '|') ++p_;  // *|foo
        comp.parts.push_back(sel);
      } else if (c == '#') {
        ++p_;
        sel.kind = SimpleSelector::kId;
        if (!ParseIdent(sel.name)) return false;
        comp.parts.push_back(sel);
      } else if (c == '.') {
        ++p_;
        sel.kind = SimpleSelector::kClass;
        if (!ParseIdent(sel.name)) return false;
        comp.parts.push_back(sel);
      } else if (c == '[') {
        if (!ParseAttr(sel)) return false;
        comp.parts.push_back(sel);
      } else if (c == ':') {
        ++p_;
        bool doubleColon = false;
        if (p_ < s_.size() && s_[p_] == ':') {
          ++p_;
          doubleColon = true;
        }
        std::string name;
        if (!ParseIdent(name)) return false;
        name = AsciiLower(name);
        if (owner.pseudo == kPseudoUnsupported) {
          if (p_ < s_.size() && s_[p_] == '(') {
            size_t close = FindClose(p_);
            if (close == std::string::npos) return false;
            p_ = close + 1;
          }
          continue;
        }
        if (owner.pseudo != kPseudoNone) return false;
        if (name == "before" || name == "after") {
          owner.pseudo = name == "before" ? kPseudoBefore : kPseudoAfter;
          continue;
        }
        if (doubleColon || name == "first-line" || name == "first-letter") {
          // Valid but unrendered pseudo-elements (::placeholder, ::marker,
          // ::backdrop, ::selection, ::-webkit-* ...): the selector stays
          // valid so that selector lists are kept, but never matches.
          owner.pseudo = kPseudoUnsupported;
          if (p_ < s_.size() && s_[p_] == '(') {
            size_t close = FindClose(p_);
            if (close == std::string::npos) return false;
            p_ = close + 1;
          }
          continue;
        }
        sel.kind = SimpleSelector::kPseudoClass;
        sel.name = name;
        if (p_ < s_.size() && s_[p_] == '(') {
          size_t close = FindClose(p_);
          if (close == std::string::npos) return false;
          std::string arg = s_.substr(p_ + 1, close - p_ - 1);
          p_ = close + 1;
          if (!ParseFunctionalPseudo(sel, arg)) return false;
        } else if (!IsKnownPseudoClass(name)) {
          return false;
        }
        comp.parts.push_back(sel);
      } else if (IsIdentChar((unsigned char)c) || c == '\\') {
        if (p_ != start) return false;  // type selector must come first
        sel.kind = SimpleSelector::kType;
        if (!ParseIdent(sel.name)) return false;
        sel.name = AsciiLower(sel.name);
        if (p_ < s_.size() && s_[p_] == '|') {  // ns|tag
          ++p_;
          if (!ParseIdent(sel.name)) return false;
          sel.name = AsciiLower(sel.name);
        }
        comp.parts.push_back(sel);
      } else {
        break;
      }
    }
    // A compound may be empty only if it carried a pseudo-element (::before).
    if (comp.parts.empty()) {
      if (owner.pseudo != kPseudoNone && p_ > start) {
        SimpleSelector u;
        comp.parts.push_back(u);
        return true;
      }
      return false;
    }
    return true;
  }

  size_t FindClose(size_t open) {
    int depth = 0;
    for (size_t i = open; i < s_.size(); ++i) {
      if (s_[i] == '\\') {
        ++i;
        continue;
      }
      if (s_[i] == '"' || s_[i] == '\'') {
        char q = s_[i];
        ++i;
        while (i < s_.size() && s_[i] != q) ++i;
        continue;
      }
      if (s_[i] == '(') ++depth;
      else if (s_[i] == ')' && --depth == 0) return i;
    }
    return std::string::npos;
  }

  static bool IsKnownPseudoClass(const std::string& n) {
    static const char* const known[] = {
        "root", "first-child", "last-child", "only-child", "first-of-type",
        "last-of-type", "only-of-type", "empty", "link", "any-link", "visited",
        "hover", "active", "focus", "focus-within", "focus-visible", "checked",
        "disabled", "enabled", "required", "optional", "read-only", "read-write",
        "placeholder-shown", "target", "default", "indeterminate", "defined",
        "valid", "invalid", "in-range", "out-of-range", "scope", "autofill",
        "user-invalid", "user-valid", "open", "closed", "modal", "popover-open",
        "fullscreen", "playing", "paused", "host", "picture-in-picture", "muted",
        "volume-locked", "buffering", "stalled", "seeking", "blank", "local-link",
        "target-within", "any-link", 0};
    for (int i = 0; known[i]; ++i)
      if (n == known[i]) return true;
    return false;
  }

  bool ParseFunctionalPseudo(SimpleSelector& sel, const std::string& arg) {
    const std::string& n = sel.name;
    if (n == "not" || n == "is" || n == "where" || n == "matches" ||
        n == "-webkit-any" || n == "-moz-any") {
      SelectorParser sub(arg);
      bool forgiving = n != "not";
      if (!sub.ParseList(sel.args, forgiving)) return false;
      if (n == "not" && sel.args.empty()) return false;
      return true;
    }
    if (n == "nth-child" || n == "nth-last-child" || n == "nth-of-type" ||
        n == "nth-last-of-type") {
      std::string a = AsciiLower(Trim(arg));
      size_t of = a.find(" of ");
      if (of != std::string::npos) a = Trim(a.substr(0, of));
      return ParseNth(a, sel.nthA, sel.nthB);
    }
    if (n == "has" || n == "host" || n == "host-context" || n == "state" ||
        n == "lang" || n == "dir" || n == "nth-col" || n == "current" ||
        n == "past" || n == "future") {
      sel.value = AsciiLower(Trim(arg));
      return true;  // evaluated as never/always matching by the matcher
    }
    return false;
  }

  static bool ParseNth(std::string a, int& A, int& B) {
    std::string s;
    for (size_t i = 0; i < a.size(); ++i)
      if (!IsAsciiSpace((unsigned char)a[i])) s += a[i];
    if (s == "odd") { A = 2; B = 1; return true; }
    if (s == "even") { A = 2; B = 0; return true; }
    size_t np = s.find('n');
    if (np == std::string::npos) {
      long long v;
      if (!ParseInt(s, v)) return false;
      A = 0;
      B = (int)v;
      return true;
    }
    std::string as = s.substr(0, np), bs = s.substr(np + 1);
    if (as.empty() || as == "+") A = 1;
    else if (as == "-") A = -1;
    else {
      long long v;
      if (!ParseInt(as, v)) return false;
      A = (int)v;
    }
    if (bs.empty()) B = 0;
    else {
      long long v;
      if (bs[0] == '+') bs = bs.substr(1);
      if (!ParseInt(bs, v)) return false;
      B = (int)v;
    }
    return true;
  }

  bool ParseAttr(SimpleSelector& sel) {
    size_t close = s_.find(']', p_);
    if (close == std::string::npos) return false;
    std::string inner = s_.substr(p_ + 1, close - p_ - 1);
    // Quoted values may contain ']'.
    size_t q = inner.find_first_of("\"'");
    if (q != std::string::npos) {
      char qc = inner[q];
      size_t endq = s_.find(qc, p_ + 1 + q + 1);
      if (endq == std::string::npos) return false;
      close = s_.find(']', endq);
      if (close == std::string::npos) return false;
      inner = s_.substr(p_ + 1, close - p_ - 1);
    }
    p_ = close + 1;
    sel.kind = SimpleSelector::kAttr;
    size_t opPos = inner.find_first_of("=~|^$*");
    if (opPos == std::string::npos) {
      sel.name = AsciiLower(Trim(inner));
      sel.op = SimpleSelector::kExists;
      return !sel.name.empty();
    }
    std::string name = Trim(inner.substr(0, opPos));
    size_t valPos = opPos + 1;
    switch (inner[opPos]) {
      case '=': sel.op = SimpleSelector::kEquals; break;
      case '~': sel.op = SimpleSelector::kIncludes; ++valPos; break;
      case '|': sel.op = SimpleSelector::kDashMatch; ++valPos; break;
      case '^': sel.op = SimpleSelector::kPrefix; ++valPos; break;
      case '$': sel.op = SimpleSelector::kSuffix; ++valPos; break;
      case '*': sel.op = SimpleSelector::kSubstring; ++valPos; break;
    }
    if (inner[opPos] != '=' && (opPos + 1 >= inner.size() || inner[opPos + 1] != '='))
      return false;
    // Strip a namespace prefix.
    size_t bar = name.find('|');
    if (bar != std::string::npos) name = name.substr(bar + 1);
    sel.name = AsciiLower(name);
    std::string v = Trim(inner.substr(valPos));
    if (!v.empty() && (v[0] == '"' || v[0] == '\'')) {
      char qc = v[0];
      size_t e = v.find(qc, 1);
      if (e == std::string::npos) return false;
      std::string flags = Trim(v.substr(e + 1));
      sel.caseInsensitive = flags == "i" || flags == "I";
      v = v.substr(1, e - 1);
    } else {
      std::vector<std::string> parts = SplitWhitespace(v);
      if (parts.empty()) return false;
      v = parts[0];
      if (parts.size() > 1) sel.caseInsensitive = parts[1] == "i" || parts[1] == "I";
    }
    // Unescape simple backslash escapes.
    std::string val;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i] == '\\' && i + 1 < v.size()) ++i;
      val += v[i];
    }
    sel.value = val;
    return !sel.name.empty();
  }

  const std::string& s_;
  size_t p_;
};

// ---------------------------------------------------------------------------
// Media queries

float MediaLength(const std::string& raw) {
  std::string s = Trim(raw);
  size_t used = 0;
  double v = ParseDoublePrefix(s, used);
  std::string unit = AsciiLower(Trim(s.substr(used)));
  if (unit == "em" || unit == "rem") return (float)(v * 16);
  if (unit == "pt") return (float)(v * 4 / 3);
  if (unit == "cm") return (float)(v * 96 / 2.54);
  if (unit == "mm") return (float)(v * 96 / 25.4);
  if (unit == "in") return (float)(v * 96);
  if (unit == "dppx" || unit == "x") return (float)v;
  if (unit == "dpi") return (float)(v / 96);
  if (unit == "dpcm") return (float)(v * 2.54 / 96);
  return (float)v;
}

bool CompareOp(float a, const std::string& op, float b) {
  if (op == "<") return a < b;
  if (op == "<=") return a <= b;
  if (op == ">") return a > b;
  if (op == ">=") return a >= b;
  return a == b;
}

float FeatureValue(const std::string& name, const MediaContext& ctx, bool& known) {
  known = true;
  if (name == "width" || name == "device-width") return ctx.width;
  if (name == "height" || name == "device-height") return ctx.height;
  if (name == "resolution" || name == "-webkit-device-pixel-ratio" ||
      name == "device-pixel-ratio")
    return 1;
  if (name == "aspect-ratio") return ctx.height > 0 ? ctx.width / ctx.height : 1;
  if (name == "color") return 8;
  known = false;
  return 0;
}

bool EvaluateFeature(const std::string& rawFeature, const MediaContext& ctx) {
  std::string f = AsciiLower(Trim(rawFeature));
  // Range syntax?
  size_t opPos = f.find_first_of("<>=");
  size_t colon = f.find(':');
  if (opPos != std::string::npos && (colon == std::string::npos || opPos < colon)) {
    std::vector<std::string> tokens;
    std::vector<std::string> ops;
    size_t i = 0, start = 0;
    while (i < f.size()) {
      if (f[i] == '<' || f[i] == '>' || f[i] == '=') {
        tokens.push_back(Trim(f.substr(start, i - start)));
        std::string op(1, f[i]);
        if (i + 1 < f.size() && f[i + 1] == '=') {
          op += '=';
          ++i;
        }
        ops.push_back(op);
        start = i + 1;
      }
      ++i;
    }
    tokens.push_back(Trim(f.substr(start)));
    if (tokens.size() < 2) return false;
    bool result = true;
    for (size_t k = 0; k + 1 < tokens.size(); ++k) {
      bool kn1, kn2;
      float a = FeatureValue(tokens[k], ctx, kn1);
      if (!kn1) a = MediaLength(tokens[k]);
      float b = FeatureValue(tokens[k + 1], ctx, kn2);
      if (!kn2) b = MediaLength(tokens[k + 1]);
      if (!kn1 && !kn2) return false;
      if (!CompareOp(a, ops[k], b)) result = false;
    }
    return result;
  }
  std::string name = colon == std::string::npos ? f : Trim(f.substr(0, colon));
  std::string value = colon == std::string::npos ? std::string() : Trim(f.substr(colon + 1));
  if (colon == std::string::npos) {
    return name == "color" || name == "hover" || name == "any-hover" ||
           name == "pointer" || name == "any-pointer";
  }
  bool isMin = StartsWith(name, "min-");
  bool isMax = StartsWith(name, "max-");
  std::string base = isMin || isMax ? name.substr(4) : name;
  if (base == "-webkit-min-device-pixel-ratio") { isMin = true; base = "resolution"; }
  if (base == "-webkit-max-device-pixel-ratio") { isMax = true; base = "resolution"; }
  if (StartsWith(name, "-webkit-min-device-pixel-ratio") ||
      StartsWith(name, "min--moz-device-pixel-ratio")) {
    isMin = true;
    base = "resolution";
  }
  if (base == "device-pixel-ratio") base = "resolution";
  bool known;
  float actual = FeatureValue(base, ctx, known);
  if (known) {
    float v;
    if (base == "aspect-ratio") {
      size_t slash = value.find('/');
      float a = MediaLength(value.substr(0, slash));
      float b = slash == std::string::npos ? 1 : MediaLength(value.substr(slash + 1));
      v = b > 0 ? a / b : 0;
    } else {
      v = MediaLength(value);
    }
    if (isMin) return actual >= v;
    if (isMax) return actual <= v;
    return actual == v;
  }
  if (name == "orientation")
    return value == (ctx.height > ctx.width ? "portrait" : "landscape");
  if (name == "prefers-color-scheme") return value == "light";
  if (name == "prefers-reduced-motion") return value == "no-preference";
  if (name == "prefers-contrast") return value == "no-preference";
  if (name == "prefers-reduced-transparency") return value == "no-preference";
  if (name == "prefers-reduced-data") return value == "no-preference";
  if (name == "hover" || name == "any-hover") return value == "hover";
  if (name == "pointer" || name == "any-pointer") return value == "fine";
  if (name == "scripting") return value == "none";
  if (name == "forced-colors") return value == "none";
  if (name == "inverted-colors") return value == "none";
  if (name == "update") return value == "fast";
  if (name == "display-mode") return value == "browser";
  if (name == "monochrome") return value == "0";
  if (name == "grid") return value == "0";
  return false;
}

bool EvaluateCondition(const std::string& raw, const MediaContext& ctx);

// Evaluates "a and b", "a or b", "not a", "(feature)" and media types.
bool EvaluateCondition(const std::string& raw, const MediaContext& ctx) {
  std::string s = Trim(raw);
  if (s.empty()) return true;
  // Split into terms at top level by whitespace keywords.
  std::vector<std::string> terms;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && IsAsciiSpace((unsigned char)s[i])) ++i;
    if (i >= s.size()) break;
    if (s[i] == '(') {
      int depth = 0;
      size_t start = i;
      for (; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) {
          ++i;
          break;
        }
      }
      terms.push_back(s.substr(start, i - start));
    } else {
      size_t start = i;
      while (i < s.size() && !IsAsciiSpace((unsigned char)s[i]) && s[i] != '(') ++i;
      terms.push_back(AsciiLower(s.substr(start, i - start)));
    }
  }
  bool result = true;
  bool haveResult = false;
  std::string joiner = "and";
  bool negateNext = false;
  for (size_t k = 0; k < terms.size(); ++k) {
    const std::string& t = terms[k];
    if (t == "and" || t == "or") {
      joiner = t;
      continue;
    }
    if (t == "not") {
      negateNext = !negateNext;
      continue;
    }
    if (t == "only") continue;
    bool v;
    if (!t.empty() && t[0] == '(') {
      std::string inner = t.substr(1, t.size() >= 2 ? t.size() - 2 : 0);
      std::string trimmed = Trim(inner);
      if (!trimmed.empty() && (trimmed[0] == '(' || StartsWithIgnoreCase(trimmed, "not ")))
        v = EvaluateCondition(inner, ctx);
      else
        v = EvaluateFeature(inner, ctx);
    } else {
      v = t == "all" || t == "screen";
    }
    if (negateNext) v = !v;
    negateNext = false;
    if (!haveResult) result = v;
    else if (joiner == "and") result = result && v;
    else result = result || v;
    haveResult = true;
  }
  return result;
}

bool EvaluateSupports(const std::string& raw);

bool EvaluateSupports(const std::string& raw) {
  std::string s = Trim(raw);
  if (s.empty()) return false;
  if (StartsWithIgnoreCase(s, "not ") || StartsWithIgnoreCase(s, "not(")) {
    return !EvaluateSupports(s.substr(3));
  }
  // Split top-level and/or.
  std::vector<std::string> terms;
  std::vector<std::string> joins;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && IsAsciiSpace((unsigned char)s[i])) ++i;
    if (i >= s.size()) break;
    size_t start = i;
    if (s[i] == '(' || StartsWithIgnoreCase(s.substr(i), "selector(")) {
      int depth = 0;
      for (; i < s.size(); ++i) {
        if (s[i] == '(') ++depth;
        else if (s[i] == ')' && --depth == 0) {
          ++i;
          break;
        }
      }
      terms.push_back(s.substr(start, i - start));
    } else {
      while (i < s.size() && !IsAsciiSpace((unsigned char)s[i]) && s[i] != '(') ++i;
      std::string w = AsciiLower(s.substr(start, i - start));
      if (w == "and" || w == "or") joins.push_back(w);
      else terms.push_back(w);
    }
  }
  bool result = false;
  for (size_t k = 0; k < terms.size(); ++k) {
    std::string t = terms[k];
    bool v = false;
    if (StartsWithIgnoreCase(t, "selector(")) {
      std::vector<ComplexSelector> sel;
      v = ParseSelectorList(t.substr(9, t.size() - 10), sel);
    } else if (!t.empty() && t[0] == '(') {
      std::string inner = Trim(t.substr(1, t.size() - 2));
      size_t colon = FindTopLevel(inner, 0, ":");
      if (colon != std::string::npos && !StartsWith(inner, "(")) {
        v = IsCssPropertySupported(AsciiLower(Trim(inner.substr(0, colon))),
                                   AsciiLower(Trim(inner.substr(colon + 1))));
      } else {
        v = EvaluateSupports(inner);
      }
    }
    if (k == 0) result = v;
    else if (k - 1 < joins.size() && joins[k - 1] == "or") result = result || v;
    else result = result && v;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Rule parsing

struct ParseState {
  Stylesheet* sheet;
  size_t order;
};

void ParseBlockContents(const std::string& block, const std::string& selectorText,
                        const std::vector<std::string>& media, ParseState& st);

std::string ResolveNestedSelector(const std::string& parent, const std::string& child) {
  std::vector<std::string> parents = SplitTopLevel(parent, ',');
  std::vector<std::string> kids = SplitTopLevel(child, ',');
  std::string out;
  for (size_t k = 0; k < kids.size(); ++k) {
    std::string kid = Trim(kids[k]);
    for (size_t p = 0; p < parents.size(); ++p) {
      std::string par = Trim(parents[p]);
      std::string sel;
      if (kid.find('&') != std::string::npos) sel = ReplaceAll(kid, "&", par);
      else sel = par + " " + kid;
      if (!out.empty()) out += ", ";
      out += sel;
    }
  }
  return out;
}

void AddRule(const std::string& selectorText, const std::vector<Declaration>& decls,
             const std::vector<std::string>& media, ParseState& st) {
  if (decls.empty()) return;
  StyleRule rule;
  if (!ParseSelectorList(Trim(selectorText), rule.selectors)) return;
  rule.declarations = decls;
  rule.media = media;
  rule.order = st.order++;
  st.sheet->rules.push_back(rule);
}

void ParseRules(const std::string& css, const std::vector<std::string>& media,
                ParseState& st, bool topLevel) {
  size_t i = 0;
  while (i < css.size()) {
    while (i < css.size() && (IsAsciiSpace((unsigned char)css[i]) || css[i] == ';')) ++i;
    if (i >= css.size()) break;
    if (css.compare(i, 4, "<!--") == 0) { i += 4; continue; }
    if (css.compare(i, 3, "-->") == 0) { i += 3; continue; }
    if (css[i] == '@') {
      size_t nameEnd = i + 1;
      while (nameEnd < css.size() && IsIdentChar((unsigned char)css[nameEnd])) ++nameEnd;
      std::string name = AsciiLower(css.substr(i + 1, nameEnd - i - 1));
      size_t stop = FindTopLevel(css, nameEnd, ";{");
      if (stop == std::string::npos) break;
      std::string prelude = Trim(css.substr(nameEnd, stop - nameEnd));
      if (css[stop] == ';') {
        if (name == "import" && topLevel) {
          std::string url = prelude;
          std::string mediaPart;
          if (StartsWithIgnoreCase(url, "url(")) {
            size_t close = url.find(')');
            mediaPart = close == std::string::npos ? "" : Trim(url.substr(close + 1));
            url = url.substr(4, close == std::string::npos ? std::string::npos : close - 4);
          } else if (!url.empty() && (url[0] == '"' || url[0] == '\'')) {
            size_t close = url.find(url[0], 1);
            mediaPart = close == std::string::npos ? "" : Trim(url.substr(close + 1));
            url = url.substr(0, close == std::string::npos ? std::string::npos : close + 1);
          }
          url = Trim(url);
          if (url.size() >= 2 && (url[0] == '"' || url[0] == '\''))
            url = url.substr(1, url.size() - 2);
          MediaContext defaultCtx;
          if (mediaPart.empty() || StartsWithIgnoreCase(mediaPart, "layer") ||
              StartsWithIgnoreCase(mediaPart, "supports") ||
              EvaluateCondition(mediaPart, defaultCtx))
            st.sheet->imports.push_back(url);
        }
        i = stop + 1;
        continue;
      }
      size_t end = MatchBrace(css, stop);
      std::string body = css.substr(stop + 1, end > stop ? end - stop - 1 : 0);
      i = end + 1;
      if (name == "media") {
        std::vector<std::string> m(media);
        m.push_back(prelude);
        ParseRules(body, m, st, false);
      } else if (name == "supports") {
        if (EvaluateSupports(prelude)) ParseRules(body, media, st, false);
      } else if (name == "font-face") {
        std::vector<Declaration> decls = ParseDeclarations(body);
        FontFace face;
        for (size_t k = 0; k < decls.size(); ++k) {
          const Declaration& d = decls[k];
          std::string v = Trim(d.value);
          if (d.property == "font-family") {
            if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'')) v = v.substr(1, v.size() - 2);
            face.family = AsciiLower(Trim(ReplaceAll(v, "\\", "")));
          } else if (d.property == "font-weight") {
            std::string w = AsciiLower(v);
            long long n;
            if (w == "bold") face.weight = 700;
            else if (ParseInt(SplitWhitespace(w).empty() ? w : SplitWhitespace(w)[0], n)) face.weight = (int)n;
          } else if (d.property == "unicode-range") {
            face.coversLatin = false;
            std::vector<std::string> ranges = SplitTopLevel(AsciiLower(v), ',');
            for (size_t q = 0; q < ranges.size(); ++q) {
              std::string r = Trim(ranges[q]);
              if (!StartsWith(r, "u+")) continue;
              r = r.substr(2);
              unsigned long lo, hi;
              size_t dash = r.find('-');
              std::string a = dash == std::string::npos ? r : r.substr(0, dash);
              std::string b = dash == std::string::npos ? r : r.substr(dash + 1);
              if (a.find('?') != std::string::npos) {
                b = ReplaceAll(a, "?", "f");
                a = ReplaceAll(a, "?", "0");
              }
              lo = strtoul(a.c_str(), 0, 16);
              hi = strtoul(b.c_str(), 0, 16);
              if (lo <= 0x61 && hi >= 0x61) face.coversLatin = true;
            }
          } else if (d.property == "font-style") {
            face.italic = StartsWithIgnoreCase(v, "italic") || StartsWithIgnoreCase(v, "oblique");
          } else if (d.property == "src") {
            std::vector<std::string> parts = SplitTopLevel(v, ',');
            for (size_t q = 0; q < parts.size(); ++q) {
              std::string part = Trim(parts[q]);
              if (!StartsWithIgnoreCase(part, "url(")) continue;
              size_t close = FindTopLevel(part, 4, ")");
              std::string url = Trim(part.substr(4, close == std::string::npos ? std::string::npos : close - 4));
              if (url.size() >= 2 && (url[0] == '"' || url[0] == '\'')) url = url.substr(1, url.size() - 2);
              std::string format;
              size_t f = AsciiLower(part).find("format(");
              if (f != std::string::npos) {
                format = AsciiLower(part.substr(f + 7));
                format = format.substr(0, format.find(')'));
                format = ReplaceAll(ReplaceAll(Trim(format), "\"", ""), "'", "");
              }
              face.sources.push_back(std::make_pair(url, format));
            }
          }
        }
        if (!face.family.empty() && !face.sources.empty()) st.sheet->fontFaces.push_back(face);
      } else if (name == "keyframes" || name == "-webkit-keyframes" || name == "-moz-keyframes" ||
                 name == "-o-keyframes") {
        KeyframesRule kr;
        kr.name = prelude;
        if (kr.name.size() >= 2 && (kr.name[0] == '"' || kr.name[0] == '\''))
          kr.name = kr.name.substr(1, kr.name.size() - 2);
        size_t k = 0;
        while (k < body.size()) {
          size_t open = FindTopLevel(body, k, "{");
          if (open == std::string::npos) break;
          size_t close = MatchBrace(body, open);
          std::string selectors = AsciiLower(body.substr(k, open - k));
          std::vector<Declaration> decls =
              ParseDeclarations(body.substr(open + 1, close > open ? close - open - 1 : 0));
          k = close + 1;
          std::vector<std::string> sels = SplitTopLevel(selectors, ',');
          for (size_t q = 0; q < sels.size(); ++q) {
            std::string sel = Trim(sels[q]);
            float off = -1;
            if (sel == "from") off = 0;
            else if (sel == "to") off = 1;
            else if (!sel.empty() && sel[sel.size() - 1] == '%') {
              size_t used = 0;
              double v = ParseDoublePrefix(sel, used);
              if (used == sel.size() - 1 && v >= 0 && v <= 100) off = (float)(v / 100);
            }
            if (off < 0) continue;
            Keyframe f;
            f.offset = off;
            f.declarations = decls;
            kr.frames.push_back(f);
          }
        }
        if (!kr.name.empty() && !kr.frames.empty()) st.sheet->keyframes.push_back(kr);
      } else if (name == "layer" || name == "scope" || name == "document" ||
                 name == "-moz-document" || name == "starting-style") {
        if (name != "starting-style") ParseRules(body, media, st, false);
      }
      // @font-face, @keyframes, @page, @container ... are ignored.
      continue;
    }
    size_t brace = FindTopLevel(css, i, "{;");
    if (brace == std::string::npos) break;
    if (css[brace] == ';') {  // stray declaration at top level
      i = brace + 1;
      continue;
    }
    size_t end = MatchBrace(css, brace);
    std::string selectorText = css.substr(i, brace - i);
    std::string body = css.substr(brace + 1, end > brace ? end - brace - 1 : 0);
    i = end + 1;
    ParseBlockContents(body, selectorText, media, st);
  }
}

// Parses declarations plus nested rules (CSS nesting) inside a style block.
void ParseBlockContents(const std::string& block, const std::string& selectorText,
                        const std::vector<std::string>& media, ParseState& st) {
  std::string decls;
  std::vector<std::pair<std::string, std::string> > nested;
  size_t i = 0;
  size_t segStart = 0;
  while (i < block.size()) {
    size_t p = FindTopLevel(block, i, ";{");
    if (p == std::string::npos) break;
    if (block[p] == ';') {
      i = p + 1;
      continue;
    }
    // '{': the segment since the last ';' is a nested rule prelude, unless it
    // is a "prop: value" pair that contains a brace (rare); treat as nested.
    size_t lastSemi = block.rfind(';', p);
    size_t preludeStart = (lastSemi == std::string::npos || lastSemi < segStart) ? segStart
                                                                                  : lastSemi + 1;
    decls += block.substr(segStart, preludeStart - segStart);
    size_t end = MatchBrace(block, p);
    nested.push_back(std::make_pair(Trim(block.substr(preludeStart, p - preludeStart)),
                                    block.substr(p + 1, end > p ? end - p - 1 : 0)));
    i = end + 1;
    segStart = i;
  }
  if (segStart < block.size()) decls += block.substr(segStart);
  AddRule(selectorText, ParseDeclarations(decls), media, st);
  for (size_t k = 0; k < nested.size(); ++k) {
    const std::string& prelude = nested[k].first;
    if (StartsWithIgnoreCase(prelude, "@media")) {
      std::vector<std::string> m(media);
      m.push_back(Trim(prelude.substr(6)));
      ParseBlockContents(nested[k].second, selectorText, m, st);
    } else if (StartsWithIgnoreCase(prelude, "@supports")) {
      if (EvaluateSupports(prelude.substr(9)))
        ParseBlockContents(nested[k].second, selectorText, media, st);
    } else if (!prelude.empty() && prelude[0] != '@') {
      ParseBlockContents(nested[k].second, ResolveNestedSelector(selectorText, prelude),
                         media, st);
    }
  }
}

}  // namespace

bool ParseSelectorList(const std::string& text, std::vector<ComplexSelector>& out) {
  SelectorParser p(text);
  return p.ParseList(out, false);
}

std::vector<Declaration> ParseDeclarations(const std::string& block) {
  std::vector<Declaration> out;
  std::vector<std::string> parts = SplitTopLevel(block, ';');
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    size_t colon = part.find(':');
    if (colon == std::string::npos) continue;
    std::string name = Trim(part.substr(0, colon));
    std::string value = Trim(part.substr(colon + 1));
    if (name.empty()) continue;
    Declaration d;
    if (!StartsWith(name, "--")) name = AsciiLower(name);
    // Reject names containing spaces or odd chars.
    bool ok = true;
    for (size_t k = 0; k < name.size(); ++k)
      if (!IsIdentChar((unsigned char)name[k])) ok = false;
    if (!ok) continue;
    d.property = name;
    // !important
    size_t bang = value.rfind('!');
    if (bang != std::string::npos) {
      std::string tail = AsciiLower(Trim(value.substr(bang + 1)));
      if (tail == "important") {
        d.important = true;
        value = Trim(value.substr(0, bang));
      }
    }
    if (value.empty() && !StartsWith(name, "--")) continue;
    d.value = value;
    out.push_back(d);
  }
  return out;
}

void ParseStylesheet(const std::string& rawCss, Stylesheet& out) {
  std::string css = StripCssComments(rawCss);
  ParseState st;
  st.sheet = &out;
  st.order = out.rules.size();
  std::vector<std::string> media;
  ParseRules(css, media, st, true);
}

bool EvaluateMediaQueryList(const std::string& query, const MediaContext& ctx) {
  std::string q = Trim(query);
  if (q.empty()) return true;
  std::vector<std::string> parts = SplitTopLevel(q, ',');
  for (size_t i = 0; i < parts.size(); ++i) {
    std::string part = Trim(parts[i]);
    // "not" at the start of a media query negates the whole query.
    if (StartsWithIgnoreCase(part, "not ")) {
      if (!EvaluateCondition(part.substr(4), ctx)) return true;
    } else if (EvaluateCondition(part, ctx)) {
      return true;
    }
  }
  return false;
}

}  // namespace kite
