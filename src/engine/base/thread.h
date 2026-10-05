// Kite Engine - threads and wake-up events for Windows 2000 (Win32 events,
// _beginthreadex) and POSIX hosts.
#ifndef KITE_BASE_THREAD_H
#define KITE_BASE_THREAD_H

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#endif

namespace kite {

// Auto-reset event: Wait() returns once Signal() was called (or after the
// timeout; returns false then).
class WakeEvent {
 public:
#ifdef _WIN32
  WakeEvent() { h_ = CreateEventW(0, FALSE, FALSE, 0); }
  ~WakeEvent() { CloseHandle(h_); }
  void Signal() { SetEvent(h_); }
  bool Wait(int timeoutMs) {
    return WaitForSingleObject(h_, timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs) == WAIT_OBJECT_0;
  }
#else
  WakeEvent() : set_(false) {
    pthread_mutex_init(&m_, 0);
    pthread_cond_init(&c_, 0);
  }
  ~WakeEvent() {
    pthread_cond_destroy(&c_);
    pthread_mutex_destroy(&m_);
  }
  void Signal() {
    pthread_mutex_lock(&m_);
    set_ = true;
    pthread_cond_signal(&c_);
    pthread_mutex_unlock(&m_);
  }
  bool Wait(int timeoutMs) {
    pthread_mutex_lock(&m_);
    if (!set_) {
      if (timeoutMs < 0) {
        while (!set_) pthread_cond_wait(&c_, &m_);
      } else {
        struct timeval now;
        gettimeofday(&now, 0);
        struct timespec until;
        long long ns = (long long)now.tv_usec * 1000 + (long long)(timeoutMs % 1000) * 1000000;
        until.tv_sec = now.tv_sec + timeoutMs / 1000 + (time_t)(ns / 1000000000);
        until.tv_nsec = (long)(ns % 1000000000);
        while (!set_ && pthread_cond_timedwait(&c_, &m_, &until) == 0) {
        }
      }
    }
    bool was = set_;
    set_ = false;
    pthread_mutex_unlock(&m_);
    return was;
  }
#endif

 private:
  WakeEvent(const WakeEvent&);
  WakeEvent& operator=(const WakeEvent&);
#ifdef _WIN32
  HANDLE h_;
#else
  pthread_mutex_t m_;
  pthread_cond_t c_;
  bool set_;
#endif
};

// Starts |fn(arg)| on a new detached thread. Returns false on failure.
inline bool StartThread(void (*fn)(void*), void* arg) {
  struct Start {
    void (*fn)(void*);
    void* arg;
  };
  Start* s = new Start;
  s->fn = fn;
  s->arg = arg;
#ifdef _WIN32
  struct Tramp {
    static unsigned __stdcall Run(void* p) {
      Start* s = (Start*)p;
      s->fn(s->arg);
      delete s;
      return 0;
    }
  };
  unsigned id;
  HANDLE h = (HANDLE)_beginthreadex(0, 1024 * 1024, Tramp::Run, s, 0, &id);
  if (!h) {
    delete s;
    return false;
  }
  CloseHandle(h);
  return true;
#else
  struct Tramp {
    static void* Run(void* p) {
      Start* s = (Start*)p;
      s->fn(s->arg);
      delete s;
      return 0;
    }
  };
  pthread_t t;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1024 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&t, &attr, Tramp::Run, s);
  pthread_attr_destroy(&attr);
  if (r != 0) {
    delete s;
    return false;
  }
  return true;
#endif
}

// Called from background threads when results for the UI thread are
// waiting (the platform layer installs a function that wakes its message
// loop, which then calls ScriptEngine::PumpAsync).
typedef void (*WakeUiFunc)(void* ctx);
void SetUiWaker(WakeUiFunc fn, void* ctx);
void WakeUi();

}  // namespace kite

#endif
