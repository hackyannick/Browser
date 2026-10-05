// Native part of Kite's DOM bindings. The web-facing API (Node, Element,
// Event, fetch, ...) is implemented in JavaScript on top of the small set of
// primitives registered here as the global "__kite" object (see dom_js.cpp).
#include "script/script.h"

#include <algorithm>
#include <cstring>
#include <set>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include <cmath>

#include "base/strings.h"
#include "canvas/canvas.h"
#include "css/style.h"
#include "css/resolver.h"
#include "css/stylesheet.h"
#include "html/parser.h"
#include "net/url.h"
#include "page/page.h"

extern "C" {
#include "quickjs.h"
}

namespace kite {

extern const char* const kDomPrelude;

namespace {

ScriptEngine* Engine(JSContext* ctx) { return (ScriptEngine*)JS_GetContextOpaque(ctx); }

std::string Str(JSContext* ctx, JSValueConst v) {
  size_t len;
  const char* s = JS_ToCStringLen(ctx, &len, v);
  if (!s) return std::string();
  std::string out(s, len);
  JS_FreeCString(ctx, s);
  return out;
}

int Int(JSContext* ctx, JSValueConst v) {
  int32_t i = 0;
  JS_ToInt32(ctx, &i, v);
  return i;
}

double Num(JSContext* ctx, JSValueConst v) {
  double d = 0;
  JS_ToFloat64(ctx, &d, v);
  return d;
}

JSValue NewStr(JSContext* ctx, const std::string& s) { return JS_NewStringLen(ctx, s.data(), s.size()); }

Node* Arg(JSContext* ctx, JSValueConst v) { return Engine(ctx)->NodeOf(Int(ctx, v)); }

JSValue Handle(JSContext* ctx, Node* n) { return JS_NewInt32(ctx, n ? Engine(ctx)->HandleOf(n) : 0); }

JSValue HandleArray(JSContext* ctx, const std::vector<Node*>& nodes) {
  JSValue arr = JS_NewArray(ctx);
  for (size_t i = 0; i < nodes.size(); ++i)
    JS_SetPropertyUint32(ctx, arr, (uint32_t)i, Handle(ctx, nodes[i]));
  return arr;
}

#define KITE_FN(name) JSValue name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
#define ARGS_AT_LEAST(n) \
  if (argc < (n)) return JS_ThrowTypeError(ctx, "not enough arguments")

KITE_FN(Doc) {
  return Handle(ctx, Engine(ctx)->page()->document()->root.get());
}

KITE_FN(NodeType) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  if (!n) return JS_NewInt32(ctx, 0);
  switch (n->type) {
    case Node::kElement: return JS_NewInt32(ctx, n->tag == "#document-fragment" ? 11 : 1);
    case Node::kText: return JS_NewInt32(ctx, 3);
    case Node::kComment: return JS_NewInt32(ctx, 8);
    case Node::kDocument: return JS_NewInt32(ctx, 9);
    case Node::kShadowRoot: return JS_NewInt32(ctx, 11);
    default: return JS_NewInt32(ctx, 10);
  }
}

KITE_FN(Name) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  return NewStr(ctx, n ? n->tag : std::string());
}

KITE_FN(Data) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  return NewStr(ctx, n ? n->text : std::string());
}

KITE_FN(SetData) {
  ARGS_AT_LEAST(2);
  Node* n = Arg(ctx, argv[0]);
  if (n) {
    n->text = Str(ctx, argv[1]);
    Engine(ctx)->MarkDirty();
  }
  return JS_UNDEFINED;
}

KITE_FN(Parent) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  return Handle(ctx, n ? n->parent : 0);
}

KITE_FN(Kids) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  std::vector<Node*> kids;
  if (n)
    for (size_t i = 0; i < n->children.size(); ++i) kids.push_back(n->children[i].get());
  return HandleArray(ctx, kids);
}

KITE_FN(GetAttr) {
  ARGS_AT_LEAST(2);
  Node* n = Arg(ctx, argv[0]);
  if (!n) return JS_NULL;
  const std::string* v = n->GetAttr(AsciiLower(Str(ctx, argv[1])));
  return v ? NewStr(ctx, *v) : JS_NULL;
}

KITE_FN(SetAttr) {
  ARGS_AT_LEAST(3);
  Node* n = Arg(ctx, argv[0]);
  if (n && n->IsElement()) {
    std::string name = AsciiLower(Str(ctx, argv[1]));
    std::string value = Str(ctx, argv[2]);
    n->SetAttr(name, value);
    if (name == "value" && (n->tag == "input")) n->formValueSet = false;
    if (name == "checked") n->checkedSet = false;
    Engine(ctx)->MarkDirty();
  }
  return JS_UNDEFINED;
}

KITE_FN(DelAttr) {
  ARGS_AT_LEAST(2);
  Node* n = Arg(ctx, argv[0]);
  if (n) {
    std::string name = AsciiLower(Str(ctx, argv[1]));
    for (size_t i = 0; i < n->attrs.size(); ++i)
      if (n->attrs[i].name == name) {
        n->attrs.erase(n->attrs.begin() + i);
        n->UpdateCaches();
        Engine(ctx)->MarkDirty();
        break;
      }
  }
  return JS_UNDEFINED;
}

KITE_FN(Attrs) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  JSValue arr = JS_NewArray(ctx);
  if (n)
    for (size_t i = 0; i < n->attrs.size(); ++i) {
      JS_SetPropertyUint32(ctx, arr, (uint32_t)(i * 2), NewStr(ctx, n->attrs[i].name));
      JS_SetPropertyUint32(ctx, arr, (uint32_t)(i * 2 + 1), NewStr(ctx, n->attrs[i].value));
    }
  return arr;
}

KITE_FN(Create) {
  ARGS_AT_LEAST(2);
  int kind = Int(ctx, argv[0]);
  std::unique_ptr<Node> n(new Node(kind == 1 ? Node::kElement : kind == 8 ? Node::kComment : Node::kText));
  if (kind == 1 || kind == 11) {
    n->type = Node::kElement;
    n->tag = kind == 11 ? std::string("#document-fragment") : AsciiLower(Str(ctx, argv[1]));
  } else {
    n->text = Str(ctx, argv[1]);
  }
  Node* raw = n.get();
  Engine(ctx)->Adopt(std::move(n));
  return Handle(ctx, raw);
}

// Detaches |n| from wherever it lives and returns ownership.
std::unique_ptr<Node> Detach(ScriptEngine* e, Node* n) { return e->DetachNode(n); }

KITE_FN(Insert) {
  ARGS_AT_LEAST(3);
  ScriptEngine* e = Engine(ctx);
  Node* parent = Arg(ctx, argv[0]);
  Node* child = Arg(ctx, argv[1]);
  Node* before = Arg(ctx, argv[2]);
  if (!parent || !child || child == parent) return JS_UNDEFINED;
  for (Node* p = parent; p; p = p->parent)
    if (p == child) return JS_ThrowTypeError(ctx, "HierarchyRequestError");
  if (before && before->parent != parent) before = 0;
  std::vector<Node*> moving;
  if (child->tag == "#document-fragment") {
    for (size_t i = 0; i < child->children.size(); ++i) moving.push_back(child->children[i].get());
  } else {
    moving.push_back(child);
  }
  for (size_t i = 0; i < moving.size(); ++i) {
    if (moving[i] == before) continue;
    std::unique_ptr<Node> owned = Detach(e, moving[i]);
    if (!owned) continue;
    Node* raw = owned.get();
    parent->InsertBefore(std::move(owned), before);
    // Dynamically inserted scripts run when they enter the document.
    if (raw->Is("script")) e->page()->OnScriptInserted(raw);
    std::vector<Node*> scripts;
    raw->FindAll("script", scripts);
    for (size_t k = 0; k < scripts.size(); ++k)
      if (scripts[k] != raw) e->page()->OnScriptInserted(scripts[k]);
  }
  e->MarkDirty();
  return JS_UNDEFINED;
}

KITE_FN(Remove) {
  ARGS_AT_LEAST(1);
  ScriptEngine* e = Engine(ctx);
  Node* n = Arg(ctx, argv[0]);
  if (n && n->parent) {
    std::unique_ptr<Node> owned = n->parent->RemoveChild(n);
    if (owned) e->Adopt(std::move(owned));
    e->MarkDirty();
  }
  return JS_UNDEFINED;
}

KITE_FN(Query) {
  ARGS_AT_LEAST(3);
  Node* root = Arg(ctx, argv[0]);
  std::string sel = Str(ctx, argv[1]);
  bool all = JS_ToBool(ctx, argv[2]) > 0;
  std::vector<ComplexSelector> parsed;
  if (!ParseSelectorList(sel, parsed)) return JS_ThrowSyntaxError(ctx, "invalid selector: %s", sel.c_str());
  std::vector<Node*> found;
  if (root) {
    ElementState st;
    std::vector<Node*> stack;
    for (size_t i = root->children.size(); i-- > 0;) stack.push_back(root->children[i].get());
    while (!stack.empty()) {
      Node* n = stack.back();
      stack.pop_back();
      if (n->IsElement()) {
        for (size_t k = 0; k < parsed.size(); ++k)
          if (MatchesSelector(parsed[k], n, st)) {
            found.push_back(n);
            break;
          }
        if (!all && !found.empty()) break;
      }
      for (size_t i = n->children.size(); i-- > 0;) stack.push_back(n->children[i].get());
    }
  }
  if (!all) return found.empty() ? JS_NULL : Handle(ctx, found[0]);
  return HandleArray(ctx, found);
}

