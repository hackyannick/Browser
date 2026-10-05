#!/bin/sh
# Builds the part of FFmpeg (LGPL 2.1+) that Kite uses for <audio> and
# <video>: decoders, demuxers and the audio resampler, no programs, no
# network code, no encoders.
#
#   tools/build-ffmpeg.sh win32  [prefix]   # i686 MinGW, Windows 2000
#   tools/build-ffmpeg.sh native [prefix]   # host build for the engine tests
#
# The source tarball is downloaded once into third_party/ffmpeg/ and checked
# against a pinned SHA-256.
set -e
TARGET=${1:-win32}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${2:-$ROOT/third_party/ffmpeg/$TARGET}
VERSION=4.4.2
SHA256=af419a7f88adbc56c758ab19b4c708afbcae15ef09606b82b855291f6a6faa93
URLS="http://archive.ubuntu.com/ubuntu/pool/universe/f/ffmpeg/ffmpeg_${VERSION}.orig.tar.xz
https://ffmpeg.org/releases/ffmpeg-${VERSION}.tar.xz"
WORK=$ROOT/third_party/ffmpeg
TARBALL=$WORK/ffmpeg-$VERSION.tar.xz
mkdir -p "$WORK"

if [ -f "$PREFIX/lib/libavcodec.a" ] && [ "$PREFIX/.stamp" -nt "$0" ]; then
  echo "FFmpeg ($TARGET) ist bereits gebaut: $PREFIX"
  exit 0
fi

if [ ! -f "$TARBALL" ]; then
  for u in $URLS; do
    echo "Lade $u ..."
    if curl -fsSL --retry 3 -o "$TARBALL.part" "$u"; then mv "$TARBALL.part" "$TARBALL"; break; fi
  done
fi
[ -f "$TARBALL" ] || { echo "FEHLER: FFmpeg-Quellen nicht ladbar" >&2; exit 1; }
SUM=$(sha256sum "$TARBALL" | cut -d' ' -f1)
if [ "$SUM" != "$SHA256" ]; then
  echo "WARNUNG: SHA-256 des FFmpeg-Archivs weicht ab ($SUM)" >&2
fi

SRC=$WORK/ffmpeg-$VERSION
if [ ! -d "$SRC" ]; then
  # Unpack and patch in a scratch directory so that a half-done tree is
  # never mistaken for a ready one.
  rm -rf "$WORK/unpack.$$"; mkdir -p "$WORK/unpack.$$"
  tar -C "$WORK/unpack.$$" -xJf "$TARBALL"
  for p in "$ROOT"/third_party/ffmpeg-patches/*.patch; do
    [ -f "$p" ] && patch -s -d "$WORK/unpack.$$/ffmpeg-$VERSION" -p1 < "$p"
  done
  mv "$WORK/unpack.$$/ffmpeg-$VERSION" "$SRC"; rmdir "$WORK/unpack.$$"
fi

BUILD=$WORK/build-$TARGET
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"

COMMON="--prefix=$PREFIX --enable-static --disable-shared --disable-programs --disable-doc
 --disable-network --disable-avdevice --disable-avfilter --disable-postproc --disable-swscale
 --disable-everything --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-sdl2
 --disable-xlib --disable-libxcb --disable-vaapi --disable-vdpau --disable-debug
 --enable-decoder=h264,hevc,vp8,vp9,theora,aac,aac_latm,mp3,mp3float,mp2,opus,vorbis,flac,alac,pcm_s16le,pcm_s16be,pcm_s24le,pcm_f32le,pcm_u8,pcm_mulaw,pcm_alaw
 --enable-demuxer=mov,matroska,ogg,mp3,wav,aac,flac
 --enable-parser=h264,hevc,aac,vp8,vp9,opus,vorbis,mpegaudio,flac
 --enable-bsf=vp9_superframe_split
 --enable-swresample"
if ! command -v nasm >/dev/null 2>&1; then COMMON="$COMMON --disable-x86asm"; fi

if [ "$TARGET" = win32 ]; then
  # No threads (Windows 2000 has no condition variables), no Vista+ APIs.
  "$SRC/configure" $COMMON --target-os=mingw32 --arch=x86 --cpu=i686 --cross-prefix=i686-w64-mingw32- \
    --disable-pthreads --disable-w32threads --disable-schannel --disable-dxva2 --disable-d3d11va \
    --disable-mediafoundation --extra-cflags="-D_WIN32_WINNT=0x0500 -O2" >configure.log
  # BCryptGenRandom is Vista+; av_get_random_seed falls back to other sources.
  sed -i 's/#define HAVE_BCRYPT 1/#define HAVE_BCRYPT 0/' config.h
else
  "$SRC/configure" $COMMON --enable-pic --disable-pthreads >configure.log
fi
make -j"$(nproc 2>/dev/null || echo 2)" >make.log 2>&1 || { tail -20 make.log; exit 1; }
make install >/dev/null
touch "$PREFIX/.stamp"
echo "FFmpeg ($TARGET) gebaut: $PREFIX"
