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
# Docker Desktop's credential helper lives inside the app bundle; when its
# directory is not on PATH, even pulling public images fails with
# 'docker-credential-desktop: executable file not found'.
for d in /Applications/Docker.app/Contents/Resources/bin "$HOME/.docker/bin"; do
  if ! command -v docker-credential-desktop >/dev/null 2>&1 && [ -x "$d/docker-credential-desktop" ]; then
    PATH="$PATH:$d"
  fi
done
export PATH
if ! command -v docker >/dev/null 2>&1; then
  echo "FEHLER: docker nicht gefunden (Docker Desktop oder OrbStack installieren und starten)" >&2
  exit 1
fi
if grep -q '"credsStore" *: *"desktop"' "$HOME/.docker/config.json" 2>/dev/null &&
   ! command -v docker-credential-desktop >/dev/null 2>&1; then
  echo "FEHLER: ~/.docker/config.json verlangt den Helfer docker-credential-desktop, der fehlt." >&2
  echo "Die Zeile mit \"credsStore\" aus ~/.docker/config.json entfernen (fuer oeffentliche" >&2
  echo "Images wie ubuntu:24.04 ist keine Anmeldung noetig) und das Skript erneut starten." >&2
  exit 1
fi
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