KITE_FN(Matches) {
  ARGS_AT_LEAST(2);
  Node* n = Arg(ctx, argv[0]);
  std::string sel = Str(ctx, argv[1]);
  std::vector<ComplexSelector> parsed;
  if (!ParseSelectorList(sel, parsed)) return JS_ThrowSyntaxError(ctx, "invalid selector: %s", sel.c_str());
  ElementState st;
  if (n && n->IsElement())
    for (size_t k = 0; k < parsed.size(); ++k)
      if (MatchesSelector(parsed[k], n, st)) return JS_TRUE;
  return JS_FALSE;
}

KITE_FN(Parse) {
  ARGS_AT_LEAST(1);
  ScriptEngine* e = Engine(ctx);
  std::string html = Str(ctx, argv[0]);
  std::unique_ptr<Document> doc = ParseHtml("<!DOCTYPE html><body>" + html);
  std::vector<Node*> out;
  Node* body = doc->Body();
  // Content that the parser put into <head> (e.g. <style>, <script>) is kept.
  Node* head = doc->Head();
  std::vector<Node*> sources;
  if (head) sources.push_back(head);
  if (body) sources.push_back(body);
  for (size_t s = 0; s < sources.size(); ++s) {
    while (!sources[s]->children.empty()) {
      std::unique_ptr<Node> c = sources[s]->RemoveChild(sources[s]->children[0].get());
      out.push_back(c.get());
      e->Adopt(std::move(c));
    }
  }
  return HandleArray(ctx, out);
}

KITE_FN(Html) {
  ARGS_AT_LEAST(2);
  Node* n = Arg(ctx, argv[0]);
  return NewStr(ctx, n ? SerializeNode(n, JS_ToBool(ctx, argv[1]) > 0) : std::string());
}

KITE_FN(Text) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  return NewStr(ctx, n ? n->TextContent() : std::string());
}

KITE_FN(GetValue) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  if (!n) return NewStr(ctx, "");
  if (n->formValueSet) return NewStr(ctx, n->formValue);
  if (n->tag == "textarea") return NewStr(ctx, n->TextContent());
  if (n->tag == "select") {
    std::vector<Node*> opts;
    n->FindAll("option", opts);
    int sel = n->selectedIndex;
    if (sel < 0)
      for (size_t i = 0; i < opts.size(); ++i)
        if (opts[i]->HasAttr("selected")) sel = (int)i;
    if (sel < 0 && !opts.empty()) sel = 0;
    if (sel < 0 || sel >= (int)opts.size()) return NewStr(ctx, "");
    Node* o = opts[sel];
    return NewStr(ctx, o->HasAttr("value") ? o->Attr("value") : CollapseWhitespace(o->TextContent()));
  }
  return NewStr(ctx, n->Attr("value"));
}

KITE_FN(SetValue) {
  ARGS_AT_LEAST(2);
  Node* n = Arg(ctx, argv[0]);
  if (!n) return JS_UNDEFINED;
  std::string v = Str(ctx, argv[1]);
  if (n->tag == "select") {
    std::vector<Node*> opts;
    n->FindAll("option", opts);
    for (size_t i = 0; i < opts.size(); ++i) {
      std::string ov = opts[i]->HasAttr("value") ? opts[i]->Attr("value") : CollapseWhitespace(opts[i]->TextContent());
      if (ov == v) n->selectedIndex = (int)i;
    }
  } else {
    n->formValue = v;
    n->formValueSet = true;
  }
  Engine(ctx)->page()->RequestRepaint();
  return JS_UNDEFINED;
}

KITE_FN(GetChecked) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  if (!n) return JS_FALSE;
  return JS_NewBool(ctx, n->checkedSet ? n->checked : n->HasAttr("checked"));
}

KITE_FN(SetChecked) {
  ARGS_AT_LEAST(2);
  Node* n = Arg(ctx, argv[0]);
  if (n) {
    n->checked = JS_ToBool(ctx, argv[1]) > 0;
    n->checkedSet = true;
    Engine(ctx)->MarkDirty();
  }
  return JS_UNDEFINED;
}

KITE_FN(SelIndex) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  if (!n) return JS_NewInt32(ctx, -1);
  if (argc > 1) {
    n->selectedIndex = Int(ctx, argv[1]);
    Engine(ctx)->page()->RequestRepaint();
  }
  int sel = n->selectedIndex;
  if (sel < 0) {
    std::vector<Node*> opts;
    n->FindAll("option", opts);
    for (size_t i = 0; i < opts.size(); ++i)
      if (opts[i]->HasAttr("selected")) sel = (int)i;
    if (sel < 0 && !opts.empty()) sel = 0;
  }
  return JS_NewInt32(ctx, sel);
}

KITE_FN(NodeRect) {
  ARGS_AT_LEAST(1);
  ScriptEngine* e = Engine(ctx);
  Node* n = Arg(ctx, argv[0]);
  e->page()->EnsureLayout();
  Rect r;
  JSValue arr = JS_NewArray(ctx);
  bool ok = n && e->page()->BoxRect(n, r);
  float sx = 0, sy = 0;
  e->host()->GetScroll(sx, sy);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, ok ? r.x - sx : 0));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, ok ? r.y - sy : 0));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, ok ? r.w : 0));
  JS_SetPropertyUint32(ctx, arr, 3, JS_NewFloat64(ctx, ok ? r.h : 0));
  return arr;
}

KITE_FN(Computed) {
  ARGS_AT_LEAST(2);
  ScriptEngine* e = Engine(ctx);
  Node* n = Arg(ctx, argv[0]);
  std::string prop = Str(ctx, argv[1]);
  e->page()->EnsureLayout();
  if (!n || !n->style) return NewStr(ctx, prop == "display" ? "none" : "");
  const ComputedStyle* s = n->style;
  char buf[64];
  if (prop == "display") {
    static const char* names[] = {"none", "inline", "block", "inline-block", "list-item", "table",
                                  "inline-table", "table-row-group", "table-header-group",
                                  "table-footer-group", "table-row", "table-cell", "table-column",
                                  "table-column-group", "table-caption", "flex", "inline-flex",
                                  "grid", "inline-grid", "contents"};
    return NewStr(ctx, names[s->display]);
  }
  if (prop == "visibility") return NewStr(ctx, s->visible ? "visible" : "hidden");
  if (prop == "position") {
    static const char* names[] = {"static", "relative", "absolute", "fixed", "sticky"};
    return NewStr(ctx, names[s->position]);
  }
  if (prop == "color" || prop == "background-color") {
    Color c = prop == "color" ? s->color : s->backgroundColor;
    if (c.a == 255) snprintf(buf, sizeof buf, "rgb(%d, %d, %d)", c.r, c.g, c.b);
    else snprintf(buf, sizeof buf, "rgba(%d, %d, %d, %g)", c.r, c.g, c.b, c.a / 255.0);
    return NewStr(ctx, buf);
  }
  if (prop == "opacity") {
    snprintf(buf, sizeof buf, "%g", s->opacity);
    return NewStr(ctx, buf);
  }
  if (prop == "font-size") {
    snprintf(buf, sizeof buf, "%gpx", s->fontSize);
    return NewStr(ctx, buf);
  }
  if (prop == "font-family") return NewStr(ctx, s->fontFamily);
  if (prop == "width" || prop == "height") {
    Rect r;
    if (e->page()->BoxRect(n, r)) {
      snprintf(buf, sizeof buf, "%gpx", prop == "width" ? r.w : r.h);
      return NewStr(ctx, buf);
    }
    return NewStr(ctx, "auto");
  }
  return NewStr(ctx, "");
}

KITE_FN(Cookie) {
  ScriptEngine* e = Engine(ctx);
  if (argc > 0) {
    e->host()->SetCookie(e->page()->url(), Str(ctx, argv[0]));
    return JS_UNDEFINED;
  }
  return NewStr(ctx, e->host()->GetCookies(e->page()->url()));
}

KITE_FN(PageUrl) { return NewStr(ctx, Engine(ctx)->page()->url()); }

KITE_FN(Resolve) {
  ARGS_AT_LEAST(1);
  Url u = Engine(ctx)->page()->ResolveUrl(Str(ctx, argv[0]));
  return u.valid() ? NewStr(ctx, u.Spec()) : JS_NULL;
}

KITE_FN(Alert) {
  Engine(ctx)->host()->Alert(argc > 0 ? Str(ctx, argv[0]) : std::string());
  return JS_UNDEFINED;
}

KITE_FN(Confirm) {
  return JS_NewBool(ctx, Engine(ctx)->host()->Confirm(argc > 0 ? Str(ctx, argv[0]) : std::string()));
}

KITE_FN(Prompt) {
  std::string r = Engine(ctx)->host()->Prompt(argc > 0 ? Str(ctx, argv[0]) : std::string(),
                                               argc > 1 ? Str(ctx, argv[1]) : std::string());
  if (r == "\x01") return JS_NULL;  // cancelled
  return NewStr(ctx, r);
}

