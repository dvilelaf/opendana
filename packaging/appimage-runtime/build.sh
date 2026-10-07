#!/usr/bin/env bash
# C6.50: reproducible build of the AppImage Type-2 runtime (own build, no binary copied
# from anywhere).
#
#   bash packaging/appimage-runtime/build.sh [--out DIR] [--sources DIR] [--fetch] [--twice]
#
#   --out DIR      where runtime-x86_64 and build-info.txt go (default build/appimage-runtime/out)
#   --sources DIR  cache of the source tarballs of sources.lock (default build/appimage-runtime/sources);
#                  every file is checked against its SHA-256 and a mismatch is fatal
#   --fetch        download the missing tarballs from the URLs of sources.lock (nothing else is
#                  ever downloaded; without --fetch a missing tarball is an error)
#   --sources-only only fetch (with --fetch) and verify the source tarballs, then stop
#   --twice        build twice from clean (two fresh containers, two fresh outputs) and require
#                  the same SHA-256; prints it. This is how the pin in packaging/pins.sh is made.
#
# The container has no network (--network none): the sources come in read-only. The image
# is built from the digest and apk versions of toolchain.lock (the apk step does need the
# network once; its result is cached by Docker).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
# Inside the repository the outputs go under build/; in the published source archive (no
# CMakeLists.txt two levels up) they go next to this script.
if [ -f "$root/CMakeLists.txt" ]; then base="$root/build/appimage-runtime"; else base="$here"; fi
out="$base/out" sources="$base/sources" fetch=0 twice=0 only=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --out) out="${2:?}"; shift 2 ;;
        --sources) sources="${2:?}"; shift 2 ;;
        --fetch) fetch=1; shift ;;
        --twice) twice=1; shift ;;
        --sources-only) only=1; shift ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done
die() { echo "error: $*" >&2; exit 1; }
[ "$only" = 1 ] || command -v docker >/dev/null || die "docker is required"
# shellcheck source=packaging/appimage-runtime/toolchain.lock
source "$here/toolchain.lock"
# The runtime is built once per source commit: a fixed epoch makes the bytes independent of the date.
epoch="${SOURCE_DATE_EPOCH:-1790553600}"   # 2026-09-28 00:00:00 UTC, the type2-runtime revision's day
mkdir -p "$sources"
sources="$(cd "$sources" && pwd)"

# --- sources: verify (and, with --fetch, download) against sources.lock
grep -v '^#' "$here/sources.lock" | while IFS=$'\t' read -r name version sha file url _licence; do
    [ -n "$name" ] || continue
    if [ ! -f "$sources/$file" ]; then
        [ "$fetch" = 1 ] || die "missing $sources/$file (use --fetch, or put the upstream tarball there)"
        echo "fetching $name $version"
        curl -fsSL --retry 3 -o "$sources/$file.part" "$url"
        mv -- "$sources/$file.part" "$sources/$file"
    fi
    actual="$(sha256sum "$sources/$file" | cut -d' ' -f1)"
    [ "$actual" = "$sha" ] || die "$file: SHA-256 mismatch (expected $sha, got $actual)"
done
cp -f -- "$here/sources.lock" "$sources/sources.lock"
[ "$only" = 0 ] || { echo "sources verified: $sources"; exit 0; }

# --- build image from the pinned digest and apk versions
tag="opendana-runtime-builder:$(sha256sum "$here/Dockerfile" "$here/toolchain.lock" | sha256sum | cut -c1-12)"
if ! docker image inspect "$tag" >/dev/null 2>&1; then
    docker build -q -t "$tag" -f "$here/Dockerfile" \
        --build-arg "OD_RT_IMAGE=$OD_RT_IMAGE" --build-arg "OD_RT_APK=$OD_RT_APK" "$here" >/dev/null
fi

build_once() { # outdir
    local dest="$1" name
    rm -rf "$dest"; mkdir -p "$dest"
    name="opendana-runtime-build-$$"
    docker run --rm --name "$name" --network none \
        -e "SOURCE_DATE_EPOCH=$epoch" \
        -v "$sources:/src:ro" -v "$here/container-build.sh:/container-build.sh:ro" -v "$dest:/out" \
        "$tag" bash /container-build.sh > "$dest/build.log" 2>&1 \
        || { tail -40 "$dest/build.log" >&2; die "container build failed (full log: $dest/build.log)"; }
}

if [ "$twice" = 1 ]; then
    build_once "$out.1"
    build_once "$out.2"
    h1="$(sha256sum "$out.1/runtime-x86_64" | cut -d' ' -f1)"
    h2="$(sha256sum "$out.2/runtime-x86_64" | cut -d' ' -f1)"
    echo "build 1: $h1"; echo "build 2: $h2"
    [ "$h1" = "$h2" ] || die "the two clean builds differ"
    rm -rf "$out"; mv "$out.1" "$out"; rm -rf "$out.2"
    echo "reproducible: $h1"
else
    build_once "$out"
    sha256sum "$out/runtime-x86_64"
fi
