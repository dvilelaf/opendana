#!/usr/bin/env bash
# C6.71 - level 1 (public tests, no ROM, no imported content) for Windows:
# cross-compiled with MinGW-w64 against the same SDL2 as the ZIP and run under
# wine64 in a private prefix.
#
#   bash packaging/windows-tests.sh
#
# Exit 0 only with 0 failures. A gate that cannot run is BLOCKED (exit 2),
# never green. What does not apply to Windows is printed at the end.
#
# Inputs (environment; nothing is downloaded or installed):
#   SDL2_SRC        SDL git tree holding OD_PIN_SDL_COMMIT (only if the SDL2 cache
#                   build/deps/sdl2-<ver>-windows is missing; packaging/build-sdl2.sh)
#   WINE64          optional wine64 loader
#   MINGW_PREFIX    default x86_64-w64-mingw32
# The wine prefix, HOME, TMPDIR, LOCALAPPDATA and APPDATA are private temporary
# directories; the user's own are never read or written.
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
cd "$OD_ROOT"

MINGW_PREFIX="${MINGW_PREFIX:-x86_64-w64-mingw32}"
CC_WIN="${MINGW_PREFIX}-gcc"
BUILD="${BUILD_DIR_WIN_TESTS:-$OD_BUILD_DIR/win-tests}"
wine64="${WINE64:-$(command -v wine64 || true)}"
[ -x "$CC_WIN" ] || command -v "$CC_WIN" >/dev/null 2>&1 \
    || { echo "BLOCKED: no $CC_WIN (apt-get install mingw-w64)" >&2; exit 2; }
[ -n "$wine64" ] || { echo 'BLOCKED: no wine64 loader (set WINE64)' >&2; exit 2; }
od_require_tools cmake sha256sum

step() { printf '\n== %s ==\n' "$*"; }

step "SDL2 for Windows (the ZIP's, from the pinned source)"
bash "$OD_ROOT/packaging/build-sdl2.sh" --target windows
SDL="$(cd "$OD_BUILD_DIR/deps/sdl2-$OD_PIN_SDL_VERSION-windows" && pwd)"
od_check_pin "$SDL/bin/SDL2.dll" "$OD_PIN_WINDOWS_SDL2_SHA256" "SDL2.dll"

step "configure + cross build (opendana_tests_public and the libraries)"
cmake -S . -B "$BUILD" -DCMAKE_TOOLCHAIN_FILE=packaging/mingw-w64.cmake \
    -DCMAKE_BUILD_TYPE=Release -DOD_BUILD_TESTS=ON -DMINGW_PREFIX="$MINGW_PREFIX" \
    -DMINGW_SDL2_DIR="$SDL" -DSDL2_DIR="$SDL/lib/cmake/SDL2" -DOD_SDL2_DLL="$SDL/bin/SDL2.dll" >/dev/null
cmake --build "$BUILD" -j"$(od_jobs)" --target opendana_tests_public opendana
BUILD="$(cd "$BUILD" && pwd)"
LIBS=("$BUILD/libopendana_ext.a" "$BUILD/libopendana_core.a")