KITE_FN(Navigate) {
  ARGS_AT_LEAST(1);
  ScriptEngine* e = Engine(ctx);
  Url u = e->page()->ResolveUrl(Str(ctx, argv[0]));
  if (u.valid()) e->host()->Navigate(u.Spec(), argc > 1 && JS_ToBool(ctx, argv[1]) > 0);
  return JS_UNDEFINED;
}

KITE_FN(SetTitle) {
  ARGS_AT_LEAST(1);
  Engine(ctx)->host()->SetTitle(Str(ctx, argv[0]));
  return JS_UNDEFINED;
}

KITE_FN(JsLog) {
  std::string line;
  for (int i = 0; i < argc; ++i) {
    if (i) line += " ";
    line += Str(ctx, argv[i]);
  }
  Engine(ctx)->Log(line);
  return JS_UNDEFINED;
}

KITE_FN(JsTimer) {
  ARGS_AT_LEAST(3);
  if (!JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
  JSValue* fn = new JSValue(JS_DupValue(ctx, argv[0]));
  return JS_NewInt32(ctx, Engine(ctx)->AddTimer(fn, Int(ctx, argv[1]), JS_ToBool(ctx, argv[2]) > 0));
}

KITE_FN(JsClearTimer) {
  ARGS_AT_LEAST(1);
  Engine(ctx)->ClearTimer(Int(ctx, argv[0]));
  return JS_UNDEFINED;
}

KITE_FN(Request) {
  ARGS_AT_LEAST(4);
  ScriptRequest r;
  r.method = AsciiUpper(Str(ctx, argv[0]));
  Url u = Engine(ctx)->page()->ResolveUrl(Str(ctx, argv[1]));
  if (!u.valid()) return JS_NewInt32(ctx, 0);
  r.url = u.Spec();
  r.body = Str(ctx, argv[2]);
  uint32_t n = 0;
  JSValue len = JS_GetPropertyStr(ctx, argv[3], "length");
  JS_ToUint32(ctx, &n, len);
  JS_FreeValue(ctx, len);
  for (uint32_t i = 0; i + 1 < n; i += 2) {
    JSValue k = JS_GetPropertyUint32(ctx, argv[3], i), v = JS_GetPropertyUint32(ctx, argv[3], i + 1);
    r.headers.push_back(std::make_pair(Str(ctx, k), Str(ctx, v)));
    JS_FreeValue(ctx, k);
    JS_FreeValue(ctx, v);
  }
  return JS_NewInt32(ctx, Engine(ctx)->AddRequest(r));
}

KITE_FN(ScrollTo) {
  ARGS_AT_LEAST(2);
  Engine(ctx)->host()->ScrollTo((float)Num(ctx, argv[0]), (float)Num(ctx, argv[1]));
  return JS_UNDEFINED;
}

KITE_FN(Scroll) {
  float x = 0, y = 0;
  Engine(ctx)->host()->GetScroll(x, y);
  JSValue arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, x));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, y));
  return arr;
}

KITE_FN(Viewport) {
  Page* p = Engine(ctx)->page();
  JSValue arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, p->viewportWidth()));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, p->viewportHeight()));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, p->ContentHeight()));
  return arr;
}

KITE_FN(Media) {
  ARGS_AT_LEAST(1);
  Page* p = Engine(ctx)->page();
  MediaContext mc;
  mc.width = p->viewportWidth();
  mc.height = p->viewportHeight();
  return JS_NewBool(ctx, EvaluateMediaQueryList(Str(ctx, argv[0]), mc));
}

KITE_FN(Dirty) {
  Engine(ctx)->MarkDirty();
  return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// <canvas>

Canvas2D* Cv(JSContext* ctx, JSValueConst v) { return Engine(ctx)->CanvasById(Int(ctx, v)); }
float F(JSContext* ctx, JSValueConst v) { return (float)Num(ctx, v); }

#define CV_FN(name) KITE_FN(name)
#define CV_GET(n)            \
  ARGS_AT_LEAST(n);          \
  Canvas2D* c = Cv(ctx, argv[0]); \
  if (!c) return JS_UNDEFINED;
#define CV_DONE() Engine(ctx)->MarkCanvasDirty()

CV_FN(CvCreate) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  if (!n) return JS_NewInt32(ctx, 0);
  return JS_NewInt32(ctx, Engine(ctx)->CreateCanvas(n));
}

CV_FN(CvResize) {
  CV_GET(3);
  c->Resize(Int(ctx, argv[1]), Int(ctx, argv[2]));
  CV_DONE();
  return JS_UNDEFINED;
}

CV_FN(CvState) {  // 0 save, 1 restore, 2 reset, 3 beginPath, 4 closePath, 5 pushPath, 6 popPath
  CV_GET(2);
  switch (Int(ctx, argv[1])) {
    case 0: c->Save(); break;
    case 1: c->Restore(); break;
    case 2: {
      int w = c->width(), h = c->height();
      c->Resize(w, h);
      CV_DONE();
      break;
    }
    case 3: c->BeginPath(); break;
    case 4: c->ClosePath(); break;
    case 5: c->PushPath(); break;
    case 6: c->PopPath(); break;
  }
  return JS_UNDEFINED;
}

CV_FN(CvMatrix) {  // (c, set?, a, b, c, d, e, f)
  CV_GET(8);
  gfx::Matrix m(F(ctx, argv[2]), F(ctx, argv[3]), F(ctx, argv[4]), F(ctx, argv[5]), F(ctx, argv[6]),
                F(ctx, argv[7]));
  if (JS_ToBool(ctx, argv[1]) > 0) c->SetTransform(m);
  else c->Transform(m);
  return JS_UNDEFINED;
}

CV_FN(CvGetMatrix) {
  CV_GET(1);
  const gfx::Matrix& m = c->transform();
  JSValue arr = JS_NewArray(ctx);
  float v[6] = {m.a, m.b, m.c, m.d, m.e, m.f};
  for (int i = 0; i < 6; ++i) JS_SetPropertyUint32(ctx, arr, i, JS_NewFloat64(ctx, v[i]));
  return arr;
}

bool ParseCanvasColor(const std::string& s, Color& out) {
  std::string v = AsciiLower(Trim(s));
  if (v == "currentcolor" || v == "inherit" || v.empty()) return false;
  return ParseColor(v, out, Color(0, 0, 0));
}

CV_FN(CvColor) {  // (c, which, css) -> bool
  CV_GET(3);
  Color col;
  if (!ParseCanvasColor(Str(ctx, argv[2]), col)) return JS_FALSE;
  CanvasPaint p;
  p.color = col;
  if (Int(ctx, argv[1])) c->SetStroke(p);
  else c->SetFill(p);
  return JS_TRUE;
}

CV_FN(CvGradient) {  // (c, which, kind, x0, y0, r0, x1, y1, r1, [offset, color, ...])
  CV_GET(10);
  std::shared_ptr<CanvasGradient> g(new CanvasGradient);
  g->kind = (CanvasGradient::Kind)Int(ctx, argv[2]);
  g->x0 = F(ctx, argv[3]);
  g->y0 = F(ctx, argv[4]);
  g->r0 = F(ctx, argv[5]);
  g->x1 = F(ctx, argv[6]);
  g->y1 = F(ctx, argv[7]);
  g->r1 = F(ctx, argv[8]);
  uint32_t n = 0;
  JSValue len = JS_GetPropertyStr(ctx, argv[9], "length");
  JS_ToUint32(ctx, &n, len);
  JS_FreeValue(ctx, len);
  for (uint32_t i = 0; i + 1 < n && i < 2048; i += 2) {
    JSValue o = JS_GetPropertyUint32(ctx, argv[9], i), col = JS_GetPropertyUint32(ctx, argv[9], i + 1);
    Color cc;
    if (ParseCanvasColor(Str(ctx, col), cc)) g->stops.push_back(std::make_pair(F(ctx, o), cc));
    JS_FreeValue(ctx, o);
    JS_FreeValue(ctx, col);
  }
  CanvasPaint p;
  p.kind = CanvasPaint::kGradient;
  p.gradient = g;
  if (Int(ctx, argv[1])) c->SetStroke(p);
  else c->SetFill(p);
  return JS_UNDEFINED;
}

// Pixels of an image source (<img> or <canvas> element).
const DecodedImage* SourcePixels(JSContext* ctx, JSValueConst v) {
  ScriptEngine* e = Engine(ctx);
  Node* n = Arg(ctx, v);
  if (!n || !n->IsElement()) return 0;
  if (n->tag == "canvas") {
    Canvas2D* c = n->canvasId ? e->CanvasById(n->canvasId) : 0;
    return c ? &c->pixels() : 0;
  }
  std::string src = n->Attr("src");
  if (src.empty()) return 0;
  Url u = e->page()->ResolveUrl(src);
  if (!u.valid() || !e->page()->images()) return 0;
  return e->page()->images()->Pixels(u.Spec());
}

CV_FN(CvSourceSize) {
  ARGS_AT_LEAST(1);
  const DecodedImage* img = SourcePixels(ctx, argv[0]);
  if (!img) return JS_NULL;
  float d = img->density > 0 ? img->density : 1;
  JSValue arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, img->width / d));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, img->height / d));
  return arr;
}

