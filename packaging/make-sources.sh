#!/usr/bin/env bash
# C6.30: generate, at build time, the corresponding sources and licence
# materials of a package. Nothing here is tracked in git as an archive.
#
#   make-sources.sh --commit SHA --platform linux|windows --out DIR
#
# Environment: SDL2_SRC (SDL git tree holding OD_PIN_SDL_COMMIT) and OD_ROM
# (the EU ROM, read only, for the auditor).
#
# DIR/src/opendana-<ver>/   the application source the package is BUILT
#                                 from (so binary and source correspond)
# DIR/materials/                  input of packaging/license-bundle.sh:
#   notices/*.txt                 from the archived commit (common + platform)
#   sources/application.tar.xz    git archive of packaging/application-source.list
#                                 minus every file the ROM auditor flags
#   sources/SDL2.tar.xz           SDL git archive at the pinned commit, the two
#                                 android-project-ant symlinks replaced by copies
#                                 of their targets, plus REVISION.txt
#   sources/{cJSON,stb_image,stb_image_write,stb_truetype,Inter}.tar.xz   vendored copies
#   sources/relink-materials.tar.xz, RELINK.txt        packaging/relink/<platform>
# DIR/application-source.json     commit, paths, exclusions and audit reports
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

commit="" platform="" out=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --commit) commit="${2:?}"; shift 2 ;;
        --platform) platform="${2:?}"; shift 2 ;;
        --out) out="${2:?}"; shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$platform" in linux|windows) ;; *) echo "error: --platform linux|windows" >&2; exit 2 ;; esac
[ -n "$commit" ] && [ -n "$out" ] || { echo "error: --commit and --out are required" >&2; exit 2; }
od_require_env SDL2_SRC "the SDL git tree that contains commit $OD_PIN_SDL_COMMIT (read only)"
od_audit_require_rom
od_require_tools git tar xz python3
[ -e "$out" ] && od_die "output $out already exists"
git -C "$SDL2_SRC" cat-file -e "${OD_PIN_SDL_COMMIT}^{commit}" 2>/dev/null \
    || od_die "SDL2_SRC=$SDL2_SRC does not contain SDL commit $OD_PIN_SDL_COMMIT"

commit="$(git -C "$OD_ROOT" rev-parse --verify --quiet "${commit}^{commit}")" \
    || od_die "--commit is not a commit of this repository"
prefix="opendana-$OD_VERSION"
epoch="$(git -C "$OD_ROOT" log -1 --format=%ct "$commit")"
mkdir -p "$out/materials/sources" "$out/materials/notices" "$out/src" "$out/tmp"
out="$(cd "$out" && pwd)"
tmp="$out/tmp"
mapfile -t paths < <(od_source_paths)

# --- application source: archive, audit, exclude flagged files, archive again
echo "== application source: git archive $commit =="
git -C "$OD_ROOT" archive --format=tar --prefix="$prefix/" "$commit" -- "${paths[@]}" > "$tmp/pass1.tar"
mkdir "$tmp/pass1" && tar -xf "$tmp/pass1.tar" -C "$tmp/pass1"
od_rom_audit --rom "${OD_ROM:-}" --tree "$tmp/pass1/$prefix" \
    --report "$out/application-source-audit-pass1.json" --flagged "$tmp/flagged.txt" || true
# A finding exits 1 with a report; an auditor/usage error writes no report.
[ -f "$out/application-source-audit-pass1.json" ] && [ -f "$tmp/flagged.txt" ] \
    || od_die "the ROM auditor could not audit the application source"
mapfile -t flagged < <(LC_ALL=C sort -u "$tmp/flagged.txt")
{
    echo "OpenDana - corresponding source of the shipped binary"
    echo "Commit: $commit"
    echo "Archived paths (packaging/application-source.list):"
    printf '  %s\n' "${paths[@]}"
    echo "Files of those paths left out because the ROM auditor flagged them as"
    echo "material of the original game (none is needed to build the binary):"
    if [ "${#flagged[@]}" -eq 0 ]; then echo "  (none)"; else printf '  %s\n' "${flagged[@]}"; fi
} > "$tmp/SOURCE-EXCLUSIONS.txt"
excludes=()
for f in "${flagged[@]}"; do excludes+=(":(exclude)$f"); done
git -C "$OD_ROOT" archive --format=tar --prefix="$prefix/" --add-file="$tmp/SOURCE-EXCLUSIONS.txt" \
    "$commit" -- "${paths[@]}" "${excludes[@]}" > "$tmp/application.tar"
tar -xf "$tmp/application.tar" -C "$out/src"
od_rom_audit --rom "${OD_ROM:-}" --tree "$out/src/$prefix" \
    --report "$out/application-source-audit.json" \
    || od_die "the application source of $commit still has genuine ROM findings after exclusion"
