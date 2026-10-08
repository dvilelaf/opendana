#!/usr/bin/env bash
# one test per interface defect of the review (tests/ui-behaviour.c,
# AddressSanitizer + UBSan when the compiler has them) and a scan for Spanish
# leaking into the English interface (UI-18). Headless, about a second.
#
# Positive control: OD_UI_SRC=DIR builds it against another tree's
# src/app/ui*.c, src/ext/{settings,input_map,leveled,catalog}.c and include/
# (the earlier sources of the older code) and expects it to
# FAIL.
set -euo pipefail
cd "$(dirname "$0")/.."
b="${BUILD_DIR:-build}"
src="${OD_UI_SRC:-.}"
for lib in "$b/libopendana_ext.a" "$b/libopendana_core.a"; do
    [ -f "$lib" ] || { echo "ui-behaviour: missing $lib (run build.sh)" >&2; exit 2; }
done
read -ra sdl <<< "$(bash tests/ui-sdl-cflags.sh)"
out="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/ui-behaviour.XXXXXX")" && pwd -P)"   # physical path: macOS TMPDIR runs through the /var symlink the thumbnail checks refuse
[ -n "${KEEP:-}" ] || trap 'rm -rf "$out"' EXIT
san=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
echo 'int main(void){return 0;}' > "$out/probe.c"
cc "${san[@]}" "$out/probe.c" -o "$out/probe" 2>/dev/null || san=()
rc=0
# shellcheck source=tests/portable.sh
. tests/portable.sh
if [ "$od_is_darwin" = 1 ]; then
    # ld64 has no --wrap: the real od_state_peek is compiled from state.c under another name
    # (it then defines every symbol of state.o, so the archive member is not pulled) and the
    # test's wrapper takes the original name. See tests/editor-ui-layout-run.sh.
    objs=()
    for f in app/ui app/ui_editor app/ui_catalog app/ui_font app/ui_strings ext/settings ext/input_map \
             ext/leveled ext/catalog render/png; do
        o="$out/$(basename "$f").o"; objs+=("$o")
        file="$src/src/$f.c"
        if [ "$f" = render/png ]; then file=src/render/png.c; fi
        cc -std=c11 -Wall -Wextra -Werror ${san[@]+"${san[@]}"} $od_plat_cflags -I"$src/include" -Iinclude "${sdl[@]}" \
            -c "$file" -o "$o"
    done
    cc -std=c11 -Wall -Wextra -Werror ${san[@]+"${san[@]}"} $od_plat_cflags -I"$src/include" -Iinclude "${sdl[@]}" \
        -Dod_state_peek=__real_od_state_peek -c "$src/src/ext/state.c" -o "$out/state.o"
    cc -std=c11 -Wall -Wextra -Werror ${san[@]+"${san[@]}"} $od_plat_cflags -I"$src/include" -Iinclude "${sdl[@]}" \
        -D__wrap_od_state_peek=od_state_peek -c tests/ui-behaviour.c -o "$out/test.o"
    cc ${san[@]+"${san[@]}"} "$out/test.o" "${objs[@]}" "$out/state.o" "$b/libopendana_ext.a" "$b/libopendana_core.a" -lm \
        -Wl,-U,_od_renderer_cursor_update -o "$out/ui-behaviour-test"
else
cc -std=c11 -Wall -Wextra -Werror "${san[@]}" -I"$src/include" -Iinclude "${sdl[@]}" \
    tests/ui-behaviour.c \
    "$src/src/app/ui.c" "$src/src/app/ui_editor.c" "$src/src/app/ui_catalog.c" \
    "$src/src/app/ui_font.c" "$src/src/app/ui_strings.c" \
    "$src/src/ext/settings.c" "$src/src/ext/input_map.c" \
    "$src/src/ext/leveled.c" "$src/src/ext/catalog.c" src/render/png.c \
    "$b/libopendana_ext.a" "$b/libopendana_core.a" -lm \
    -Wl,--wrap=od_state_peek -o "$out/ui-behaviour-test"
fi
ASAN_OPTIONS=detect_leaks=0 TMPDIR="$out" "$out/ui-behaviour-test" || rc=1

# UI-18: no Spanish words of the old host/editor messages in the interface.
if grep -nE 'Prueba terminada|Importacion fallida|Exportacion fallida|"Duplicado|"Importado|Exportado a|No se pud|" copia"' \
        "$src"/src/app/ui*.c "$src"/src/app/main.c; then
    echo "FAIL UI-18: Spanish text in the English interface (above)"
    rc=1
else
    echo "GREEN UI-18 no Spanish in the interface sources"
fi

if [ "$src" = "." ]; then
    exit "$rc"
fi
if [ "$rc" -eq 0 ]; then
    echo "ui-behaviour: positive control PASSED on $src (the tests are blind)" >&2
    exit 1
fi
echo "ui-behaviour: positive control failed on $src, as expected"
