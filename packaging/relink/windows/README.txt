SDL2 relink materials - OpenDana (Windows ZIP)
============================================================

The ZIP contains no LGPL component. SDL2 2.32.10 (Zlib) is the only
third-party DLL; opendana.exe loads it from its own directory (bin\SDL2.dll), so an
ABI-compatible SDL2.dll can replace it in place.

Reproduce the shipped SDL2.dll
------------------------------
1. Unpack the corresponding source shipped in the license bundle:
       tar -xJf ../sources/SDL2.tar.xz       # SDL2-2.32.10/
2. Verify the source revision:
       cat SDL2-2.32.10/REVISION.txt         # release-2.32.10-0-g5d24957
3. Cross-compile it from Linux with MinGW-w64 exactly as the shipped DLL was
   built (same recipe as packaging/build-sdl2.sh in sources/application.tar.xz):
       bash build-sdl.sh SDL2-2.32.10 "$PWD/prefix"
   flags.txt records the compiler, flags and link line; SDL_config.h is the
   configuration header produced by that build.

Expected result
---------------
    bin/SDL2.dll
    SHA-256 17134f9b04ba24041298e788025e30f02ebaebed392b4b39a12807fe260101b3
    SDL_GetRevision(): SDL-release-2.32.10-0-g5d24957

The build uses -Wl,--no-insert-timestamp (PE time stamp 0), -ffile-prefix-map
and a fixed SOURCE_DATE_EPOCH, and the DLL is stripped, so the same toolchain
gives the same bytes.

The Zlib license imposes no relink obligation. This material is provided so
the shipped DLL can be rebuilt and independently verified.
