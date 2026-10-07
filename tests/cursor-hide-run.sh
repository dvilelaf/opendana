#!/usr/bin/env bash
# cursor auto-hide. Unit part always; with OD_CURSOR_BIN and
# OD_CURSOR_CONTENT (an imported content dir) it also runs the real window under
# Xvfb: in fullscreen the cursor hides after 3 s and returns on xdotool mousemove;
# in a window it never hides. Positive control: run it with OD_CURSOR_BIN set to
# a binary built before the feature; the log never says "cursor: hidden" and it fails.
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d "${TMPDIR:-/tmp}/cursor-hide.XXXXXX")"
xp=""; app=""
cleanup() { [ -n "$app" ] && kill "$app" 2>/dev/null || true; [ -n "$xp" ] && kill "$xp" 2>/dev/null || true; rm -rf "$out"; }
trap cleanup EXIT
cc -std=c11 -Wall -Wextra -Werror tests/cursor-hide.c -o "$out/cursor-hide"
"$out/cursor-hide"
[ -n "${OD_CURSOR_BIN:-}" ] || exit 0
bin=$(realpath "$OD_CURSOR_BIN"); content=$(realpath "${OD_CURSOR_CONTENT:?content dir}")
Xvfb -displayfd 9 -screen 0 1280x1024x24 9>"$out/disp" >/dev/null 2>&1 &
xp=$!
for _ in $(seq 50); do [ -s "$out/disp" ] && break; sleep 0.1; done
export DISPLAY=":$(head -1 "$out/disp" | tr -d '[:space:]')" SDL_AUDIODRIVER=dummy
export HOME="$out/home" XDG_DATA_HOME="$out/data" XDG_CONFIG_HOME="$out/config"
mkdir -p "$HOME"
fail() { echo "FAIL: $*"; exit 1; }
seen() { grep -q -- "$1" "$out/$2.log"; }
run() {   # run NAME FLAGS... ; sets $app
    "$bin" --content "$content" --scale 2 "${@:2}" >"$out/$1.log" 2>&1 &
    app=$!
    sleep 1.5
}
stop() { kill "$app" 2>/dev/null || true; wait "$app" 2>/dev/null || true; app=""; }
# Fullscreen: visible at first, hidden after 3 s, visible after a motion, hidden again.
run fs --fullscreen
seen "cursor: hidden" fs && fail "hidden before 3 s"
sleep 3
seen "cursor: hidden" fs || fail "cursor not hidden after 3 s of stillness (fullscreen)"
xdotool mousemove 100 100; sleep 0.5
seen "cursor: visible" fs || fail "cursor not shown after mouse motion"
sleep 3.2
[ "$(grep -c "cursor: hidden" "$out/fs.log")" -ge 2 ] || fail "cursor not hidden again after motion stopped"
stop
# Window: nothing changes.
run win
sleep 4
seen "cursor: hidden" win && fail "cursor hidden in a window"
stop
echo "PASS: cursor hides after 3 s in fullscreen, returns on motion, untouched in a window"