work="$(mktemp -d "${TMPDIR:-/tmp}/opendana-win-tests.XXXXXX")"
work="$(cd "$work" && pwd)"
export WINEPREFIX="$work/wine" WINEDEBUG=-all WINEARCH=win64 HOME="$work/home"
mkdir -p "$HOME" "$work/bin"
cleanup() { wineserver -k 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT
cp "$SDL/bin/SDL2.dll" "$work/bin/"
cp "$BUILD/opendana_tests_public.exe" "$work/bin/"
cp "$BUILD/opendana.exe" "$work/bin/"

sdl_flags=(-I"$SDL/include/SDL2" -DSDL_MAIN_HANDLED)
cflags=(-std=c11 -Wall -Wextra -Werror -Werror=implicit-function-declaration -Iinclude)
fails=0 gates=0 blocked=0
declare -a summary=()

# gate NAME EXE [ARGS...]: run in a fresh private environment; the last line of
# the output is kept in the summary. Windows paths are Z:/... (wine's root).
gate() {
    local name="$1" exe="$2"; shift 2
    local t; t="${GATE_DIR:-$(mktemp -d "$work/gate.XXXXXX")}"
    mkdir -p "$t/local" "$t/roaming" "$t/tmp"
    gates=$((gates + 1))
    local rc=0
    ( cd "$OD_ROOT"
      TMPDIR="Z:$t/tmp" TEMP="Z:$t/tmp" TMP="Z:$t/tmp" \
      LOCALAPPDATA="Z:$t/local" APPDATA="Z:$t/roaming" USERPROFILE="Z:$t" \
      timeout 900 "$wine64" "$exe" "$@" ) > "$t/out.txt" 2>&1 || rc=$?
    tr -d '\r' < "$t/out.txt" > "$t/out.lf"
    cat "$t/out.lf"
    local last; last="$(grep -v '^$' "$t/out.lf" | tail -1)"
    if [ "$rc" -ne 0 ]; then
        fails=$((fails + 1)); summary+=("FAIL  $name (exit $rc): $last")
    else
        summary+=("OK    $name: $last")
    fi
    [ -n "${GATE_DIR:-}" ] || rm -rf "$t"
}

build_gate() {   # build_gate OUT.exe  cc-args...
    local out="$1"; shift
    "$CC_WIN" "$@" -o "$work/bin/$out" || { echo "FAIL: cannot build $out" >&2; fails=$((fails + 1)); summary+=("FAIL  build $out"); return 1; }
}

step "public unit tests (opendana_tests_public --category unit)"
# Two runs in the same private TMPDIR: test_ui_catalog.c exports a level only if the
# catalogue directory still holds entries (3 checks), as on a Linux machine that has
# run the suite before. The second, steady-state run is the one that counts.
GATE_DIR="$work/unit-tmp"; mkdir -p "$GATE_DIR"
gate "unit: model tests, first run (clean TMPDIR)" "$work/bin/opendana_tests_public.exe" --category unit
gate "unit: model tests (opendana_tests_public, steady state)" "$work/bin/opendana_tests_public.exe" --category unit
unset GATE_DIR

step "unit: editor/catalogue layout"
if build_gate editor-ui-layout.exe "${cflags[@]}" tests/editor-ui-layout.c \
    src/app/ui_editor.c src/app/ui_catalog.c src/app/ui_font.c src/app/ui_strings.c \
    "${LIBS[@]}" -lm -Wl,--wrap=od_ui_font_text; then
    gate "unit: editor/catalogue layout" "$work/bin/editor-ui-layout.exe"
fi

step "unit: interface screens"
if build_gate ui-screens-layout.exe "${cflags[@]}" "${sdl_flags[@]}" tests/ui-screens-layout.c \
    src/app/ui.c src/app/ui_import.c src/app/ui_editor.c src/app/ui_catalog.c \
    src/app/ui_font.c src/app/ui_strings.c "${LIBS[@]}" -lm -Wl,--wrap=od_ui_font_text; then
    gate "unit: interface screens" "$work/bin/ui-screens-layout.exe"
fi

step "unit: interface behaviour (no sanitizers on MinGW; UI-18 text scan runs in the Linux channel)"
if build_gate ui-behaviour.exe "${cflags[@]}" "${sdl_flags[@]}" tests/ui-behaviour.c \
    src/app/ui.c src/app/ui_editor.c src/app/ui_catalog.c src/app/ui_font.c \
    src/app/ui_strings.c src/ext/settings.c src/ext/input_map.c src/ext/leveled.c \
    src/ext/catalog.c src/render/png.c "${LIBS[@]}" -lm -Wl,--wrap=od_state_peek; then
    gate "unit: interface behaviour" "$work/bin/ui-behaviour.exe"
fi

step "unit: first-run screen layout"
if build_gate first-run-layout.exe "${cflags[@]}" -Iexternal/stb -Isrc/import tests/first-run-layout.c \
    src/app/ui_import_view.c src/app/ui_ttf.c src/app/ui_import.c src/app/ui_font.c \
    src/app/ui_strings.c "$BUILD/libopendana_core.a" -lm; then
    gate "unit: first-run screen layout" "$work/bin/first-run-layout.exe"
fi

step "unit: states across versions"
if build_gate state-compat.exe "${cflags[@]}" tests/state-compat.c "${LIBS[@]}" -lm \
    -Wl,--allow-multiple-definition; then
    gate "unit: states across versions" "$work/bin/state-compat.exe" "Z:$work/s.odstate"
fi

step "unit: cursor auto-hide timer (unit part)"
if build_gate cursor-hide.exe -std=c11 -Wall -Wextra -Werror tests/cursor-hide.c; then
    gate "unit: cursor auto-hide timer" "$work/bin/cursor-hide.exe"
fi

step "Windows files, paths and clock (tests/windows-*.c, session-userdir-win.c)"
if build_gate windows-fs.exe "${cflags[@]}" -D_DEFAULT_SOURCE -Isrc/import tests/windows-fs.c src/import/fsutil.c; then
    gate "windows: UTF-8 file helpers" "$work/bin/windows-fs.exe" "Z:$work"
fi
if build_gate windows-dir.exe "${cflags[@]}" -D_DEFAULT_SOURCE -Isrc/import tests/windows-import-dir.c "$BUILD/libopendana_core.a" -lm; then
    gate "windows: default import directory" "$work/bin/windows-dir.exe"
fi
if build_gate windows-clock.exe "${cflags[@]}" tests/windows-clock.c src/core/clock.c; then
    gate "windows: host clock" "$work/bin/windows-clock.exe"
fi
if build_gate paths.exe "${cflags[@]}" -DSESSION_HAVE_USERDIR -DSESSION_STUB_PROGRESS tests/session-userdir-win.c \
    src/core/userdir.c src/core/save.c src/core/common.c src/ext/settings.c src/import/fsutil.c -Isrc/import; then
    # Wine fills LOCALAPPDATA/APPDATA from its own profile (the private prefix):
    # the program prints what it sees and every path must lie under them.
    t="$work/paths"; mkdir -p "$t"; gates=$((gates + 1))
    env -u XDG_DATA_HOME -u XDG_CONFIG_HOME "$wine64" "$work/bin/paths.exe" 2>&1 | tr -d '\r' > "$t/out.txt" || true
    cat "$t/out.txt"
    local_dir="$(sed -n 's/^env-local //p' "$t/out.txt")"
    roam_dir="$(sed -n 's/^env-roaming //p' "$t/out.txt")"
    if [ -n "$local_dir" ] && [ "$local_dir" != "(unset)" ] && [ -n "$roam_dir" ] &&
       grep -qxF "save $local_dir/opendana/save.dat" "$t/out.txt" &&
       grep -qxF "settings $roam_dir/opendana/settings.ini" "$t/out.txt" &&
       grep -qxF "states $local_dir/opendana/states/slot-1.odstate" "$t/out.txt"; then
        summary+=("OK    windows: per-user directories under LOCALAPPDATA/APPDATA: 3 paths")
    else
        fails=$((fails + 1)); summary+=("FAIL  windows: per-user directories")
    fi
fi

step "ROM chooser owned by the game window (C6.76: wine64 on Xvfb)"
if ! command -v Xvfb >/dev/null 2>&1; then
    echo "BLOCKED: the chooser gate needs Xvfb (wine opens a real window)"
    fails=$((fails + 1)); summary+=("BLOCKED windows: ROM chooser owned by the game window (no Xvfb)")
elif build_gate picker-owner.exe "${cflags[@]}" -D_DEFAULT_SOURCE -Isrc/import tests/picker-owner-probe.c \
        src/app/rom_picker.c src/import/fsutil.c src/core/userdir.c -lcomdlg32 -lole32 -luser32; then
    xdisp="$work/xdisplay"
    Xvfb -displayfd 9 -screen 0 1280x1024x24 9>"$xdisp" >/dev/null 2>&1 &
    xvfb_pid=$!
    for _ in $(seq 100); do [ -s "$xdisp" ] && break; sleep 0.1; done
    DISPLAY=":$(head -1 "$xdisp" | tr -d '[:space:]')" \
        gate "windows: ROM chooser owned by the game window, game loop runs, cancel" "$work/bin/picker-owner.exe"
    kill "$xvfb_pid" 2>/dev/null || true
fi

step "first run (C6.79): the product starts its first-run screen under wine64"
# The screen's frame once lived on the stack (1.67 MiB of a 2 MiB reserve): the
# exe died with a stack overflow, which the layout gate cannot see because it
# does not run the binary. OD_WIN_FIRST_RUN_EXE points the gate at another exe (the
# positive control: the exe of 6ca6547 must fail here).
fr_exe="${OD_WIN_FIRST_RUN_EXE:-$work/bin/opendana.exe}"
fr_t="$work/first-run"; mkdir -p "$fr_t/home" "$fr_t/local" "$fr_t/roaming" "$fr_t/tmp"
gates=$((gates + 1)); fr_rc=0
( cd "$fr_t" && HOME="$fr_t/home" USERPROFILE="Z:$fr_t/home" LOCALAPPDATA="Z:$fr_t/local" APPDATA="Z:$fr_t/roaming" \
    TMPDIR="Z:$fr_t/tmp" TEMP="Z:$fr_t/tmp" SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    timeout 120 "$wine64" "$fr_exe" --first-run-shot "Z:$fr_t/first-run.png" ) > "$fr_t/out.txt" 2>&1 || fr_rc=$?
tr -d '\r' < "$fr_t/out.txt" | sed -n 1,5p
fr_dim="$(od -An -tu1 -j16 -N8 "$fr_t/first-run.png" 2>/dev/null | tr -s ' ' | sed 's/^ //' || true)"
# IHDR width/height as big-endian bytes: 0 0 1 0 (256) and 0 0 0 240.
if [ "$fr_rc" -eq 0 ] && [ "$fr_dim" = "0 0 1 0 0 0 0 240" ]; then
    summary+=("OK    windows: first-run screen captured by the exe (256x240 PNG, $(sha256sum < "$fr_t/first-run.png" | cut -c1-12))")
else
    fails=$((fails + 1)); summary+=("FAIL  windows: first-run screen (exit $fr_rc, PNG header '${fr_dim:-none}')")
fi

step "summary"
printf '%s\n' "${summary[@]}"
echo "windows-tests: $gates gates, $fails failures"
cat <<'EOF'
Not applicable on Windows (declared, not green): desktop-menu launcher
(Linux XDG), ROM chooser child (POSIX fork/PDEATHSIG), Linux package controls,
the Xvfb/xdotool part of the cursor gate, sanitizers (ASan/UBSan), and the
symbolic-link case UI-14 where the Windows runtime does not create links
(wine 6.0.3 accepts CreateSymbolicLink and creates nothing: reported as SKIP).
Platform-independent channels (Python contracts, mods CHR, text scans) run in
tests/run.sh --unit on Linux.
EOF
[ "$fails" -eq 0 ] || exit 1
