#!/bin/sh
# Builds dist/Kite/kite.exe inside an Ubuntu 24.04 container with the same
# MinGW toolchain as the CI (msvcrt, Windows 2000 compatible). Works with
# Docker or OrbStack on macOS (Intel and Apple Silicon) and Linux.
#
# The sources are copied into the container without any build output from
# the host (build-win, dist, third_party/ffmpeg), so builds made with other
# toolchains (e.g. Homebrew's UCRT mingw-w64) cannot interfere. FFmpeg is
# cached in the Docker volume "kite-build-cache".
set -e
cd "$(dirname "$0")/.."
mkdir -p dist
docker run --rm \
  -v "$PWD":/host:ro \
  -v "$PWD/dist":/out \
  -v kite-build-cache:/cache \
  ubuntu:24.04 sh -ec '
    echo "== Pakete installieren"
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      cmake make g++ g++-mingw-w64-i686 nasm curl ca-certificates xz-utils patch >/dev/null
    echo "== Quellen kopieren"
    mkdir -p /work
    cd /host
    tar --exclude=./.git --exclude=./build-win --exclude=./build-linux --exclude=./dist \
        --exclude=./third_party/ffmpeg -cf - . | tar -C /work -xf -
    mkdir -p /cache/ffmpeg
    ln -s /cache/ffmpeg /work/third_party/ffmpeg
    cd /work
    i686-w64-mingw32-gcc-win32 --version | head -1
    ./build-win2k.sh
    rm -rf /out/Kite
    cp -r dist/Kite /out/
    echo "== Fertig: dist/Kite/kite.exe"
  '
