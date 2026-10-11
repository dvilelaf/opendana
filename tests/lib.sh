#!/usr/bin/env bash
# Common setup of the test levels, loaded by tests/run.sh and the level scripts;
# it runs no checks by itself. When tests/lib-lab.sh exists (lab-only tiers) it
# is loaded at the end.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD_DIR="${BUILD_DIR:-build}"
export BUILD_DIR
bin="$(pwd)/$BUILD_DIR/opendana"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/opendana-tests.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
export bin tmp ROOT

if [ -f "$ROOT/tests/lib-lab.sh" ]; then
    # shellcheck source=/dev/null
    . "$ROOT/tests/lib-lab.sh"
fi
