#!/usr/bin/env bash
# C5.11 / C6.9 / C6.30 — Linux package of OpenDana.
#
#   bash packaging/build-linux.sh [--mode tar|appimage]
#
#   tar           (default) build/dist/opendana-<ver>-Linux-x86_64.tar.gz,
#                 its ROM audit report and SHA256SUMS.
#   appimage      the same TAR plus the publishable AppImage (C6.50) in the same
#                 output directory:
#                   opendana-<ver>-x86_64.AppImage            (+ .audit.json)
#                   opendana-<ver>-appimage-runtime-source.tar.xz   (LGPL-2.1 source
#                                                               and relink offer)
#                 It carries OUR runtime (packaging/appimage-runtime/, reproducible,
#                 pinned in packaging/pins.sh), a reproducible SquashFS, the licence
#                 bundle in `appimage` mode (libfuse and the other runtime components
#                 with notices and sources) and no test marker. The old
#                 `appimage-test` mode (local image with an unresolved runtime) is
#                 retired: this mode supersedes it.
#
# Inputs (environment, checked with clear errors; nothing is downloaded):
#   SDL2_DIR      optional: an SDL2 prefix whose lib/libSDL2-2.0.so.0 is the pinned
#                 build (packaging/pins.sh, OD_PIN_LINUX_SDL2_SHA256). By default
#                 packaging/build-sdl2.sh builds it from SDL2_SRC into build/deps/
#                 (C6.51: neutral paths, stripped, reproducible)
#   SDL2_SRC      SDL git tree containing OD_PIN_SDL_COMMIT (corresponding source)
#   OD_ROM       your EU ROM, read only: the ROM auditor checks every package
#   OD_APPIMAGE_RUNTIME  (appimage, optional) a built Type-2 runtime file whose SHA-256
#                 is OD_PIN_APPIMAGE_RUNTIME_SHA256. Without it the script uses the cached
#                 build/appimage-runtime/out/runtime-x86_64 if it matches the pin, or
#                 builds it with packaging/appimage-runtime/build.sh (needs Docker). A
#                 runtime that does not match the pin is never used.
#   OD_RUNTIME_SOURCES   (appimage) directory with the upstream tarballs of
#                 packaging/appimage-runtime/sources.lock (default
#                 build/appimage-runtime/sources); OD_RUNTIME_FETCH=1 downloads the
#                 missing ones from the lock's URLs (the only download of this script).
#   OD_SOURCE_COMMIT     optional; default HEAD (which must be clean)
#
# What it does: generates the corresponding sources with `git archive`
# (packaging/make-sources.sh), BUILDS FROM THAT ARCHIVE (so binary, `--version`
# commit and source correspond), installs the Runtime component into a fresh
# stage, binds notices/sources/relink materials with packaging/license-bundle.sh,
# writes a reproducible tar (sorted, root-owned, fixed mtime, modes u=rwX,go=rX:
# no umask dependence), audits the tar with packaging/audit-package.sh and only
# then moves it into build/dist. Failures never touch earlier outputs.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
cd "$OD_ROOT"

mode="${OD_LINUX_MODE:-tar}"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --mode) mode="${2:?--mode requires tar|appimage}"; shift 2 ;;
        --mode=*) mode="${1#--mode=}"; shift ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$mode" in
    tar|appimage) ;;
    appimage-test)
        echo "error: --mode appimage-test is retired (C6.50): --mode appimage builds the publishable image" >&2
        exit 2 ;;
    *) echo "error: --mode must be 'tar' or 'appimage' (got '$mode')" >&2; exit 2 ;;
esac

step() { printf '\n== %s ==\n' "$*"; }

step "preflight ($mode)"
od_require_env SDL2_SRC "the SDL git tree holding commit $OD_PIN_SDL_COMMIT"
od_audit_require_rom
od_require_tools git cmake cc tar gzip xz python3 sha256sum readelf
od_audit_check_rom
if [ -z "${SDL2_DIR:-}" ]; then
    bash "$OD_ROOT/packaging/build-sdl2.sh" --target linux
    SDL2_DIR="$OD_BUILD_DIR/deps/sdl2-$OD_PIN_SDL_VERSION-linux"
