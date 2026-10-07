#!/usr/bin/env bash
# C6.50: licence materials and corresponding source of the AppImage Type-2 runtime.
#
#   materials.sh --sources DIR --app APP_SRC --materials DIR --source-archive FILE [--epoch N]
#
#   --sources DIR         the upstream tarballs of sources.lock (each verified against its SHA-256)
#   --app APP_SRC         the application source tree the package is built from (notices,
#                         RELINK.txt and these scripts come from it, i.e. from the packaged commit)
#   --materials DIR       the TAR materials made by make-sources.sh; they are EXTENDED in place:
#                           notices/<component>.txt        the seven runtime notices
#                           sources/appimage-runtime/<c>   one upstream tarball per component
#                                                          (zstd without tests/ and with build/ renamed
#                                                          build-files/: see the notice)
#                           RELINK.txt                     the AppImage one
#                           sources/relink-materials.tar.xz  SDL recipe + runtime build scripts
#   --source-archive FILE writes opendana-<ver>-appimage-runtime-source.tar.xz: pristine upstream
#                         tarballs, lock files and build scripts. It is the LGPL-2.1 source and relink
#                         offer for the statically linked libfuse.
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/common.sh"

sources="" app="" materials="" archive="" epoch=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --sources) sources="${2:?}"; shift 2 ;;
        --app) app="${2:?}"; shift 2 ;;
        --materials) materials="${2:?}"; shift 2 ;;
        --source-archive) archive="${2:?}"; shift 2 ;;
        --epoch) epoch="${2:?}"; shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done
[ -d "$sources" ] && [ -d "$app" ] && [ -d "$materials" ] && [ -n "$archive" ] || od_die "usage: materials.sh --sources DIR --app APP_SRC --materials DIR --source-archive FILE"
epoch="${epoch:-${SOURCE_DATE_EPOCH:-$OD_PIN_SDL_EPOCH}}"
rt="$app/packaging/appimage-runtime"
lock="$rt/sources.lock"
od_require_tools tar xz
tmp="$(mktemp -d "${TMPDIR:-/tmp}/appimage-materials.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

# --- every source tarball is checked against the lock of the PACKAGED commit
names=()
while IFS=$'\t' read -r name _version sha file _url _licence; do
    [ -n "$name" ] || continue
    [ -f "$sources/$file" ] || od_die "missing source tarball $sources/$file (see packaging/appimage-runtime/build.sh --fetch)"
    od_check_pin "$sources/$file" "$sha" "runtime source $file"
    names+=("$name:$file")
done < <(grep -v '^#' "$lock")
[ "${#names[@]}" -eq 7 ] || od_die "sources.lock lists ${#names[@]} components, expected 7"

# --- notices (full LGPL-2.1 text in libfuse's)
for n in type2-runtime libfuse squashfuse zstd zlib mimalloc musl; do
    [ -s "$app/packaging/notices/appimage/$n.txt" ] || od_die "missing notice packaging/notices/appimage/$n.txt"
    cp -p "$app/packaging/notices/appimage/$n.txt" "$materials/notices/$n.txt"
done

# --- sources: the upstream tarballs; zstd without tests/
mkdir -p "$materials/sources/appimage-runtime"
for entry in "${names[@]}"; do
    name="${entry%%:*}"; file="${entry#*:}"
    [ "$name" = zstd ] || cp -p "$sources/$file" "$materials/sources/appimage-runtime/$name"
done
zfile="$(awk -F'\t' '$1=="zstd"{print $4}' "$lock")"
mkdir "$tmp/zstd"
tar -xf "$sources/$zfile" -C "$tmp/zstd"
zdir="$(ls "$tmp/zstd")"
# zstd's tests/ (not compiled by the runtime build) is left out: it holds the four symlinks the
# archive gate forbids and compressed test inputs the ROM auditor cannot attribute to an upstream
# git tree. BSD-licensed code needs no source offer, and lib/ (all that is compiled) is untouched.
rm -rf -- "${tmp:?}/zstd/$zdir/tests"
[ -z "$(find "$tmp/zstd" -type l)" ] || od_die "zstd still has symlinks after dropping tests/"
# The ROM auditor skips any directory called "build"; zstd's build/ (CMake, Meson, Visual Studio
# files, not used by the runtime build) is renamed so that the audit can read it.
if [ -d "$tmp/zstd/$zdir/build" ]; then mv "$tmp/zstd/$zdir/build" "$tmp/zstd/$zdir/build-files"; fi
od_det_tar "$tmp/zstd" "$zdir" "$tmp/zstd.tar" "$epoch"
od_xz "$tmp/zstd.tar" "$materials/sources/appimage-runtime/zstd"

