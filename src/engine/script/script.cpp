// Native part of Kite's DOM bindings. The web-facing API (Node, Element,
// Event, fetch, ...) is implemented in JavaScript on top of the small set of
// primitives registered here as the global "__kite" object (see dom_js.cpp).
#include "script/script.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "base/strings.h"
#include "css/resolver.h"
#include "css/stylesheet.h"
#include "html/parser.h"
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


// ---------------------------------------------------------------------------
// ScriptEngine

static int InterruptCb(JSRuntime*, void* opaque);

ScriptEngine::ScriptEngine(Page* page, ScriptHost* host)
    : page_(page), host_(host), nextTimer_(1), nextRequest_(1), dirty_(false), currentScript_(0),
      deadline_(0) {
  handles_.push_back(0);  // handle 0 = null
  JSRuntime* rt = JS_NewRuntime();
  JS_SetMemoryLimit(rt, 128 * 1024 * 1024);
  JS_SetMaxStackSize(rt, 256 * 1024);
  JS_SetInterruptHandler(rt, InterruptCb, this);
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
  };
  for (size_t i = 0; i < sizeof fns / sizeof fns[0]; ++i)
    JS_SetPropertyStr(ctx, k, fns[i].name, JS_NewCFunction(ctx, fns[i].fn, fns[i].name, fns[i].argc));
  JS_SetPropertyStr(ctx, global, "__kite", k);
  JS_FreeValue(ctx, global);
  Execute(kDomPrelude, "kite:dom.js", 0);
}

ScriptEngine::~ScriptEngine() {
  JSContext* ctx = (JSContext*)ctx_;
  for (size_t i = 0; i < timers_.size(); ++i) {
    JS_FreeValue(ctx, *(JSValue*)timers_[i].fn);
    delete (JSValue*)timers_[i].fn;
  }
  JS_FreeContext(ctx);
  JS_FreeRuntime((JSRuntime*)rt_);
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

void ScriptEngine::Adopt(std::unique_ptr<Node> n) {
  n->parent = 0;
  detached_.push_back(std::move(n));
}

static void ClearLayout(Node* n) {
  n->layoutBox = nullptr;  // boxes belong to the document's layout tree
  for (size_t i = 0; i < n->children.size(); ++i) ClearLayout(n->children[i].get());
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

}  // namespace kite
