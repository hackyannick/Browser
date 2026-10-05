#include "script/worker.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#include "base/strings.h"
#include "net/http.h"
#include "net/url.h"
#include "script/common.h"

extern "C" {
#include "quickjs.h"
}

namespace kite {

extern const char* const kWorkerPrelude;

namespace {

long long NowMs() {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  return (long long)((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000ULL);
#else
  struct timeval tv;
  gettimeofday(&tv, 0);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

struct Timer {
  int id;
  long long due;
  int interval;
  bool repeat;
  JSValue fn;
};

struct WorkerCtx {
  WorkerThread* worker;
  JSRuntime* rt;
  JSContext* ctx;
  std::vector<Timer> timers;
  int nextTimer = 1;
  std::vector<std::pair<void*, std::string> > rejections;
};

WorkerCtx* Ctx(JSContext* ctx) { return (WorkerCtx*)JS_GetContextOpaque(ctx); }

std::string Str(JSContext* ctx, JSValueConst v) {
  size_t len;
  const char* s = JS_ToCStringLen(ctx, &len, v);
  if (!s) return std::string();
  std::string out(s, len);
  JS_FreeCString(ctx, s);
  return out;
}

std::string ExceptionText(JSContext* ctx) {
  JSValue ex = JS_GetException(ctx);
  std::string msg = Str(ctx, ex);
  if (JS_IsError(ctx, ex)) {
    JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
    if (JS_IsString(stack)) msg += "\n" + Str(ctx, stack);
    JS_FreeValue(ctx, stack);
  }
  JS_FreeValue(ctx, ex);
  return msg;
}

bool Fetch(const std::string& url, std::string& body, std::string* finalUrl) {
  FetchRequest r;
  r.url = url;
  r.accept = "*/*";
  FetchResponse s = Network::Get().Fetch(r);
  if (!s.ok || s.status >= 400) return false;
  body = ConvertToUtf8(s.body, s.Charset().empty() ? "utf-8" : s.Charset());
  if (finalUrl) *finalUrl = s.finalUrl.empty() ? url : s.finalUrl;
  return true;
}

void RunJobs(WorkerCtx* w) {
  JSContext* pctx;
  for (int i = 0; i < 100000; ++i) {
    int r = JS_ExecutePendingJob(w->rt, &pctx);
    if (r <= 0) {
      if (r < 0) w->worker->Emit(WorkerThread::Event::kError, ExceptionText(w->ctx));
      break;
    }
  }
  for (size_t i = 0; i < w->rejections.size(); ++i)
    w->worker->Emit(WorkerThread::Event::kLog, "Fehler: Uncaught (in promise) " + w->rejections[i].second);
  w->rejections.clear();
}

#define WFN(name) JSValue name(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)

WFN(Post) {
  if (argc > 0) Ctx(ctx)->worker->Emit(WorkerThread::Event::kMessage, Str(ctx, argv[0]));
  return JS_UNDEFINED;
}

WFN(Log) {
  if (argc > 0) Ctx(ctx)->worker->Emit(WorkerThread::Event::kLog, Str(ctx, argv[0]));
  return JS_UNDEFINED;
}

WFN(SetTimer) {
  if (argc < 3 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
  WorkerCtx* w = Ctx(ctx);
  int32_t ms = 0;
  JS_ToInt32(ctx, &ms, argv[1]);
  Timer t;
  t.id = w->nextTimer++;
  t.repeat = JS_ToBool(ctx, argv[2]) != 0;
  t.interval = std::max(t.repeat ? 4 : 0, (int)ms);
  t.due = NowMs() + t.interval;
  t.fn = JS_DupValue(ctx, argv[0]);
  w->timers.push_back(t);
  return JS_NewInt32(ctx, t.id);
}

WFN(ClearTimer) {
  if (argc < 1) return JS_UNDEFINED;
  WorkerCtx* w = Ctx(ctx);
  int32_t id = 0;
  JS_ToInt32(ctx, &id, argv[0]);
  for (size_t i = 0; i < w->timers.size(); ++i)
    if (w->timers[i].id == id) {
      JS_FreeValue(ctx, w->timers[i].fn);
      w->timers.erase(w->timers.begin() + i);
      break;
    }
  return JS_UNDEFINED;
}

WFN(ImportScripts) {
  WorkerCtx* w = Ctx(ctx);
  Url base = Url::Parse(w->worker->url());
  for (int i = 0; i < argc; ++i) {
    Url u = base.Resolve(Str(ctx, argv[i]));
    std::string src, finalUrl;
    if (!u.valid() || !Fetch(u.Spec(), src, &finalUrl))
      return JS_ThrowTypeError(ctx, "Failed to execute 'importScripts': The script at '%s' failed to load.", u.Spec().c_str());
    JSValue r = JS_Eval(ctx, src.c_str(), src.size(), finalUrl.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) return r;
    JS_FreeValue(ctx, r);
  }
  return JS_UNDEFINED;
}

// fetchSync(method, url, body, headersFlat) -> [status, statusText, ArrayBuffer, headersFlat, url] | null
WFN(FetchSync) {
  if (argc < 4) return JS_NULL;
  WorkerCtx* w = Ctx(ctx);
  FetchRequest r;
  r.method = AsciiUpper(Str(ctx, argv[0]));
  Url u = Url::Parse(w->worker->url()).Resolve(Str(ctx, argv[1]));
  if (!u.valid()) return JS_NULL;
  r.url = u.Spec();
  r.referrer = w->worker->url();
  if (!JsBytes(ctx, &argv[2], r.body)) r.body = Str(ctx, argv[2]);
  uint32_t n = 0;
  JSValue len = JS_GetPropertyStr(ctx, argv[3], "length");
  JS_ToUint32(ctx, &n, len);
  JS_FreeValue(ctx, len);
  for (uint32_t i = 0; i + 1 < n; i += 2) {
    JSValue k = JS_GetPropertyUint32(ctx, argv[3], i), v = JS_GetPropertyUint32(ctx, argv[3], i + 1);
    std::string name = AsciiLower(Str(ctx, k)), value = Str(ctx, v);
    if (name == "content-type") r.contentType = value;
    else if (name == "accept") r.accept = value;
    else r.headers.push_back(std::make_pair(name, value));
    JS_FreeValue(ctx, k);
    JS_FreeValue(ctx, v);
  }
  if (r.accept.empty()) r.accept = "*/*";
  FetchResponse s = Network::Get().Fetch(r);
  if (!s.ok) return JS_NULL;
  JSValue out = JS_NewArray(ctx);
  JS_SetPropertyUint32(ctx, out, 0, JS_NewInt32(ctx, s.status));
  JS_SetPropertyUint32(ctx, out, 1, JS_NewString(ctx, s.status < 400 ? "OK" : ""));
  JS_SetPropertyUint32(ctx, out, 2, JS_NewArrayBufferCopy(ctx, (const uint8_t*)s.body.data(), s.body.size()));
  JSValue hdrs = JS_NewArray(ctx);
  for (size_t i = 0; i < s.headers.size(); ++i) {
    std::string k = AsciiLower(s.headers[i].first);
    JS_SetPropertyUint32(ctx, hdrs, (uint32_t)(i * 2), JS_NewStringLen(ctx, k.data(), k.size()));
    JS_SetPropertyUint32(ctx, hdrs, (uint32_t)(i * 2 + 1),
                         JS_NewStringLen(ctx, s.headers[i].second.data(), s.headers[i].second.size()));
  }
  JS_SetPropertyUint32(ctx, out, 3, hdrs);
  std::string fu = s.finalUrl.empty() ? r.url : s.finalUrl;
  JS_SetPropertyUint32(ctx, out, 4, JS_NewStringLen(ctx, fu.data(), fu.size()));
  return out;
}

WFN(Close) {
  Ctx(ctx)->worker->Terminate();
  return JS_UNDEFINED;
}

WFN(WorkerUrl) {
  const std::string& u = Ctx(ctx)->worker->url();
  return JS_NewStringLen(ctx, u.data(), u.size());
}

WFN(WorkerName) {
  const std::string& n = Ctx(ctx)->worker->name();
  return JS_NewStringLen(ctx, n.data(), n.size());
}

int Interrupt(JSRuntime*, void* opaque) { return ((WorkerThread*)opaque)->terminated() ? 1 : 0; }

void Rejection(JSContext* ctx, JSValueConst promise, JSValueConst reason, JS_BOOL handled, void*) {
  WorkerCtx* w = Ctx(ctx);
  void* key = JS_VALUE_GET_PTR(promise);
  if (handled) {
    for (size_t i = 0; i < w->rejections.size(); ++i)
      if (w->rejections[i].first == key) {
        w->rejections.erase(w->rejections.begin() + i);
        break;
      }
    return;
  }
  w->rejections.push_back(std::make_pair(key, Str(ctx, reason)));
}

char* Normalize(JSContext* ctx, const char* base, const char* name, void*) {
  std::string spec = name;
  if (!(StartsWith(spec, "/") || StartsWith(spec, "./") || StartsWith(spec, "../") || spec.find(':') != std::string::npos)) {
    JS_ThrowTypeError(ctx, "Failed to resolve module specifier \"%s\"", name);
    return 0;
  }
  Url b = Url::Parse(base);
  if (!b.valid()) b = Url::Parse(Ctx(ctx)->worker->url());
  std::string url = b.Resolve(spec).SpecNoFragment();
  char* out = (char*)js_malloc(ctx, url.size() + 1);
  if (out) memcpy(out, url.c_str(), url.size() + 1);
  return out;
}

JSModuleDef* LoadModule(JSContext* ctx, const char* name, void*) {
  std::string src;
  if (!Fetch(name, src, 0)) {
    JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
    return 0;
  }
  JSValue m = JS_Eval(ctx, src.c_str(), src.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(m)) return 0;
  JSModuleDef* def = (JSModuleDef*)JS_VALUE_GET_PTR(m);
  JSValue meta = JS_GetImportMeta(ctx, def);
  if (!JS_IsException(meta)) JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, name));
  JS_FreeValue(ctx, meta);
  JS_FreeValue(ctx, m);
  return def;
}

}  // namespace

std::shared_ptr<WorkerThread> WorkerThread::Start(const std::string& url, bool module, const std::string& name) {
  std::shared_ptr<WorkerThread> w(new WorkerThread);
  w->url_ = url;
  w->module_ = module;
  w->name_ = name;
  w->self_ = w;
  if (!StartThread(ThreadMain, w.get())) {
    w->self_.reset();
    w->Emit(Event::kError, "Worker-Thread konnte nicht gestartet werden");
  }
  return w;
}

WorkerThread::~WorkerThread() {}

void WorkerThread::ThreadMain(void* arg) {
  WorkerThread* w = (WorkerThread*)arg;
  std::shared_ptr<WorkerThread> keep;
  {
    MutexLock l(w->mu_);
    keep.swap(w->self_);
  }
  w->Run();
}

void WorkerThread::PostMessage(const std::string& serialized) {
  {
    MutexLock l(mu_);
    inbox_.push_back(serialized);
  }
  wake_.Signal();
}

void WorkerThread::Terminate() {
  {
    MutexLock l(mu_);
    terminate_ = true;
  }
  wake_.Signal();
}

bool WorkerThread::terminated() {
  MutexLock l(mu_);
  return terminate_;
}

void WorkerThread::Emit(Event::Type type, const std::string& data) {
  {
    MutexLock l(mu_);
    if (terminate_ && type != Event::kError) return;
    Event e;
    e.type = type;
    e.data = data;
    outbox_.push_back(e);
  }
  WakeUi();
}

bool WorkerThread::TakeEvents(std::vector<Event>& out) {
  MutexLock l(mu_);
  if (outbox_.empty()) return false;
  out.insert(out.end(), outbox_.begin(), outbox_.end());
  outbox_.clear();
  return true;
}

void WorkerThread::Run() {
  std::string source, finalUrl;
  if (!Fetch(url_, source, &finalUrl)) {
    Emit(Event::kError, "Das Worker-Skript " + url_ + " konnte nicht geladen werden");
    return;
  }
  WorkerCtx w;
  w.worker = this;
  w.rt = JS_NewRuntime();
  JS_SetMemoryLimit(w.rt, 128 * 1024 * 1024);
  JS_SetMaxStackSize(w.rt, 512 * 1024);
  JS_SetInterruptHandler(w.rt, Interrupt, this);
  JS_SetModuleLoaderFunc(w.rt, Normalize, LoadModule, 0);
  w.ctx = JS_NewContext(w.rt);
  JS_SetContextOpaque(w.ctx, &w);
  JS_SetHostPromiseRejectionTracker(w.rt, Rejection, 0);
  JSContext* ctx = w.ctx;
  InstallCommonApis(ctx);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue k = JS_NewObject(ctx);
  struct Fn {
    const char* name;
    JSCFunction* fn;
    int argc;
  };
  static const Fn fns[] = {{"post", Post, 1},           {"log", Log, 1},
                           {"timer", SetTimer, 3},      {"clearTimer", ClearTimer, 1},
                           {"importScripts", ImportScripts, 1}, {"fetchSync", FetchSync, 4},
                           {"close", Close, 0},         {"url", WorkerUrl, 0},
                           {"name", WorkerName, 0}};
  for (size_t i = 0; i < sizeof fns / sizeof fns[0]; ++i)
    JS_SetPropertyStr(ctx, k, fns[i].name, JS_NewCFunction(ctx, fns[i].fn, fns[i].name, fns[i].argc));
  JS_SetPropertyStr(ctx, global, "__w", k);
  JSValue r = JS_Eval(ctx, kWorkerPrelude, strlen(kWorkerPrelude), "kite:worker.js", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) Emit(Event::kError, ExceptionText(ctx));
  JS_FreeValue(ctx, r);
  // The worker script.
  if (module_) {
    JSValue m = JS_Eval(ctx, source.c_str(), source.size(), finalUrl.c_str(),
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (!JS_IsException(m)) {
      JSValue meta = JS_GetImportMeta(ctx, (JSModuleDef*)JS_VALUE_GET_PTR(m));
      if (!JS_IsException(meta)) JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, finalUrl.c_str()));
      JS_FreeValue(ctx, meta);
      m = JS_EvalFunction(ctx, m);
    }
    if (JS_IsException(m)) Emit(Event::kError, ExceptionText(ctx));
    JS_FreeValue(ctx, m);
  } else {
    r = JS_Eval(ctx, source.c_str(), source.size(), finalUrl.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) Emit(Event::kError, ExceptionText(ctx));
    JS_FreeValue(ctx, r);
  }
  RunJobs(&w);
  JSValue onMessage = JS_GetPropertyStr(ctx, global, "__kiteWorkerMessage");
  while (!terminated()) {
    std::vector<std::string> inbox;
    {
      MutexLock l(mu_);
      inbox.swap(inbox_);
    }
    for (size_t i = 0; i < inbox.size() && !terminated(); ++i) {
      JSValue arg = JS_NewStringLen(ctx, inbox[i].data(), inbox[i].size());
      JSValue res = JS_Call(ctx, onMessage, global, 1, &arg);
      if (JS_IsException(res)) Emit(Event::kError, ExceptionText(ctx));
      JS_FreeValue(ctx, res);
      JS_FreeValue(ctx, arg);
      RunJobs(&w);
    }
    // Timers.
    long long now = NowMs();
    std::vector<int> due;
    for (size_t i = 0; i < w.timers.size(); ++i)
      if (w.timers[i].due <= now) due.push_back(w.timers[i].id);
    for (size_t d = 0; d < due.size() && !terminated(); ++d) {
      size_t i = 0;
      while (i < w.timers.size() && w.timers[i].id != due[d]) ++i;
      if (i == w.timers.size()) continue;
      JSValue fn = JS_DupValue(ctx, w.timers[i].fn);
      if (w.timers[i].repeat) {
        w.timers[i].due = now + w.timers[i].interval;
      } else {
        JS_FreeValue(ctx, w.timers[i].fn);
        w.timers.erase(w.timers.begin() + i);
      }
      JSValue res = JS_Call(ctx, fn, global, 0, 0);
      if (JS_IsException(res)) Emit(Event::kError, ExceptionText(ctx));
      JS_FreeValue(ctx, res);
      JS_FreeValue(ctx, fn);
      RunJobs(&w);
    }
    long long next = -1;
    now = NowMs();
    for (size_t i = 0; i < w.timers.size(); ++i) {
      long long dly = w.timers[i].due - now;
      if (dly < 0) dly = 0;
      if (next < 0 || dly < next) next = dly;
    }
    bool pending;
    {
      MutexLock l(mu_);
      pending = !inbox_.empty();
    }
    if (!pending) wake_.Wait(next < 0 ? 1000 : (int)std::min(next, 1000LL));
  }
  JS_FreeValue(ctx, onMessage);
  for (size_t i = 0; i < w.timers.size(); ++i) JS_FreeValue(ctx, w.timers[i].fn);
  w.timers.clear();
  JS_FreeValue(ctx, global);
  JS_FreeContext(ctx);
  JS_FreeRuntime(w.rt);
}

}  // namespace kite