fi
SDL2_DIR="$(cd "$SDL2_DIR" && pwd)"
sdl_lib="$SDL2_DIR/lib/libSDL2-2.0.so.0"
[ -f "$sdl_lib" ] || od_die "SDL2_DIR=$SDL2_DIR has no lib/libSDL2-2.0.so.0"
od_check_pin "$(readlink -f "$sdl_lib")" "$OD_PIN_LINUX_SDL2_SHA256" "SDL2 shared library (SDL2_DIR)"
if [ "$mode" = appimage ]; then
    od_require_tools mksquashfs
    rt_sources="${OD_RUNTIME_SOURCES:-$OD_BUILD_DIR/appimage-runtime/sources}"
    rt_args=(--sources "$rt_sources")
    [ "${OD_RUNTIME_FETCH:-0}" != 1 ] || rt_args+=(--fetch)
    # The runtime: given, cached, or built. Whatever its origin, it must match the pin.
    runtime_file="${OD_APPIMAGE_RUNTIME:-$OD_BUILD_DIR/appimage-runtime/out/runtime-x86_64}"
    if [ -z "${OD_APPIMAGE_RUNTIME:-}" ] && ! [ -f "$runtime_file" ]; then
        echo "no cached runtime: building it (packaging/appimage-runtime/build.sh)"
        bash "$OD_ROOT/packaging/appimage-runtime/build.sh" "${rt_args[@]}" --out "$(dirname "$runtime_file")"
    elif [ -z "${OD_APPIMAGE_RUNTIME:-}" ]; then
        bash "$OD_ROOT/packaging/appimage-runtime/build.sh" "${rt_args[@]}" --sources-only
    fi
    [ -d "$rt_sources" ] || od_die "runtime sources not found: $rt_sources (set OD_RUNTIME_SOURCES or OD_RUNTIME_FETCH=1)"
    bash "$OD_ROOT/packaging/appimage-runtime-check.sh" "$runtime_file" "$OD_PIN_APPIMAGE_RUNTIME_SHA256"
fi
commit="$(od_source_commit)"
epoch="${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct "$commit")}"
echo "commit $commit (mtime $epoch)"

mkdir -p "$OD_BUILD_DIR"
work="$(mktemp -d "$OD_BUILD_DIR/package-linux.XXXXXX")"
work="$(cd "$work" && pwd)"

step "corresponding sources (git archive, ROM audit, SDL recipe)"
bash "$OD_ROOT/packaging/make-sources.sh" --commit "$commit" --platform linux --out "$work/sources"
src="$work/sources/src/opendana-$OD_VERSION"
materials="$work/sources/materials"

step "configure + build FROM the source archive"
cmake -S "$src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DOD_PACKAGE=ON -DOD_BUILD_TESTS=OFF \
    -DCMAKE_INSTALL_BINDIR=bin -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_PREFIX_PATH="$SDL2_DIR"
cmake --build "$work/build" -j"$(od_jobs)" --target opendana

tar_name="opendana-${OD_VERSION}-Linux-x86_64"
stage="$work/stage/$tar_name"
cmake --install "$work/build" --prefix "$stage" --component Runtime --strip
od_check_pin "$stage/lib/libSDL2-2.0.so.0" "$OD_PIN_LINUX_SDL2_SHA256" "staged SDL2"
[ "$(LC_ALL=C readelf -d "$stage/bin/opendana" | sed -n 's/.*Library runpath: \[\(.*\)\]/\1/p')" = '$ORIGIN/../lib' ] \
    || od_die "bin/opendana RUNPATH is not \$ORIGIN/../lib"
version_out="$(env -i HOME="$work" PATH=/usr/bin:/bin "$stage/bin/opendana" --version)"
echo "staged: $version_out"
[ "$version_out" = "opendana $OD_VERSION (commit $commit)" ] \
    || od_die "bin/opendana --version says '$version_out', expected commit $commit"

step "licence gate (notices, corresponding sources, relink, commit binding)"
toolchain="$(cc --version | head -1); $(ld --version | head -1); cmake $(cmake --version | head -1 | awk '{print $3}'); SDL2: GCC Ubuntu 11.4.0-1ubuntu1~22.04.3 (see relink materials)"
flags="cmake -DCMAKE_BUILD_TYPE=Release -DOD_PACKAGE=ON -DOD_BUILD_TESTS=OFF -DCMAKE_INSTALL_BINDIR=bin -DCMAKE_INSTALL_LIBDIR=lib (C flags -O3 -DNDEBUG, RUNPATH \$ORIGIN/../lib); SDL2: sources/relink-materials.tar.xz flags.txt"
manifest_sha="$(bash "$OD_ROOT/packaging/write-manifest.sh" --materials "$materials" --platform linux \
    --commit "$commit" --source-info "$work/sources/application-source.json" \
    --application "$stage/bin/opendana" --sdl2-sha256 "$OD_PIN_LINUX_SDL2_SHA256" \
    --toolchain "$toolchain" --flags "$flags")"
bash "$OD_ROOT/packaging/license-bundle.sh" --manifest "$materials/manifest.json" --sha256 "$manifest_sha" \
    --mode tar --artifact "application=$stage/bin/opendana" --artifact "SDL2=$stage/lib/libSDL2-2.0.so.0" \
    --output "$stage/share/doc/opendana/license-bundle"

step "reproducible tar"
out="$work/out"
mkdir "$out"
od_det_tar "$work/stage" "$tar_name" "$out/$tar_name.tar" "$epoch"
od_gzip "$out/$tar_name.tar" "$out/$tar_name.tar.gz"
rm "$out/$tar_name.tar"

step "ROM audit of the tar (including its source archives)"
od_rom_audit --rom "${OD_ROM:-}" --package "$out/$tar_name.tar.gz" \
    --report "$out/$tar_name.audit.json" \
    --provenance "SDL2-$OD_PIN_SDL_VERSION/=$SDL2_SRC@$OD_PIN_SDL_COMMIT"

