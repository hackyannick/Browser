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
