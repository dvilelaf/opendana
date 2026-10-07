#!/usr/bin/env bash
# layout gate of the modern first-run screen (tests/first-run-layout.c):
# headless pure C, no SDL, under a second. The positive controls (the earlier
# 8x8 screen measured through its real hit test, and a too-wide text) are inside
# the program; it fails if the checker does not reject them.
set -euo pipefail
cd "$(dirname "$0")/.."
b="${BUILD_DIR:-build}"
[ -f "$b/libopendana_core.a" ] || { echo "first-run-layout: missing $b/libopendana_core.a (run build.sh)" >&2; exit 2; }
out="$(mktemp -d "${TMPDIR:-/tmp}/first-run-layout.XXXXXX")"
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -Iinclude -Iexternal/stb -Isrc/import \
    tests/first-run-layout.c src/app/ui_import_view.c src/app/ui_ttf.c \
    src/app/ui_import.c src/app/ui_font.c src/app/ui_strings.c \
    "$b/libopendana_core.a" -lm -o "$out/first-run-layout"
"$out/first-run-layout"
