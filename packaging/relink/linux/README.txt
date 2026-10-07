SDL2 relink materials - OpenDana (Linux TAR)
==========================================================

The TAR contains no LGPL component. The only third-party shared library is
SDL2 2.32.10, licensed under Zlib (permissive) and shared-substitutable.

How to replace the shipped SDL2
-------------------------------
The game resolves SDL2 through RUNPATH $ORIGIN/../lib, so an ABI-compatible
build can replace the file in place:

    cp <your-libSDL2-2.0.so.0> <package>/lib/libSDL2-2.0.so.0

The game must keep DT_NEEDED / SONAME libSDL2-2.0.so.0.

Reproduce the shipped library
-----------------------------
1. Unpack the corresponding source shipped in the license bundle:
       tar -xJf ../sources/SDL2.tar.xz       # SDL2-2.32.10/
2. Verify the source revision:
       cat SDL2-2.32.10/REVISION.txt         # release-2.32.10-0-g5d24957
3. Build with the recipe in build-sdl.sh (the SDL_config.h and per-file flags
   used for the shipped binary are in this directory). It adds
   -ffile-prefix-map (no machine path in the library), SOURCE_DATE_EPOCH and
   installs stripped:
       bash build-sdl.sh SDL2-2.32.10 "$PWD/prefix"

Expected result
---------------
The shipped shared library (built with GCC Ubuntu 11.4.0-1ubuntu1~22.04.3):

    lib/libSDL2-2.0.so.0
    SHA-256 19636a84c70c7488f5400e99d93cbf8c5821b36d363c4328ea9d74a3c3903cab
    SONAME  libSDL2-2.0.so.0
    version 2.32.10 (SDL_MAJOR/MINOR/PATCHLEVEL = 2/32/10)

A different compiler or system headers give a different (equally usable)
binary; the hash only identifies the exact shipped file.

Notes
-----
- build-sdl.sh needs no network: the corresponding source is shipped in this
  package (the same tree packaging/build-sdl2.sh builds from).
- The Zlib license imposes no relink obligation. This material is provided so
  the shipped binary can be rebuilt and independently verified.
