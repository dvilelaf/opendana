#!/usr/bin/env bash
# Standalone regression, intentionally independent of the shared suite.
set -euo pipefail
cd "$(dirname "$0")/.."
work="$(mktemp -d "${TMPDIR:-/tmp}/opendana-windows-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT
flags=(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror=implicit-function-declaration -I src/import -I include)
cc "${flags[@]}" tests/windows-fs.c src/import/fsutil.c -o "$work/native"
"$work/native" "$work"
cc "${flags[@]}" -ffunction-sections -fdata-sections tests/windows-import-dir.c src/import/import.c -Wl,--gc-sections -o "$work/native-dir"
"$work/native-dir"
cross="${MINGW_PREFIX:-x86_64-w64-mingw32}-gcc"
"$cross" "${flags[@]}" tests/windows-fs.c src/import/fsutil.c -o "$work/windows.exe"
"$cross" "${flags[@]}" -c src/import/import.c -o "$work/import.obj"
for source in src/import/*.c; do
    "$cross" "${flags[@]}" -I external/cJSON -I external/stb -I assets -c "$source" -o "$work/$(basename "$source").obj"
done
if [ -f "${BUILD_DIR_WIN:-build-win}/libopendana_core.a" ]; then
    "$cross" "${flags[@]}" tests/windows-import-dir.c "${BUILD_DIR_WIN:-build-win}/libopendana_core.a" -lm -o "$work/windows-dir.exe"
else
    echo 'SKIP Windows default-dir link: build the Windows core first'
fi
wine64="${WINE64:-}"
if [ -z "$wine64" ]; then
    if command -v wine64 >/dev/null; then wine64="$(command -v wine64)"
    elif [ -x /usr/lib/wine/wine64 ]; then wine64=/usr/lib/wine/wine64
    fi
fi
if [ -n "$wine64" ]; then
    # Private prefix: never alter the user's prefix or terminate other Wine jobs.
    export WINEPREFIX="$work/wine" WINEDEBUG=-all WINEARCH=win64
    # Wait for this prefix only before deleting it; no global wineserver/pkill.
    trap 'WINEPREFIX="$work/wine" wineserver -w; rm -rf "$work"' EXIT
    "$wine64" "$work/windows.exe" "Z:${work//\//\\}"
    if [ -f "$work/windows-dir.exe" ]; then "$wine64" "$work/windows-dir.exe"; fi
else
    echo 'SKIP Windows runtime: no 64-bit Wine loader (cross compilation passed)'
    if [ "${REQUIRE_WINE64:-0}" = 1 ]; then exit 1; fi
fi
