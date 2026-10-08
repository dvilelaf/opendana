#!/usr/bin/env bash
# Build the libraries, then the unit runner from the `unit` rows of every
# tests/*-tests-cases.inc present (the public tree has only the public one).
set -euo pipefail
cd "$(dirname "$0")/.."
build="${BUILD_DIR:-build}"
libs="$build/public-libraries"
mkdir -p "$build"
cmake -S . -B "$libs" -DOD_BUILD_APP=OFF -DOD_BUILD_TESTS=OFF
cmake --build "$libs" --target opendana_core opendana_ext -j2
sources=()
while IFS= read -r source; do sources+=("$source"); done < <(python3 - <<'PY'
import pathlib, re
s = ''.join(f.read_text() for f in sorted(pathlib.Path('tests').glob('*-tests-cases.inc')))
rows = re.findall(r'\{"([^"]+)", "([^"]+)", "([^"]+)", "([^"]+)"\}', s)
for f in sorted({f for f,_,c,_ in rows if c == 'unit'}):
    print('tests/' + f)
PY
)
# shellcheck source=tests/portable.sh
. tests/portable.sh
# shellcheck disable=SC2086
"${CC:-cc}" -std=c11 -D_XOPEN_SOURCE=700 $od_plat_cflags -DOD_PUBLIC_TESTS_ONLY \
    "-DOD_REPO_ROOT=\"$PWD\"" -Iinclude -Itests -Isrc/app -Isrc/import -Iexternal \
    tests/test_main.c "${sources[@]}" src/render/png.c \
    src/app/ui_strings.c src/app/ui_font.c src/app/ui_editor.c \
    src/app/ui_catalog.c src/app/ui_import.c "$libs/libopendana_ext.a" \
    "$libs/libopendana_core.a" -lm -o "$build/opendana_tests_public"