if [ "$mode" = appimage ]; then
    step "AppImage runtime materials (notices, sources, relink, LGPL source offer)"
    cp -a "$materials" "$work/materials-appimage"
    runtime_source="opendana-${OD_VERSION}-appimage-runtime-source.tar.xz"
    bash "$OD_ROOT/packaging/appimage-runtime/materials.sh" --sources "$rt_sources" --app "$src" \
        --materials "$work/materials-appimage" --source-archive "$out/$runtime_source" --epoch "$epoch"
    rt_toolchain="runtime: $(. "$OD_ROOT/packaging/appimage-runtime/toolchain.lock"; echo "image $OD_RT_IMAGE (Alpine $OD_RT_IMAGE_RELEASE), apk $OD_RT_APK")"
    rt_flags="runtime: packaging/appimage-runtime/container-build.sh (clang -Os -static-pie, -ffile-prefix-map, --build-id=none, strip), sources.lock, SOURCE_DATE_EPOCH"
    manifest_sha_app="$(bash "$OD_ROOT/packaging/write-manifest.sh" --materials "$work/materials-appimage" --platform linux \
        --commit "$commit" --source-info "$work/sources/application-source.json" \
        --application "$stage/bin/opendana" --sdl2-sha256 "$OD_PIN_LINUX_SDL2_SHA256" \
        --toolchain "$toolchain; $rt_toolchain" --flags "$flags; $rt_flags" \
        --runtime-lock "$src/packaging/appimage-runtime/sources.lock" --runtime-sha256 "$OD_PIN_APPIMAGE_RUNTIME_SHA256")"

    step "AppImage (our runtime, reproducible SquashFS)"
    appdir="$work/AppDir"
    mkdir -p "$appdir/usr"
    cp -a "$stage/." "$appdir/usr/"
    rm -rf "$appdir/usr/share/doc/opendana/license-bundle"
    bash "$OD_ROOT/packaging/license-bundle.sh" --manifest "$work/materials-appimage/manifest.json" \
        --sha256 "$manifest_sha_app" --mode appimage --artifact "application=$stage/bin/opendana" \
        --artifact "SDL2=$stage/lib/libSDL2-2.0.so.0" --artifact "type2-runtime=$runtime_file" \
        --output "$appdir/usr/share/doc/opendana/license-bundle"
    cp -- "$OD_ROOT/packaging/linux/opendana.desktop" "$appdir/opendana.desktop"
    cp -- "$OD_ROOT/packaging/linux/opendana.png" "$appdir/opendana.png"
    ln -s opendana.png "$appdir/.DirIcon"
    cat > "$appdir/AppRun" <<'SH'
#!/bin/sh
appdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$appdir/usr/bin/opendana" "$@"
SH
    chmod 755 "$appdir/AppRun"
    chmod -R u=rwX,go=rX "$appdir"
    image="opendana-${OD_VERSION}-x86_64.AppImage"
    mksquashfs "$appdir" "$work/image.squashfs" -noappend -all-root -no-xattrs -comp gzip \
        -mkfs-time "$epoch" -all-time "$epoch" -processors 1 >/dev/null
    cat "$runtime_file" "$work/image.squashfs" > "$out/$image"
    chmod 755 "$out/$image"
    step "ROM audit of the AppImage tree (AppDir, runtime, runtime source archive)"
    mkdir "$work/appimage-audit"
    cp -a "$appdir" "$work/appimage-audit/AppDir"
    cp -- "$runtime_file" "$work/appimage-audit/runtime-x86_64"
    cp -- "$out/$runtime_source" "$work/appimage-audit/$runtime_source"
    od_rom_audit --rom "${OD_ROM:-}" --tree "$work/appimage-audit" \
        --report "$out/$image.audit.json" \
        --provenance "SDL2-$OD_PIN_SDL_VERSION/=$SDL2_SRC@$OD_PIN_SDL_COMMIT"
fi

step "checksums and publication into $OD_DIST_DIR"
( cd "$out" && sha256sum -- "$tar_name.tar.gz" "$tar_name.audit.json" > SHA256SUMS && sha256sum -c SHA256SUMS )
mkdir -p "$OD_DIST_DIR"
mv -- "$out/$tar_name.tar.gz" "$out/$tar_name.audit.json" "$OD_DIST_DIR/"
mv -- "$out/SHA256SUMS" "$OD_DIST_DIR/SHA256SUMS"
cat "$OD_DIST_DIR/SHA256SUMS"
if [ "$mode" = appimage ]; then
    ( cd "$out" && sha256sum -- "$image" "$image.audit.json" "$runtime_source" >> "$OD_DIST_DIR/SHA256SUMS" )
    mv -- "$out/$image" "$out/$image.audit.json" "$out/$runtime_source" "$OD_DIST_DIR/"
    ( cd "$OD_DIST_DIR" && sha256sum -c SHA256SUMS )
    echo "AppImage: $OD_DIST_DIR/$image"
fi
if [ "${OD_KEEP_WORK:-0}" != 1 ]; then rm -rf "$work"; fi
step "result ($mode, commit $commit)"
ls -l "$OD_DIST_DIR"
