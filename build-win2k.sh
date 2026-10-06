#!/bin/sh
# Cross-compiles Kite for Windows 2000 (32-bit) with MinGW-w64 and packages
# kite.exe together with the root certificates into dist/.
# Requires: cmake, i686-w64-mingw32-g++ (Debian/Ubuntu: g++-mingw-w64-i686).
set -e
cd "$(dirname "$0")"
# Windows 2000 only has the classic msvcrt.dll. Toolchains that default to
# the Universal CRT (e.g. Homebrew's mingw-w64) produce an EXE that cannot
# start there, so stop before building anything.
CC=$(command -v i686-w64-mingw32-gcc-win32 || command -v i686-w64-mingw32-gcc || true)
if [ -z "$CC" ]; then
  echo "FEHLER: i686-w64-mingw32-gcc nicht gefunden (MinGW-w64 installieren)" >&2
  exit 1
fi
if echo '#include <stdio.h>' | "$CC" -dM -E - | grep -q '_UCRT'; then
  echo "FEHLER: $CC verwendet die Universal CRT (ucrtbase.dll), die es unter Windows 2000 nicht gibt." >&2
  echo "Bitte eine MinGW-w64-Toolchain mit msvcrt verwenden, z. B. per Docker (siehe README, Abschnitt Bauen):" >&2
  echo "  docker run --rm -v \"\$PWD\":/src -w /src ubuntu:24.04 sh -c 'apt-get update && apt-get install -y cmake g++ g++-mingw-w64-i686 nasm curl xz-utils make && ./build-win2k.sh'" >&2
  exit 1
fi
# FFmpeg libraries built by a different toolchain cannot be reused.
CCID=$("$CC" --version | head -1)
if [ -d third_party/ffmpeg/win32 ] && [ "$(cat third_party/ffmpeg/win32/.toolchain 2>/dev/null)" != "$CCID" ]; then
  rm -rf third_party/ffmpeg/win32 build-win
fi
# Audio/video decoders (FFmpeg, LGPL). KITE_NO_FFMPEG=1 builds without media playback.
if [ "${KITE_NO_FFMPEG:-0}" != 1 ]; then
  sh tools/build-ffmpeg.sh win32
  echo "$CCID" > third_party/ffmpeg/win32/.toolchain
fi
# A build directory configured on another machine (different paths) is stale.
if [ -f build-win/CMakeCache.txt ] && ! grep -q "CMAKE_HOME_DIRECTORY:INTERNAL=$(pwd)\$" build-win/CMakeCache.txt; then
  rm -rf build-win
fi
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
i686-w64-mingw32-strip build-win/kite.exe
sh tools/check-win2k-imports.sh build-win/kite.exe
rm -rf dist && mkdir -p dist/Kite
cp build-win/kite.exe resources/cacert.pem dist/Kite/
cp README.md dist/Kite/LIESMICH.md
cp third_party/bearssl/LICENSE.txt dist/Kite/LICENSE-BearSSL.txt
cp third_party/quickjs/LICENSE dist/Kite/LICENSE-QuickJS.txt
cp third_party/libwebp/COPYING dist/Kite/LICENSE-libwebp.txt
cp third_party/libwebp/PATENTS dist/Kite/PATENTS-libwebp.txt
cp third_party/brotli/LICENSE dist/Kite/LICENSE-Brotli.txt
cp third_party/dav1d/COPYING dist/Kite/LICENSE-dav1d.txt
if [ -f third_party/ffmpeg/ffmpeg-4.4.2/COPYING.LGPLv2.1 ] && [ -f third_party/ffmpeg/win32/lib/libavcodec.a ]; then
  cp third_party/ffmpeg/ffmpeg-4.4.2/COPYING.LGPLv2.1 dist/Kite/LICENSE-FFmpeg.txt
fi
echo "Fertig: dist/Kite/kite.exe"
