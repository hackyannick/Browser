/*
 * Windows 2000 compatibility shims.
 *
 * Recent MinGW-w64 runtimes (libstdc++/libgcc thread support) import
 * GetThreadId, which only exists from Windows XP SP2/Vista on. kernel32.dll on
 * Windows 2000 does not export it, so the loader would refuse to start
 * kite.exe. We provide the import pointer ourselves so the linker resolves it
 * here instead of from kernel32.
 */
#include <windows.h>

typedef LONG(WINAPI* NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);

typedef struct {
  LONG ExitStatus;
  PVOID TebBaseAddress;
  struct {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
  } ClientId;
  ULONG_PTR AffinityMask;
  LONG Priority;
  LONG BasePriority;
} KITE_THREAD_BASIC_INFORMATION;

static DWORD WINAPI KiteGetThreadId(HANDLE thread) {
  static NtQueryInformationThreadFn query = 0;
  KITE_THREAD_BASIC_INFORMATION info;
  if (thread == GetCurrentThread()) return GetCurrentThreadId();
  if (!query) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) query = (NtQueryInformationThreadFn)GetProcAddress(ntdll, "NtQueryInformationThread");
  }
  if (query && query(thread, 0 /* ThreadBasicInformation */, &info, sizeof info, 0) >= 0)
    return (DWORD)(ULONG_PTR)info.ClientId.UniqueThread;
  return 0;
}

#if defined(__MINGW32__) && defined(_X86_)
/* i686 stdcall decoration: __imp__GetThreadId@4 */
void* kite_imp_GetThreadId __asm__("__imp__GetThreadId@4") = (void*)KiteGetThreadId;
#endif

/*
 * Condition variables (Vista+) used by the MinGW-w64 thread support layer.
 * Emulated with a semaphore per condition variable, allocated lazily and
 * published through the CONDITION_VARIABLE pointer slot. Spurious wake-ups
 * are permitted by the API contract, which keeps the emulation simple.
 */
typedef struct {
  CRITICAL_SECTION lock;
  LONG waiters;
  HANDLE sema;
} KiteCondVar;

static KiteCondVar* KiteCvGet(PVOID* slot) {
  KiteCondVar* cv = (KiteCondVar*)*slot;
  KiteCondVar* fresh;
  if (cv) return cv;
  fresh = (KiteCondVar*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(KiteCondVar));
  InitializeCriticalSection(&fresh->lock);
  fresh->sema = CreateSemaphoreW(0, 0, 0x7fffffff, 0);
  cv = (KiteCondVar*)InterlockedCompareExchangePointer(slot, fresh, 0);
  if (cv) {
    /* Another thread won the race. */
    CloseHandle(fresh->sema);
    DeleteCriticalSection(&fresh->lock);
    HeapFree(GetProcessHeap(), 0, fresh);
    return cv;
  }
  return fresh;
}

static VOID WINAPI KiteInitializeConditionVariable(PVOID* slot) { *slot = 0; }

static BOOL WINAPI KiteSleepConditionVariableCS(PVOID* slot, CRITICAL_SECTION* cs, DWORD ms) {
  KiteCondVar* cv = KiteCvGet(slot);
  DWORD r;
  EnterCriticalSection(&cv->lock);
  cv->waiters++;
  LeaveCriticalSection(&cv->lock);
  LeaveCriticalSection(cs);
  r = WaitForSingleObject(cv->sema, ms);
  if (r != WAIT_OBJECT_0) {
    EnterCriticalSection(&cv->lock);
    if (cv->waiters > 0) cv->waiters--;
    else WaitForSingleObject(cv->sema, 0); /* a waker already released us */
    LeaveCriticalSection(&cv->lock);
  }
  EnterCriticalSection(cs);
  if (r != WAIT_OBJECT_0) {
    SetLastError(ERROR_TIMEOUT);
    return FALSE;
  }
  return TRUE;
}

static VOID WINAPI KiteWakeConditionVariable(PVOID* slot) {
  KiteCondVar* cv = KiteCvGet(slot);
  EnterCriticalSection(&cv->lock);
  if (cv->waiters > 0) {
    cv->waiters--;
    ReleaseSemaphore(cv->sema, 1, 0);
  }
  LeaveCriticalSection(&cv->lock);
}

static VOID WINAPI KiteWakeAllConditionVariable(PVOID* slot) {
  KiteCondVar* cv = KiteCvGet(slot);
  EnterCriticalSection(&cv->lock);
  if (cv->waiters > 0) {
    ReleaseSemaphore(cv->sema, cv->waiters, 0);
    cv->waiters = 0;
  }
  LeaveCriticalSection(&cv->lock);
}

#if defined(__MINGW32__) && defined(_X86_)
void* kite_imp_InitializeConditionVariable __asm__("__imp__InitializeConditionVariable@4") =
    (void*)KiteInitializeConditionVariable;
void* kite_imp_SleepConditionVariableCS __asm__("__imp__SleepConditionVariableCS@12") =
    (void*)KiteSleepConditionVariableCS;
void* kite_imp_WakeConditionVariable __asm__("__imp__WakeConditionVariable@4") =
    (void*)KiteWakeConditionVariable;
