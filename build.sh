#!/usr/bin/env bash
# Build OpenDana. SDL2 comes from SDL2_DIR (an installed SDL2
# prefix) when it is set, otherwise CMake searches the system (e.g. libsdl2-dev).
# Nothing is downloaded and no root privileges are needed. No machine-specific
# path is assumed (C6.30): without SDL2_DIR and without a system SDL2 the build
# stops with an error instead of silently skipping the game.
set -euo pipefail

cd "$(dirname "$0")"

SDL2_DIR="${SDL2_DIR:-}"
BUILD_DIR="${BUILD_DIR:-build}"

# Portable job count: nproc is Linux-only, hw.ncpu covers macOS/BSD.
if [ -z "${JOBS:-}" ]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
    else
        JOBS=2
    fi
fi

cmake_args=(-S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release)
if [ -n "$SDL2_DIR" ]; then
    if [ ! -d "$SDL2_DIR" ]; then
        echo "error: SDL2_DIR=$SDL2_DIR is not a directory (an installed SDL2 prefix)" >&2
        exit 1
    fi
    cmake_args+=(-DCMAKE_PREFIX_PATH="$SDL2_DIR")
fi

cmake "${cmake_args[@]}" >/dev/null

cmake --build "$BUILD_DIR" -j"$JOBS"

if [ ! -x "$BUILD_DIR/opendana" ]; then
    echo "error: SDL2 development files not found, so $BUILD_DIR/opendana was not built." >&2
    echo "       Set SDL2_DIR to an installed SDL2 prefix (the packages use SDL 2.32.10)," >&2
    echo "       e.g.  SDL2_DIR=/path/to/sdl2-install bash build.sh" >&2
    exit 1
fi

echo
echo "built: $BUILD_DIR/opendana (game), $BUILD_DIR/opendana-dev (harness) and $BUILD_DIR/opendana_tests_public"
echo "private comparators: OD_PRIVATE_ROOT=<private data repo> bash tests/public-tests-private-run.sh <category>"
echo "run:   ./$BUILD_DIR/opendana"
echo "dev:   ./$BUILD_DIR/opendana-dev --level 0  "