# --- relink materials: SDL recipe (as the TAR) + the runtime build scripts and locks
mkdir -p "$tmp/relink-materials/appimage-runtime"
for f in "$app/packaging/relink/linux"/*; do
    [ "$(basename "$f")" = RELINK.txt ] || cp -p "$f" "$tmp/relink-materials/"
done
for f in Dockerfile build.sh container-build.sh sources.lock toolchain.lock; do
    cp -p "$rt/$f" "$tmp/relink-materials/appimage-runtime/"
done
od_det_tar "$tmp" relink-materials "$tmp/relink-materials.tar" "$epoch"
od_xz "$tmp/relink-materials.tar" "$materials/sources/relink-materials.tar.xz"
cp -p "$app/packaging/relink/appimage/RELINK.txt" "$materials/RELINK.txt"

# --- the LGPL source and relink offer: opendana-<ver>-appimage-runtime-source.tar.xz
top="opendana-$OD_VERSION-appimage-runtime-source"
mkdir -p "$tmp/offer/$top/sources"
for f in Dockerfile build.sh container-build.sh toolchain.lock; do
    cp -p "$rt/$f" "$tmp/offer/$top/$f"
done
cp -p "$app/packaging/relink/appimage/RELINK.txt" "$tmp/offer/$top/RELINK.txt"
mkdir -p "$tmp/offer/$top/notices"
cp -p "$app/packaging/notices/appimage"/*.txt "$tmp/offer/$top/notices/"
# The audit gates reject zstd's test links and files, so the offer carries zstd without the
# test symlinks and files (the same normalised archive as the licence bundle) and its sources.lock
# names that file and hash instead of the pristine tarball. Only lib/ of zstd is compiled, so
# the runtime built from the offer has the same SHA-256.
zname="zstd-$(awk -F'\t' '$1=="zstd"{print $2}' "$lock")-trimmed.tar.xz"
for entry in "${names[@]}"; do
    [ "${entry%%:*}" = zstd ] || cp -p "$sources/${entry#*:}" "$tmp/offer/$top/sources/"
done
cp -p "$materials/sources/appimage-runtime/zstd" "$tmp/offer/$top/sources/$zname"
zsha="$(od_sha256 "$tmp/offer/$top/sources/$zname")"
awk -F'\t' -v OFS='\t' -v z="$zname" -v h="$zsha" '$1=="zstd" && $0 !~ /^#/ {$3=h; $4=z} {print}' "$lock" > "$tmp/offer/$top/sources.lock"
cat > "$tmp/offer/$top/README.txt" <<EOF
OpenDana $OD_VERSION - source and rebuild kit of the AppImage runtime
=======================================================================
The AppImage runtime statically links libfuse (LGPL-2.1), squashfuse, zstd, zlib,
mimalloc and musl. This archive is the corresponding source and the means to
rebuild (or relink with a modified libfuse) the exact runtime of the AppImage:

  sources/        the upstream tarballs (SHA-256 in sources.lock; zstd without its test symlinks)
  sources.lock    name, version, SHA-256 and URL of each
  toolchain.lock  Alpine image digest and compiler packages
  Dockerfile, build.sh, container-build.sh   the reproducible build
  notices/        the licence notice of each component (libfuse: full LGPL-2.1 text)
  RELINK.txt      step by step

  bash build.sh --sources ./sources --out ./out      (needs Docker)
EOF
od_det_tar "$tmp/offer" "$top" "$tmp/offer.tar" "$epoch"
od_xz "$tmp/offer.tar" "$archive"
echo "runtime materials ready: $materials ($(basename "$archive"))"
