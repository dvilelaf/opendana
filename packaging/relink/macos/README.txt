SDL2 relink materials - OpenDana (macOS .dmg)
============================================================

The package contains no LGPL component. SDL2 2.32.10 (Zlib) is the only third-party
library; OpenDana.app/Contents/MacOS/opendana loads it through the run path
@executable_path/../Frameworks (Contents/Frameworks/libSDL2-2.0.0.dylib, install name
@rpath/libSDL2-2.0.0.dylib), so an ABI-compatible SDL2 2.x universal dylib can replace it
in place (re-sign the app afterwards, see RELINK.txt).

Reproduce the shipped libSDL2-2.0.0.dylib
-----------------------------------------
1. Unpack the corresponding source shipped in the license bundle:
       tar -xJf ../sources/SDL2.tar.xz       # SDL2-2.32.10/
2. Verify the source revision:
       cat SDL2-2.32.10/REVISION.txt         # release-2.32.10-0-g5d24957
3. On macOS with Xcode (the shipped one was built with Xcode 26.5, AppleClang 21) and CMake
   3.30, build it exactly as the shipped library was built (same recipe as
   packaging/build-sdl2.sh --target macos in sources/application.tar.xz):
       bash build-sdl.sh SDL2-2.32.10 "$PWD/prefix"
   flags.txt records the compiler, flags and link line, objects.txt the 185 compiled
   units and SDL_config.h is the configuration header produced by that build.

Expected result
---------------
    lib/libSDL2-2.0.0.dylib
    SHA-256 6b7102ce71827c187bfce5f89b73c1cf52b44ce445369969db630d3e6a223f75
    (universal x86_64 + arm64, stripped, without code signature)

The build uses -ffile-prefix-map and a fixed SOURCE_DATE_EPOCH and the library is stripped
and left unsigned, so the same toolchain gives the same bytes. The shipped copy additionally
carries an ad hoc code signature (one load command and the signature data in __LINKEDIT);
every section of both architectures is byte-identical to the file above.

The Zlib license imposes no relink obligation. This material is provided so the shipped
library can be rebuilt and independently verified.
