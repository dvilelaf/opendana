#!/usr/bin/env bash
# C6.1/C6.30 module test of tests/windows-package.sh with a synthetic package:
# a probe PE linked against the given SDL2 (never the game), the real notices of
# packaging/notices and dummy archives. Positive control: the complete package
# is accepted. Negative controls: extra content, a missing notice, a notice
# without the mingw-w64 runtime texts, a stale commit and a bundle whose
# SHA256SUMS no longer matches are each rejected.
#   MINGW_SDL2_DIR=<prefix with bin/SDL2.dll, include/SDL2, lib> bash tests/windows-package-controls.sh
set -euo pipefail
cd "$(dirname "$0")/.."
sdl="${MINGW_SDL2_DIR:?set MINGW_SDL2_DIR to the SDL2 MinGW x86_64 prefix}"
work="$(mktemp -d "${TMPDIR:-/tmp}/opendana-package-controls.XXXXXX")"
trap 'rm -rf "$work"' EXIT
commit=0123456789abcdef0123456789abcdef01234567
top=synthetic
reseal() { ( cd "$1/$top/share/doc/opendana/license-bundle" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS ); }
make_tree() { # dir
    local root="$1/$top" lb="$1/$top/share/doc/opendana/license-bundle"
    mkdir -p "$root/bin" "$lb/notices" "$lb/sources"
    "${MINGW_PREFIX:-x86_64-w64-mingw32}-gcc" tests/windows-package-probe.c "-DOD_PROBE_COMMIT=\"$commit\"" \
        -I "$sdl/include/SDL2" -L "$sdl/lib" -lSDL2 -o "$root/bin/opendana.exe"
    cp "$sdl/bin/SDL2.dll" "$root/bin/SDL2.dll"
    cp packaging/README-package.md "$root/share/doc/opendana/README.md"
    cp LICENSE NOTICE "$root/share/doc/opendana/"
    mkdir -p "$root/share/opendana/fonts" "$root/share/opendana/ui"
    cp assets/fonts/Inter-Regular.ttf assets/fonts/Inter-SemiBold.ttf assets/fonts/OFL.txt "$root/share/opendana/fonts/"
    cp packaging/icon/png/opendana-128.png "$root/share/opendana/ui/"
    cp packaging/notices/*.txt packaging/notices/windows/*.txt "$lb/notices/"
    for n in application SDL2 cJSON stb_image stb_image_write stb_truetype Inter relink-materials; do
        printf 'synthetic %s\n' "$n" | xz -c > "$lb/sources/$n.tar.xz"
    done
    for n in README.txt BUILD-IDENTITY.json INPUT-MANIFEST.json RELINK.txt; do echo synthetic > "$lb/$n"; done
    printf '{\n  "application_commit": "%s"\n}\n' "$commit" > "$lb/SOURCES.json"
    reseal "$1"
}
pack() { ( cd "$1" && rm -f "$2" && zip -qrX "$2" "$top" ); }
expect_reject() { # zip label
    if bash tests/windows-package.sh "$1" "$sdl/bin/SDL2.dll" "$commit" > "$work/log" 2>&1; then
        cat "$work/log"; echo "error: control '$2' was accepted" >&2; exit 1
    fi
    echo "rejected as expected: $2 ($(grep -m1 error "$work/log"))"
}

make_tree "$work/good"
pack "$work/good" "$work/good.zip"
bash tests/windows-package.sh "$work/good.zip" "$sdl/bin/SDL2.dll" "$commit"

cp -a "$work/good" "$work/extra"; printf 'synthetic\n' > "$work/extra/$top/original.nes"
pack "$work/extra" "$work/extra.zip"; expect_reject "$work/extra.zip" 'extra content'

cp -a "$work/good" "$work/missing"; rm "$work/missing/$top/share/doc/opendana/license-bundle/notices/SDL2-embedded.txt"
pack "$work/missing" "$work/missing.zip"; expect_reject "$work/missing.zip" 'missing SDL2-embedded notice'

cp -a "$work/good" "$work/nolucent"
sed -i 's/Lucent Technologies/L. Technologies/' "$work/nolucent/$top/share/doc/opendana/license-bundle/notices/mingw-w64-runtime.txt"
reseal "$work/nolucent"
pack "$work/nolucent" "$work/nolucent.zip"; expect_reject "$work/nolucent.zip" 'mingw-w64 runtime notice without the Lucent text'

cp -a "$work/good" "$work/stale"
printf '{\n  "application_commit": "%s"\n}\n' ffffffffffffffffffffffffffffffffffffffff \
    > "$work/stale/$top/share/doc/opendana/license-bundle/SOURCES.json"
reseal "$work/stale"
pack "$work/stale" "$work/stale.zip"; expect_reject "$work/stale.zip" 'source of another commit'

cp -a "$work/good" "$work/tampered"
echo tampered >> "$work/tampered/$top/share/doc/opendana/license-bundle/RELINK.txt"
pack "$work/tampered" "$work/tampered.zip"; expect_reject "$work/tampered.zip" 'bundle SHA256SUMS mismatch'

echo 'windows-package-controls: complete synthetic ZIP accepted; 5 defective ZIPs rejected'
