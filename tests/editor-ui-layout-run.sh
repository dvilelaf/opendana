#!/usr/bin/env bash
# layout gate of the modern editor and catalogue (tests/editor-ui-layout.c).
# Headless, pure C, no game content, well under a second. Needs the built
# libraries (bash build.sh) for the level model and the catalogue.
#
# Positive control: OD_UI_SRC=DIR builds the same gate against another copy
# of src/app/ui_{editor,catalog,font,strings}.c and include/opendana/ui_*.h (for
# example the earlier files of the older code) and
# then expects it to FAIL.
set -euo pipefail
cd "$(dirname "$0")/.."
b="${BUILD_DIR:-build}"
src="${OD_UI_SRC:-.}"
for lib in "$b/libopendana_ext.a" "$b/libopendana_core.a"; do
    [ -f "$lib" ] || { echo "editor-ui-layout: missing $lib (run build.sh)" >&2; exit 2; }
done
out="$(mktemp -d "${TMPDIR:-/tmp}/editor-ui-layout.XXXXXX")"
trap 'rm -rf "$out"' EXIT
# shellcheck source=tests/portable.sh
. tests/portable.sh
if [ "$od_is_darwin" = 1 ]; then
    # ld64 has no --wrap: the real od_ui_font_text is renamed in its own file and the
    # test's wrapper takes the original name (same two functions, same call graph).
    objs=()
    for f in "$src/src/app/ui_editor.c" "$src/src/app/ui_catalog.c" "$src/src/app/ui_strings.c"; do
        o="$out/$(basename "$f" .c).o"; objs+=("$o")
        cc -std=c11 -Wall -Wextra -Werror $od_plat_cflags -I"$src/include" -Iinclude -c "$f" -o "$o"
    done
    cc -std=c11 -Wall -Wextra -Werror $od_plat_cflags -I"$src/include" -Iinclude \
        -Dod_ui_font_text=__real_od_ui_font_text -c "$src/src/app/ui_font.c" -o "$out/ui_font.o"
    cc -std=c11 -Wall -Wextra -Werror $od_plat_cflags -I"$src/include" -Iinclude \
        -D__wrap_od_ui_font_text=od_ui_font_text -c tests/editor-ui-layout.c -o "$out/test.o"
    cc "$out/test.o" "${objs[@]}" "$out/ui_font.o" "$b/libopendana_ext.a" "$b/libopendana_core.a" -lm \
        -o "$out/editor-ui-layout"
else
cc -std=c11 -Wall -Wextra -Werror -I"$src/include" -Iinclude \
    tests/editor-ui-layout.c \
    "$src/src/app/ui_editor.c" "$src/src/app/ui_catalog.c" \
    "$src/src/app/ui_font.c" "$src/src/app/ui_strings.c" \
    "$b/libopendana_ext.a" "$b/libopendana_core.a" -lm \
    -Wl,--wrap=od_ui_font_text -o "$out/editor-ui-layout"
fi
if [ "$src" = "." ]; then
    TMPDIR="$out" "$out/editor-ui-layout"
else
    if TMPDIR="$out" "$out/editor-ui-layout"; then
        echo "editor-ui-layout: positive control PASSED on $src (the gate is blind)" >&2
        exit 1
    fi
    echo "editor-ui-layout: positive control failed on $src, as expected"
fi
