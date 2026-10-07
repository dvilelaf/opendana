#!/usr/bin/env bash
# Shared helpers for the packaging scripts (docs/PLAN-V2.md §9.2, C6.30).
# Sourced, never executed directly. No machine-specific paths live here: every
# external input is an environment variable checked with a clear error.

set -euo pipefail

# Repository root (the directory that holds CMakeLists.txt).
od_repo_root() {
    local here
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd "$here/.." && pwd
}

# Project version, read from the single source of truth (CMakeLists.txt).
od_version() {
    local root
    root="$(od_repo_root)"
    sed -n 's/.*project([^)]*VERSION[[:space:]]\+\([0-9][0-9.]*\).*/\1/p' \
        "$root/CMakeLists.txt" | head -1
}

OD_ROOT="$(od_repo_root)"
OD_VERSION="$(od_version)"
OD_BUILD_DIR="${BUILD_DIR:-build}"
OD_DIST_DIR="${DIST_DIR:-$OD_BUILD_DIR/dist}"
# shellcheck source=packaging/pins.sh
source "$OD_ROOT/packaging/pins.sh"

od_die() { echo "error: $*" >&2; exit 1; }

# od_require_env VAR "what it must point at"
od_require_env() {
    local name="$1" what="$2"
    if [ -z "${!name:-}" ]; then
        od_die "$name is not set: point it at $what"
    fi
}

od_require_tools() {
    local tool
    for tool in "$@"; do
        command -v "$tool" >/dev/null 2>&1 || od_die "required tool not found: $tool (nothing is installed or downloaded)"
    done
}

od_sha256() { sha256sum -- "$1" | cut -d' ' -f1; }

# od_check_pin FILE EXPECTED "what"
od_check_pin() {
    local actual
    actual="$(od_sha256 "$1")"
    if [ "$actual" != "$2" ]; then
        od_die "$3: SHA-256 mismatch for $1 (expected $2, got $actual); see packaging/pins.sh"
    fi
}

# Portable job count (Linux nproc, macOS/BSD sysctl).
od_jobs() {
    if [ -n "${JOBS:-}" ]; then echo "$JOBS"; return; fi
    if command -v nproc >/dev/null 2>&1; then nproc
    elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.ncpu 2>/dev/null || echo 2
    else echo 2; fi
}

# Private work directory outside the repository (the ROM auditor refuses a scan
# directory that git could treat as part of a work tree).
od_workdir() {
    mktemp -d "${TMPDIR:-/tmp}/$1.XXXXXX"
}

# Paths (git pathspecs) that form the application's corresponding source.
od_source_paths() {
    sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d' "$OD_ROOT/packaging/application-source.list"
}

# The commit the packages are built from: OD_SOURCE_COMMIT if given, else HEAD,
# which must then have no uncommitted change in the source paths (the package is
# built from `git archive`, so uncommitted edits would silently not be shipped).
od_source_commit() {
    local commit paths
    if [ -n "${OD_SOURCE_COMMIT:-}" ]; then
        commit="$(git -C "$OD_ROOT" rev-parse --verify --quiet "${OD_SOURCE_COMMIT}^{commit}")" \
            || od_die "OD_SOURCE_COMMIT=$OD_SOURCE_COMMIT is not a commit of this repository"
    else
        commit="$(git -C "$OD_ROOT" rev-parse --verify HEAD)"
        mapfile -t paths < <(od_source_paths)
        if [ -n "$(git -C "$OD_ROOT" status --porcelain --untracked-files=no -- "${paths[@]}")" ]; then
            git -C "$OD_ROOT" status --short --untracked-files=no -- "${paths[@]}" >&2
            od_die "uncommitted changes in the application source paths; commit them or set OD_SOURCE_COMMIT"
        fi
    fi
    echo "$commit"
}

# Reproducible archive of DIR/MEMBER into OUT (uncompressed tar): sorted names,
# numeric root owner, fixed mtime and normalized modes (u=rwX,go=rX), so the
# bytes never depend on the umask, the user or the file system order.
od_det_tar() {
    local parent="$1" member="$2" out="$3" epoch="${4:-${SOURCE_DATE_EPOCH:-$OD_PIN_SDL_EPOCH}}"
    LC_ALL=C tar --sort=name --format=gnu --owner=0 --group=0 --numeric-owner \
        --mtime="@$epoch" --mode='u=rwX,go=rX' --hard-dereference \
        -cf "$out" -C "$parent" "$member"
}

