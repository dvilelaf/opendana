#!/usr/bin/env bash
# C6.1/C6.30: inspect the actual Windows ZIP, never a stale staging directory.
#   windows-package.sh PACKAGE.zip SDL2.dll [COMMIT]
# Closed inventory: the two executables, README/LICENSE/NOTICE and the licence
# bundle (every notice that applies to opendana.exe and SDL2.dll, including the
# MinGW-w64 runtime members linked into them, and the corresponding sources).
# Also: PE x86-64, the shipped DLL is the given one, no unbundled MinGW runtime
# DLL, the bundle's own SHA256SUMS, and (with COMMIT) the commit binding.
set -euo pipefail
export LC_ALL=C
zip="$(realpath "${1:?usage: windows-package.sh PACKAGE.zip SDL2.dll [COMMIT]}")"
dll="$(realpath "${2:?supply the SDL2.dll used for the build}")"
commit="${3:-}"
work="$(mktemp -d "${TMPDIR:-/tmp}/opendana-package-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT
fail() { echo "error: windows-package: $*" >&2; exit 1; }

doc='share/doc/opendana'
lb="$doc/license-bundle"
expected=(bin/opendana.exe bin/SDL2.dll "$doc/README.md" "$doc/LICENSE" "$doc/NOTICE"
    "$lb/README.txt" "$lb/SOURCES.json" "$lb/BUILD-IDENTITY.json" "$lb/INPUT-MANIFEST.json"
    "$lb/RELINK.txt" "$lb/SHA256SUMS"
    # C6.48: the first-run screen's fonts (with their licence) and icon, loaded at run time
    share/opendana/fonts/Inter-Regular.ttf share/opendana/fonts/Inter-SemiBold.ttf
    share/opendana/fonts/OFL.txt share/opendana/ui/opendana-128.png)
for n in application SDL2 SDL2-embedded cJSON stb_image stb_image_write stb_truetype Inter mingw-w64-runtime; do
    expected+=("$lb/notices/$n.txt")
done
for n in application SDL2 cJSON stb_image stb_image_write stb_truetype Inter relink-materials; do
    expected+=("$lb/sources/$n.tar.xz")
done

mapfile -t files < <(unzip -Z1 "$zip" | grep -v '/$' | sort)
[ "${#files[@]}" -gt 0 ] || fail 'empty ZIP'
top="${files[0]%%/*}"
for f in "${files[@]}"; do
    [[ "$f" != *'..'* && "$f" != /* && "$f" == "$top/"* ]] || fail "unsafe or foreign member: $f"
done
want="$(printf "$top/%s\n" "${expected[@]}" | sort)"
have="$(printf '%s\n' "${files[@]}")"
if [ "$want" != "$have" ]; then
    diff <(echo "$want") <(echo "$have") >&2 || true
    fail 'ZIP inventory differs from the closed list (- missing, + unexpected)'
fi
unzip -q "$zip" -d "$work"
pkg="$work/$top"
cmp "$dll" "$pkg/bin/SDL2.dll" || fail 'bin/SDL2.dll is not the given SDL2.dll'
objdump="${MINGW_PREFIX:-x86_64-w64-mingw32}-objdump"
for binary in "$pkg/bin/opendana.exe" "$pkg/bin/SDL2.dll"; do
    "$objdump" -f "$binary" > "$work/header"
    grep -q 'file format pei-x86-64' "$work/header" || fail "$binary is not PE x86-64"
    "$objdump" -p "$binary" > "$work/pe"
    # MinGW runtime DLLs are not bundled: fail instead of shipping a broken ZIP.
    if grep -Ei 'DLL Name: (libgcc|libstdc|libwinpthread|libssp)' "$work/pe"; then
        fail "unbundled MinGW runtime dependency in $binary"
    fi
done
"$objdump" -p "$pkg/bin/opendana.exe" > "$work/exe-pe"
grep -qi 'DLL Name: SDL2.dll' "$work/exe-pe" || fail 'opendana.exe does not import SDL2.dll'
grep -q 'GNU GENERAL PUBLIC LICENSE' "$pkg/$doc/LICENSE" || fail 'LICENSE is not the GPL text'
grep -q 'GPL-3.0-or-later' "$pkg/$doc/NOTICE" || fail 'NOTICE does not state the licence'
need() { grep -q "$2" "$pkg/$lb/notices/$1.txt" || fail "notices/$1.txt lacks '$2'"; }
need SDL2-embedded 'Sun Microsystems'
need Inter 'SIL OPEN FONT LICENSE Version 1.1'
need stb_truetype 'Sean Barrett'
need SDL2-embedded 'Signal 11 Software'
need SDL2-embedded 'Adrien Descamps'
need SDL2-embedded 'Jamey Sharp'
need SDL2-embedded 'Khronos Group'
need mingw-w64-runtime 'Redistributions in binary form must reproduce the accompanying'
need mingw-w64-runtime 'Lucent Technologies'
need mingw-w64-runtime 'David M. Gay'
need mingw-w64-runtime 'Regents of the University of California'
need mingw-w64-runtime 'Keith Marshall'
( cd "$pkg/$lb" && sha256sum -c --quiet SHA256SUMS ) || fail 'licence bundle SHA256SUMS mismatch'
if [ -n "$commit" ]; then
    grep -aqF "(commit $commit)" "$pkg/bin/opendana.exe" || fail "opendana.exe does not contain (commit $commit)"
    grep -q "\"application_commit\": \"$commit\"" "$pkg/$lb/SOURCES.json" || fail "SOURCES.json is not of commit $commit"
fi
sha256sum "$zip" "$pkg/bin/opendana.exe" "$pkg/bin/SDL2.dll"
printf 'windows-package: validated %s (%d members)\n' "$zip" "${#files[@]}"
