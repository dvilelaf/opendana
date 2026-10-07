# CMake toolchain for the Windows cross build (C5.11).
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=packaging/mingw-w64.cmake \
#             -DMINGW_SDL2_DIR=<SDL2 MinGW prefix (packaging/build-sdl2.sh --target windows)> ...
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(MINGW_PREFIX "x86_64-w64-mingw32" CACHE STRING "MinGW-w64 target triplet")
set(CMAKE_C_COMPILER   "${MINGW_PREFIX}-gcc")
set(CMAKE_RC_COMPILER  "${MINGW_PREFIX}-windres")
# POSIX-only calls must fail at compilation, not survive as implicit declarations
# until the final link. Existing unrelated warnings remain visible.
set(CMAKE_C_FLAGS_INIT "-Werror=implicit-function-declaration")
# C6.30: no link time stamp in the PE header, so the same commit and toolchain
# give the same opendana.exe (reproducible ZIP).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--no-insert-timestamp")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-Wl,--no-insert-timestamp")

# Let find_package look inside the MinGW tree and the SDL2 dev package only.
set(CMAKE_FIND_ROOT_PATH "/usr/${MINGW_PREFIX}" "${MINGW_SDL2_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
