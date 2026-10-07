#!/usr/bin/env bash
# C6.51 - SDL2 for the packages, built here from the pinned source.
#
#   bash packaging/build-sdl2.sh --target linux|windows [--out DIR] [--force] [--no-pin-check]
#
# Builds the pinned SDL2 (packaging/pins.sh) from the SAME source tree that
# packaging/make-sources.sh ships as sources/SDL2.tar.xz (od_sdl_source_tree), in
# a neutral directory, with the configure options recorded in
# packaging/relink/<platform>/flags.txt. The result goes to a cache under build/
# (default build/deps/sdl2-<ver>-<target>), laid out as the package scripts expect:
#   linux   lib/libSDL2-2.0.so.0 (+ cmake files)           -> SDL2_DIR
#   windows bin/SDL2.dll, lib/cmake/SDL2/SDL2Config.cmake  -> MINGW_SDL2_DIR
#
# Reproducibility: -ffile-prefix-map (no machine path in __FILE__ or debug info),
# SOURCE_DATE_EPOCH (= the pinned SDL commit time), PE: -Wl,--no-insert-timestamp;
# the runtime library is stripped. Two builds give the same SHA-256.
#
# Inputs: SDL2_SRC (SDL git tree holding OD_PIN_SDL_COMMIT; read only). Nothing is
# downloaded. A cache that matches the pin is reused unless --force.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
cd "$OD_ROOT"

target="" out="" force=0 pincheck=1
while [ "$#" -gt 0 ]; do
    case "$1" in
        --target) target="${2:?--target requires linux|windows}"; shift 2 ;;
        --out) out="${2:?--out requires a directory}"; shift 2 ;;
        --force) force=1; shift ;;
        --no-pin-check) pincheck=0; shift ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$target" in linux|windows) ;; *) echo "error: --target linux|windows is required" >&2; exit 2 ;; esac
[ -n "$out" ] || out="$OD_BUILD_DIR/deps/sdl2-$OD_PIN_SDL_VERSION-$target"
case "$target" in
    linux)   lib_rel=lib/libSDL2-2.0.so.0; pin="$OD_PIN_LINUX_SDL2_SHA256" ;;
    windows) lib_rel=bin/SDL2.dll;         pin="$OD_PIN_WINDOWS_SDL2_SHA256" ;;
esac

step() { printf '\n== %s ==\n' "$*"; }

if [ "$force" != 1 ] && [ -f "$out/$lib_rel" ] && [ "$(od_sha256 "$out/$lib_rel")" = "$pin" ]; then
    echo "cached: $out/$lib_rel matches the pin ($pin)"
    exit 0
fi

od_require_env SDL2_SRC "the SDL git tree that contains commit $OD_PIN_SDL_COMMIT (read only)"
od_require_tools git cmake tar sha256sum strip
if [ "$target" = windows ]; then
    mingw="${MINGW_PREFIX:-x86_64-w64-mingw32}"
    od_require_tools "$mingw-gcc" "$mingw-strip" "$mingw-windres"
fi
generator=("Unix Makefiles")
if [ "$target" = linux ]; then command -v ninja >/dev/null 2>&1 && generator=(Ninja); fi

mkdir -p "$OD_BUILD_DIR"
work="$(mktemp -d "$OD_BUILD_DIR/sdl2-build.XXXXXX")"
work="$(cd "$work" && pwd)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$(dirname "$out")"
out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"

step "SDL $OD_PIN_SDL_VERSION source tree (the one the licence bundle ships)"
od_sdl_source_tree "$work/src"
srcdir="$work/src/SDL2-$OD_PIN_SDL_VERSION"
builddir="$work/build"
destdir="$work/destdir"

export SOURCE_DATE_EPOCH="$OD_PIN_SDL_EPOCH"
export TZ=UTC LC_ALL=C
map="-ffile-prefix-map=$work=. -ffile-prefix-map=$srcdir=SDL2-$OD_PIN_SDL_VERSION -ffile-prefix-map=$builddir=build"

step "configure ($target)"
common=(-S "$srcdir" -B "$builddir" -G "${generator[*]}" -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_INSTALL_PREFIX="$out" "-DCMAKE_C_FLAGS=$map"
        -DSDL_SHARED=ON -DSDL_TEST=OFF)
if [ "$target" = linux ]; then
    cmake "${common[@]}" -DSDL_STATIC=OFF
else
    cmake "${common[@]}" -DSDL_STATIC=ON -DSDL_TESTS=OFF \
        -DCMAKE_TOOLCHAIN_FILE="$srcdir/build-scripts/cmake-toolchain-mingw64-x86_64.cmake" \
        "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--no-insert-timestamp"
fi

step "build + install (stripped)"
cmake --build "$builddir" -j"$(od_jobs)"
DESTDIR="$destdir" cmake --install "$builddir" --strip >/dev/null
stage="$destdir$out"

[ -f "$stage/$lib_rel" ] || od_die "no $lib_rel produced"
if [ "$target" = windows ]; then
    [ -f "$stage/lib/cmake/SDL2/SDL2Config.cmake" ] || od_die "no lib/cmake/SDL2/SDL2Config.cmake produced"
    "$mingw-objdump" -f "$stage/$lib_rel" | grep -q 'pei-x86-64' || od_die "SDL2.dll is not PE x86-64"
fi
actual="$(od_sha256 "$stage/$lib_rel")"
echo "$lib_rel sha256 $actual"
if [ "$pincheck" = 1 ] && [ "$actual" != "$pin" ]; then
    od_die "$lib_rel is $actual but packaging/pins.sh pins $pin (rebuild with a changed toolchain? use --no-pin-check to see the new hash and update the pin with its relink materials)"
fi

step "no machine path in the library"
leak_re='/(med[i]a|hom[e]|tm[p])/|[s]olomons|[r]ecomp|[o]racle'
if grep -aEq "$leak_re" "$stage/$lib_rel"; then
    grep -aEo "[[:print:]]*($leak_re)[[:print:]]*" "$stage/$lib_rel" | head >&2
    od_die "$lib_rel still embeds a machine path"
fi

rm -rf "$out.new"
mv "$stage" "$out.new"
rm -rf "$out"
mv "$out.new" "$out"
echo "ready: $out/$lib_rel"
