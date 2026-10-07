#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d "${TMPDIR:-/tmp}/opendana-clock-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
cc -std=c11 -Wall -Wextra -Werror -Iinclude tests/windows-clock.c src/core/clock.c -o "$tmp/native"
"$tmp/native"
x86_64-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror -Iinclude tests/windows-clock.c src/core/clock.c -o "$tmp/windows.exe"
x86_64-w64-mingw32-objdump -p "$tmp/windows.exe" | grep 'DLL Name:'
echo 'Windows clock PE linked; runtime Windows not tested by this script'