CV_FN(CvPattern) {  // (c, which, sourceHandle, repetition, a, b, c, d, e, f) -> bool
  CV_GET(10);
  const DecodedImage* img = SourcePixels(ctx, argv[2]);
  if (!img || img->width <= 0) return JS_FALSE;
  std::shared_ptr<CanvasPattern> p(new CanvasPattern);
  p->image = *img;
  std::string rep = Str(ctx, argv[3]);
  p->repeatX = rep.empty() || rep == "repeat" || rep == "repeat-x";
  p->repeatY = rep.empty() || rep == "repeat" || rep == "repeat-y";
  float d = img->density > 0 ? img->density : 1;
  p->transform = gfx::Matrix(F(ctx, argv[4]), F(ctx, argv[5]), F(ctx, argv[6]), F(ctx, argv[7]), F(ctx, argv[8]),
                             F(ctx, argv[9])) * gfx::Matrix(1 / d, 0, 0, 1 / d, 0, 0);
  CanvasPaint paint;
  paint.kind = CanvasPaint::kPattern;
  paint.pattern = p;
  if (Int(ctx, argv[1])) c->SetStroke(paint);
  else c->SetFill(paint);
  return JS_TRUE;
}

CV_FN(CvProp) {  // (c, prop, value)
  CV_GET(3);
  int prop = Int(ctx, argv[1]);
  switch (prop) {
    case 0: c->SetLineWidth(F(ctx, argv[2])); break;
    case 1: c->SetLineCap((gfx::LineCap)Int(ctx, argv[2])); break;
    case 2: c->SetLineJoin((gfx::LineJoin)Int(ctx, argv[2])); break;
    case 3: c->SetMiterLimit(F(ctx, argv[2])); break;
    case 4: c->SetGlobalAlpha(F(ctx, argv[2])); break;
    case 5: c->SetCompositeOp((CompositeOp)Int(ctx, argv[2])); break;
    case 6: c->SetTextAlign(Int(ctx, argv[2])); break;
    case 7: c->SetTextBaseline(Int(ctx, argv[2])); break;
    case 8: c->SetImageSmoothing(JS_ToBool(ctx, argv[2]) > 0); break;
    case 9: c->SetLineDashOffset(F(ctx, argv[2])); break;
  }
  return JS_UNDEFINED;
}

CV_FN(CvFont) {
  CV_GET(2);
  return JS_NewBool(ctx, c->SetFont(Str(ctx, argv[1])));
}

CV_FN(CvShadow) {  // (c, color, blur, ox, oy)
  CV_GET(5);
  Color col;
  if (!ParseCanvasColor(Str(ctx, argv[1]), col)) col = Color();
  c->SetShadow(col, F(ctx, argv[2]), F(ctx, argv[3]), F(ctx, argv[4]));
  return JS_UNDEFINED;
}

CV_FN(CvDash) {
  CV_GET(2);
  std::vector<float> d;
  uint32_t n = 0;
  JSValue len = JS_GetPropertyStr(ctx, argv[1], "length");
  JS_ToUint32(ctx, &n, len);
  JS_FreeValue(ctx, len);
  bool ok = true;
  for (uint32_t i = 0; i < n && i < 1024; ++i) {
    JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
    float f = F(ctx, v);
    JS_FreeValue(ctx, v);
    if (!(f >= 0) || !std::isfinite(f)) ok = false;
    d.push_back(f);
  }
  if (!ok) return JS_UNDEFINED;
  if (d.size() % 2) d.insert(d.end(), d.begin(), d.end());
  float sum = 0;
  for (size_t i = 0; i < d.size(); ++i) sum += d[i];
  if (sum <= 0) d.clear();
  c->SetLineDash(d);
  return JS_UNDEFINED;
}

CV_FN(CvPath) {  // (c, op, ...numbers)
  CV_GET(2);
  float a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 8 && i + 2 < argc; ++i) a[i] = F(ctx, argv[i + 2]);
  switch (Int(ctx, argv[1])) {
    case 0: c->MoveTo(a[0], a[1]); break;
    case 1: c->LineTo(a[0], a[1]); break;
    case 2: c->QuadraticCurveTo(a[0], a[1], a[2], a[3]); break;
    case 3: c->BezierCurveTo(a[0], a[1], a[2], a[3], a[4], a[5]); break;
    case 4: c->Arc(a[0], a[1], a[2], a[3], a[4], a[5] != 0); break;
    case 5: c->ArcTo(a[0], a[1], a[2], a[3], a[4]); break;
    case 6: c->Ellipse(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7] != 0); break;
    case 7: c->Rect(a[0], a[1], a[2], a[3]); break;
    case 8: {
      float r[4] = {a[4], a[5], a[6], a[7]};
      c->RoundRect(a[0], a[1], a[2], a[3], r);
      break;
    }
  }
  return JS_UNDEFINED;
}

CV_FN(CvSvgPath) {
  CV_GET(2);
  c->SvgPath(Str(ctx, argv[1]));
  return JS_UNDEFINED;
}

CV_FN(CvDraw) {  // (c, op, ...) 0 fill(evenOdd) 1 stroke 2 clip(evenOdd) 3 fillRect 4 strokeRect 5 clearRect
  CV_GET(2);
  float a[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4 && i + 2 < argc; ++i) a[i] = F(ctx, argv[i + 2]);
  switch (Int(ctx, argv[1])) {
    case 0: c->Fill(a[0] != 0); break;
    case 1: c->Stroke(); break;
    case 2: c->Clip(a[0] != 0); return JS_UNDEFINED;
    case 3: c->FillRect(a[0], a[1], a[2], a[3]); break;
    case 4: c->StrokeRect(a[0], a[1], a[2], a[3]); break;
    case 5: c->ClearRect(a[0], a[1], a[2], a[3]); break;
  }
  CV_DONE();
  return JS_UNDEFINED;
}

CV_FN(CvHit) {  // (c, stroke?, x, y, evenOdd)
  CV_GET(5);
  float x = F(ctx, argv[2]), y = F(ctx, argv[3]);
  if (JS_ToBool(ctx, argv[1]) > 0) return JS_NewBool(ctx, c->IsPointInStroke(x, y));
  return JS_NewBool(ctx, c->IsPointInPath(x, y, JS_ToBool(ctx, argv[4]) > 0));
}

CV_FN(CvText) {  // (c, text, x, y, maxWidth, stroke)
  CV_GET(6);
  c->FillText(Str(ctx, argv[1]), F(ctx, argv[2]), F(ctx, argv[3]), F(ctx, argv[4]), JS_ToBool(ctx, argv[5]) > 0);
  CV_DONE();
  return JS_UNDEFINED;
}

CV_FN(CvMeasure) {
  CV_GET(2);
  TextMetricsResult m = c->MeasureText(Str(ctx, argv[1]));
  JSValue arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, m.width));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, m.ascent));
  JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, m.descent));
  return arr;
}

CV_FN(CvImage) {  // (c, source, sx, sy, sw, sh, dx, dy, dw, dh)
  CV_GET(10);
  const DecodedImage* img = SourcePixels(ctx, argv[1]);
  if (!img) return JS_UNDEFINED;
  float d = img->density > 0 ? img->density : 1;
  float v[8];
  for (int i = 0; i < 8; ++i) v[i] = F(ctx, argv[i + 2]);
  // Copy when drawing a canvas onto itself.
  if (img == &c->pixels()) {
    DecodedImage copy = *img;
    c->DrawImage(copy, v[0] * d, v[1] * d, v[2] * d, v[3] * d, v[4], v[5], v[6], v[7]);
  } else {
    c->DrawImage(*img, v[0] * d, v[1] * d, v[2] * d, v[3] * d, v[4], v[5], v[6], v[7]);
  }
  CV_DONE();
  return JS_UNDEFINED;
}

CV_FN(CvGetData) {  // (c, x, y, w, h) -> ArrayBuffer
  CV_GET(5);
  std::string d = c->GetImageData(Int(ctx, argv[1]), Int(ctx, argv[2]), Int(ctx, argv[3]), Int(ctx, argv[4]));
  return JS_NewArrayBufferCopy(ctx, (const uint8_t*)d.data(), d.size());
}

CV_FN(CvPutData) {  // (c, typedArray, w, h, dx, dy, dirtyX, dirtyY, dirtyW, dirtyH)
  CV_GET(10);
  size_t off = 0, len = 0, bpe = 0;
  JSValue buf = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &len, &bpe);
  if (JS_IsException(buf)) return JS_EXCEPTION;
  size_t size = 0;
  uint8_t* p = JS_GetArrayBuffer(ctx, &size, buf);
  int w = Int(ctx, argv[2]), h = Int(ctx, argv[3]);
  if (p && w > 0 && h > 0 && off + len <= size && (size_t)w * h * 4 <= len)
    c->PutImageData(p + off, w, h, Int(ctx, argv[4]), Int(ctx, argv[5]), Int(ctx, argv[6]), Int(ctx, argv[7]),
                    Int(ctx, argv[8]), Int(ctx, argv[9]));
  JS_FreeValue(ctx, buf);
  CV_DONE();
  return JS_UNDEFINED;
}

