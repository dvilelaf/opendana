#!/usr/bin/env bash
# Rebuild the shipped libSDL2-2.0.so.0 from the corresponding source
# (sources/SDL2.tar.xz -> SDL2-2.32.10/). Same recipe as packaging/build-sdl2.sh.
#   bash build-sdl.sh <unpacked SDL2-2.32.10 dir> <output prefix>
# Result: <output prefix>/lib/libSDL2-2.0.so.0 (stripped, no machine paths).
set -euo pipefail
src="$(cd "${1:?usage: build-sdl.sh <SDL2-2.32.10 dir> <output prefix>}" && pwd)"
prefix="${2:?usage: build-sdl.sh <SDL2-2.32.10 dir> <output prefix>}"
mkdir -p "$prefix"
prefix="$(cd "$prefix" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
export SOURCE_DATE_EPOCH=1756762407 TZ=UTC LC_ALL=C
gen=("Unix Makefiles")
command -v ninja >/dev/null 2>&1 && gen=(Ninja)
cmake -S "$src" -B "$work/build" -G "${gen[*]}" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  "-DCMAKE_C_FLAGS=-ffile-prefix-map=$work=. -ffile-prefix-map=$src=SDL2-2.32.10 -ffile-prefix-map=$work/build=build" \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build "$work/build" -j"${JOBS:-4}"
cmake --install "$work/build" --strip
sha256sum "$prefix/lib/libSDL2-2.0.so.0"
