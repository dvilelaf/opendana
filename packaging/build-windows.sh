#!/usr/bin/env bash
# C5.11 / C6.30 — Windows package of OpenDana, cross-compiled
# from Linux with MinGW-w64 (docs/PLAN-V2.md §9.2).
#
#   bash packaging/build-windows.sh
#
# Produces build/dist-win/opendana-<ver>-Windows-x86_64.zip (bin/opendana.exe,
# bin/SDL2.dll, README.md, LICENSE, NOTICE and the licence bundle with every
# notice that applies to the two executables and their corresponding sources),
# its ROM audit report and SHA256SUMS. Same gates as the Linux TAR: built FROM
# the `git archive` source, commit binding, licence gate, reproducible archive,
# ROM auditor; a failure never touches earlier outputs.
#
# Inputs (environment; nothing is downloaded or installed):
#   MINGW_SDL2_DIR  optional: SDL2 MinGW x86_64 prefix (bin/SDL2.dll pinned by
#                   OD_PIN_WINDOWS_SDL2_SHA256, lib/cmake/SDL2/SDL2Config.cmake).
#                   By default packaging/build-sdl2.sh builds it from SDL2_SRC into
#                   build/deps/ (C6.51: neutral paths, stripped, reproducible)
#   SDL2_SRC        SDL git tree containing OD_PIN_SDL_COMMIT
#   OD_ROM         your EU ROM, read only, for the ROM auditor
#   WINE64          optional wine64 loader for the smoke test (REQUIRE_WINE64=1
#                   makes its absence an error)
#   OD_SOURCE_COMMIT optional; default HEAD (which must be clean)
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
cd "$OD_ROOT"

BUILD="${BUILD_DIR_WIN:-build/win}"  # inside the ignored build/ tree
DIST="$(od_abs "${DIST_DIR_WIN:-build/dist-win}")"
MINGW_PREFIX="${MINGW_PREFIX:-x86_64-w64-mingw32}"
# The MinGW-w64 runtime whose linked members notices/windows/mingw-w64-runtime.txt covers.
MINGW_RUNTIME_EXPECTED="mingw-w64-x86-64-dev 8.0.0-1, gcc-mingw-w64-x86-64-win32 10.3.0-14ubuntu1+24.3"

step() { printf '\n== %s ==\n' "$*"; }

step "preflight"
if ! command -v "${MINGW_PREFIX}-gcc" >/dev/null 2>&1; then
    od_die "MinGW-w64 toolchain not found ('${MINGW_PREFIX}-gcc'); install mingw-w64 (e.g. apt-get install mingw-w64)"
fi
od_require_env SDL2_SRC "the SDL git tree holding commit $OD_PIN_SDL_COMMIT"
od_audit_require_rom
od_require_tools git cmake tar xz python3 sha256sum "${MINGW_PREFIX}-objdump" "${MINGW_PREFIX}-strip"
od_audit_check_rom
if [ -z "${MINGW_SDL2_DIR:-}" ]; then
    bash "$OD_ROOT/packaging/build-sdl2.sh" --target windows
    MINGW_SDL2_DIR="$OD_BUILD_DIR/deps/sdl2-$OD_PIN_SDL_VERSION-windows"
fi
MINGW_SDL2_DIR="$(cd "$MINGW_SDL2_DIR" && pwd)"
[ -f "$MINGW_SDL2_DIR/bin/SDL2.dll" ] && [ -f "$MINGW_SDL2_DIR/lib/cmake/SDL2/SDL2Config.cmake" ] \
    || od_die "MINGW_SDL2_DIR must contain bin/SDL2.dll and lib/cmake/SDL2/SDL2Config.cmake"
od_check_pin "$MINGW_SDL2_DIR/bin/SDL2.dll" "$OD_PIN_WINDOWS_SDL2_SHA256" "SDL2.dll (MINGW_SDL2_DIR)"
mingw_runtime="${OD_MINGW_RUNTIME_VERSION:-}"
if [ -z "$mingw_runtime" ]; then
    command -v dpkg-query >/dev/null 2>&1 \
        || od_die "cannot identify the MinGW-w64 runtime (no dpkg-query); set OD_MINGW_RUNTIME_VERSION to '$MINGW_RUNTIME_EXPECTED' only after checking it"
    mingw_runtime="mingw-w64-x86-64-dev $(dpkg-query -W -f='${Version}' mingw-w64-x86-64-dev), gcc-mingw-w64-x86-64-win32 $(dpkg-query -W -f='${Version}' gcc-mingw-w64-x86-64-win32)"
fi
[ "$mingw_runtime" = "$MINGW_RUNTIME_EXPECTED" ] \
    || od_die "MinGW-w64 runtime is '$mingw_runtime', but notices/windows/mingw-w64-runtime.txt was reviewed for '$MINGW_RUNTIME_EXPECTED'"
commit="$(od_source_commit)"
epoch="${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct "$commit")}"
echo "commit $commit (mtime $epoch)"

mkdir -p "$BUILD"
work="$(mktemp -d "$BUILD/package-windows.XXXXXX")"
work="$(cd "$work" && pwd)"

step "corresponding sources (git archive, ROM audit, SDL recipe)"
bash "$OD_ROOT/packaging/make-sources.sh" --commit "$commit" --platform windows --out "$work/sources"
src="$work/sources/src/opendana-$OD_VERSION"
materials="$work/sources/materials"

