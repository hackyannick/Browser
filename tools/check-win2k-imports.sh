#!/bin/sh
# Verifies that kite.exe only imports functions that exist on Windows 2000.
# Usage: tools/check-win2k-imports.sh build-win/kite.exe
set -e
EXE=${1:-build-win/kite.exe}
OBJDUMP=${OBJDUMP:-i686-w64-mingw32-objdump}

# APIs introduced after Windows 2000 that toolchains commonly pull in.
DENY='GetThreadId|GetTickCount64|InitializeSRWLock|AcquireSRWLock|ReleaseSRWLock|TryAcquireSRWLock
|InitializeConditionVariable|SleepConditionVariable|WakeConditionVariable|WakeAllConditionVariable
|InitOnceExecuteOnce|InitOnceBeginInitialize|InitializeCriticalSectionEx|CreateEventEx|CreateMutexEx
|CreateSemaphoreEx|GetFileInformationByHandleEx|SetFileInformationByHandle|GetNativeSystemInfo
|AddVectoredExceptionHandler|RemoveVectoredExceptionHandler|EncodePointer|DecodePointer
|IsWow64Process|GetModuleHandleEx|FlsAlloc|FlsFree|FlsGetValue|FlsSetValue
|GetLogicalProcessorInformation|RtlCaptureStackBackTrace|SetThreadStackGuarantee|GetErrorMode
|CompareStringEx|LCMapStringEx|LocaleNameToLCID|GetUserDefaultLocaleName|GetSystemTimePreciseAsFileTime
|getaddrinfo|freeaddrinfo|getnameinfo|GetAddrInfoW|FreeAddrInfoW|inet_pton|inet_ntop|WSAPoll
|CreateSymbolicLink|GetFinalPathNameByHandle|QueryFullProcessImageName|_aligned_malloc|_aligned_free
|_time64|_localtime64|_gmtime64|_mktime64|_stat64|_wstat64|_fstat64|_ftime64|_findfirst64|_wfindfirst64
|_vscprintf|_vscwprintf|_strtoi64|_strtoui64|_wcstoi64|_set_invalid_parameter_handler|__iob_func
|SetDllDirectory|AttachConsole|GetConsoleWindow|SetProcessDEPPolicy|WTSGetActiveConsoleSessionId'
DENY=$(echo "$DENY" | tr -d '\n')

PE=$($OBJDUMP -p "$EXE")
echo "$PE" | grep -E "Major(OSystem|Subsystem)Version|Minor(OSystem|Subsystem)Version"
OSV=$(echo "$PE" | awk '/MajorOSystemVersion/{print $2}')
SSV=$(echo "$PE" | awk '/MajorSubsystemVersion/{print $2}')
if [ "$OSV" -gt 5 ] || [ "$SSV" -gt 5 ]; then
  echo "FEHLER: PE-Header verlangt eine neuere Windows-Version als 5.0" >&2
  exit 1
fi
IMPORTS=$(echo "$PE" | sed -n '/The Import Tables/,/The Export Tables\|Resource/p' |
          grep -E '^\s+[0-9a-f]+\s+[0-9]+ ' | awk '{print $3}')
BAD=$(echo "$IMPORTS" | grep -E "^($DENY)\$" || true)
if [ -n "$BAD" ]; then
  echo "FEHLER: Importe, die es unter Windows 2000 nicht gibt:" >&2
  echo "$BAD" >&2
  exit 1
fi
echo "OK: $(echo "$IMPORTS" | wc -l) Importe, alle Windows-2000-kompatibel."
