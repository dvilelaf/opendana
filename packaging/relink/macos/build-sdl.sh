#!/usr/bin/env bash
# Rebuild the shipped libSDL2-2.0.0.dylib from the corresponding source
# (sources/SDL2.tar.xz -> SDL2-2.32.10/) on macOS with Xcode's compiler and CMake.
# Same recipe as packaging/build-sdl2.sh --target macos.
#   bash build-sdl.sh <unpacked SDL2-2.32.10 dir> <output prefix>
# Result: <output prefix>/lib/libSDL2-2.0.0.dylib (universal x86_64+arm64, stripped, no code
# signature, no machine paths). cmake, cc, lipo, strip and codesign must be on the PATH.
set -euo pipefail
src="$(cd "${1:?usage: build-sdl.sh <SDL2-2.32.10 dir> <output prefix>}" && pwd)"
prefix="${2:?usage: build-sdl.sh <SDL2-2.32.10 dir> <output prefix>}"
mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export SOURCE_DATE_EPOCH=1756762407 TZ=UTC LC_ALL=C
cmake -S "$src" -B "$work/build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  "-DCMAKE_C_FLAGS=-ffile-prefix-map=$work=. -ffile-prefix-map=$src=SDL2-2.32.10 -ffile-prefix-map=$work/build=build" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_MACOSX_RPATH=ON
cmake --build "$work/build" -j"${JOBS:-4}"
cmake --install "$work/build" --strip
codesign --remove-signature "$prefix/lib/libSDL2-2.0.0.dylib" 2>/dev/null || true
shasum -a 256 "$prefix/lib/libSDL2-2.0.0.dylib"
