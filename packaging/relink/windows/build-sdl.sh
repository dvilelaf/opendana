#!/usr/bin/env bash
# Rebuild the shipped SDL2.dll from the corresponding source
# (sources/SDL2.tar.xz -> SDL2-2.32.10/) with MinGW-w64 on Linux.
# Same recipe as packaging/build-sdl2.sh.
#   bash build-sdl.sh <unpacked SDL2-2.32.10 dir> <output prefix>
# Result: <output prefix>/bin/SDL2.dll (stripped, no machine paths, no PE time stamp).
set -euo pipefail
src="$(cd "${1:?usage: build-sdl.sh <SDL2-2.32.10 dir> <output prefix>}" && pwd)"
prefix="${2:?usage: build-sdl.sh <SDL2-2.32.10 dir> <output prefix>}"
mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export SOURCE_DATE_EPOCH=1756762407 TZ=UTC LC_ALL=C
cmake -S "$src" -B "$work/build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$src/build-scripts/cmake-toolchain-mingw64-x86_64.cmake" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  "-DCMAKE_C_FLAGS=-ffile-prefix-map=$work=. -ffile-prefix-map=$src=SDL2-2.32.10 -ffile-prefix-map=$work/build=build" \
  "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-insert-timestamp" \
  -DSDL_SHARED=ON -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL_TESTS=OFF
cmake --build "$work/build" -j"${JOBS:-4}"
cmake --install "$work/build" --strip
sha256sum "$prefix/bin/SDL2.dll"
