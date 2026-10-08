#!/usr/bin/env bash
# opt-in applications-menu integration (Linux). Always runs the module
# test (temporary XDG data home; the real ~/.local/share is never read or
# written). With OD_DESKTOP_BIN (a built opendana) and OD_DESKTOP_CONTENT (an
# imported content dir) it also drives the real binary:
#   - --install-desktop / --uninstall-desktop, validated with
#     desktop-file-validate when installed;
#   - under Xvfb with xdotool: the first opening as a package asks once;
#     No creates nothing and the question never returns; Yes installs; a
#     development build (not packaged) and a headless run are never asked.
#   OD_DESKTOP_SHOT=PNG saves the screen with the question open.
# Positive control: run it with OD_DESKTOP_BIN set to a binary built before
# an older build; the question never appears and the run fails.
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp -d "${TMPDIR:-/tmp}/desktop.XXXXXX")"
xp=""; app=""
cleanup() { [ -n "$app" ] && kill "$app" 2>/dev/null || true; [ -n "$xp" ] && kill "$xp" 2>/dev/null || true; rm -rf "$out"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }

# The package's icon tree, as share/icons/hicolor in the TAR and the AppImage.
hi="$out/hicolor"
for n in 16 24 32 48 64 128 256 512; do
    mkdir -p "$hi/${n}x${n}/apps"
    cp "packaging/icon/png/opendana-$n.png" "$hi/${n}x${n}/apps/opendana.png"
done
mkdir -p "$hi/scalable/apps"
cp packaging/icon/opendana.svg "$hi/scalable/apps/opendana.svg"

# OD_DESKTOP_SRC=DIR builds the module of another tree (positive control: the
# icon-cache checks must FAIL on 8f71512, before the icon-theme.cache fix).
msrc="${OD_DESKTOP_SRC:-.}"
# shellcheck source=tests/portable.sh
. tests/portable.sh
# shellcheck disable=SC2086
cc -std=c11 -Wall -Wextra -Werror $od_plat_cflags -I"$msrc/src/app" tests/desktop-integration.c "$msrc/src/app/desktop_integration.c" -o "$out/unit"
if [ -n "${OD_DESKTOP_SRC:-}" ]; then
    if "$out/unit" "$hi" "$out/work"; then echo "desktop: positive control PASSED on $msrc (the test is blind)"; exit 1; fi
    echo "desktop: positive control failed on $msrc, as expected"; exit 0
fi
"$out/unit" "$hi" "$out/work"
[ -n "${OD_DESKTOP_BIN:-}" ] || exit 0

bin=$(realpath "$OD_DESKTOP_BIN"); content=$(realpath "${OD_DESKTOP_CONTENT:?content dir}")
export OD_DESKTOP_ICON_SRC="$hi"
newhome() {   # newhome NAME: a clean HOME for one scenario
    export HOME="$out/$1/home" XDG_DATA_HOME="$out/$1/data" XDG_CONFIG_HOME="$out/$1/config"
    rm -rf "$out/$1"; mkdir -p "$HOME"
    desk="$XDG_DATA_HOME/applications/opendana.desktop"
}

# --- command line (OD_DESKTOP_WINDOW_ONLY=1 skips it: control on an older binary) ---
if [ -z "${OD_DESKTOP_WINDOW_ONLY:-}" ]; then
newhome cli
unset APPIMAGE
"$bin" --install-desktop >"$out/cli.log" 2>&1 || fail "--install-desktop: $(cat "$out/cli.log")"
[ -f "$desk" ] || fail "no launcher after --install-desktop"
grep -q "^X-OpenDana-Generated=true$" "$desk" || fail "launcher lacks the marker"
grep -q "^StartupWMClass=opendana$" "$desk" || fail "launcher lacks StartupWMClass"
grep -q "^Exec=\"$bin\"$" "$desk" || fail "Exec is not the real binary: $(grep ^Exec "$desk")"
[ -f "$XDG_DATA_HOME/icons/hicolor/48x48/apps/opendana.png" ] || fail "no 48 px icon"
if command -v desktop-file-validate >/dev/null; then desktop-file-validate "$desk" || fail "desktop-file-validate"; fi
"$bin" --uninstall-desktop >"$out/cli.log" 2>&1 || fail "--uninstall-desktop"
[ ! -e "$desk" ] && [ ! -e "$XDG_DATA_HOME/icons/hicolor/48x48/apps/opendana.png" ] || fail "uninstall left files"
echo "PASS: --install-desktop / --uninstall-desktop"
fi

# --- the question in the real window ---------------------------------------
Xvfb -displayfd 9 -screen 0 1280x1024x24 9>"$out/disp" >/dev/null 2>&1 &
xp=$!
for _ in $(seq 50); do [ -s "$out/disp" ] && break; sleep 0.1; done
export DISPLAY=":$(head -1 "$out/disp" | tr -d '[:space:]')" SDL_AUDIODRIVER=dummy
run() {   # run NAME [FLAGS...]; sets $app, log $out/NAME.log
    "$bin" --content "$content" --scale 2 "${@:2}" >"$out/$1.log" 2>&1 &
    app=$!
    sleep "${OD_DESKTOP_SETTLE:-4}"
}
stop() { kill "$app" 2>/dev/null || true; wait "$app" 2>/dev/null || true; app=""; }
waitq() { for _ in $(seq 150); do grep -q "desktop question shown" "$out/$1.log" && return 0; sleep 0.1; done; }
asked() { grep -q "desktop question shown" "$out/$1.log"; }
key() { xdotool mousemove 300 300; xdotool key "$@"; sleep 0.6; }
pkg() { export APPIMAGE="$out/OpenDana.AppImage"; }   # makes this run a "package"

# Question: appears, No is focused, Enter = No -> nothing is created.
newhome no; pkg
run no
waitq no
asked no || fail "the question did not appear on the first opening (see $out/no.log)"
[ -z "${OD_DESKTOP_SHOT:-}" ] || import -window root "$OD_DESKTOP_SHOT"
[ ! -e "$XDG_DATA_HOME/applications" ] || fail "files created before the answer"
key Return
[ ! -e "$desk" ] && [ ! -e "$XDG_DATA_HOME/icons" ] || fail "No created files"
grep -q "desktop_prompted = 1" "$XDG_CONFIG_HOME/opendana/settings.ini" || fail "the No was not remembered"
stop
run no2
asked no2 && fail "the question came back after No"
stop
echo "PASS: No creates nothing and the question never returns"

# Yes (Left/Right moves to Yes) installs, Exec = $APPIMAGE.
newhome yes; pkg
run yes
waitq yes
asked yes || fail "no question in the Yes run"
key Right; key Return
[ -f "$desk" ] || fail "Yes did not create the launcher (see $out/yes.log)"
grep -q "^Exec=\"$APPIMAGE\"$" "$desk" || fail "Exec is not \$APPIMAGE: $(grep ^Exec "$desk")"
stop
echo "PASS: Yes installs the launcher with Exec = \$APPIMAGE"

# Never asked: development build (not a package), headless run, already installed.
newhome dev; unset APPIMAGE OD_DESKTOP_ICON_SRC
run dev
asked dev && fail "a development build was asked"
stop
newhome head; pkg
"$bin" --content "$content" --headless --frames 5 >"$out/head.log" 2>&1 || true
asked head && fail "a headless run was asked"
[ ! -e "$XDG_DATA_HOME/applications" ] || fail "a headless run wrote files"
echo "PASS: no question in a development build or a headless run"
