#!/usr/bin/env bash
# C6.30: tests/linux-package-check.py on a real Linux TAR, with controls.
#   bash tests/linux-package-inventory.sh PACKAGE.tar.gz COMMIT
# Positive control: the unpacked TAR passes (inventory, notices, bundle sums,
# ELF/SDL resolution, --version and SOURCES.json of COMMIT). Negative controls,
# each on a fresh copy: a missing notice, an extra file, a tampered source
# archive (bundle SHA256SUMS) and the wrong expected commit must all fail.
set -euo pipefail
cd "$(dirname "$0")/.."
tarball="$(realpath "${1:?usage: linux-package-inventory.sh PACKAGE.tar.gz COMMIT}")"
commit="${2:?expected commit}"
work="$(mktemp -d "${TMPDIR:-/tmp}/opendana-linux-inventory.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir "$work/good"
tar -xzf "$tarball" -C "$work/good"
top="$(ls "$work/good")"
lb="share/doc/opendana/license-bundle"
python3 tests/linux-package-check.py "$work/good/$top" --commit "$commit"
reject() { # label dir [commit]
    if python3 tests/linux-package-check.py "$2/$top" --commit "${3:-$commit}" > "$work/log" 2>&1; then
        cat "$work/log"; echo "error: control '$1' was accepted" >&2; exit 1
    fi
    echo "rejected as expected: $1 ($(grep -m1 FAIL "$work/log"))"
}
cp -a "$work/good" "$work/c1"; rm "$work/c1/$top/$lb/notices/SDL2-embedded.txt"; reject 'missing notice' "$work/c1"
cp -a "$work/good" "$work/c2"; echo x > "$work/c2/$top/bin/extra"; reject 'extra file' "$work/c2"
cp -a "$work/good" "$work/c3"; echo x >> "$work/c3/$top/$lb/sources/cJSON.tar.xz"; reject 'tampered source' "$work/c3"
reject 'other commit' "$work/good" ffffffffffffffffffffffffffffffffffffffff
echo 'linux-package-inventory: PASS (1 positive, 4 negative controls)'
