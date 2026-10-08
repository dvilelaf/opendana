#!/usr/bin/env bash
# the ROM chooser (zenity/kdialog child of the first-run
# screen) never survives the game, and the fork is done with care.
#
# A fake `zenity` first in PATH records its pid, its open descriptors and its
# environment, then sleeps. tests/import-picker-child-probe.c drives the same
# src/app/rom_picker.c the product links:
#   1. the game returns from main with the chooser open -> the chooser is gone;
#   2. od_rom_picker_cancel() kills and reaps it immediately;
#   3. the game is SIGKILLed -> the kernel ends the chooser (Linux PDEATHSIG);
#   4. the chooser inherits only stdin/stdout/stderr (no leaked descriptor) and
#      gets GTK_USE_PORTAL=0 / NO_AT_BRIDGE=1 without setenv after fork.
# POSIX only (the Windows dialog is modal and in-process). No ROM needed.
# macOS (C6.70): the chooser there is osascript (`choose file`), so the stand-in is a fake
# `osascript`, descriptors are listed with lsof instead of /proc, and check 4 (SIGKILL of the
# game) is reported as SKIPPED because macOS has no PDEATHSIG: an osascript dialog outlives a
# killed game until it is dismissed. A fifth check (macOS only) proves that the folder reaches
# osascript as an argument and never inside the script text.
set -euo pipefail
cd "$(dirname "$0")/.."
case "$(uname -s)" in
    Linux) tool=zenity; darwin=0 ;;
    Darwin) tool=osascript; darwin=1 ;;
    *) echo "BLOCKED: neither Linux nor macOS" >&2; exit 2 ;;
esac

tmp="$(mktemp -d "${TMPDIR:-/tmp}/opendana-picker-child.XXXXXX")"
trap 'kill $(cat "$tmp"/*.pid 2>/dev/null) 2>/dev/null || true; rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/home"
if [ "$darwin" = 1 ]; then
cat > "$tmp/bin/$tool" <<EOF
#!/bin/sh
echo \$\$ > "\$FAKE_PICKER_DIR/chooser.pid"
printf '%s\n' "\$@" > "\$FAKE_PICKER_DIR/argv.txt"
lsof -a -nP -p \$\$ -d 0-9999 -Ffn | awk '/^f/ {fd=substr(\$0,2)} /^n/ {print fd, substr(\$0,2)}' > "\$FAKE_PICKER_DIR/fds.txt"
echo "portal=\$GTK_USE_PORTAL bridge=\$NO_AT_BRIDGE" > "\$FAKE_PICKER_DIR/env.txt"
touch "\$FAKE_PICKER_DIR/ready"
exec sleep 300
EOF
else
cat > "$tmp/bin/$tool" <<EOF
#!/bin/sh
echo \$\$ > "\$FAKE_PICKER_DIR/chooser.pid"
printf '%s\n' "\$@" > "\$FAKE_PICKER_DIR/argv.txt"
ls -l /proc/\$\$/fd/ | sed -n "s/.* \\([0-9][0-9]*\\) -> \\(.*\\)/\\1 \\2/p" > "\$FAKE_PICKER_DIR/fds.txt"
echo "portal=\$GTK_USE_PORTAL bridge=\$NO_AT_BRIDGE" > "\$FAKE_PICKER_DIR/env.txt"
touch "\$FAKE_PICKER_DIR/ready"
exec sleep 300
EOF
fi
chmod +x "$tmp/bin/$tool"
"${CC:-cc}" -std=gnu11 -Iinclude tests/import-picker-child-probe.c src/app/rom_picker.c \
    src/import/fsutil.c src/core/userdir.c -o "$tmp/probe"

