#!/usr/bin/env bash
# C6.51 - SDL2 for the packages, built here from the pinned source.
#
#   bash packaging/build-sdl2.sh --target linux|windows|macos [--out DIR] [--force] [--no-pin-check]
#                                [--source-archive SDL2.tar.xz]
#
# Builds the pinned SDL2 (packaging/pins.sh) from the SAME source tree that
# packaging/make-sources.sh ships as sources/SDL2.tar.xz (od_sdl_source_tree), in
# a neutral directory, with the configure options recorded in
# packaging/relink/<platform>/flags.txt. The result goes to a cache under build/
# (default build/deps/sdl2-<ver>-<target>), laid out as the package scripts expect:
#   linux   lib/libSDL2-2.0.so.0 (+ cmake files)           -> SDL2_DIR
#   windows bin/SDL2.dll, lib/cmake/SDL2/SDL2Config.cmake  -> MINGW_SDL2_DIR
#   macos   lib/libSDL2-2.0.0.dylib (universal x86_64+arm64, no code signature,
#           stripped) + cmake files                         -> SDL2_DIR (C6.70)
#           This target runs ON macOS (the VM) with Xcode's compiler and a CMake from the
#           PATH. There is no SDL git tree there: --source-archive is the very
#           sources/SDL2.tar.xz that the licence bundle ships (make-sources.sh), so the
#           library is built from the shipped source.
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

target="" out="" force=0 pincheck=1 source_archive=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --target) target="${2:?--target requires linux|windows}"; shift 2 ;;
        --out) out="${2:?--out requires a directory}"; shift 2 ;;
        --force) force=1; shift ;;
        --no-pin-check) pincheck=0; shift ;;
        --source-archive) source_archive="${2:?--source-archive requires SDL2.tar.xz}"; shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$target" in linux|windows|macos) ;; *) echo "error: --target linux|windows|macos is required" >&2; exit 2 ;; esac
[ -n "$out" ] || out="$OD_BUILD_DIR/deps/sdl2-$OD_PIN_SDL_VERSION-$target"
case "$target" in
    linux)   lib_rel=lib/libSDL2-2.0.so.0; pin="$OD_PIN_LINUX_SDL2_SHA256" ;;
    windows) lib_rel=bin/SDL2.dll;         pin="$OD_PIN_WINDOWS_SDL2_SHA256" ;;
    macos)   lib_rel=lib/libSDL2-2.0.0.dylib; pin="$OD_PIN_MACOS_SDL2_SHA256" ;;
esac

step() { printf '\n== %s ==\n' "$*"; }

if [ "$force" != 1 ] && [ -f "$out/$lib_rel" ] && [ "$(od_sha256 "$out/$lib_rel")" = "$pin" ]; then
    echo "cached: $out/$lib_rel matches the pin ($pin)"
    exit 0
fi

if [ "$target" = macos ]; then
    [ -f "$source_archive" ] || od_die "--source-archive SDL2.tar.xz is required for --target macos"
    [ "$(uname -s)" = Darwin ] || od_die "--target macos runs on macOS"
    od_require_tools cmake tar strip lipo codesign shasum
else
    od_require_env SDL2_SRC "the SDL git tree that contains commit $OD_PIN_SDL_COMMIT (read only)"
    od_require_tools git cmake tar sha256sum strip
fi
if [ "$target" = windows ]; then
    mingw="${MINGW_PREFIX:-x86_64-w64-mingw32}"
    od_require_tools "$mingw-gcc" "$mingw-strip" "$mingw-windres"
fi
generator=("Unix Makefiles")
if [ "$target" = linux ]; then command -v ninja >/dev/null 2>&1 && generator=(Ninja); fi
if [ "$target" = linux ]; then
    # SDL's CMake silently leaves out an audio backend whose development files are not on
    # the build machine. The package must offer all of them, so stop here instead.
    od_require_tools pkg-config
    for dev in "libpulse >= 0.9.15|libpulse-dev" "libpipewire-0.3 >= 0.3.20|libpipewire-0.3-dev" \
               "alsa|libasound2-dev" "jack|libjack-jackd2-dev"; do
        pkg-config --exists "${dev%%|*}" \
            || od_die "pkg-config cannot find ${dev%%|*}: install the development package ${dev##*|} (the SDL2 of the package needs the PulseAudio, PipeWire, ALSA and JACK audio backends)"
    done
    echo "audio development files: libpulse $(pkg-config --modversion libpulse), libpipewire-0.3 $(pkg-config --modversion libpipewire-0.3), alsa $(pkg-config --modversion alsa), jack $(pkg-config --modversion jack)"
fi

