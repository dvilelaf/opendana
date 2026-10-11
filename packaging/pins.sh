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
# SDL2-embedded notice and RELINK describe this file. Audio drivers of the pinned file
# (C6.93): pulseaudio, alsa, jack, pipewire, dsp, disk, dummy; pulseaudio, pipewire, alsa
# and jack are loaded at run time (no libpulse/libpipewire in DT_NEEDED). Built against
# libpulse-dev 15.99.1, libpipewire-0.3-dev and libspa-0.2-dev 0.3.48, libasound2-dev
# 1.2.6.1 and libjack-jackd2-dev 1.9.20; two clean builds gave the same bytes.
OD_PIN_LINUX_SDL2_SHA256=3d5c61bad2a0484484ac2219ae109e4e96ae225505c59c42a33958657d4f838f

# Windows: SDL2.dll (stripped, no PE time stamp) built from the same tree by
# packaging/build-sdl2.sh --target windows
# (MinGW-w64 GCC 10-win32). packaging/notices/windows/ lists its compiled units.
OD_PIN_WINDOWS_SDL2_SHA256=17134f9b04ba24041298e788025e30f02ebaebed392b4b39a12807fe260101b3

# AppImage Type-2 runtime (C6.50): built by packaging/appimage-runtime/build.sh from the
# sources in sources.lock inside the Alpine image of toolchain.lock. Two clean builds gave
# this hash. Changing the sources, the toolchain or the build script
# changes it: re-run `build.sh --twice`, review the notices and update this line.
OD_PIN_APPIMAGE_RUNTIME_SHA256=df23e6925ed82e34e3589d915bbef4e2a5c15253f61b571e7556dd625393ee66

# macOS: libSDL2-2.0.0.dylib (universal x86_64+arm64, stripped, WITHOUT code signature),
# built on the Tahoe VM from the same source tree by packaging/build-sdl2.sh --target macos
# (Xcode 26.5 AppleClang). The package ships it ad hoc signed: signing adds a load command and
# data in __LINKEDIT but leaves every section byte-identical (tests/macos-package.py compares
# them), so this hash binds the shipped code although the shipped file's own hash differs.
OD_PIN_MACOS_SDL2_SHA256=6b7102ce71827c187bfce5f89b73c1cf52b44ce445369969db630d3e6a223f75