# Deterministic compressors (single-threaded xz, gzip without name/time).
od_xz() { xz -9 -T1 --check=crc64 -c -- "$1" > "$2"; }
od_gzip() { gzip -9 -n -c -- "$1" > "$2"; }

# SHA-256 sidecar for every package produced in $1 (kept for build-macos.sh).
od_write_checksums() {
    local dir="$1" out="${2:-SHA256SUMS}"
    ( cd "$dir" && sha256sum -- * > "$out" 2>/dev/null || true )
    [ -f "$dir/$out" ] && { echo "checksums: $dir/$out"; cat "$dir/$out"; }
}

# C6.51: the pinned SDL source tree, prepared once for the source archive and for
# the SDL build, so the binary is built from the very tree the licence bundle ships.
# od_sdl_source_tree DEST  ->  DEST/SDL2-<ver>/ (git archive of the pinned commit,
# the two android-project-ant links replaced by copies, REVISION.txt).
od_sdl_source_tree() {
    local dest="$1" sdl="SDL2-$OD_PIN_SDL_VERSION" links
    od_require_env SDL2_SRC "the SDL git tree that contains commit $OD_PIN_SDL_COMMIT (read only)"
    git -C "$SDL2_SRC" cat-file -e "${OD_PIN_SDL_COMMIT}^{commit}" 2>/dev/null \
        || od_die "SDL2_SRC=$SDL2_SRC does not contain SDL commit $OD_PIN_SDL_COMMIT"
    mkdir -p "$dest"
    git -C "$SDL2_SRC" archive --format=tar --prefix="$sdl/" "$OD_PIN_SDL_COMMIT" | tar -xf - -C "$dest"
    mapfile -t links < <(cd "$dest" && find "$sdl" -type l | LC_ALL=C sort)
    [ "${links[*]}" = "$sdl/android-project-ant/AndroidManifest.xml $sdl/android-project-ant/src" ] \
        || od_die "unexpected symlinks in the SDL archive: ${links[*]}"
    rm "$dest/$sdl/android-project-ant/AndroidManifest.xml" "$dest/$sdl/android-project-ant/src"
    cp -p "$dest/$sdl/android-project/app/src/main/AndroidManifest.xml" "$dest/$sdl/android-project-ant/AndroidManifest.xml"
    cp -pR "$dest/$sdl/android-project/app/src/main/java" "$dest/$sdl/android-project-ant/src"
    printf '%s\n' "$OD_PIN_SDL_REVISION" > "$dest/$sdl/REVISION.txt"
}

# ---------------------------------------------------------------------------
# ROM audit of the packages. packaging/audit-package.sh is a maintainer tool
# that is not part of every checkout. When it is present the audit is
# mandatory (and OD_ROM must point at the ROM, read only). When it is absent
# the step is skipped with a clear note and the report says "NOT-RUN"; the
# maintainers run the audit before publishing a package.
# ---------------------------------------------------------------------------
od_audit_available() { [ -f "$OD_ROOT/packaging/audit-package.sh" ]; }

od_audit_require_rom() {
    if od_audit_available; then
        od_require_env OD_ROM "your EU ROM (read only) for the ROM auditor"
    else
        echo "note: packaging/audit-package.sh is not in this checkout; the ROM audit of packages will be skipped" >&2
    fi
}

od_audit_check_rom() {
    if od_audit_available; then
        [ -f "$OD_ROM" ] || od_die "OD_ROM=$OD_ROM is not a file"
    fi
}

# od_rom_audit <same arguments as audit-package.sh>
od_rom_audit() {
    if od_audit_available; then
        bash "$OD_ROOT/packaging/audit-package.sh" "$@"
        return
    fi
    local report="" flagged="" prev="" arg
    for arg in "$@"; do
        case "$prev" in
            --report) report="$arg" ;;
            --flagged) flagged="$arg" ;;
        esac
        prev="$arg"
    done
    echo "note: ROM audit skipped (packaging/audit-package.sh is not in this checkout); this is NOT a verified package" >&2
    if [ -n "$report" ]; then
        printf '{\n  "result": "NOT-RUN",\n  "reason": "packaging/audit-package.sh not present in this checkout"\n}\n' > "$report"
    fi
    if [ -n "$flagged" ]; then : > "$flagged"; fi
    return 0
}
