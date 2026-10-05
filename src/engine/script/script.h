// Kite Engine - JavaScript support: QuickJS plus Kite's own DOM bindings.
#ifndef KITE_SCRIPT_SCRIPT_H
#define KITE_SCRIPT_SCRIPT_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dom/node.h"

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

  // Set when scripts changed the document or its styles.
  bool TakeDirty();
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

 private:
  void RunJobs();
  void ReportException();
  long long NowMs();

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
