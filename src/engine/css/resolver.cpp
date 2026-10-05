#include "css/resolver.h"

#include <algorithm>
#include <set>

#include "base/strings.h"
#include "css/properties.h"

namespace kite {

// ---------------------------------------------------------------------------
// Selector matching

namespace {

bool IsAncestorOrSelf(const Node* ancestor, const Node* n) {
  for (; n; n = n->parent)
    if (n == ancestor) return true;
  return false;
}

bool IsFormControl(const Node* el) {
  return el->tag == "input" || el->tag == "select" || el->tag == "textarea" ||
         el->tag == "button" || el->tag == "option" || el->tag == "optgroup" ||
         el->tag == "fieldset";
}

bool MatchNth(int a, int b, int idx) {
  if (a == 0) return idx == b;
  int diff = idx - b;
  if (diff % a != 0) return false;
  return diff / a >= 0;
}

bool AttrMatches(const SimpleSelector& s, const Node* el) {
  const std::string* val = el->GetAttr(s.name);
  if (!val) return false;
  if (s.op == SimpleSelector::kExists) return true;
  bool ci = s.caseInsensitive || s.name == "type";
  std::string a = ci ? AsciiLower(*val) : *val;
  std::string b = ci ? AsciiLower(s.value) : s.value;
  switch (s.op) {
    case SimpleSelector::kEquals: return a == b;
    case SimpleSelector::kIncludes: {
      if (b.empty()) return false;
      std::vector<std::string> parts = SplitWhitespace(a);
      for (size_t i = 0; i < parts.size(); ++i)
        if (parts[i] == b) return true;
      return false;
    }
    case SimpleSelector::kDashMatch: return a == b || StartsWith(a, b + "-");
    case SimpleSelector::kPrefix: return !b.empty() && StartsWith(a, b);
    case SimpleSelector::kSuffix: return !b.empty() && EndsWith(a, b);
    case SimpleSelector::kSubstring: return !b.empty() && a.find(b) != std::string::npos;
    default: return false;
  }
}

bool MatchPseudo(const SimpleSelector& s, const Node* el, const ElementState& st) {
  const std::string& n = s.name;
  const Node* parent = el->parent;
  if (n == "root" || n == "scope") return parent && parent->type == Node::kDocument;
  if (n == "first-child") return !el->PreviousElementSibling();
  if (n == "last-child") return !el->NextElementSibling();
  if (n == "only-child") return !el->PreviousElementSibling() && !el->NextElementSibling();
  if (n == "first-of-type" || n == "last-of-type" || n == "only-of-type" ||
      n == "nth-child" || n == "nth-last-child" || n == "nth-of-type" ||
      n == "nth-last-of-type") {
    if (!parent) return false;
    bool ofType = n.find("of-type") != std::string::npos;
    int before = 0, after = 0;
    for (size_t i = 0; i < parent->children.size(); ++i) {
      const Node* c = parent->children[i].get();
      if (!c->IsElement() || (ofType && c->tag != el->tag)) continue;
      if (i < el->index) ++before;
      else if (i > el->index) ++after;
    }
    if (n == "first-of-type") return before == 0;
    if (n == "last-of-type") return after == 0;
    if (n == "only-of-type") return before == 0 && after == 0;
    bool fromEnd = n.find("last") != std::string::npos;
    return MatchNth(s.nthA, s.nthB, (fromEnd ? after : before) + 1);
  }
  if (n == "empty") {
    for (size_t i = 0; i < el->children.size(); ++i) {
      const Node* c = el->children[i].get();
      if (c->IsElement()) return false;
      if (c->IsText() && !c->text.empty()) return false;
    }
    return true;
  }
  if (n == "link" || n == "any-link")
    return (el->tag == "a" || el->tag == "area") && el->HasAttr("href");
  if (n == "hover") return st.hovered && IsAncestorOrSelf(el, st.hovered);
  if (n == "focus" || n == "focus-visible") return st.focused == el;
  if (n == "focus-within") return st.focused && IsAncestorOrSelf(el, st.focused);
  if (n == "checked") {
    if (el->tag == "option") return el->HasAttr("selected");
    return el->checkedSet ? el->checked : el->HasAttr("checked");
  }
  if (n == "disabled") return IsFormControl(el) && el->HasAttr("disabled");
  if (n == "enabled") return IsFormControl(el) && !el->HasAttr("disabled");
  if (n == "required") return el->HasAttr("required");
  if (n == "optional") return IsFormControl(el) && !el->HasAttr("required");
  if (n == "read-write")
    return (el->tag == "input" || el->tag == "textarea") && !el->HasAttr("readonly") &&
           !el->HasAttr("disabled");
  if (n == "read-only")
    return !((el->tag == "input" || el->tag == "textarea") && !el->HasAttr("readonly") &&
             !el->HasAttr("disabled"));
  if (n == "placeholder-shown")
    return el->HasAttr("placeholder") &&
           (el->formValueSet ? el->formValue.empty() : el->Attr("value").empty());
  if (n == "valid" || n == "in-range" || n == "paused") return true;
  if (n == "open") return el->HasAttr("open");
  if (n == "closed") return !el->HasAttr("open");
  if (n == "dir") return s.value == "ltr";
  if (n == "lang") {
    for (const Node* p = el; p; p = p->parent) {
      const std::string* l = p->type == Node::kElement ? p->GetAttr("lang") : 0;
      if (l) {
        std::string v = AsciiLower(*l);
        return v == s.value || StartsWith(v, s.value + "-");
      }
    }
    return false;
  }
  if (n == "not") {
    for (size_t i = 0; i < s.args.size(); ++i)
      if (MatchesSelector(s.args[i], el, st)) return false;
    return true;
  }
  if (n == "is" || n == "where" || n == "matches" || n == "-webkit-any" || n == "-moz-any") {
    for (size_t i = 0; i < s.args.size(); ++i)
      if (MatchesSelector(s.args[i], el, st)) return true;
    return false;
  }
  if (n == "host") {
    if (!st.scope || el != st.scope->host) return false;
    ElementState outer = st;
    outer.scope = 0;
    for (size_t i = 0; i < s.args.size(); ++i)
      if (MatchesSelector(s.args[i], el, outer)) return true;
    return s.args.empty();
  }
  if (n == "host-context") {
    if (!st.scope || el != st.scope->host) return false;
    ElementState outer = st;
    outer.scope = 0;
    for (const Node* p = el; p && p->type == Node::kElement; p = p->parent)
      for (size_t i = 0; i < s.args.size(); ++i)
        if (MatchesSelector(s.args[i], p, outer)) return true;
    return false;
  }
  if (n == "has") {
    // Relative selectors: ' ' descendant, '>' child, '+' next, '~' later sibling.
    for (size_t i = 0; i < s.args.size(); ++i) {
      const ComplexSelector& rel = s.args[i];
      if (rel.leading == '>' || rel.leading == ' ') {
        std::vector<const Node*> stack;
        for (size_t k = el->children.size(); k-- > 0;) stack.push_back(el->children[k].get());
        while (!stack.empty()) {
          const Node* c = stack.back();
          stack.pop_back();
          if (!c->IsElement()) continue;
          if (MatchesSelector(rel, c, st)) return true;
          if (rel.leading == ' ')
            for (size_t k = c->children.size(); k-- > 0;) stack.push_back(c->children[k].get());
        }
      } else {
        for (const Node* c = el->NextElementSibling(); c; c = c->NextElementSibling()) {
          if (MatchesSelector(rel, c, st)) return true;
          if (rel.leading == '+') break;
        }
      }
    }
    return false;
  }
  if (n == "defined") return el->tag.find('-') == std::string::npos || el->customDefined;
  return false;  // visited, active, target ...
}

// Parent for ancestor combinators: inside a shadow tree, the walk ends at
// the shadow root, except that :host compounds may match the host.
const Node* MatchParent(const Node* el, const ElementState& st) {
  const Node* p = el->parent;
  if (!p) return 0;
  if (p->type == Node::kElement) return p;
  if (p->type == Node::kShadowRoot && p == st.scope) return p->host;
  return 0;
}

bool MatchCompound(const CompoundSelector& c, const Node* el, const ElementState& st) {
  if (st.scope && el == st.scope->host) {
    // Seen from inside its shadow tree, the host only matches :host().
    bool host = false;
    for (size_t i = 0; i < c.parts.size(); ++i) {
      const SimpleSelector& s = c.parts[i];
      if (s.kind == SimpleSelector::kPseudoClass && (s.name == "host" || s.name == "host-context")) host = true;
      else if (s.kind != SimpleSelector::kUniversal && s.kind != SimpleSelector::kPseudoClass) return false;
    }
    if (!host) return false;
  }
  for (size_t i = 0; i < c.parts.size(); ++i) {
    const SimpleSelector& s = c.parts[i];
    switch (s.kind) {
      case SimpleSelector::kUniversal: break;
      case SimpleSelector::kType:
        if (el->tag != s.name) return false;
        break;
      case SimpleSelector::kId:
        if (el->id != s.name) return false;
        break;
      case SimpleSelector::kClass: {
        bool found = false;
        for (size_t k = 0; k < el->classes.size(); ++k)
          if (el->classes[k] == s.name) {
            found = true;
            break;
          }
        if (!found) return false;
        break;
      }
      case SimpleSelector::kAttr:
        if (!AttrMatches(s, el)) return false;
        break;
      case SimpleSelector::kPseudoClass:
        if (!MatchPseudo(s, el, st)) return false;
        break;
    }
  }
  return true;
}

bool MatchFrom(const ComplexSelector& sel, int idx, const Node* el, const ElementState& st) {
  if (!MatchCompound(sel.compounds[idx], el, st)) return false;
  if (idx == 0) return true;
  char comb = sel.combinators[idx - 1];
  switch (comb) {
    case '>': {
      const Node* p = MatchParent(el, st);
      return p && MatchFrom(sel, idx - 1, p, st);
    }
    case 's': {  // ::slotted(): el is assigned to a slot of the scope
      const Node* slot = el->AssignedSlot();
      return slot && st.scope && slot->TreeRoot() == st.scope && MatchFrom(sel, idx - 1, slot, st);
    }
    case '+': {
      const Node* p = el->PreviousElementSibling();
      return p && MatchFrom(sel, idx - 1, p, st);
    }
    case '~': {
      for (const Node* p = el->PreviousElementSibling(); p; p = p->PreviousElementSibling())
        if (MatchFrom(sel, idx - 1, p, st)) return true;
      return false;
    }
    default: {
      for (const Node* p = MatchParent(el, st); p; p = MatchParent(p, st))
        if (MatchFrom(sel, idx - 1, p, st)) return true;
      return false;
    }
  }
}

}  // namespace

bool MatchesSelector(const ComplexSelector& sel, const Node* el, const ElementState& st) {
  if (sel.compounds.empty() || sel.pseudo == kPseudoUnsupported) return false;
  return MatchFrom(sel, (int)sel.compounds.size() - 1, el, st);
}

// ---------------------------------------------------------------------------
// Cascade

namespace {

typedef std::map<std::string, std::string> CustomMap;

struct MatchedDecl {
  const Declaration* decl;
  int level;
  int specificity;
  size_t order;
  const std::string* baseUrl;
};

bool DeclLess(const MatchedDecl& a, const MatchedDecl& b) {
  if (a.level != b.level) return a.level < b.level;
  if (a.specificity != b.specificity) return a.specificity < b.specificity;
  return a.order < b.order;
}

// Replaces var(--x, fallback) references. Returns false if a reference
// cannot be resolved (the declaration is then invalid at computed time).
bool SubstituteVars(const std::string& in, const CustomMap* vars, std::string& out, int depth) {
  if (depth > 16) return false;
  size_t p = in.find("var(");
  if (p == std::string::npos) {
    out = in;
    return true;
  }
  out.clear();
  size_t pos = 0;
  while (p != std::string::npos) {
    out += in.substr(pos, p - pos);
    // Find matching paren.
    int d = 0;
    size_t end = std::string::npos;
    for (size_t i = p + 3; i < in.size(); ++i) {
      if (in[i] == '(') ++d;
      else if (in[i] == ')' && --d == 0) {
        end = i;
        break;
      }
    }
    if (end == std::string::npos) return false;
    std::string inner = in.substr(p + 4, end - p - 4);
    size_t comma = inner.find(',');
    std::string name = Trim(inner.substr(0, comma));
    bool hasFallback = comma != std::string::npos;
    std::string fallback = hasFallback ? Trim(inner.substr(comma + 1)) : std::string();
    std::string resolved;
    bool ok = false;
    if (vars) {
      CustomMap::const_iterator it = vars->find(name);
      if (it != vars->end() && !Trim(it->second).empty() &&
          AsciiLower(Trim(it->second)) != "initial") {
        ok = SubstituteVars(it->second, vars, resolved, depth + 1);
      }
    }
    if (!ok && hasFallback) ok = SubstituteVars(fallback, vars, resolved, depth + 1);
    if (!ok) return false;
    out += resolved;
    pos = end + 1;
    p = in.find("var(", pos);
  }
  out += in.substr(pos);
  return true;
}

std::string SubstituteAttr(const std::string& value, const Node* el) {
  std::string lv = AsciiLower(value);
  size_t p = lv.find("attr(");
  if (p == std::string::npos) return value;
  size_t end = value.find(')', p);
  if (end == std::string::npos) return value;
  std::string name = AsciiLower(Trim(value.substr(p + 5, end - p - 5)));
  std::string attr = el->Attr(name);
  std::string quoted = "\"" + ReplaceAll(attr, "\"", "\\\"") + "\"";
  return SubstituteAttr(value.substr(0, p) + quoted + value.substr(end + 1), el);
}

void AddHint(std::vector<Declaration>& out, const char* prop, const std::string& value) {
  Declaration d;
  d.property = prop;
  d.value = value;
  out.push_back(d);
}

// HTML attribute value as a CSS length ("100" -> "100px", "50%" stays).
std::string HtmlLength(const std::string& raw) {
  std::string v = Trim(raw);
  if (v.empty()) return std::string();
  size_t used;
  double n = ParseDoublePrefix(v, used);
  if (used == 0 || n < 0) return std::string();
  if (used < v.size() && v[used] == '%') return DoubleToString(n) + "%";
  return DoubleToString(n) + "px";
}

std::string HtmlColor(const std::string& raw) {
  std::string v = Trim(raw);
  if (v.empty()) return std::string();
  Color c;
  if (ParseColor(v, c, Color())) return v;
  // Legacy "ff0000" without '#'.
  bool hex = v.size() == 6 || v.size() == 3;
  for (size_t i = 0; i < v.size(); ++i)
    if (!IsAsciiHex((unsigned char)v[i])) hex = false;
  if (hex) return "#" + v;
  return std::string();
}

const Node* FindTable(const Node* el) {
  for (const Node* p = el->parent; p; p = p->parent)
    if (p->Is("table")) return p;
  return 0;
}

void PresentationalHints(const Node* el, std::vector<Declaration>& out) {
  const std::string& t = el->tag;
  std::string v;
  if (!(v = HtmlColor(el->Attr("bgcolor"))).empty())
    AddHint(out, "background-color", v);
  if (el->HasAttr("background") && (t == "body" || t == "table" || t == "td" || t == "th"))
    AddHint(out, "background-image", "url(\"" + el->Attr("background") + "\")");
  if (t == "body") {
    if (!(v = HtmlColor(el->Attr("text"))).empty()) AddHint(out, "color", v);
    if (!(v = HtmlLength(el->Attr("marginwidth"))).empty()) {
      AddHint(out, "margin-left", v);
      AddHint(out, "margin-right", v);
    }
    if (!(v = HtmlLength(el->Attr("leftmargin"))).empty()) AddHint(out, "margin-left", v);
    if (!(v = HtmlLength(el->Attr("topmargin"))).empty()) AddHint(out, "margin-top", v);
    if (!(v = HtmlLength(el->Attr("marginheight"))).empty()) {
      AddHint(out, "margin-top", v);
      AddHint(out, "margin-bottom", v);
    }
  }
  if (t == "font") {
    if (!(v = HtmlColor(el->Attr("color"))).empty()) AddHint(out, "color", v);
    if (el->HasAttr("face")) AddHint(out, "font-family", el->Attr("face"));
    std::string size = Trim(el->Attr("size"));
    if (!size.empty()) {
      long long n;
      bool rel = size[0] == '+' || size[0] == '-';
      if (ParseInt(size, n)) {
        if (rel) n += 3;
        if (n < 1) n = 1;
        if (n > 7) n = 7;
        static const char* sizes[] = {"", "10px", "13px", "16px", "18px", "24px", "32px", "48px"};
        AddHint(out, "font-size", sizes[n]);
      }
    }
  }
  if (t == "a" && el->HasAttr("href")) {
    // <body link=...>
    for (const Node* p = el->parent; p; p = p->parent) {
      if (p->Is("body")) {
        if (!(v = HtmlColor(p->Attr("link"))).empty()) AddHint(out, "color", v);
        break;
      }
    }
  }
  std::string align = AsciiLower(el->Attr("align"));
  if (!align.empty()) {
    if (t == "div" || t == "p" || t == "h1" || t == "h2" || t == "h3" || t == "h4" ||
        t == "h5" || t == "h6" || t == "td" || t == "th" || t == "tr" || t == "caption" ||
        t == "thead" || t == "tbody" || t == "tfoot" || t == "center") {
      if (align == "center" || align == "middle" || align == "absmiddle")
        AddHint(out, "text-align", "center");
      else if (align == "right" || align == "left" || align == "justify")
        AddHint(out, "text-align", align);
    } else if (t == "table" || t == "hr") {
      if (align == "center") {
        AddHint(out, "margin-left", "auto");
        AddHint(out, "margin-right", "auto");
      } else if (t == "table" && (align == "left" || align == "right")) {
        AddHint(out, "float", align);
      } else if (t == "hr" && align == "left") {
        AddHint(out, "margin-left", "0");
      } else if (t == "hr" && align == "right") {
        AddHint(out, "margin-right", "0");
      }
    } else if (t == "img" || t == "iframe" || t == "object" || t == "embed" || t == "input") {
      if (align == "left" || align == "right") AddHint(out, "float", align);
      else if (align == "middle" || align == "absmiddle" || align == "center")
        AddHint(out, "vertical-align", "middle");
      else if (align == "top" || align == "texttop") AddHint(out, "vertical-align", "top");
      else if (align == "bottom" || align == "absbottom" || align == "baseline")
        AddHint(out, "vertical-align", "baseline");
    }
  }
  std::string valign = AsciiLower(el->Attr("valign"));
  if (!valign.empty() && (t == "td" || t == "th" || t == "tr" || t == "tbody" ||
                          t == "thead" || t == "tfoot"))
    AddHint(out, "vertical-align", valign == "center" ? "middle" : valign);
  if (t == "img" || t == "table" || t == "td" || t == "th" || t == "iframe" ||
      t == "video" || t == "canvas" || t == "object" || t == "embed" || t == "hr" ||
      t == "col" || t == "svg" || t == "input" || t == "colgroup" || t == "tr") {
    if (!(v = HtmlLength(el->Attr("width"))).empty() && t != "tr") {
      if (!(t == "input" && AsciiLower(el->Attr("type")) != "image"))
        AddHint(out, "width", v);
    }
    if (!(v = HtmlLength(el->Attr("height"))).empty() && t != "col" && t != "colgroup") {
      if (!(t == "input" && AsciiLower(el->Attr("type")) != "image"))
        AddHint(out, "height", v);
    }
  }
  if (t == "img" || t == "object" || t == "embed") {
    if (!(v = HtmlLength(el->Attr("hspace"))).empty()) {
      AddHint(out, "margin-left", v);
      AddHint(out, "margin-right", v);
    }
    if (!(v = HtmlLength(el->Attr("vspace"))).empty()) {
      AddHint(out, "margin-top", v);
      AddHint(out, "margin-bottom", v);
    }
    if (el->HasAttr("border")) {
      v = HtmlLength(el->Attr("border"));
      if (v.empty()) v = "0";
      AddHint(out, "border-width", v);
      AddHint(out, "border-style", "solid");
    }
  }
  if ((t == "td" || t == "th") && el->HasAttr("nowrap")) AddHint(out, "white-space", "nowrap");
  if (t == "hr") {
    if (!(v = HtmlLength(el->Attr("size"))).empty()) AddHint(out, "height", v);
    if (!(v = HtmlColor(el->Attr("color"))).empty()) {
      AddHint(out, "border-color", v);
      AddHint(out, "background-color", v);
    }
    if (el->HasAttr("noshade")) {
      AddHint(out, "border-style", "solid");
      AddHint(out, "background-color", "gray");
    }
  }
  if (t == "table") {
    if (el->HasAttr("border")) {
      v = HtmlLength(el->Attr("border"));
      if (v.empty() || v == "0px") v = el->Attr("border").empty() ? "1px" : v;
      if (v != "0px") {
        AddHint(out, "border-width", v);
        AddHint(out, "border-style", "outset");
        AddHint(out, "border-color", "#808080");
      }
    }
    if (el->HasAttr("cellspacing")) {
      v = HtmlLength(el->Attr("cellspacing"));
      if (!v.empty()) AddHint(out, "border-spacing", v);
    }
    if (!(v = HtmlColor(el->Attr("bordercolor"))).empty()) AddHint(out, "border-color", v);
  }
  if (t == "td" || t == "th") {
    const Node* table = FindTable(el);
    if (table) {
      if (table->HasAttr("cellpadding")) {
        v = HtmlLength(table->Attr("cellpadding"));
        if (!v.empty()) AddHint(out, "padding", v);
      }
      if (table->HasAttr("border")) {
        std::string b = Trim(table->Attr("border"));
        if (b != "0") {
          AddHint(out, "border-width", "1px");
          AddHint(out, "border-style", "inset");
          AddHint(out, "border-color", "#808080");
        }
      }
    }
  }
  if (t == "input") {
    std::string type = AsciiLower(el->Attr("type"));
    long long n;
    if ((type.empty() || type == "text" || type == "search" || type == "password" ||
         type == "email" || type == "url" || type == "tel" || type == "number") &&
        ParseInt(el->Attr("size"), n) && n > 0 && n < 500)
      AddHint(out, "width", IntToString(n * 7 + 6) + "px");
  }
  if (t == "textarea") {
    long long n;
    if (ParseInt(el->Attr("cols"), n) && n > 0 && n < 500)
      AddHint(out, "width", IntToString(n * 8 + 6) + "px");
    if (ParseInt(el->Attr("rows"), n) && n > 0 && n < 500)
      AddHint(out, "height", DoubleToString(n * 1.2) + "em");
  }
  if (t == "ol" || t == "ul" || t == "li") {
    std::string type = el->Attr("type");
    if (type == "1") AddHint(out, "list-style-type", "decimal");
    else if (type == "a") AddHint(out, "list-style-type", "lower-alpha");
    else if (type == "A") AddHint(out, "list-style-type", "upper-alpha");
    else if (type == "i") AddHint(out, "list-style-type", "lower-roman");
    else if (type == "I") AddHint(out, "list-style-type", "upper-roman");
  }
}

}  // namespace

StyleResolver::StyleResolver() : rootFontSize_(16) {
  std::shared_ptr<Stylesheet> ua(new Stylesheet);
  ParseStylesheet(UserAgentCss(), *ua);
  SheetEntry e;
  e.sheet = ua;
  e.origin = 0;
  sheets_.push_back(e);
}

void StyleResolver::AddAuthorSheet(std::shared_ptr<Stylesheet> sheet, const std::string& baseUrl,
                                   const Node* scope) {
  SheetEntry e;
  e.sheet = sheet;
  e.baseUrl = baseUrl;
  e.origin = 1;
  e.scope = scope;
  sheets_.push_back(e);
}

void StyleResolver::ClearAuthorSheets() { sheets_.resize(1); }

void StyleResolver::BuildIndex(const MediaContext& media) {
  index_ = RuleIndex();
  std::map<std::string, bool> mediaCache;
  for (size_t si = 0; si < sheets_.size(); ++si) {
    const Stylesheet& sheet = *sheets_[si].sheet;
    for (size_t ri = 0; ri < sheet.rules.size(); ++ri) {
      const StyleRule& rule = sheet.rules[ri];
      bool ok = true;
      for (size_t m = 0; m < rule.media.size() && ok; ++m) {
        std::map<std::string, bool>::iterator it = mediaCache.find(rule.media[m]);
        if (it == mediaCache.end()) {
          bool r = EvaluateMediaQueryList(rule.media[m], media);
          mediaCache[rule.media[m]] = r;
          ok = r;
        } else {
          ok = it->second;
        }
      }
      if (!ok) continue;
      for (size_t k = 0; k < rule.selectors.size(); ++k) {
        const ComplexSelector& sel = rule.selectors[k];
        IndexedRule ir;
        ir.rule = &rule;
        ir.selector = &sel;
        ir.sheetIndex = (int)si;
        ir.origin = sheets_[si].origin;
        ir.order = (si << 22) + rule.order;
        ir.scope = sheets_[si].scope;
        ir.hasHost = ir.hasSlotted = false;
        if (ir.scope) {
          for (size_t c = 0; c < sel.combinators.size(); ++c)
            if (sel.combinators[c] == 's') ir.hasSlotted = true;
          for (size_t c = 0; c < sel.compounds.size(); ++c)
            for (size_t p = 0; p < sel.compounds[c].parts.size(); ++p) {
              const SimpleSelector& sp = sel.compounds[c].parts[p];
              if (sp.kind == SimpleSelector::kPseudoClass && (sp.name == "host" || sp.name == "host-context"))
                ir.hasHost = true;
            }
        }
        const CompoundSelector& last = sel.compounds.back();
        std::string id, cls, tag;
        for (size_t p = 0; p < last.parts.size(); ++p) {
          const SimpleSelector& s = last.parts[p];
          if (s.kind == SimpleSelector::kId && id.empty()) id = s.name;
          else if (s.kind == SimpleSelector::kClass && cls.empty()) cls = s.name;
          else if (s.kind == SimpleSelector::kType && tag.empty()) tag = s.name;
        }
        if (!id.empty()) index_.byId[id].push_back(ir);
        else if (!cls.empty()) index_.byClass[cls].push_back(ir);
        else if (!tag.empty()) index_.byTag[tag].push_back(ir);
        else index_.universal.push_back(ir);
      }
    }
  }
}

void StyleResolver::CollectMatches(const Node* el, const ElementState& state,
                                   std::vector<IndexedRule>& out) {
  // Shadow DOM: rules only see their own tree, except :host rules (which
  // match the host) and ::slotted() rules (host children in slots).
  const Node* root = el->TreeRoot();
  const Node* elScope = root->type == Node::kShadowRoot ? root : nullptr;
  ElementState st = state;
  auto consider = [&](const IndexedRule& ir) {
    if (ir.origin != 0 && ir.scope != elScope) {
      if (!ir.scope) return;
      bool host = ir.hasHost && ir.scope->host == el;
      bool slotted = ir.hasSlotted && el->parent == ir.scope->host;
      if (!host && !slotted) return;
    }
    st.scope = ir.origin == 0 ? elScope : ir.scope;
    if (MatchesSelector(*ir.selector, el, st)) out.push_back(ir);
  };
  std::map<std::string, std::vector<IndexedRule> >::const_iterator it;
  if (!el->id.empty() && (it = index_.byId.find(el->id)) != index_.byId.end())
    for (size_t i = 0; i < it->second.size(); ++i) consider(it->second[i]);
  for (size_t c = 0; c < el->classes.size(); ++c) {
    bool dup = false;
    for (size_t d = 0; d < c; ++d)
      if (el->classes[d] == el->classes[c]) dup = true;
    if (dup) continue;
    if ((it = index_.byClass.find(el->classes[c])) != index_.byClass.end())
      for (size_t i = 0; i < it->second.size(); ++i) consider(it->second[i]);
  }
  if ((it = index_.byTag.find(el->tag)) != index_.byTag.end())
    for (size_t i = 0; i < it->second.size(); ++i) consider(it->second[i]);
  for (size_t i = 0; i < index_.universal.size(); ++i) consider(index_.universal[i]);
}

namespace {

void ComputeFromDecls(std::vector<MatchedDecl>& decls, ComputedStyle& s,
                      const ComputedStyle& parent, const Node* el, float rootFontSize,
                      const MediaContext& media) {
  std::stable_sort(decls.begin(), decls.end(), DeclLess);
  // 1. Custom properties.
  CustomMap* own = 0;
  for (size_t i = 0; i < decls.size(); ++i) {
    const Declaration& d = *decls[i].decl;
    if (!StartsWith(d.property, "--")) continue;
    if (!own) own = s.customProperties ? new CustomMap(*s.customProperties) : new CustomMap;
    (*own)[d.property] = d.value;
  }
  if (own) s.customProperties.reset(own);
  const CustomMap* vars = s.customProperties.get();

  // 2. Specified values for longhands.
  std::vector<std::string> values(kPropCount);
  std::vector<const std::string*> bases(kPropCount, (const std::string*)0);
  std::vector<char> has(kPropCount, 0);
  std::vector<std::pair<int, std::string> > expanded;
  for (size_t i = 0; i < decls.size(); ++i) {
    const Declaration& d = *decls[i].decl;
    if (StartsWith(d.property, "--")) continue;
    std::string value;
    if (!SubstituteVars(d.value, vars, value, 0)) value = "unset";
    if (d.property == "content") value = SubstituteAttr(value, el);
    expanded.clear();
    if (!ExpandProperty(d.property, value, expanded)) continue;
    for (size_t k = 0; k < expanded.size(); ++k) {
      int id = expanded[k].first;
      values[id] = expanded[k].second;
      bases[id] = decls[i].baseUrl;
      has[id] = 1;
    }
  }

  // 3. Computed values (property enum order puts font-size and color first).
  ApplyContext ctx;
  ctx.parent = &parent;
  ctx.rootFontSize = rootFontSize;
  ctx.viewportW = media.width;
  ctx.viewportH = media.height;
  static const std::string kEmpty;
  for (int id = 0; id < kPropCount; ++id) {
    if (!has[id]) continue;
    ctx.baseUrl = bases[id] ? bases[id] : &kEmpty;
    ApplyProperty(id, values[id], s, ctx);
  }
  if (s.lineHeightFactor > 0) s.lineHeight = s.lineHeightFactor * s.fontSize;
  // Blockification of floats / absolutely positioned boxes.
  if (s.IsFloating() || s.IsOutOfFlow()) {
    switch (s.display) {
      case kDisplayInline:
      case kDisplayInlineBlock:
      case kDisplayTableRow:
      case kDisplayTableRowGroup:
      case kDisplayTableHeaderGroup:
      case kDisplayTableFooterGroup:
      case kDisplayTableCell:
      case kDisplayTableCaption:
      case kDisplayTableColumn:
      case kDisplayTableColumnGroup:
        s.display = kDisplayBlock;
        break;
      case kDisplayInlineTable: s.display = kDisplayTable; break;
      case kDisplayInlineFlex: s.display = kDisplayFlex; break;
      case kDisplayInlineGrid: s.display = kDisplayGrid; break;
      default: break;
    }
  }
  if (s.IsOutOfFlow()) s.floating = kFloatNone;
}

}  // namespace

void StyleResolver::ResolveElement(Node* el, const ComputedStyle& parent,
                                   const MediaContext& media, const std::string& docUrl,
                                   const ElementState& state) {
  std::vector<IndexedRule> matched;
  CollectMatches(el, state, matched);

  std::vector<MatchedDecl> decls, before, after;
  for (size_t i = 0; i < matched.size(); ++i) {
    const IndexedRule& ir = matched[i];
    std::vector<MatchedDecl>* target = &decls;
    if (ir.selector->pseudo == kPseudoUnsupported) continue;
    if (ir.selector->pseudo == kPseudoBefore) target = &before;
    else if (ir.selector->pseudo == kPseudoAfter) target = &after;
    for (size_t k = 0; k < ir.rule->declarations.size(); ++k) {
      const Declaration& d = ir.rule->declarations[k];
      MatchedDecl md;
      md.decl = &d;
      if (ir.origin == 0) md.level = d.important ? 6 : 0;
      else md.level = d.important ? 4 : 2;
      md.specificity = ir.selector->specificity;
      md.order = ir.order;
      md.baseUrl = &sheets_[ir.sheetIndex].baseUrl;
      target->push_back(md);
    }
  }
  std::vector<Declaration> hints;
  PresentationalHints(el, hints);
  for (size_t i = 0; i < hints.size(); ++i) {
    MatchedDecl md;
    md.decl = &hints[i];
    md.level = 1;
    md.specificity = 0;
    md.order = i;
    md.baseUrl = &docUrl;
    decls.push_back(md);
  }
  std::vector<Declaration> inlineDecls;
  const std::string* styleAttr = el->GetAttr("style");
  if (styleAttr) {
    inlineDecls = ParseDeclarations(StripCssComments(*styleAttr));
    for (size_t i = 0; i < inlineDecls.size(); ++i) {
      MatchedDecl md;
      md.decl = &inlineDecls[i];
      md.level = inlineDecls[i].important ? 5 : 3;
      md.specificity = 0;
      md.order = i;
      md.baseUrl = &docUrl;
      decls.push_back(md);
    }
  }

  ComputedStyle* s = new ComputedStyle;
  s->InheritFrom(parent);
  ComputeFromDecls(decls, *s, parent, el, rootFontSize_, media);
  if (el->parent && el->parent->type == Node::kDocument) {
    rootFontSize_ = s->fontSize;
    if (s->display == kDisplayInline || s->display == kDisplayContents) s->display = kDisplayBlock;
  }

  const std::vector<MatchedDecl>* pseudos[2] = {&before, &after};
  for (int k = 0; k < 2; ++k) {
    if (pseudos[k]->empty()) continue;
    std::vector<MatchedDecl> list = *pseudos[k];
    std::unique_ptr<ComputedStyle> ps(new ComputedStyle);
    ps->InheritFrom(*s);
    ComputeFromDecls(list, *ps, *s, el, rootFontSize_, media);
    if (ps->hasContent && ps->display != kDisplayNone) {
      if (k == 0) s->before = std::move(ps);
      else s->after = std::move(ps);
    }
  }

  delete el->style;
  el->style = s;

  // Children. Elements with display:none still get styles (cheap) only if
  // needed; skip their subtrees for speed.
  if (s->display == kDisplayNone) {
    for (size_t i = 0; i < el->children.size(); ++i) {
      Node* c = el->children[i].get();
      if (c->style) {
        delete c->style;
        c->style = 0;
      }
    }
    return;
  }
  // Inheritance follows the flat tree (shadow trees, slotted children).
  std::vector<Node*> kids;
  el->FlatChildren(kids);
  for (size_t i = 0; i < kids.size(); ++i)
    if (kids[i]->IsElement()) ResolveElement(kids[i], *s, media, docUrl, state);
}

void StyleResolver::ResolveDocument(Document& doc, const MediaContext& media,
                                    const std::string& documentUrl, const ElementState& state) {
  BuildIndex(media);
  rootFontSize_ = 16;
  ComputedStyle initial;
  initial.display = kDisplayBlock;
  for (size_t i = 0; i < doc.root->children.size(); ++i) {
    Node* c = doc.root->children[i].get();
    if (c->IsElement()) ResolveElement(c, initial, media, documentUrl, state);
  }
}

bool SubstituteCssVars(const std::string& value, const ComputedStyle& style, std::string& out) {
  return SubstituteVars(value, style.customProperties.get(), out, 0);
}

const KeyframesRule* StyleResolver::FindKeyframes(const std::string& name) const {
  for (size_t i = sheets_.size(); i-- > 0;) {
    const std::vector<KeyframesRule>& kf = sheets_[i].sheet->keyframes;
    for (size_t k = kf.size(); k-- > 0;)
      if (kf[k].name == name) return &kf[k];
  }
  return 0;
}

}  // namespace kite
