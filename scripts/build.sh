#!/usr/bin/env bash
# Builds dist/Disegno.exe (portable) and dist/Disegno-<version>-setup.msi from Linux.
# Needs: cmake, ninja, mingw-w64 (x86_64-w64-mingw32-g++), wixl (msitools).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(sed -n 's/^project(Disegno VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
BUILD="${BUILD_DIR:-$ROOT/build/windows}"
DIST="$ROOT/dist"

echo "==> Disegno $VERSION"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw-w64-x86_64.cmake"
cmake --build "$BUILD" --parallel

mkdir -p "$DIST"
cp "$BUILD/Disegno.exe" "$DIST/Disegno.exe"

echo "==> MSI"
wixl -a x64 \
  -D Version="$VERSION" \
  -D ExePath="$DIST/Disegno.exe" \
  -D IconPath="$ROOT/assets/disegno.ico" \
  -o "$DIST/Disegno-$VERSION-setup.msi" \
  "$ROOT/installer/Disegno.wxs"

(cd "$DIST" && sha256sum Disegno.exe "Disegno-$VERSION-setup.msi" > SHA256SUMS.txt)
echo "==> Done:"
ls -l "$DIST"
