// Kite Engine - Web Workers: each worker runs its own QuickJS runtime on a
// background thread. Messages are structured-clone encoded strings.
#ifndef KITE_SCRIPT_WORKER_H
#define KITE_SCRIPT_WORKER_H

#include <memory>
#include <string>
#include <vector>

#include "base/mutex.h"
#include "base/thread.h"

namespace kite {

class WorkerThread {
 public:
  struct Event {
    enum Type { kMessage, kError, kLog };
    Type type;
    std::string data;
  };
  // Fetches and runs |url| (a classic script or, with |module|, an ES module).
  static std::shared_ptr<WorkerThread> Start(const std::string& url, bool module, const std::string& name);
  ~WorkerThread();

  void PostMessage(const std::string& serialized);
  void Terminate();
  bool TakeEvents(std::vector<Event>& out);

  // Called on the worker thread (by its natives).
  void Emit(Event::Type type, const std::string& data);
  bool terminated();
  const std::string& url() const { return url_; }
  const std::string& name() const { return name_; }

 private:
  WorkerThread() {}
  static void ThreadMain(void* arg);
  void Run();

  std::string url_, name_;
  bool module_ = false;
  Mutex mu_;
  WakeEvent wake_;
  std::vector<std::string> inbox_;
  std::vector<Event> outbox_;
  bool terminate_ = false;
  std::shared_ptr<WorkerThread> self_;
};

}  // namespace kite

#endif
