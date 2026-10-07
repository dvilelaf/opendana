#!/usr/bin/env bash
# states travel between OpenDana versions (tests/state-compat.c).
# Positive control: OD_STATE_SRC=DIR builds against another tree's
# src/ext/state.c and include/ (e.g. the parent revision) and expects
# the check to FAIL.
set -euo pipefail
cd "$(dirname "$0")/.."
b="${BUILD_DIR:-build}"
src="${OD_STATE_SRC:-.}"
for lib in "$b/libopendana_ext.a" "$b/libopendana_core.a"; do
    [ -f "$lib" ] || { echo "state-compat: missing $lib (run build.sh)" >&2; exit 2; }
done
out="$(mktemp -d "${TMPDIR:-/tmp}/state-compat.XXXXXX")"
trap 'rm -rf "$out"' EXIT
extra=()
[ "$src" = "." ] || extra=(-DOD_COMPAT_NO_REASON "$src/src/ext/state.c")
cc -std=c11 -Wall -Wextra -Werror -I"$src/include" -Iinclude "${extra[@]}" \
    tests/state-compat.c "$b/libopendana_ext.a" "$b/libopendana_core.a" -lm \
    -Wl,--allow-multiple-definition -o "$out/state-compat"
if [ "$src" = "." ]; then
    "$out/state-compat" "$out/s.odstate"
else
    if "$out/state-compat" "$out/s.odstate"; then
        echo "state-compat: positive control PASSED on $src (the gate is blind)" >&2
        exit 1
    fi
    echo "state-compat: positive control failed on $src, as expected"
fi