step "configure + cross build FROM the source archive ($OD_VERSION)"
cmake -S "$src" -B "$work/build" \
    -DCMAKE_TOOLCHAIN_FILE="$src/packaging/mingw-w64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DOD_PACKAGE=ON -DOD_BUILD_TESTS=OFF \
    -DMINGW_PREFIX="$MINGW_PREFIX" -DMINGW_SDL2_DIR="$MINGW_SDL2_DIR" \
    -DSDL2_DIR="$MINGW_SDL2_DIR/lib/cmake/SDL2" -DOD_SDL2_DLL="$MINGW_SDL2_DIR/bin/SDL2.dll"
cmake --build "$work/build" -j"$(od_jobs)" --target opendana

zip_name="opendana-${OD_VERSION}-Windows-x86_64"
stage="$work/stage/$zip_name"
cmake --install "$work/build" --prefix "$stage" --component Runtime --strip
od_check_pin "$stage/bin/SDL2.dll" "$OD_PIN_WINDOWS_SDL2_SHA256" "staged SDL2.dll"
grep -aqF "(commit $commit)" "$stage/bin/opendana.exe" || od_die "opendana.exe does not contain (commit $commit)"

step "licence gate (notices incl. MinGW-w64 runtime, corresponding sources, commit binding)"
toolchain="$("${MINGW_PREFIX}-gcc" --version | head -1); $("${MINGW_PREFIX}-ld" --version | head -1); $mingw_runtime"
flags="cmake -DCMAKE_TOOLCHAIN_FILE=packaging/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release -DOD_PACKAGE=ON -DOD_BUILD_TESTS=OFF (C flags -Werror=implicit-function-declaration -O3 -DNDEBUG, link -Wl,--no-insert-timestamp); SDL2.dll: sources/relink-materials.tar.xz flags.txt"
manifest_sha="$(bash "$OD_ROOT/packaging/write-manifest.sh" --materials "$materials" --platform windows \
    --commit "$commit" --source-info "$work/sources/application-source.json" \
    --application "$stage/bin/opendana.exe" --sdl2-sha256 "$OD_PIN_WINDOWS_SDL2_SHA256" \
    --toolchain "$toolchain" --flags "$flags" --mingw-version "$mingw_runtime")"
bash "$OD_ROOT/packaging/license-bundle.sh" --manifest "$materials/manifest.json" --sha256 "$manifest_sha" \
    --mode windows --artifact "application=$stage/bin/opendana.exe" --artifact "SDL2=$stage/bin/SDL2.dll" \
    --output "$stage/share/doc/opendana/license-bundle"

step "reproducible zip"
out="$work/out"
mkdir "$out"
python3 - "$work/stage" "$zip_name" "$out/$zip_name.zip" "$epoch" <<'PY'
import os, sys, time, zipfile
from pathlib import Path
root, top, target, epoch = Path(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4])
stamp = max(time.gmtime(epoch)[:6], (1980, 1, 1, 0, 0, 0))
paths = sorted(p for p in (root / top).rglob('*'))
with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in [root / top] + paths:
        rel = path.relative_to(root).as_posix()
        if path.is_symlink():
            sys.exit('error: symlink in Windows stage: ' + rel)
        if path.is_dir():
            info = zipfile.ZipInfo(rel + '/', stamp); info.external_attr = (0o40755 << 16) | 0x10
            info.create_system = 3
            archive.writestr(info, b'')
            continue
        info = zipfile.ZipInfo(rel, stamp)
        info.create_system = 3
        info.external_attr = (0o100755 if os.access(path, os.X_OK) or rel.endswith(('.exe', '.dll')) else 0o100644) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(info, path.read_bytes(), compresslevel=9)
PY
bash "$OD_ROOT/tests/windows-package.sh" "$out/$zip_name.zip" "$MINGW_SDL2_DIR/bin/SDL2.dll" "$commit"

step "ROM audit of the zip (including its source archives)"
od_rom_audit --rom "${OD_ROM:-}" --package "$out/$zip_name.zip" \
    --report "$out/$zip_name.audit.json" \
    --provenance "SDL2-$OD_PIN_SDL_VERSION/=$SDL2_SRC@$OD_PIN_SDL_COMMIT"

step "checksums"
( cd "$out" && sha256sum -- "$zip_name.zip" "$zip_name.audit.json" > SHA256SUMS && sha256sum -c SHA256SUMS )

wine64="${WINE64:-}"
if [ -z "$wine64" ] && command -v wine64 >/dev/null 2>&1; then wine64="$(command -v wine64)"; fi
if [ -n "$wine64" ]; then
    step "wine64 smoke test (--version)"
    smoke="$work/wine"
    mkdir -p "$smoke/unpacked"
    python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
        "$out/$zip_name.zip" "$smoke/unpacked"
    got="$(WINEPREFIX="$smoke/prefix" WINEARCH=win64 WINEDEBUG=-all HOME="$smoke" \
        "$wine64" "$smoke/unpacked/$zip_name/bin/opendana.exe" --version | tr -d '\r')"
    WINEPREFIX="$smoke/prefix" wineserver -k 2>/dev/null || true
    echo "wine64: $got"
    [ "$got" = "opendana $OD_VERSION (commit $commit)" ] || od_die "opendana.exe --version under wine64 says '$got'"
else
    echo 'SKIP Windows runtime smoke: no 64-bit Wine loader; runtime acceptance pending'
    if [ "${REQUIRE_WINE64:-0}" = 1 ]; then exit 1; fi
fi

step "publication into $DIST"
mkdir -p "$DIST"
mv -- "$out/$zip_name.zip" "$out/$zip_name.audit.json" "$DIST/"
mv -- "$out/SHA256SUMS" "$DIST/SHA256SUMS"
cat "$DIST/SHA256SUMS"
if [ "${OD_KEEP_WORK:-0}" != 1 ]; then rm -rf "$work"; fi
step "result (commit $commit)"
ls -l "$DIST"
