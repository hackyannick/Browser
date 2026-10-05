// Kite Engine - JavaScript support: QuickJS plus Kite's own DOM bindings.
#ifndef KITE_SCRIPT_SCRIPT_H
#define KITE_SCRIPT_SCRIPT_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dom/node.h"
#include "script/storage.h"

namespace kite {
class Canvas2D;
}

namespace kite {

class Page;

// Services the platform provides to scripts.
class ScriptHost {
 public:
  virtual ~ScriptHost() {}
  virtual std::string GetCookies(const std::string& url) = 0;
  virtual void SetCookie(const std::string& url, const std::string& cookie) = 0;
  virtual void Alert(const std::string& message) = 0;
  virtual bool Confirm(const std::string& message) = 0;
  virtual std::string Prompt(const std::string& message, const std::string& def) = 0;
  // location.href = ..., location.reload(), form.submit() ...
  virtual void Navigate(const std::string& url, bool replace) = 0;
  virtual void SetTitle(const std::string& title) = 0;
  virtual void ScrollTo(float x, float y) = 0;
  virtual void GetScroll(float& x, float& y) = 0;
  // Blocking fetch, used for modules that could not be loaded ahead of
  // time (dynamic import()). Returns false if unavailable.
  virtual bool FetchSync(const std::string& url, std::string& body) { return false; }
  // localStorage (shared, persistent) and this tab's sessionStorage. Null
  // means "keep the data inside the script engine".
  virtual WebStorage* Storage(bool session) { return 0; }
  // history.pushState/replaceState (same origin, already checked): adds or
  // replaces a session history entry for this document. |stateId| comes
  // back through ScriptEngine::PopState when the user returns to it.
  virtual bool PushState(const std::string& url, bool replace, int stateId) { return true; }
  virtual int HistoryLength() { return 1; }
  virtual void HistoryGo(int delta) {}
};

// A network request started by fetch()/XMLHttpRequest.
struct ScriptRequest {
  int id;
  std::string method, url, body;
  std::vector<std::pair<std::string, std::string> > headers;
};

class ScriptEngine {
 public:
  ScriptEngine(Page* page, ScriptHost* host);
  ~ScriptEngine();

  // Runs a classic script. Errors go to the console log.
  void Execute(const std::string& source, const std::string& fileName, Node* scriptElement);
  // Same-document history traversal (or fragment navigation): makes the
  // entry's state current and fires popstate (and hashchange).
  void PopState(int stateId, bool hashChanged, const std::string& oldUrl);
  // Fires DOMContentLoaded / load.
  void DispatchDocumentEvent(const char* type);
  void DispatchWindowEvent(const char* type);
  // Dispatches a DOM event (click, input, change, submit ...) to |target|.
  // Returns false if a listener called preventDefault().
  bool DispatchEvent(Node* target, const char* type);

  // Timers: milliseconds until the next timer is due (-1 if none).
  int NextTimerDelay();
  void RunDueTimers();

  // fetch()/XMLHttpRequest.
  std::vector<ScriptRequest> TakeRequests();
  void DeliverResponse(int id, int status, const std::string& statusText, const std::string& body,
                       const std::vector<std::pair<std::string, std::string> >& headers,
                       const std::string& finalUrl, bool networkError);

  // ES modules. Module scripts are compiled as their sources arrive; the
  // URLs of their imports are queued for fetching (TakeModuleRequests,
  // answered with ProvideModule). A module runs once its whole graph is
  // compiled (ModuleGraphState: 0 pending, 1 ready, -1 failed).
  void SetImportMap(const std::string& json, const std::string& baseUrl);
  void AddModule(const std::string& url, const std::string& source);
  std::vector<std::string> TakeModuleRequests();
  bool ProvideModule(const std::string& url, const std::string& source, bool ok);
  int ModuleGraphState(const std::string& url);
  void RunModule(const std::string& url, Node* scriptElement);
  std::string ResolveModuleSpecifier(const std::string& spec, const std::string& base);
  // Called by the QuickJS module loader.
  void* LoadModuleNow(const std::string& url);
  void NoteRejection(void* promise, const std::string& message);
  void ForgetRejection(void* promise);