mkdir -p "$OD_BUILD_DIR"
work="$(mktemp -d "$OD_BUILD_DIR/sdl2-build.XXXXXX")"
work="$(cd "$work" && pwd)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$(dirname "$out")"
out="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"

step "SDL $OD_PIN_SDL_VERSION source tree (the one the licence bundle ships)"
if [ "$target" = macos ]; then
    mkdir -p "$work/src"
    tar -xJf "$source_archive" -C "$work/src"
else
    od_sdl_source_tree "$work/src"
fi
srcdir="$work/src/SDL2-$OD_PIN_SDL_VERSION"
[ -f "$srcdir/REVISION.txt" ] && [ "$(cat "$srcdir/REVISION.txt")" = "$OD_PIN_SDL_REVISION" ] \
    || od_die "the SDL source is not the pinned revision $OD_PIN_SDL_REVISION"
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
    # Audio backends, requested explicitly and all loaded at run time (dlopen): the library
    # has no fixed dependency on libpulse, libpipewire, libasound or libjack.
    cmake "${common[@]}" -DSDL_STATIC=OFF \
        -DSDL_PULSEAUDIO=ON -DSDL_PULSEAUDIO_SHARED=ON -DSDL_PIPEWIRE=ON -DSDL_PIPEWIRE_SHARED=ON \
        -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_JACK=ON -DSDL_JACK_SHARED=ON
elif [ "$target" = macos ]; then
    # Universal in one pass, the deployment target of the app, no static library, and a
    # fixed install name: the app finds the dylib through @rpath (Contents/Frameworks).
    cmake "${common[@]}" -DSDL_STATIC=OFF -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_MACOSX_RPATH=ON
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
if [ "$target" = macos ]; then
    # The pin is the file as the linker and strip leave it, without any code signature
    # (the package signs ad hoc afterwards; signing leaves every section byte-identical).
    codesign --remove-signature "$stage/$lib_rel" 2>/dev/null || true
    lipo -info "$stage/$lib_rel" | grep -q 'x86_64 arm64' || od_die "libSDL2 is not universal x86_64+arm64"
    otool -D "$stage/$lib_rel" | tail -1 | grep -qx '@rpath/libSDL2-2.0.0.dylib' || od_die "unexpected install name: $(otool -D "$stage/$lib_rel" | tail -1)"
fi
if [ "$target" = linux ]; then
    step "audio backends of the library"
    od_require_tools cc readelf
    probe="$work/audio-probe"
    mkdir -p "$probe"
    cat > "$probe/probe.c" <<'EOF'
#include <SDL.h>
#include <stdio.h>
int main(void) {
    for (int i = 0; i < SDL_GetNumAudioDrivers(); i++) puts(SDL_GetAudioDriver(i));
    return 0;
}
EOF
    cc -o "$probe/probe" "$probe/probe.c" -I"$stage/include/SDL2" "$stage/$lib_rel" -Wl,-rpath,"$(dirname "$stage/$lib_rel")" \
        || od_die "cannot compile the audio-driver probe against the built SDL2"
    drivers="$("$probe/probe")"
    echo "audio drivers: $(echo $drivers)"
    for driver in pulseaudio pipewire alsa; do
        grep -qx "$driver" <<<"$drivers" || od_die "the built $lib_rel does not offer the $driver audio driver"
    done
    needed="$(LC_ALL=C readelf -d "$stage/$lib_rel" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')"
    if grep -Eq '^lib(pulse|pipewire)' <<<"$needed"; then
        od_die "the built $lib_rel links libpulse or libpipewire at load time (DT_NEEDED: $(echo $needed)); they must be loaded at run time"
    fi
fi
actual="$(od_sha256 "$stage/$lib_rel")"
echo "$lib_rel sha256 $actual"
if [ "$pincheck" = 1 ] && [ "$actual" != "$pin" ]; then
    od_die "$lib_rel is $actual but packaging/pins.sh pins $pin (rebuild with a changed toolchain? use --no-pin-check to see the new hash and update the pin with its relink materials)"
fi

step "no machine path in the library"
leak_re='/(med[i]a|hom[e]|tm[p]|User[s]|privat[e]|va[r]/folders)/|[s]olomons|[r]ecomp|[o]racle'
if grep -aEq "$leak_re" "$stage/$lib_rel"; then
    grep -aEo "[[:print:]]*($leak_re)[[:print:]]*" "$stage/$lib_rel" | head >&2
    od_die "$lib_rel still embeds a machine path"
fi

rm -rf "$out.new"
mv "$stage" "$out.new"
rm -rf "$out"
mv "$out.new" "$out"
echo "ready: $out/$lib_rel"
