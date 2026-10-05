#!/bin/sh
# Cross-compiles Kite for Windows 2000 (32-bit) with MinGW-w64 and packages
# kite.exe together with the root certificates into dist/.
# Requires: cmake, i686-w64-mingw32-g++ (Debian/Ubuntu: g++-mingw-w64-i686).
set -e
cd "$(dirname "$0")"
# Audio/video decoders (FFmpeg, LGPL). KITE_NO_FFMPEG=1 builds without media playback.
if [ "${KITE_NO_FFMPEG:-0}" != 1 ]; then sh tools/build-ffmpeg.sh win32; fi
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j"$(nproc 2>/dev/null || echo 2)"
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