CV_FN(CvDataUrl) {
  CV_GET(1);
  return NewStr(ctx, "data:image/png;base64," + Base64Encode(c->ToPng()));
}

// Image loading for scripts: (imgHandle) -> 0 pending, 1 loaded, 2 failed.
CV_FN(ImgLoad) {
  ARGS_AT_LEAST(1);
  ScriptEngine* e = Engine(ctx);
  Node* n = Arg(ctx, argv[0]);
  if (!n) return JS_NewInt32(ctx, 2);
  std::string src = n->Attr("src");
  if (src.empty()) return JS_NewInt32(ctx, 2);
  Url u = e->page()->ResolveUrl(src);
  if (!u.valid() || !e->page()->images()) return JS_NewInt32(ctx, 2);
  int w = 0, h = 0;
  ImageProvider::State st = e->page()->images()->GetImage(u.Spec(), w, h);
  if (st == ImageProvider::kLoaded) return JS_NewInt32(ctx, 1);
  if (st == ImageProvider::kFailed || st == ImageProvider::kUnsupported) return JS_NewInt32(ctx, 2);
  e->WatchImage(n, u.Spec(), true);
  return JS_NewInt32(ctx, 0);
}

CV_FN(ImgSize) {
  ARGS_AT_LEAST(1);
  ScriptEngine* e = Engine(ctx);
  Node* n = Arg(ctx, argv[0]);
  int w = 0, h = 0;
  if (n && e->page()->images()) {
    Url u = e->page()->ResolveUrl(n->Attr("src"));
    if (u.valid() && e->page()->images()->GetImage(u.Spec(), w, h) != ImageProvider::kLoaded) w = h = 0;
  }
  JSValue arr = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, w));
  JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, h));
  return arr;
}

KITE_FN(Cur) { return Handle(ctx, Engine(ctx)->currentScript()); }

KITE_FN(Submit) {
  ARGS_AT_LEAST(1);
  Node* form = Arg(ctx, argv[0]);
  ScriptEngine* e = Engine(ctx);
  FormSubmission sub;
  if (form && e->page()->BuildFormSubmission(form, 0, sub)) e->page()->QueueFormSubmission(sub);
  return JS_UNDEFINED;
}

}  // namespace

// ---------------------------------------------------------------------------

namespace {
const char* const kVoidTags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                                 "link", "meta", "param", "source", "track", "wbr", 0};

void Serialize(const Node* n, std::string& out) {
  switch (n->type) {
    case Node::kText: {
      const Node* p = n->parent;
      if (p && (p->tag == "script" || p->tag == "style" || p->tag == "noscript")) out += n->text;
      else out += ReplaceAll(ReplaceAll(ReplaceAll(n->text, "&", "&amp;"), "<", "&lt;"), ">", "&gt;");
      return;
    }
    case Node::kComment:
      out += "<!--" + n->text + "-->";
      return;
    case Node::kElement: {
      if (n->tag == "#document-fragment") break;
      out += "<" + n->tag;
      for (size_t i = 0; i < n->attrs.size(); ++i)
        out += " " + n->attrs[i].name + "=\"" + ReplaceAll(ReplaceAll(n->attrs[i].value, "&", "&amp;"), "\"", "&quot;") + "\"";
      out += ">";
      for (int k = 0; kVoidTags[k]; ++k)
        if (n->tag == kVoidTags[k]) return;
      for (size_t i = 0; i < n->children.size(); ++i) Serialize(n->children[i].get(), out);
      out += "</" + n->tag + ">";
      return;
    }
    default:
      break;
  }
  for (size_t i = 0; i < n->children.size(); ++i) Serialize(n->children[i].get(), out);
}
}  // namespace

std::string SerializeNode(const Node* n, bool outer) {
  std::string out;
  if (outer) Serialize(n, out);
  else
    for (size_t i = 0; i < n->children.size(); ++i) Serialize(n->children[i].get(), out);
  return out;
}


// Shadow DOM and custom elements.
KITE_FN(AttachShadow) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  if (!n || !n->IsElement() || n->shadowRoot) return JS_NewInt32(ctx, 0);
  n->shadowRoot.reset(new Node(Node::kShadowRoot));
  n->shadowRoot->tag = "#shadow-root";
  n->shadowRoot->host = n;
  Engine(ctx)->MarkDirty();
  return Handle(ctx, n->shadowRoot.get());
}

KITE_FN(ShadowOf) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  return Handle(ctx, n ? n->shadowRoot.get() : 0);
}

KITE_FN(HostOf) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  return Handle(ctx, n && n->type == Node::kShadowRoot ? n->host : 0);
}

KITE_FN(SetDefined) {
  ARGS_AT_LEAST(1);
  Node* n = Arg(ctx, argv[0]);
  if (n && !n->customDefined) {
    n->customDefined = true;
    Engine(ctx)->MarkDirty();  // :defined changed
  }
  return JS_UNDEFINED;
}

// Elements with a '-' in their name in the subtree of |h| (itself and
// shadow trees included), in tree order.
KITE_FN(Customs) {
  ARGS_AT_LEAST(1);
  Node* root = Arg(ctx, argv[0]);
  std::vector<Node*> found;
  std::vector<Node*> stack;
  if (root) stack.push_back(root);
  while (!stack.empty()) {
    Node* n = stack.back();
    stack.pop_back();
    if (n->IsElement() && n->tag.find('-') != std::string::npos) found.push_back(n);
    if (n->shadowRoot) stack.push_back(n->shadowRoot.get());
    for (size_t i = n->children.size(); i-- > 0;) stack.push_back(n->children[i].get());
  }
  return HandleArray(ctx, found);
}

// Connected to the document (across shadow boundaries).
KITE_FN(Connected) {
  ARGS_AT_LEAST(1);
  for (Node* n = Arg(ctx, argv[0]); n; n = n->type == Node::kShadowRoot ? n->host : n->parent)
    if (n->type == Node::kDocument) return JS_TRUE;
  return JS_FALSE;
}

// pushState(url, replace, stateId) -> bool
KITE_FN(PushState) {
  ARGS_AT_LEAST(3);
  ScriptEngine* e = Engine(ctx);
  std::string url = Str(ctx, argv[0]);
  Url target = Url::Parse(url), cur = Url::Parse(e->page()->url());
  if (!target.valid() || target.Origin() != cur.Origin()) return JS_FALSE;
  if (e->host() && !e->host()->PushState(target.Spec(), JS_ToBool(ctx, argv[1]) != 0, Int(ctx, argv[2])))
    return JS_FALSE;
  e->page()->SetUrl(target.Spec());
  return JS_TRUE;
}

KITE_FN(HistLen) { return JS_NewInt32(ctx, Engine(ctx)->host() ? Engine(ctx)->host()->HistoryLength() : 1); }

KITE_FN(HistGo) {
  ARGS_AT_LEAST(1);
  if (Engine(ctx)->host()) Engine(ctx)->host()->HistoryGo(Int(ctx, argv[0]));
  return JS_UNDEFINED;
}

// storage(op, session, key, value): 0 get, 1 set, 2 remove, 3 clear, 4 keys.
KITE_FN(StorageOp) {
  ARGS_AT_LEAST(2);
  ScriptEngine* e = Engine(ctx);
  WebStorage* st = e->StorageFor(JS_ToBool(ctx, argv[1]) != 0);
  std::string origin = Url::Parse(e->page()->url()).Origin();
  int op = Int(ctx, argv[0]);
  std::string key = argc > 2 ? Str(ctx, argv[2]) : std::string();
  switch (op) {
    case 0: {
      const std::string* v = st->Get(origin, key);
      return v ? NewStr(ctx, *v) : JS_NULL;
    }
    case 1:
      return JS_NewBool(ctx, st->Set(origin, key, argc > 3 ? Str(ctx, argv[3]) : std::string()));
    case 2:
      st->Remove(origin, key);
      return JS_UNDEFINED;
    case 3:
      st->Clear(origin);
      return JS_UNDEFINED;
    default: {
      JSValue arr = JS_NewArray(ctx);
      const WebStorage::Items* items = st->ItemsOf(origin);
      if (items)
        for (size_t i = 0; i < items->size(); ++i)
          JS_SetPropertyUint32(ctx, arr, (uint32_t)i, NewStr(ctx, (*items)[i].first));
      return arr;
    }
  }
}

// ---------------------------------------------------------------------------
// ES module loading (QuickJS callbacks)

static char* NormalizeModule(JSContext* ctx, const char* base, const char* name, void*) {
  std::string url = Engine(ctx)->ResolveModuleSpecifier(name, base);
  if (url.empty()) {
    JS_ThrowTypeError(ctx, "Failed to resolve module specifier \"%s\"", name);
    return 0;
  }
  char* out = (char*)js_malloc(ctx, url.size() + 1);
  if (!out) return 0;
  memcpy(out, url.c_str(), url.size() + 1);
  return out;
}

static JSModuleDef* LoadModule(JSContext* ctx, const char* name, void*) {
  JSModuleDef* m = (JSModuleDef*)Engine(ctx)->LoadModuleNow(name);
  if (!m && !JS_HasException(ctx)) JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
  return m;
}

