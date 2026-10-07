#!/usr/bin/env bash
# the example mods hold no CHR of the original in the repository.
#
# `mods/ejemplo` and `content/mods/test-gfx` change Hero's CHR tiles. A full
# derived sheet would version the rest of the original CHR and the auditor
# `tools/audit/rom_free.py` flags it. The fix
# is to derive the sheet from the user's imported
# package at run/test time with `tools/content/make_mod_chr.py`, never commit it.
#
# This is the public, ROM-free control (level 1):
#   1. the generator's self-test (synthetic sheet, no ROM);
#   2. no CHR sheet is versioned under the mods;
#   3. the generated path is ignored, so a dev run cannot dirty the tree;
#   4. the two mod builders use the shared generator.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

echo "== the example mods derive their CHR (no versioned CHR) =="

python3 tools/content/make_mod_chr.py --self-test

for rel in mods/ejemplo/graphics/chr/chr-01.png \
           content/mods/test-gfx/graphics/chr/chr-01.png; do
    if git ls-files --cached --error-unmatch "$rel" >/dev/null 2>&1; then
        echo "ERROR: the repository versions the derived CHR '$rel'" >&2
        exit 1
    fi
done
echo "no derived mod CHR is versioned"

for rel in mods/ejemplo/graphics/chr/chr-01.png \
           content/mods/test-gfx/graphics/chr/chr-01.png; do
    if ! git check-ignore -q "$rel"; then
        echo "ERROR: generated '$rel' is not ignored (a dev run would dirty the tree)" >&2
        exit 1
    fi
done
echo "the generated mod CHR paths are ignored"

for script in tools/content/make_example_mod.py tools/content/make_test_mod.py; do
    if ! grep -q 'make_mod_chr' "$script"; then
        echo "ERROR: $script does not use the shared CHR generator" >&2
        exit 1
    fi
done
echo "both mod builders use tools/content/make_mod_chr.py"

echo "mods-chr contract ok"