stamped="$(cat "$out/src/$prefix/packaging/source-commit.txt")"
[ "$stamped" = "$commit" ] || od_die "packaging/source-commit.txt of the archive is '$stamped', not $commit (export-subst missing?)"
od_xz "$tmp/application.tar" "$out/materials/sources/application.tar.xz"
app="$out/src/$prefix"

# --- SDL source: pinned commit, symlinks serialized, REVISION.txt
echo "== SDL $OD_PIN_SDL_VERSION source: git archive $OD_PIN_SDL_COMMIT =="
sdl="SDL2-$OD_PIN_SDL_VERSION"
od_sdl_source_tree "$tmp/sdl"
od_det_tar "$tmp/sdl" "$sdl" "$tmp/SDL2.tar" "$OD_PIN_SDL_EPOCH"
od_xz "$tmp/SDL2.tar" "$out/materials/sources/SDL2.tar.xz"

# --- vendored libraries, as archived in the application source
vendored() { # name version files...
    local name="$1" version="$2"; shift 2
    mkdir -p "$tmp/vendor/$name-$version"
    cp -p "$@" "$tmp/vendor/$name-$version/"
    od_det_tar "$tmp/vendor" "$name-$version" "$tmp/$name.tar" "$epoch"
    od_xz "$tmp/$name.tar" "$out/materials/sources/$name.tar.xz"
}
cjson_version="$(sed -n 's/^#define CJSON_VERSION_\(MAJOR\|MINOR\|PATCH\) \([0-9]*\).*/\2/p' "$app/external/cJSON/cJSON.h" | paste -sd.)"
stbi_version="$(sed -n '1s/.*stb_image - v\([0-9.]*\).*/\1/p' "$app/external/stb/stb_image.h")"
stbw_version="$(sed -n '1s/.*stb_image_write - v\([0-9.]*\).*/\1/p' "$app/external/stb/stb_image_write.h")"
stbt_version="$(sed -n '1s/.*stb_truetype.h - v\([0-9.]*\).*/\1/p' "$app/external/stb/stb_truetype.h")"
inter_version="$(tr -d ' \r\n' < "$app/assets/fonts/VERSION")"
[ -n "$cjson_version" ] && [ -n "$stbi_version" ] && [ -n "$stbw_version" ] && [ -n "$stbt_version" ] && [ -n "$inter_version" ] || od_die "cannot read vendored versions"
vendored cJSON "$cjson_version" "$app/external/cJSON/cJSON.c" "$app/external/cJSON/cJSON.h" "$app/external/cJSON/LICENSE"
vendored stb_image "$stbi_version" "$app/external/stb/stb_image.h"
vendored stb_image_write "$stbw_version" "$app/external/stb/stb_image_write.h"
vendored stb_truetype "$stbt_version" "$app/external/stb/stb_truetype.h"
# C6.48: the Inter fonts shipped under share/opendana/fonts (SIL OFL 1.1)
vendored Inter "$inter_version" "$app/assets/fonts/Inter-Regular.ttf" "$app/assets/fonts/Inter-SemiBold.ttf" \
    "$app/assets/fonts/OFL.txt" "$app/assets/fonts/VERSION"

# --- notices and relink materials, from the archived commit
cp -p "$app"/packaging/notices/*.txt "$app/packaging/notices/$platform"/*.txt "$out/materials/notices/"
mkdir "$tmp/relink-materials"
for f in "$app/packaging/relink/$platform"/*; do
    [ "$(basename "$f")" = RELINK.txt ] || cp -p "$f" "$tmp/relink-materials/"
done
od_det_tar "$tmp" relink-materials "$tmp/relink-materials.tar" "$epoch"
od_xz "$tmp/relink-materials.tar" "$out/materials/sources/relink-materials.tar.xz"
cp -p "$app/packaging/relink/$platform/RELINK.txt" "$out/materials/RELINK.txt"

python3 - "$out/application-source.json" "$commit" "$prefix" "$cjson_version" "$stbi_version" \
    "$stbw_version" "$stbt_version" "$inter_version" "${#flagged[@]}" "${flagged[@]}" <<'PY'
import json, sys
out, commit, prefix, cjson, stbi, stbw, stbt, inter, count, *flagged = sys.argv[1:]
json.dump({'commit': commit, 'prefix': prefix,
           'versions': {'cJSON': cjson, 'stb_image': stbi, 'stb_image_write': stbw,
                        'stb_truetype': stbt, 'Inter': inter},
           'excluded_by_auditor': flagged[:int(count)]},
          open(out, 'w'), indent=2, sort_keys=True)
PY
rm -rf "$tmp"
echo "sources ready: $out/materials/sources"
ls -l "$out/materials/sources"
