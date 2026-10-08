#!/usr/bin/env bash
# C6.76 (level 1, Linux): the ROM chooser opens on top of the game window, as its child, and the game
# is NOT minimised. Real SDL window under Xvfb, doubles of zenity and kdialog that record their arguments.
#
#   1. zenity present: the chooser gets --attach=<X11 id of the game window> and --modal, the front end
#      does not minimise the window, and it keeps drawing while the dialog is open (the status line it
#      draws changes the window's pixels);
#   2. only kdialog present: --attach <id> (kdialog has no --modal option);
#   3. a tool that rejects --attach (exits 255 at once): started again without it and the front end
#      says so and minimises the window, the last resort;
#   4. with the real zenity, if it is installed: the dialog opens, is modal and the window is not
#      minimised; its WM_TRANSIENT_FOR must be the game window when the tool sets one (zenity 3.42
#      sets none for --file-selection; the test says so).
# The key that opens the chooser (Enter) is sent to OUR Xvfb server only. No ROM needed.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" = Darwin ]; then
    echo "skipped: Linux only (Xvfb, zenity/kdialog). On macOS the chooser is osascript: tests/import-picker-child.sh"
    echo "checks its arguments, and the real dialog over the game window is looked at on a Mac."
    exit 0
fi
for tool in Xvfb xdotool xwd xprop xwininfo; do
    command -v "$tool" >/dev/null 2>&1 || { echo "BLOCKED: $tool is needed for the Xvfb test" >&2; exit 2; }
done
b="${BUILD_DIR:-build}"
bin="$(pwd)/$b/opendana"
[ -x "$bin" ] || { echo "BLOCKED: no $bin (run build.sh)" >&2; exit 2; }

tmp="$(mktemp -d "${TMPDIR:-/tmp}/opendana-picker-attach.XXXXXX")"
xpid="" gpid=""
cleanup() {
    [ -n "$gpid" ] && kill "$gpid" 2>/dev/null || true
    pkill -f "$tmp/bin" 2>/dev/null || true
    [ -n "$xpid" ] && kill "$xpid" 2>/dev/null || true
    rm -rf "$tmp"
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
cases=0
ok() { cases=$((cases + 1)); echo "ok $cases - $1"; }

Xvfb -displayfd 9 -screen 0 1280x1024x24 9>"$tmp/disp" >/dev/null 2>&1 &
xpid=$!
for _ in $(seq 100); do [ -s "$tmp/disp" ] && break; sleep 0.1; done
export DISPLAY=":$(head -1 "$tmp/disp" | tr -d '[:space:]')"
export SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11
export HOME="$tmp/home" XDG_DATA_HOME="$tmp/data" XDG_CONFIG_HOME="$tmp/config"
mkdir -p "$HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"

# A double: records its arguments, one per line, in $FAKE_LOG (appending one block per run), then
# stays open like a dialog. REJECT_ATTACH=1: exit 255 at once if an argument is an --attach option.
make_double() { # name dir
    mkdir -p "$2"
    cat > "$2/$1" <<'DOUBLE'
#!/bin/sh
{ echo "== run"; for a in "$@"; do echo "$a"; done; } >> "$FAKE_LOG"
if [ "${REJECT_ATTACH:-0}" = 1 ]; then
    for a in "$@"; do case "$a" in --attach*) echo "unknown option" >&2; exit 255 ;; esac; done
fi
: > "$FAKE_READY"
exec sleep 300
DOUBLE
    chmod +x "$2/$1"
    ln -sf "$(command -v sleep)" "$2/sleep"
}

game_window() { xdotool search --onlyvisible --name "^OpenDana" 2>/dev/null | head -1 || true; }
shot_md5() { xwd -display "$DISPLAY" -id "$1" 2>/dev/null | md5sum | cut -d' ' -f1; }

# run_game DIR_OF_DOUBLES [REJECT]: starts the first-run screen, presses Enter, waits for the chooser.
run_game() {
    local dir="$1"
    rm -f "$tmp/log" "$tmp/ready" "$tmp/game.err"
    : > "$tmp/log"
    FAKE_LOG="$tmp/log" FAKE_READY="$tmp/ready" REJECT_ATTACH="${2:-0}" PATH="$dir" \
        "$bin" --first-run >"$tmp/game.out" 2>"$tmp/game.err" &
    gpid=$!
    wid=""
    for _ in $(seq 100); do wid="$(game_window)"; [ -n "$wid" ] && break; sleep 0.1; done
    [ -n "$wid" ] || fail "the first-run window never appeared: $(cat "$tmp/game.err")"
    sleep 2
    before="$(shot_md5 "$wid")"
    # Enter opens the chooser; the first key can arrive before the event loop runs: try again
    for _ in 1 2 3 4 5; do
        xdotool windowfocus "$wid" 2>/dev/null || true
        xdotool key --window "$wid" Return
        for _ in $(seq 20); do [ -f "$tmp/ready" ] && break; sleep 0.1; done
        [ -f "$tmp/ready" ] && break
    done
    [ -f "$tmp/ready" ] || fail "the chooser double never started (Enter did not open it): $(cat "$tmp/game.err")"
    sleep 1
}
stop_game() {
    kill "$gpid" 2>/dev/null || true
    wait "$gpid" 2>/dev/null || true
    gpid=""
    pkill -f "$tmp/bin" 2>/dev/null || true
    sleep 0.3
}

