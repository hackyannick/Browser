cmake_minimum_required(VERSION 3.10)
project(KiteBrowser C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# BearSSL (TLS 1.2, MIT licensed, vendored in third_party/bearssl)
file(GLOB BEARSSL_SOURCES
  ${CMAKE_SOURCE_DIR}/third_party/bearssl/src/*.c
  ${CMAKE_SOURCE_DIR}/third_party/bearssl/src/*/*.c)
add_library(bearssl STATIC ${BEARSSL_SOURCES})
target_include_directories(bearssl
  PUBLIC ${CMAKE_SOURCE_DIR}/third_party/bearssl/inc
  PRIVATE ${CMAKE_SOURCE_DIR}/third_party/bearssl/src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(bearssl PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# QuickJS (JavaScript engine, MIT licensed, vendored in third_party/quickjs)
add_library(quickjs STATIC
  ${CMAKE_SOURCE_DIR}/third_party/quickjs/quickjs.c
  ${CMAKE_SOURCE_DIR}/third_party/quickjs/libregexp.c
  ${CMAKE_SOURCE_DIR}/third_party/quickjs/libunicode.c
  ${CMAKE_SOURCE_DIR}/third_party/quickjs/cutils.c
  ${CMAKE_SOURCE_DIR}/third_party/quickjs/dtoa.c)
target_include_directories(quickjs PUBLIC ${CMAKE_SOURCE_DIR}/third_party/quickjs)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="2026-06-04")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(quickjs PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# libwebp decoder (BSD licensed, vendored in third_party/libwebp)
file(GLOB WEBP_SOURCES
  ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/dec/*.c
  ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/dsp/*.c
  ${CMAKE_SOURCE_DIR}/third_party/libwebp/src/utils/*.c)
add_library(webpdec STATIC ${WEBP_SOURCES})
target_include_directories(webpdec PUBLIC ${CMAKE_SOURCE_DIR}/third_party/libwebp)

# Brotli decoder (MIT licensed, vendored in third_party/brotli)
file(GLOB BROTLI_SOURCES
  ${CMAKE_SOURCE_DIR}/third_party/brotli/dec/*.c
  ${CMAKE_SOURCE_DIR}/third_party/brotli/common/*.c)
add_library(brotlidec STATIC ${BROTLI_SOURCES})
target_include_directories(brotlidec PUBLIC ${CMAKE_SOURCE_DIR}/third_party/brotli/include)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(webpdec PRIVATE -w)
  target_compile_options(brotlidec PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# dav1d AV1 decoder for AVIF (BSD licensed, C only, vendored in third_party/dav1d)
set(DAV1D_DIR ${CMAKE_SOURCE_DIR}/third_party/dav1d)
file(GLOB DAV1D_TMPL_SOURCES ${DAV1D_DIR}/src/*_tmpl.c)
file(GLOB DAV1D_SOURCES ${DAV1D_DIR}/src/*.c)
list(REMOVE_ITEM DAV1D_SOURCES ${DAV1D_TMPL_SOURCES})
if(WIN32)
  list(APPEND DAV1D_SOURCES ${DAV1D_DIR}/src/win32/thread.c)
endif()
set(DAV1D_INCLUDES ${DAV1D_DIR}/config ${DAV1D_DIR} ${DAV1D_DIR}/include)
foreach(bits 8 16)
  add_library(dav1d_bits${bits} OBJECT ${DAV1D_TMPL_SOURCES})
  target_include_directories(dav1d_bits${bits} PRIVATE ${DAV1D_INCLUDES})
  target_compile_definitions(dav1d_bits${bits} PRIVATE BITDEPTH=${bits})
endforeach()
add_library(dav1d STATIC ${DAV1D_SOURCES} $<TARGET_OBJECTS:dav1d_bits8> $<TARGET_OBJECTS:dav1d_bits16>)
target_include_directories(dav1d PRIVATE ${DAV1D_INCLUDES} PUBLIC ${DAV1D_DIR}/include)
set_property(TARGET dav1d dav1d_bits8 dav1d_bits16 PROPERTY C_STANDARD 11)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dav1d PRIVATE -w)
  target_compile_options(dav1d_bits8 PRIVATE -w)
  target_compile_options(dav1d_bits16 PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# Kite engine (portable C++11: HTML, CSS, layout, paint, network)
file(GLOB_RECURSE KITE_ENGINE_SOURCES ${CMAKE_SOURCE_DIR}/src/engine/*.cpp)
add_library(kite_engine STATIC ${KITE_ENGINE_SOURCES})
target_include_directories(kite_engine PUBLIC
  ${CMAKE_SOURCE_DIR}/src/engine
  ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(kite_engine PUBLIC bearssl quickjs webpdec brotlidec dav1d)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(kite_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(WIN32)
  # Target Windows 2000 (NT 5.0) APIs only.
  target_compile_definitions(bearssl PRIVATE _WIN32_WINNT=0x0500 WINVER=0x0500)
  target_compile_definitions(quickjs PRIVATE _WIN32_WINNT=0x0500 WINVER=0x0500)
  foreach(t dav1d dav1d_bits8 dav1d_bits16)
    target_compile_definitions(${t} PRIVATE _WIN32_WINNT=0x0500 WINVER=0x0500)
  endforeach()
  target_compile_definitions(kite_engine PUBLIC
    _WIN32_WINNT=0x0500 WINVER=0x0500 _WIN32_IE=0x0501 UNICODE _UNICODE)
  target_link_libraries(kite_engine PUBLIC ws2_32 advapi32)

  file(GLOB KITE_WIN32_SOURCES ${CMAKE_SOURCE_DIR}/src/win32/*.cpp ${CMAKE_SOURCE_DIR}/src/win32/*.c)
  add_executable(kite WIN32 ${KITE_WIN32_SOURCES} ${CMAKE_SOURCE_DIR}/src/win32/kite.rc)
  target_include_directories(kite PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(kite PRIVATE kite_engine comctl32 comdlg32 gdi32 user32 shell32)
  if(MINGW)
    # Fully static runtime (no libstdc++/libgcc DLLs) and a PE header that
    # Windows 2000 accepts (OS/subsystem version 5.0).
    target_link_options(kite PRIVATE
      -static -static-libgcc -static-libstdc++
      -Wl,--major-os-version,5 -Wl,--minor-os-version,0
      -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,0)
  endif()
  # Ship the root certificates next to the executable.
  add_custom_command(TARGET kite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_SOURCE_DIR}/resources/cacert.pem $<TARGET_FILE_DIR:kite>/cacert.pem)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(kite_engine PUBLIC Threads::Threads m)
  add_executable(kite-dump ${CMAKE_SOURCE_DIR}/src/tools/kite_dump.cpp)
  target_link_libraries(kite-dump PRIVATE kite_engine)

  enable_testing()
  add_executable(kite-tests ${CMAKE_SOURCE_DIR}/tests/engine_tests.cpp)
  target_link_libraries(kite-tests PRIVATE kite_engine)
  add_test(NAME engine COMMAND kite-tests)
endif()
