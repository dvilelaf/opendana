#!/usr/bin/env bash
# layout gate of the overlay screens (tests/ui-screens-layout.c):
# headless, pure C plus the SDL headers, well under a second.
#
# Positive control: OD_UI_SRC=DIR builds it against another tree's
# src/app/ui*.c, src/ext/settings.c and include/ (e.g. the earlier sources
# of the older code) and then expects it to FAIL.
set -euo pipefail
cd "$(dirname "$0")/.."
b="${BUILD_DIR:-build}"
src="${OD_UI_SRC:-.}"
for lib in "$b/libopendana_ext.a" "$b/libopendana_core.a"; do
    [ -f "$lib" ] || { echo "ui-screens-layout: missing $lib (run build.sh)" >&2; exit 2; }
done
read -ra sdl <<< "$(bash tests/ui-sdl-cflags.sh)"
out="$(mktemp -d "${TMPDIR:-/tmp}/ui-screens-layout.XXXXXX")"
trap 'rm -rf "$out"' EXIT
# shellcheck source=tests/portable.sh
. tests/portable.sh
if [ "$od_is_darwin" = 1 ]; then
    # ld64 has no --wrap: see tests/editor-ui-layout-run.sh
    objs=()
    for f in ui ui_import ui_editor ui_catalog ui_strings; do
        o="$out/$f.o"; objs+=("$o")
        cc -std=c11 -Wall -Wextra -Werror $od_plat_cflags -I"$src/include" -Iinclude "${sdl[@]}" \
            -c "$src/src/app/$f.c" -o "$o"
    done
    cc -std=c11 -Wall -Wextra -Werror $od_plat_cflags -I"$src/include" -Iinclude "${sdl[@]}" \
        -Dod_ui_font_text=__real_od_ui_font_text -c "$src/src/app/ui_font.c" -o "$out/ui_font.o"
    cc -std=c11 -Wall -Wextra -Werror $od_plat_cflags -I"$src/include" -Iinclude "${sdl[@]}" \
        -D__wrap_od_ui_font_text=od_ui_font_text -c tests/ui-screens-layout.c -o "$out/test.o"
    # ui.c declares od_renderer_cursor_update weak (absent in the headless tests): on Mach-O a
    # weak undefined symbol must be allowed explicitly at link time.
    cc "$out/test.o" "${objs[@]}" "$out/ui_font.o" "$b/libopendana_ext.a" "$b/libopendana_core.a" -lm \
        -Wl,-U,_od_renderer_cursor_update -o "$out/ui-screens-layout"
else
cc -std=c11 -Wall -Wextra -Werror -I"$src/include" -Iinclude "${sdl[@]}" \
    tests/ui-screens-layout.c \
    "$src/src/app/ui.c" "$src/src/app/ui_import.c" "$src/src/app/ui_editor.c" \
    "$src/src/app/ui_catalog.c" "$src/src/app/ui_font.c" \
    "$src/src/app/ui_strings.c" \
    "$b/libopendana_ext.a" "$b/libopendana_core.a" -lm \
    -Wl,--wrap=od_ui_font_text -o "$out/ui-screens-layout"
fi
mkdir -p "$out/xdg"
if [ "$src" = "." ]; then
    XDG_DATA_HOME="$out/xdg" TMPDIR="$out" "$out/ui-screens-layout"
else
    if XDG_DATA_HOME="$out/xdg" TMPDIR="$out" "$out/ui-screens-layout"; then
        echo "ui-screens-layout: positive control PASSED on $src (the gate is blind)" >&2
        exit 1
    fi
    echo "ui-screens-layout: positive control failed on $src, as expected"
fi