  // Set when scripts changed the document or its styles.
  bool TakeDirty();
  // Set when a canvas bitmap changed (repaint only, no relayout).
  bool TakeCanvasDirty() {
    bool d = canvasDirty_;
    canvasDirty_ = false;
    return d;
  }
  // Images requested by scripts (new Image().src = ...) that the platform
  // should load; report completion with ImageLoaded.
  std::vector<std::string> TakeImageLoads();
  void ImageLoaded(const std::string& url, bool ok);
  // Maximum run time of one script or event dispatch.
  void SetTimeLimit(int ms) { timeLimit_ = ms; }
  const std::vector<std::string>& console() const { return console_; }
  void Log(const std::string& line);

  // Internal (used by the native bindings).
  Page* page() { return page_; }
  ScriptHost* host() { return host_; }
  int HandleOf(Node* n);
  Node* NodeOf(int handle);
  void Adopt(std::unique_ptr<Node> detached);
  std::unique_ptr<Node> DetachNode(Node* n);
  void MarkDirty() { dirty_ = layoutStale_ = true; }
  bool TakeLayoutStale() {
    bool s = layoutStale_;
    layoutStale_ = false;
    return s;
  }
  Node* currentScript() { return currentScript_; }
  int AddTimer(void* fn, int delay, bool repeat);
  void ClearTimer(int id);
  int AddRequest(const ScriptRequest& r);
  Canvas2D* CanvasById(int id);
  int CreateCanvas(Node* n);
  void MarkCanvasDirty() { canvasDirty_ = true; }
  void WatchImage(Node* img, const std::string& url, bool request);
  std::map<int, std::shared_ptr<void> >& canvasObjects() { return canvasObjects_; }

 private:
  void RunJobs();
  void ReportException();
  long long NowMs();

  struct ModuleRec {
    int state = 0;  // 0 fetching, 1 compiled, -1 failed
    void* value = 0;  // JSValue* of the compiled module
    std::string source;
    std::vector<std::string> deps;
  };
  bool CompileModule(const std::string& url, const std::string& source);
  std::map<std::string, ModuleRec> modules_;
  std::vector<std::string> moduleRequests_;
  std::vector<std::pair<std::string, std::string> > importMap_;  // specifier (prefix) -> URL
  std::vector<std::pair<void*, std::string> > rejections_;  // unhandled so far
 public:
  WebStorage* StorageFor(bool session);
 private:
  WebStorage ownLocal_, ownSession_;

  struct Timer {
    int id;
    long long due;
    int interval;
    bool repeat;
    void* fn;  // JSValue* (owned)
  };
  Page* page_;
  ScriptHost* host_;
  void* rt_;   // JSRuntime*
  void* ctx_;  // JSContext*
  std::vector<Node*> handles_;
  std::vector<std::unique_ptr<Node> > detached_;
  std::vector<Timer> timers_;
  int nextTimer_;
  std::vector<ScriptRequest> requests_;
  int nextRequest_;
  std::vector<std::string> console_;
  bool dirty_;
  bool layoutStale_ = false;
  bool canvasDirty_ = false;
  std::map<int, std::unique_ptr<Canvas2D> > canvases_;
  std::map<int, std::shared_ptr<void> > canvasObjects_;  // patterns
  std::vector<std::pair<std::string, Node*> > imageWaiters_;
  std::vector<std::string> imageLoads_;
  Node* currentScript_;
  long long deadline_;
  int timeLimit_ = 8000;

 public:
  long long deadline() const { return deadline_; }
};

// HTML serialization (innerHTML/outerHTML).
std::string SerializeNode(const Node* n, bool outer);

}  // namespace kite

#endif