alive() { kill -0 "$1" 2>/dev/null && [ "$(ps -o stat= -p "$1" | cut -c1)" != Z ]; }
run_mode() { # mode -> sets chooser pid
    rm -f "$tmp/ready" "$tmp/chooser.pid"
    PATH="$tmp/bin:$PATH" HOME="$tmp/home" XDG_DATA_HOME="$tmp/xdg" \
        FAKE_PICKER_DIR="$tmp" FAKE_PICKER_READY="$tmp/ready" "$tmp/probe" "$1"
}
cases=0
ok() { cases=$((cases + 1)); echo "ok $cases - $1"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== the ROM chooser child never outlives the game =="
run_mode exit > "$tmp/exit.log"
chooser=$(cat "$tmp/chooser.pid")
sleep 0.2
alive "$chooser" && fail "the chooser (pid $chooser) survived the game's exit"
ok "game exits with the chooser open -> chooser $chooser is gone"

fds=$(cut -d' ' -f1 "$tmp/fds.txt" | tr '\n' ' ')
# Beyond 0-2, only what sh itself opens for the fake script is allowed; the
# probe's /dev/null (opened without O_CLOEXEC) must not be there.
extra=$(awk -v t="/bin/$tool" '$1 > 2 && substr($2, length($2) - length(t) + 1) != t' "$tmp/fds.txt")
[ -z "$extra" ] || fail "the chooser inherited descriptors: $(tr '\n' ';' < "$tmp/fds.txt")"
grep -q 'portal=0 bridge=1' "$tmp/env.txt" || fail "chooser environment: $(cat "$tmp/env.txt")"
ok "the chooser sees only fds [$fds] and GTK_USE_PORTAL=0, NO_AT_BRIDGE=1"

run_mode cancel > "$tmp/cancel.log" || { cat "$tmp/cancel.log" >&2; fail "cancel did not reap the child"; }
chooser=$(cat "$tmp/chooser.pid")
alive "$chooser" && fail "od_rom_picker_cancel left the chooser running"
ok "od_rom_picker_cancel kills and reaps the chooser ($(tail -1 "$tmp/cancel.log" | sed 's/probe: //'))"

if [ "$darwin" = 1 ]; then
    # argv of the fake osascript, recorded by the first run: [-e script]... title folder
    run_mode exit > "$tmp/exit2.log"
    grep -qx -- 'on run argv' "$tmp/argv.txt" || fail "osascript did not get the script: $(cat "$tmp/argv.txt")"
    grep -q 'choose file' "$tmp/argv.txt" || fail "the script does not call choose file"
    [ "$(tail -1 "$tmp/argv.txt")" = "$tmp/home" ] || fail "the start folder is not the last argument: $(tail -1 "$tmp/argv.txt")"
    [ "$(grep -cF "$tmp/home" "$tmp/argv.txt")" = 1 ] || fail "the folder appears more than once: it leaked into the script text"
    ok "osascript gets the prompt and the folder as arguments of 'on run argv', not inside the script"
    echo "skipped 4 - SIGKILL of the game ends the chooser: Linux only (PDEATHSIG); on macOS the dialog outlives a killed game"
    echo "import-picker-child: $cases cases OK, 1 skipped (macOS has no PDEATHSIG)"
    exit 0
fi
rm -f "$tmp/ready" "$tmp/chooser.pid"
PATH="$tmp/bin:$PATH" HOME="$tmp/home" XDG_DATA_HOME="$tmp/xdg" \
    FAKE_PICKER_DIR="$tmp" FAKE_PICKER_READY="$tmp/ready" "$tmp/probe" hang > "$tmp/hang.log" &
game=$!
for _ in $(seq 100); do [ -f "$tmp/ready" ] && break; sleep 0.05; done
chooser=$(cat "$tmp/chooser.pid")
alive "$chooser" || fail "the chooser did not start"
kill -9 "$game"; wait "$game" 2>/dev/null || true
for _ in $(seq 40); do alive "$chooser" || break; sleep 0.05; done
alive "$chooser" && fail "the chooser survived a SIGKILL of the game"
ok "game killed with SIGKILL -> the kernel ends the chooser (PDEATHSIG)"

echo "import-picker-child: $cases cases OK"