// ---------------------------------------------------------------------------
// ScriptEngine

static int InterruptCb(JSRuntime*, void* opaque);

// Unhandled promise rejections are reported once the job queue is empty.
static void RejectionTracker(JSContext* ctx, JSValueConst promise, JSValueConst reason, JS_BOOL handled,
                             void* opaque) {
  ScriptEngine* e = (ScriptEngine*)opaque;
  void* key = JS_VALUE_GET_PTR(promise);
  if (handled) {
    e->ForgetRejection(key);
    return;
  }
  std::string msg = Str(ctx, reason);
  if (JS_IsError(ctx, reason)) {
    JSValue stack = JS_GetPropertyStr(ctx, reason, "stack");
    if (JS_IsString(stack)) msg += "\n" + Str(ctx, stack);
    JS_FreeValue(ctx, stack);
  }
  e->NoteRejection(key, msg);
}

ScriptEngine::ScriptEngine(Page* page, ScriptHost* host)
    : page_(page), host_(host), nextTimer_(1), nextRequest_(1), dirty_(false), currentScript_(0),
      deadline_(0) {
  handles_.push_back(0);  // handle 0 = null
  JSRuntime* rt = JS_NewRuntime();
  JS_SetMemoryLimit(rt, 128 * 1024 * 1024);
  JS_SetMaxStackSize(rt, 256 * 1024);
  JS_SetInterruptHandler(rt, InterruptCb, this);
  JS_SetModuleLoaderFunc(rt, NormalizeModule, LoadModule, this);
  JS_SetHostPromiseRejectionTracker(rt, RejectionTracker, this);
  JSContext* ctx = JS_NewContext(rt);
  JS_SetContextOpaque(ctx, this);
  rt_ = rt;
  ctx_ = ctx;
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue k = JS_NewObject(ctx);
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int argc;
  };
  static const Fn fns[] = {
      {"doc", Doc, 0}, {"type", NodeType, 1}, {"name", Name, 1}, {"data", Data, 1},
      {"setData", SetData, 2}, {"parent", Parent, 1}, {"kids", Kids, 1}, {"attr", GetAttr, 2},
      {"setAttr", SetAttr, 3}, {"delAttr", DelAttr, 2}, {"attrs", Attrs, 1}, {"create", Create, 2},
      {"insert", Insert, 3}, {"remove", Remove, 1}, {"query", Query, 3}, {"matches", Matches, 2},
      {"parse", Parse, 1}, {"html", Html, 2}, {"text", Text, 1}, {"value", GetValue, 1},
      {"setValue", SetValue, 2}, {"checked", GetChecked, 1}, {"setChecked", SetChecked, 2},
      {"selIndex", SelIndex, 2}, {"rect", NodeRect, 1}, {"computed", Computed, 2}, {"cookie", Cookie, 1},
      {"url", PageUrl, 0}, {"resolve", Resolve, 1}, {"alert", Alert, 1}, {"confirm", Confirm, 1},
      {"prompt", Prompt, 2}, {"navigate", Navigate, 2}, {"setTitle", SetTitle, 1}, {"log", JsLog, 1},
      {"timer", JsTimer, 3}, {"clearTimer", JsClearTimer, 1}, {"request", Request, 4},
      {"scrollTo", ScrollTo, 2}, {"scroll", Scroll, 0}, {"viewport", Viewport, 0},
      {"media", Media, 1}, {"dirty", Dirty, 0}, {"submit", Submit, 1}, {"cur", Cur, 0},
      {"cvCreate", CvCreate, 1}, {"cvResize", CvResize, 3}, {"cvState", CvState, 2},
      {"cvMatrix", CvMatrix, 8}, {"cvGetMatrix", CvGetMatrix, 1}, {"cvColor", CvColor, 3},
      {"cvGradient", CvGradient, 10}, {"cvPattern", CvPattern, 10}, {"cvSourceSize", CvSourceSize, 1},
      {"cvProp", CvProp, 3}, {"cvFont", CvFont, 2}, {"cvShadow", CvShadow, 5}, {"cvDash", CvDash, 2},
      {"cvPath", CvPath, 10}, {"cvDraw", CvDraw, 6}, {"cvHit", CvHit, 5}, {"cvText", CvText, 6},
      {"cvMeasure", CvMeasure, 2}, {"cvImage", CvImage, 10}, {"cvGetData", CvGetData, 5},
      {"cvPutData", CvPutData, 10}, {"cvDataUrl", CvDataUrl, 1}, {"imgLoad", ImgLoad, 1},
      {"imgSize", ImgSize, 1}, {"cvSvgPath", CvSvgPath, 2},
      {"storage", StorageOp, 4}, {"pushState", PushState, 3},
      {"attachShadow", AttachShadow, 1}, {"shadow", ShadowOf, 1}, {"host", HostOf, 1},
      {"setDefined", SetDefined, 1}, {"customs", Customs, 1}, {"connected", Connected, 1}, {"histLen", HistLen, 0}, {"histGo", HistGo, 1},
  };
  for (size_t i = 0; i < sizeof fns / sizeof fns[0]; ++i)
    JS_SetPropertyStr(ctx, k, fns[i].name, JS_NewCFunction(ctx, fns[i].fn, fns[i].name, fns[i].argc));
  JS_SetPropertyStr(ctx, global, "__kite", k);
  JS_FreeValue(ctx, global);
  Execute(kDomPrelude, "kite:dom.js", 0);
}

ScriptEngine::~ScriptEngine() {
  JSContext* ctx = (JSContext*)ctx_;
  for (std::map<std::string, ModuleRec>::iterator it = modules_.begin(); it != modules_.end(); ++it)
    if (it->second.value) {
      JS_FreeValue(ctx, *(JSValue*)it->second.value);
      delete (JSValue*)it->second.value;
    }
  for (size_t i = 0; i < timers_.size(); ++i) {
    JS_FreeValue(ctx, *(JSValue*)timers_[i].fn);
    delete (JSValue*)timers_[i].fn;
  }
  JS_FreeContext(ctx);
  JS_FreeRuntime((JSRuntime*)rt_);
  canvases_.clear();
}

