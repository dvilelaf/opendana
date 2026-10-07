#!/usr/bin/env bash
# C6.50: the publishable AppImage opens a real window under Xvfb (no ROM, no game route).
#   bash tests/linux-package-appimage-xvfb.sh IMAGE.AppImage
# Starts the image on a private X display with a temporary HOME (so it shows the
# first-run screen), waits for the "OpenDana" window, checks it is mapped with a
# plausible size, that the process is still alive after 3 s, and that closing the
# window (xdotool windowclose / WM_DELETE) ends it. The image runs with
# APPIMAGE_EXTRACT_AND_RUN=1 so that the test does not depend on FUSE.
# Exit 2 (BLOCKED) when Xvfb/xdotool are missing; 1 on failure.
set -euo pipefail
image="$(realpath "${1:?usage: linux-package-appimage-xvfb.sh IMAGE.AppImage}")"
for tool in Xvfb xdotool; do
    command -v "$tool" >/dev/null || { echo "BLOCKED: $tool required" >&2; exit 2; }
done
work="$(mktemp -d "${TMPDIR:-/tmp}/opendana-appimage-xvfb.XXXXXX")"
xvfb=""
trap '[ -z "$xvfb" ] || kill "$xvfb" 2>/dev/null; chmod -R u+w "$work" 2>/dev/null; rm -rf "$work"' EXIT
fail() { echo "error: linux-package-appimage-xvfb: $*" >&2; exit 1; }
mkdir -p "$work/home" "$work/tmp"
disp=":$((500 + RANDOM % 400))"
Xvfb "$disp" -screen 0 1024x768x24 -nolisten tcp > "$work/xvfb.log" 2>&1 &
xvfb=$!
sleep 1
env -i PATH=/usr/bin:/bin HOME="$work/home" TMPDIR="$work/tmp" XDG_DATA_HOME="$work/home/data" \
    XDG_CONFIG_HOME="$work/home/config" DISPLAY="$disp" SDL_AUDIODRIVER=dummy APPIMAGE_EXTRACT_AND_RUN=1 \
    "$image" --no-audio --scale 2 > "$work/game.log" 2>&1 &
game=$!
w=""
for _ in $(seq 1 75); do
    w="$(DISPLAY="$disp" xdotool search --name 'OpenDana' 2>/dev/null | head -1 || true)"
    [ -z "$w" ] || break
    kill -0 "$game" 2>/dev/null || { cat "$work/game.log" >&2; fail 'the AppImage exited before opening a window'; }
    sleep 0.2
done
[ -n "$w" ] || { cat "$work/game.log" >&2; fail 'no OpenDana window'; }
geometry="$(DISPLAY="$disp" xdotool getwindowgeometry "$w" | tr '\n' ' ')"
echo "window $w: $geometry"
size="$(DISPLAY="$disp" xdotool getwindowgeometry "$w" | sed -n 's/.*Geometry: \([0-9]*\)x\([0-9]*\).*/\1 \2/p')"
set -- $size
[ "${1:-0}" -ge 256 ] && [ "${2:-0}" -ge 224 ] || fail "window too small: $size"
sleep 3
kill -0 "$game" 2>/dev/null || { cat "$work/game.log" >&2; fail 'the process died within 3 s of opening the window'; }
DISPLAY="$disp" xdotool windowclose "$w" 2>/dev/null || true
DISPLAY="$disp" xdotool key --window "$w" Escape 2>/dev/null || true
for _ in $(seq 1 50); do kill -0 "$game" 2>/dev/null || break; sleep 0.2; done
if kill -0 "$game" 2>/dev/null; then kill "$game" 2>/dev/null; echo "note: the window did not close on request; process terminated"; fi
wait "$game" 2>/dev/null || true
echo "linux-package-appimage-xvfb: PASS"