echo "== the chooser is a child of the game window, the game is not minimised =="
make_double zenity "$tmp/bin"
run_game "$tmp/bin"
id_dec="$(printf '%d' "$wid")"
grep -qx -- "--attach=$id_dec" "$tmp/log" || fail "zenity did not get --attach=$id_dec: $(tr '\n' ' ' < "$tmp/log")"
grep -qx -- "--modal" "$tmp/log" || fail "zenity did not get --modal"
[ "$(grep -c '^== run' "$tmp/log")" = 1 ] || fail "the chooser started more than once"
! grep -q "minimising" "$tmp/game.err" || fail "the window was minimised although the chooser attached: $(cat "$tmp/game.err")"
after="$(shot_md5 "$wid")"
[ "$before" != "$after" ] || fail "the window did not change while the chooser was open: the game stopped drawing"
ok "zenity gets --attach=$id_dec and --modal; no minimising; the window kept drawing (its pixels changed)"
stop_game

echo "== only kdialog =="
make_double kdialog "$tmp/binK"
run_game "$tmp/binK"
id_dec="$(printf '%d' "$wid")"
grep -qx -- "--attach" "$tmp/log" && grep -qx -- "$id_dec" "$tmp/log" || fail "kdialog did not get --attach $id_dec: $(tr '\n' ' ' < "$tmp/log")"
! grep -qx -- "--modal" "$tmp/log" || fail "kdialog got --modal, which it does not have"
! grep -q "minimising" "$tmp/game.err" || fail "the window was minimised although kdialog attached"
ok "kdialog gets --attach $id_dec; no minimising"
stop_game

echo "== a tool that rejects --attach =="
run_game "$tmp/bin" 1
[ "$(grep -c '^== run' "$tmp/log")" = 2 ] || fail "expected two runs (with and without --attach): $(tr '\n' ' ' < "$tmp/log")"
second="$(awk '/^== run/{n++} n==2' "$tmp/log")"
! grep -q -- "--attach" <<<"$second" || fail "the second run still had --attach"
grep -q "minimising it (last resort)" "$tmp/game.err" || fail "the window was not minimised as a last resort: $(cat "$tmp/game.err")"
ok "the tool refused --attach: started again without it, and the window was minimised (last resort), saying so"
stop_game

if command -v zenity >/dev/null 2>&1 && [ -z "${OD_NO_REAL_ZENITY:-}" ]; then
    echo "== the real zenity =="
    : > "$tmp/log"
    PATH="/usr/bin:/bin" GTK_USE_PORTAL=0 "$bin" --first-run >"$tmp/game.out" 2>"$tmp/game.err" &
    gpid=$!
    wid=""
    for _ in $(seq 100); do wid="$(game_window)"; [ -n "$wid" ] && break; sleep 0.1; done
    [ -n "$wid" ] || fail "no window for the real zenity case"
    sleep 2
    xdotool windowfocus "$wid" 2>/dev/null || true
    xdotool key --window "$wid" Return
    dlg=""
    for _ in $(seq 150); do
        for cand in $(xdotool search --name 'Select your' 2>/dev/null); do dlg="$cand"; done
        [ -n "$dlg" ] && break
        sleep 0.1
    done
    [ -n "$dlg" ] || fail "the real zenity dialog never appeared: $(cat "$tmp/game.err")"
    st="$(xprop -id "$dlg" _NET_WM_STATE 2>/dev/null || true)"
    case "$st" in *_NET_WM_STATE_MODAL*) ;; *) fail "the real zenity dialog is not modal: '$st'" ;; esac
    tf="$(xprop -id "$dlg" WM_TRANSIENT_FOR 2>/dev/null || true)"
    want="$(printf '0x%x' "$wid")"
    case "$tf" in
        *"$want"*) hint="WM_TRANSIENT_FOR is the game window ($want)" ;;
        *"not found"*) hint="NOTE this zenity ($(zenity --version)) accepts --attach but sets no WM_TRANSIENT_FOR on --file-selection (it does on --info): a modal top-level" ;;
        *) fail "WM_TRANSIENT_FOR of the real zenity is '$tf', not the game window $want" ;;
    esac
    ! grep -q "minimising" "$tmp/game.err" || fail "the window was minimised with the real zenity"
    ok "real zenity: dialog open and modal, game window not minimised; $hint"
    stop_game
    pkill -x zenity 2>/dev/null || true
else
    echo "skipped 4 - the real zenity is not installed (or OD_NO_REAL_ZENITY is set); the doubles above cover the arguments"
fi
echo "picker-attach: $cases cases OK"