long long ScriptEngine::NowMs() {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (long long)(t / 10000ULL);
#else
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

static int InterruptCb(JSRuntime*, void* opaque) {
  ScriptEngine* e = (ScriptEngine*)opaque;
  static int counter = 0;
  if (++counter < 64) return 0;
  counter = 0;
  long long d = e->deadline();
  if (d <= 0) return 0;
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  long long now = (long long)((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000ULL);
#else
  struct timeval tv;
  gettimeofday(&tv, 0);
  long long now = (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
  return now > d ? 1 : 0;
}

void ScriptEngine::Log(const std::string& line) {
  if (console_.size() > 2000) console_.erase(console_.begin(), console_.begin() + 500);
  console_.push_back(line);
}

void ScriptEngine::ReportException() {
  JSContext* ctx = (JSContext*)ctx_;
  JSValue ex = JS_GetException(ctx);
  std::string msg = Str(ctx, ex);
  if (JS_IsError(ctx, ex)) {
    JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
    if (!JS_IsUndefined(stack)) msg += "\n" + Str(ctx, stack);
    JS_FreeValue(ctx, stack);
  }
  JS_FreeValue(ctx, ex);
  Log("Fehler: " + msg);
}

void ScriptEngine::RunJobs() {
  JSContext* pctx;
  for (int i = 0; i < 100000; ++i) {
    int r = JS_ExecutePendingJob((JSRuntime*)rt_, &pctx);
    if (r <= 0) {
      if (r < 0) ReportException();
      break;
    }
  }
  for (size_t i = 0; i < rejections_.size(); ++i)
    Log("Fehler: Uncaught (in promise) " + rejections_[i].second);
  rejections_.clear();
}

WebStorage* ScriptEngine::StorageFor(bool session) {
  WebStorage* st = host_ ? host_->Storage(session) : 0;
  return st ? st : (session ? &ownSession_ : &ownLocal_);
}

void ScriptEngine::NoteRejection(void* promise, const std::string& message) {
  rejections_.push_back(std::make_pair(promise, message));
}

void ScriptEngine::ForgetRejection(void* promise) {
  for (size_t i = 0; i < rejections_.size(); ++i)
    if (rejections_[i].first == promise) {
      rejections_.erase(rejections_.begin() + i);
      return;
    }
}

void ScriptEngine::Execute(const std::string& source, const std::string& fileName, Node* scriptElement) {
  JSContext* ctx = (JSContext*)ctx_;
  Node* prev = currentScript_;
  currentScript_ = scriptElement;
  deadline_ = NowMs() + timeLimit_;
  JSValue r = JS_Eval(ctx, source.c_str(), source.size(), fileName.c_str(), JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) ReportException();
  JS_FreeValue(ctx, r);
  RunJobs();
  deadline_ = 0;
  currentScript_ = prev;
}

static void CallKiteHook(ScriptEngine* e, JSContext* ctx, const char* hook, int argc, JSValue* argv,
                         JSValue* result) {
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue fn = JS_GetPropertyStr(ctx, global, hook);
  JSValue r = JS_UNDEFINED;
  if (JS_IsFunction(ctx, fn)) r = JS_Call(ctx, fn, global, argc, argv);
  if (JS_IsException(r)) {
    JSValue ex = JS_GetException(ctx);
    e->Log("Fehler: " + Str(ctx, ex));
    JS_FreeValue(ctx, ex);
    r = JS_UNDEFINED;
  }
  if (result) *result = r;
  else JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, global);
}

void ScriptEngine::DispatchDocumentEvent(const char* type) {
  JSContext* ctx = (JSContext*)ctx_;
  deadline_ = NowMs() + timeLimit_;
  JSValue arg = JS_NewString(ctx, type);
  CallKiteHook(this, ctx, "__kiteDocEvent", 1, &arg, 0);
  JS_FreeValue(ctx, arg);
  RunJobs();
  deadline_ = 0;
}

void ScriptEngine::PopState(int stateId, bool hashChanged, const std::string& oldUrl) {
  JSContext* ctx = (JSContext*)ctx_;
  deadline_ = NowMs() + timeLimit_;
  JSValue args[3] = {JS_NewInt32(ctx, stateId), JS_NewBool(ctx, hashChanged), NewStr(ctx, oldUrl)};
  CallKiteHook(this, ctx, "__kitePopState", 3, args, 0);
  JS_FreeValue(ctx, args[2]);
  RunJobs();
  deadline_ = 0;
}

void ScriptEngine::DispatchWindowEvent(const char* type) {
  JSContext* ctx = (JSContext*)ctx_;
  deadline_ = NowMs() + timeLimit_;
  JSValue arg = JS_NewString(ctx, type);
  CallKiteHook(this, ctx, "__kiteWinEvent", 1, &arg, 0);
  JS_FreeValue(ctx, arg);
  RunJobs();
  deadline_ = 0;
}

bool ScriptEngine::DispatchEvent(Node* target, const char* type) {
  JSContext* ctx = (JSContext*)ctx_;
  deadline_ = NowMs() + timeLimit_;
  JSValue args[2] = {JS_NewInt32(ctx, HandleOf(target)), JS_NewString(ctx, type)};
  JSValue r;
  CallKiteHook(this, ctx, "__kiteDispatch", 2, args, &r);
  bool ok = JS_ToBool(ctx, r) != 0 || JS_IsUndefined(r);
  JS_FreeValue(ctx, r);
  JS_FreeValue(ctx, args[1]);
  RunJobs();
  deadline_ = 0;
  return ok;
}

int ScriptEngine::HandleOf(Node* n) {
  if (!n) return 0;
  if (n->scriptHandle > 0 && n->scriptHandle < (int)handles_.size() && handles_[n->scriptHandle] == n)
    return n->scriptHandle;
  handles_.push_back(n);
  n->scriptHandle = (int)handles_.size() - 1;
  return n->scriptHandle;
}

Node* ScriptEngine::NodeOf(int h) {
  if (h <= 0 || h >= (int)handles_.size()) return 0;
  return handles_[h];
}

Canvas2D* ScriptEngine::CanvasById(int id) {
  std::map<int, std::unique_ptr<Canvas2D> >::iterator it = canvases_.find(id);
  return it == canvases_.end() ? 0 : it->second.get();
}

int ScriptEngine::CreateCanvas(Node* n) {
  if (n->canvasId && CanvasById(n->canvasId)) return n->canvasId;
  long long w = 300, h = 150;
  std::string ws = n->Attr("width"), hs = n->Attr("height");
  if (!ws.empty() && (!ParseInt(Trim(ws), w) || w < 0)) w = 300;
  if (!hs.empty() && (!ParseInt(Trim(hs), h) || h < 0)) h = 150;
  std::unique_ptr<Canvas2D> c(new Canvas2D((int)std::min(w, 16384LL), (int)std::min(h, 16384LL), page_->fonts()));
  int id = c->id();
  canvases_[id] = std::move(c);
  n->canvasId = id;
  MarkDirty();  // the painter now shows the bitmap
  return id;
}

void ScriptEngine::WatchImage(Node* img, const std::string& url, bool request) {
  for (size_t i = 0; i < imageWaiters_.size(); ++i)
    if (imageWaiters_[i].second == img && imageWaiters_[i].first == url) return;
  imageWaiters_.push_back(std::make_pair(url, img));
  if (request && std::find(imageLoads_.begin(), imageLoads_.end(), url) == imageLoads_.end())
    imageLoads_.push_back(url);
}

std::vector<std::string> ScriptEngine::TakeImageLoads() {
  std::vector<std::string> out;
  out.swap(imageLoads_);
  return out;
}

void ScriptEngine::ImageLoaded(const std::string& url, bool ok) {
  std::vector<Node*> nodes;
  for (size_t i = 0; i < imageWaiters_.size();) {
    if (imageWaiters_[i].first == url) {
      nodes.push_back(imageWaiters_[i].second);
      imageWaiters_.erase(imageWaiters_.begin() + i);
    } else {
      ++i;
    }
  }
  for (size_t i = 0; i < nodes.size(); ++i) DispatchEvent(nodes[i], ok ? "load" : "error");
  if (!nodes.empty()) MarkDirty();
}

void ScriptEngine::Adopt(std::unique_ptr<Node> n) {
  n->parent = 0;
  detached_.push_back(std::move(n));
}

static void ClearLayout(Node* n) {
  n->layoutBox = nullptr;  // boxes belong to the document's layout tree
  for (size_t i = 0; i < n->children.size(); ++i) ClearLayout(n->children[i].get());
  if (n->shadowRoot) ClearLayout(n->shadowRoot.get());
}

std::unique_ptr<Node> ScriptEngine::DetachNode(Node* n) {
  if (n->parent) {
    ClearLayout(n);
    return n->parent->RemoveChild(n);
  }
  for (size_t i = detached_.size(); i-- > 0;)
    if (detached_[i].get() == n) {
      std::unique_ptr<Node> out = std::move(detached_[i]);
      detached_.erase(detached_.begin() + i);
      return out;
    }
  return std::unique_ptr<Node>();
}

int ScriptEngine::AddTimer(void* fn, int delay, bool repeat) {
  Timer t;
  t.id = nextTimer_++;
  if (delay < 0) delay = 0;
  if (repeat && delay < 10) delay = 10;
  t.interval = delay;
  t.due = NowMs() + delay;
  t.repeat = repeat;
  t.fn = fn;
  timers_.push_back(t);
  return t.id;
}

void ScriptEngine::ClearTimer(int id) {
  JSContext* ctx = (JSContext*)ctx_;
  for (size_t i = 0; i < timers_.size(); ++i)
    if (timers_[i].id == id) {
      JS_FreeValue(ctx, *(JSValue*)timers_[i].fn);
      delete (JSValue*)timers_[i].fn;
      timers_.erase(timers_.begin() + i);
      return;
    }
}

int ScriptEngine::NextTimerDelay() {
  if (timers_.empty()) return -1;
  long long now = NowMs(), best = -1;
  for (size_t i = 0; i < timers_.size(); ++i) {
    long long d = timers_[i].due - now;
    if (d < 0) d = 0;
    if (best < 0 || d < best) best = d;
  }
  return (int)best;
}

void ScriptEngine::RunDueTimers() {
  JSContext* ctx = (JSContext*)ctx_;
  long long now = NowMs();
  // Collect due timers first: callbacks may add or clear timers.
  std::vector<int> due;
  for (size_t i = 0; i < timers_.size(); ++i)
    if (timers_[i].due <= now) due.push_back(timers_[i].id);
  for (size_t k = 0; k < due.size(); ++k) {
    size_t i = 0;
    while (i < timers_.size() && timers_[i].id != due[k]) ++i;
    if (i == timers_.size()) continue;
    JSValue fn = JS_DupValue(ctx, *(JSValue*)timers_[i].fn);
    if (timers_[i].repeat) {
      timers_[i].due = now + timers_[i].interval;
    } else {
      JS_FreeValue(ctx, *(JSValue*)timers_[i].fn);
      delete (JSValue*)timers_[i].fn;
      timers_.erase(timers_.begin() + i);
    }
    deadline_ = NowMs() + timeLimit_;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue r = JS_Call(ctx, fn, global, 0, 0);
    if (JS_IsException(r)) ReportException();
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, fn);
    RunJobs();
    deadline_ = 0;
  }
}

int ScriptEngine::AddRequest(const ScriptRequest& r) {
  ScriptRequest req = r;
  req.id = nextRequest_++;
  requests_.push_back(req);
  return req.id;
}

std::vector<ScriptRequest> ScriptEngine::TakeRequests() {
  std::vector<ScriptRequest> out;
  out.swap(requests_);
  return out;
}

void ScriptEngine::DeliverResponse(int id, int status, const std::string& statusText,
                                   const std::string& body,
                                   const std::vector<std::pair<std::string, std::string> >& headers,
                                   const std::string& finalUrl, bool networkError) {
  JSContext* ctx = (JSContext*)ctx_;
  JSValue hdrs = JS_NewArray(ctx);
  for (size_t i = 0; i < headers.size(); ++i) {
    JS_SetPropertyUint32(ctx, hdrs, (uint32_t)(i * 2), NewStr(ctx, AsciiLower(headers[i].first)));
    JS_SetPropertyUint32(ctx, hdrs, (uint32_t)(i * 2 + 1), NewStr(ctx, headers[i].second));
  }
  JSValue args[6] = {JS_NewInt32(ctx, id), JS_NewInt32(ctx, status), NewStr(ctx, statusText),
                     NewStr(ctx, body), hdrs, NewStr(ctx, finalUrl)};
  JSValue arr[7];
  for (int i = 0; i < 6; ++i) arr[i] = args[i];
  arr[6] = JS_NewBool(ctx, networkError);
  deadline_ = NowMs() + timeLimit_;
  CallKiteHook(this, ctx, "__kiteResponse", 7, arr, 0);
  for (int i = 0; i < 7; ++i) JS_FreeValue(ctx, arr[i]);
  RunJobs();
  deadline_ = 0;
}

bool ScriptEngine::TakeDirty() {
  bool d = dirty_;
  dirty_ = false;
  return d;
}

// ---------------------------------------------------------------------------
// ES modules

void ScriptEngine::SetImportMap(const std::string& json, const std::string& baseUrl) {
  JSContext* ctx = (JSContext*)ctx_;
  JSValue obj = JS_ParseJSON(ctx, json.c_str(), json.size(), "importmap");
  if (JS_IsException(obj)) {
    ReportException();
    return;
  }
  JSValue imports = JS_GetPropertyStr(ctx, obj, "imports");
  JSPropertyEnum* tab = 0;
  uint32_t len = 0;
  Url base = Url::Parse(baseUrl);
  if (JS_IsObject(imports) &&
      JS_GetOwnPropertyNames(ctx, &tab, &len, imports, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
    for (uint32_t i = 0; i < len; ++i) {
      JSValue v = JS_GetProperty(ctx, imports, tab[i].atom);
      const char* key = JS_AtomToCString(ctx, tab[i].atom);
      if (key && JS_IsString(v)) {
        Url u = base.Resolve(Str(ctx, v));
        if (u.valid()) importMap_.push_back(std::make_pair(std::string(key), u.Spec()));
      }
      if (key) JS_FreeCString(ctx, key);
      JS_FreeValue(ctx, v);
      JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
  }
  JS_FreeValue(ctx, imports);
  JS_FreeValue(ctx, obj);
}

std::string ScriptEngine::ResolveModuleSpecifier(const std::string& spec, const std::string& base) {
  // Import map: exact entries, then the longest matching "prefix/" entry.
  size_t best = std::string::npos, bestLen = 0;
  for (size_t i = 0; i < importMap_.size(); ++i) {
    const std::string& k = importMap_[i].first;
    if (k == spec) return importMap_[i].second;
    if (!k.empty() && k[k.size() - 1] == '/' && spec.compare(0, k.size(), k) == 0 && k.size() > bestLen) {
      best = i;
      bestLen = k.size();
    }
  }
  if (best != std::string::npos) return importMap_[best].second + spec.substr(bestLen);
  bool relative = StartsWith(spec, "/") || StartsWith(spec, "./") || StartsWith(spec, "../");
  bool absolute = false;
  size_t colon = spec.find(':');
  if (colon != std::string::npos && colon > 0) {
    absolute = true;
    for (size_t i = 0; i < colon; ++i) {
      char c = spec[i];
      if (!(isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.')) absolute = false;
    }
  }
  if (!relative && !absolute) return std::string();  // bare specifier without a mapping
  Url b = Url::Parse(base);
  if (!b.valid() || b.scheme() == "javascript") b = Url::Parse(page_->url());
  Url u = b.Resolve(spec);
  if (!u.valid()) return std::string();
  return u.SpecNoFragment();
}

bool ScriptEngine::CompileModule(const std::string& url, const std::string& source) {
  JSContext* ctx = (JSContext*)ctx_;
  ModuleRec& rec = modules_[url];
  rec.source = source;
  deadline_ = NowMs() + timeLimit_;
  JSValue m = JS_Eval(ctx, source.c_str(), source.size(), url.c_str(),
                      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_RESOLVE);
  deadline_ = 0;
  if (JS_IsException(m)) {
    ReportException();
    rec.state = -1;
    return false;
  }
  JSModuleDef* def = (JSModuleDef*)JS_VALUE_GET_PTR(m);
  JSValue meta = JS_GetImportMeta(ctx, def);
  if (!JS_IsException(meta)) JS_SetPropertyStr(ctx, meta, "url", NewStr(ctx, url));
  JS_FreeValue(ctx, meta);
  rec.value = new JSValue(m);
  rec.state = 1;
  int n = JS_GetModuleRequestCount(def);
  std::vector<std::string> deps;
  for (int i = 0; i < n; ++i) {
    JSAtom a = JS_GetModuleRequest(ctx, def, i);
    const char* spec = JS_AtomToCString(ctx, a);
    JS_FreeAtom(ctx, a);
    if (!spec) continue;
    std::string dep = ResolveModuleSpecifier(spec, url);
    if (dep.empty()) Log(std::string("Fehler: Modul \"") + spec + "\" kann nicht aufgel\xC3\xB6st werden (" + url + ")");
    JS_FreeCString(ctx, spec);
    if (dep.empty()) {
      modules_[url].state = -1;
      continue;
    }
    deps.push_back(dep);
    if (!modules_.count(dep)) {
      modules_[dep].state = 0;
      moduleRequests_.push_back(dep);
    }
  }
  modules_[url].deps = deps;
  return true;
}

void ScriptEngine::AddModule(const std::string& url, const std::string& source) {
  std::map<std::string, ModuleRec>::iterator it = modules_.find(url);
  if (it != modules_.end() && it->second.state != 0) return;  // already known
  CompileModule(url, source);
}

std::vector<std::string> ScriptEngine::TakeModuleRequests() {
  std::vector<std::string> out;
  out.swap(moduleRequests_);
  return out;
}

bool ScriptEngine::ProvideModule(const std::string& url, const std::string& source, bool ok) {
  std::map<std::string, ModuleRec>::iterator it = modules_.find(url);
  if (it == modules_.end() || it->second.state != 0) return false;
  if (!ok) {
    it->second.state = -1;
    Log("Fehler: Modul " + url + " konnte nicht geladen werden");
    return true;
  }
  CompileModule(url, source);
  return true;
}

int ScriptEngine::ModuleGraphState(const std::string& url) {
  std::vector<std::string> stack(1, url);
  std::set<std::string> seen;
  int state = 1;
  while (!stack.empty()) {
    std::string u = stack.back();
    stack.pop_back();
    if (!seen.insert(u).second) continue;
    std::map<std::string, ModuleRec>::iterator it = modules_.find(u);
    if (it == modules_.end() || it->second.state < 0) return -1;
    if (it->second.state == 0) {
      state = 0;
      continue;
    }
    for (size_t i = 0; i < it->second.deps.size(); ++i) stack.push_back(it->second.deps[i]);
  }
  return state;
}

void ScriptEngine::RunModule(const std::string& url, Node* scriptElement) {
  JSContext* ctx = (JSContext*)ctx_;
  std::map<std::string, ModuleRec>::iterator it = modules_.find(url);
  if (it == modules_.end() || !it->second.value) return;
  Node* prev = currentScript_;
  currentScript_ = 0;  // document.currentScript is null in modules
  deadline_ = NowMs() + timeLimit_;
  JSValue m = *(JSValue*)it->second.value;
  if (JS_ResolveModule(ctx, m) < 0) {
    ReportException();
  } else {
    JSValue r = JS_EvalFunction(ctx, JS_DupValue(ctx, m));
    if (JS_IsException(r)) {
      ReportException();
    } else {
      RunJobs();
      if (JS_PromiseState(ctx, r) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(ctx, r);
        std::string msg = Str(ctx, reason);
        if (JS_IsError(ctx, reason)) {
          JSValue stack = JS_GetPropertyStr(ctx, reason, "stack");
          if (!JS_IsUndefined(stack)) msg += "\n" + Str(ctx, stack);
          JS_FreeValue(ctx, stack);
        }
        JS_FreeValue(ctx, reason);
        Log("Fehler: " + msg);
      }
    }
    JS_FreeValue(ctx, r);
  }
  RunJobs();
  deadline_ = 0;
  currentScript_ = prev;
}

void* ScriptEngine::LoadModuleNow(const std::string& url) {
  JSContext* ctx = (JSContext*)ctx_;
  std::string source;
  std::map<std::string, ModuleRec>::iterator it = modules_.find(url);
  if (it != modules_.end() && it->second.state == 1) {
    source = it->second.source;  // compiled before but dropped by QuickJS: compile again
  } else if (!host_ || !host_->FetchSync(url, source)) {
    return 0;
  }
  JSValue m = JS_Eval(ctx, source.c_str(), source.size(), url.c_str(),
                      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(m)) return 0;
  JSModuleDef* def = (JSModuleDef*)JS_VALUE_GET_PTR(m);
  JSValue meta = JS_GetImportMeta(ctx, def);
  if (!JS_IsException(meta)) JS_SetPropertyStr(ctx, meta, "url", NewStr(ctx, url));
  JS_FreeValue(ctx, meta);
  JS_FreeValue(ctx, m);  // the context's module list keeps it
  return def;
}

}  // namespace kite