void* kite_imp_WakeAllConditionVariable __asm__("__imp__WakeAllConditionVariable@4") =
    (void*)KiteWakeAllConditionVariable;
#endif

/*
 * C runtime functions FFmpeg uses that the msvcrt.dll of Windows 2000
 * (version 6.x) does not export.
 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Aligned allocations: the original pointer and the size precede the block. */
static void* __cdecl KiteAlignedMalloc(size_t size, size_t align) {
  unsigned char* raw;
  size_t a = align < sizeof(void*) ? sizeof(void*) : align;
  if ((a & (a - 1)) != 0) {
    errno = EINVAL;
    return 0;
  }
  raw = (unsigned char*)malloc(size + a + 2 * sizeof(void*));
  if (!raw) return 0;
  {
    ULONG_PTR p = ((ULONG_PTR)raw + 2 * sizeof(void*) + a - 1) & ~(ULONG_PTR)(a - 1);
    ((void**)p)[-1] = raw;
    ((size_t*)p)[-2] = size;
    return (void*)p;
  }
}

static void __cdecl KiteAlignedFree(void* p) {
  if (p) free(((void**)p)[-1]);
}

static void* __cdecl KiteAlignedRealloc(void* p, size_t size, size_t align) {
  void* n;
  size_t old;
  if (!p) return KiteAlignedMalloc(size, align);
  if (size == 0) {
    KiteAlignedFree(p);
    return 0;
  }
  n = KiteAlignedMalloc(size, align);
  if (!n) return 0;
  old = ((size_t*)p)[-2];
  memcpy(n, p, old < size ? old : size);
  KiteAlignedFree(p);
  return n;
}

static unsigned __int64 KiteParseU64(const char* s, char** end, int base, int* neg, int* overflow) {
  const char* p = s;
  unsigned __int64 v = 0;
  int any = 0;
  *neg = 0;
  *overflow = 0;
  while (*p == ' ' || (*p >= '\t' && *p <= '\r')) ++p;
  if (*p == '+' || *p == '-') *neg = *p++ == '-';
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    p += 2;
    base = 16;
  } else if (base == 0) {
    base = p[0] == '0' ? 8 : 10;
  }
  for (;; ++p) {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
    else break;
    if (d >= base) break;
    any = 1;
    if (v > (~(unsigned __int64)0 - (unsigned)d) / (unsigned)base) *overflow = 1;
    else v = v * base + d;
  }
  if (end) *end = (char*)(any ? p : s);
  return v;
}

static __int64 __cdecl KiteStrtoi64(const char* s, char** end, int base) {
  int neg, ovf;
  unsigned __int64 v = KiteParseU64(s, end, base, &neg, &ovf);
  if (ovf || (!neg && v > (unsigned __int64)_I64_MAX) || (neg && v > (unsigned __int64)_I64_MAX + 1)) {
    errno = ERANGE;
    return neg ? _I64_MIN : _I64_MAX;
  }
  return neg ? (__int64)(0 - v) : (__int64)v;
}

static unsigned __int64 __cdecl KiteStrtoui64(const char* s, char** end, int base) {
  int neg, ovf;
  unsigned __int64 v = KiteParseU64(s, end, base, &neg, &ovf);
  if (ovf) {
    errno = ERANGE;
    return _UI64_MAX;
  }
  return neg ? 0 - v : v;
}

#if defined(__MINGW32__) && defined(_X86_)
void* kite_imp_aligned_malloc __asm__("__imp___aligned_malloc") = (void*)KiteAlignedMalloc;
void* kite_imp_aligned_free __asm__("__imp___aligned_free") = (void*)KiteAlignedFree;
void* kite_imp_aligned_realloc __asm__("__imp___aligned_realloc") = (void*)KiteAlignedRealloc;
void* kite_imp_strtoi64 __asm__("__imp___strtoi64") = (void*)KiteStrtoi64;
void* kite_imp_strtoui64 __asm__("__imp___strtoui64") = (void*)KiteStrtoui64;
/* Direct (non-dllimport) calls resolve to these. */
__int64 __cdecl kite_strtoi64(const char* s, char** e, int b) __asm__("__strtoi64");
__int64 __cdecl kite_strtoi64(const char* s, char** e, int b) { return KiteStrtoi64(s, e, b); }
unsigned __int64 __cdecl kite_strtoui64(const char* s, char** e, int b) __asm__("__strtoui64");
unsigned __int64 __cdecl kite_strtoui64(const char* s, char** e, int b) { return KiteStrtoui64(s, e, b); }
/* mingw-w64 maps strtoll/strtoull to those two msvcrt exports. */
__int64 __cdecl kite_strtoll(const char* s, char** e, int b) __asm__("_strtoll");
__int64 __cdecl kite_strtoll(const char* s, char** e, int b) { return KiteStrtoi64(s, e, b); }
unsigned __int64 __cdecl kite_strtoull(const char* s, char** e, int b) __asm__("_strtoull");
unsigned __int64 __cdecl kite_strtoull(const char* s, char** e, int b) { return KiteStrtoui64(s, e, b); }
void* kite_imp_strtoll __asm__("__imp__strtoll") = (void*)KiteStrtoi64;
void* kite_imp_strtoull __asm__("__imp__strtoull") = (void*)KiteStrtoui64;
#endif
