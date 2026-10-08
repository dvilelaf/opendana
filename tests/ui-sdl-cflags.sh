#!/usr/bin/env bash
# Print the compiler flags for the SDL2 headers (only headers: the UI
# gates stub the few SDL calls they link). Sources, in order: $SDL2_CONFIG,
# sdl2-config on PATH, pkg-config sdl2, the SDL2_DIR recorded by CMake in
# $BUILD_DIR/CMakeCache.txt. Exit 2 (BLOCKED) when none is found.
set -euo pipefail
b="${BUILD_DIR:-build}"
if [ -n "${SDL2_CONFIG:-}" ]; then "$SDL2_CONFIG" --cflags; exit 0; fi
if command -v sdl2-config >/dev/null 2>&1; then sdl2-config --cflags; exit 0; fi
if pkg-config --exists sdl2 2>/dev/null; then pkg-config --cflags sdl2; exit 0; fi
if [ -f "$b/CMakeCache.txt" ]; then
    dir="$(sed -n 's/^SDL2_DIR:PATH=//p' "$b/CMakeCache.txt")"
    if [ -n "$dir" ] && [ -f "$dir/../../../include/SDL2/SDL.h" ]; then
        prefix="$(cd "$dir/../../.." && pwd)"
        echo "-I$prefix/include/SDL2 -D_REENTRANT"
        exit 0
    fi
fi
if [ -f "$b/CMakeCache.txt" ]; then
    # C6.70: a macOS build against a framework (-DOD_SDL2_FRAMEWORK=...)
    fw="$(sed -n 's/^OD_SDL2_FRAMEWORK:PATH=//p' "$b/CMakeCache.txt")"
    if [ -n "$fw" ] && [ -f "$fw/Headers/SDL.h" ]; then
        echo "-I$fw/Headers -F$fw/.. -D_REENTRANT"
        exit 0
    fi
fi
echo "BLOCKED: no SDL2 headers (set SDL2_CONFIG or install sdl2-config)" >&2
exit 2
