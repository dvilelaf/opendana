# Sourced by the level-1 test scripts that compile product sources by hand (C6.70).
# It carries the few differences of macOS (Darwin) from Linux:
#   od_plat_cflags  feature macro that Darwin needs next to _POSIX_C_SOURCE/_XOPEN_SOURCE so
#                   that st_mtimespec, O_NOFOLLOW, mkdtemp... stay visible (CMake sets the same
#                   definition for the product on Apple). Empty elsewhere: use it UNQUOTED.
#   od_is_darwin    1 on macOS, else 0.
# macOS has bash 3.2 and ld64: no mapfile, no empty-array expansion under `set -u`, no --wrap.
# Where a test needs --wrap, the script renames the wrapped definition at compile time instead
# (see tests/editor-ui-layout-run.sh), which links the same two functions the same way.
if [ "$(uname -s)" = Darwin ]; then
    od_is_darwin=1
    od_plat_cflags="-D_DARWIN_C_SOURCE"
else
    od_is_darwin=0
    od_plat_cflags=""
fi
