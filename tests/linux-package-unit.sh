#!/usr/bin/env bash
# C6.30: packaging controls that need no ROM, no SDL and no build (seconds).
# Run by tests/run.sh for every gate. The input-dependent package checks are
# listed in docs/RELEASE.md (sources, audit, inventory, play, AppImage smoke).
set -euo pipefail
cd "$(dirname "$0")/.."
echo '== C6.30 packaging: licence gate, AppImage runtime guard, CMake controls =='
python3 tests/linux-package-license-bundle.py
python3 tests/linux-package-appimage-runtime.py
python3 tests/linux-package-appimage-gate.py
python3 tests/macos-package-controls.py
# Its controls build ELF libraries with GNU ld options (-soname): they only run on Linux.
if [ "$(uname -s)" = Linux ]; then
    python3 -I tests/linux-package-sdl-audio.py --control
else
    echo 'skipped: linux-package-sdl-audio controls (Linux only)'
fi
bash tests/linux-package-cmake.sh
# The leak test lives in the maintainers' private tree; run it only when present.
if [ -f tests/package-leaks.sh ]; then
    bash tests/package-leaks.sh --control
else
    echo 'skipped: package-leaks (maintainers only)'
fi
