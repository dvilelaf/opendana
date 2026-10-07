#!/usr/bin/env bash
# C6.30: pinned identities of the third-party inputs the packages are built from.
# Sourced by packaging/common.sh. Changing a pin means the notices, relink
# materials and corresponding source of that input were reviewed again.

# SDL 2.32.10 source tree (git), used for the corresponding source and for both
# shared libraries below. OD_PIN_SDL_EPOCH is the commit time of that revision
# (used as the fixed mtime of the generated SDL source archive).
OD_PIN_SDL_VERSION=2.32.10
OD_PIN_SDL_COMMIT=5d249570393f7a37e037abf22cd6012a4cc56a71
OD_PIN_SDL_REVISION=release-2.32.10-0-g5d24957
OD_PIN_SDL_EPOCH=1756762407

# Linux: libSDL2-2.0.so.0 (stripped) built from that tree by packaging/build-sdl2.sh
# --target linux (GCC Ubuntu 11.4.0; recipe in packaging/relink/linux/). The
# SDL2-embedded notice and RELINK describe this file.
OD_PIN_LINUX_SDL2_SHA256=19636a84c70c7488f5400e99d93cbf8c5821b36d363c4328ea9d74a3c3903cab

# Windows: SDL2.dll (stripped, no PE time stamp) built from the same tree by
# packaging/build-sdl2.sh --target windows
# (MinGW-w64 GCC 10-win32). packaging/notices/windows/ lists its compiled units.
OD_PIN_WINDOWS_SDL2_SHA256=17134f9b04ba24041298e788025e30f02ebaebed392b4b39a12807fe260101b3

# AppImage Type-2 runtime (C6.50): built by packaging/appimage-runtime/build.sh from the
# sources in sources.lock inside the Alpine image of toolchain.lock. Two clean builds gave
# this hash. Changing the sources, the toolchain or the build script
# changes it: re-run `build.sh --twice`, review the notices and update this line.
OD_PIN_APPIMAGE_RUNTIME_SHA256=df23e6925ed82e34e3589d915bbef4e2a5c15253f61b571e7556dd625393ee66
