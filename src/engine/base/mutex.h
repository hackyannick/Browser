// Kite Engine - minimal mutex that works on Windows 2000 (no SRW locks,
// no std::mutex dependency on newer Win32 APIs) and on POSIX hosts.
#ifndef KITE_BASE_MUTEX_H
#define KITE_BASE_MUTEX_H

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace kite {

class Mutex {
 public:
#ifdef _WIN32
  Mutex() { InitializeCriticalSection(&cs_); }
  ~Mutex() { DeleteCriticalSection(&cs_); }
  void Lock() { EnterCriticalSection(&cs_); }
  void Unlock() { LeaveCriticalSection(&cs_); }
#else
  Mutex() { pthread_mutex_init(&m_, 0); }
  ~Mutex() { pthread_mutex_destroy(&m_); }
  void Lock() { pthread_mutex_lock(&m_); }
  void Unlock() { pthread_mutex_unlock(&m_); }
#endif

 private:
  Mutex(const Mutex&);
  Mutex& operator=(const Mutex&);
#ifdef _WIN32
  CRITICAL_SECTION cs_;
#else
  pthread_mutex_t m_;
#endif
};

class MutexLock {
 public:
  explicit MutexLock(Mutex& m) : m_(m) { m_.Lock(); }
  ~MutexLock() { m_.Unlock(); }

 private:
  Mutex& m_;
};

}  // namespace kite

#endif
