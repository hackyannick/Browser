# Cross-compilation toolchain for Windows 2000 (32-bit) using MinGW-w64.
# The "-win32" compiler variants use the native Win32 thread model, which
# avoids a dependency on winpthreads.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

find_program(MINGW_CC NAMES i686-w64-mingw32-gcc-win32 i686-w64-mingw32-gcc)
find_program(MINGW_CXX NAMES i686-w64-mingw32-g++-win32 i686-w64-mingw32-g++)
set(CMAKE_C_COMPILER ${MINGW_CC})
set(CMAKE_CXX_COMPILER ${MINGW_CXX})
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)

# Pentium-class CPUs: no SSE2 assumptions.
set(CMAKE_C_FLAGS_INIT "-march=i686 -mtune=generic")
set(CMAKE_CXX_FLAGS_INIT "-march=i686 -mtune=generic")

set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
