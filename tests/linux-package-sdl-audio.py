#!/usr/bin/env python3
"""C6.93: the SDL2 library of a Linux package offers the sound-server audio drivers.

    python3 -I tests/linux-package-sdl-audio.py PACKAGE_ROOT   # unpacked TAR, or an AppDir (usr/lib)
    python3 -I tests/linux-package-sdl-audio.py --control      # no inputs: positive and negative controls

The bundled libSDL2-2.0.so.0 is loaded and asked for its audio drivers; it must offer
`pulseaudio`, `pipewire` and `alsa`. libpulse and libpipewire must not appear in its DT_NEEDED
(they are loaded at run time, so a system without them still starts). A library built without
the development packages lacks those drivers and the game then reaches the sound server only
through the ALSA plug-in, or not at all.
"""
import ctypes
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REQUIRED = ('pulseaudio', 'pipewire', 'alsa')
FORBIDDEN_NEEDED = re.compile(r'^lib(pulse|pipewire)')
LIB = 'libSDL2-2.0.so.0'


def find_library(root):
    for candidate in (root / 'lib' / LIB, root / 'usr' / 'lib' / LIB):
        if candidate.is_file():
            return candidate
    return None


def check_library(lib):
    """Return (errors, drivers) for one library file."""
    errors = []
    dynamic = subprocess.run(['readelf', '-d', str(lib)], env={'LC_ALL': 'C', 'PATH': '/usr/bin:/bin'},
                             capture_output=True, text=True, check=True).stdout
    needed = re.findall(r'\(NEEDED\).*\[([^]]+)\]', dynamic)
    for name in needed:
        if FORBIDDEN_NEEDED.match(name):
            errors.append(f'{lib.name} links {name} at load time (DT_NEEDED): it must be loaded at run time')
    try:
        handle = ctypes.CDLL(str(lib))
    except OSError as error:
        return errors + [f'{lib.name} cannot be loaded: {error}'], []
    handle.SDL_GetAudioDriver.restype = ctypes.c_char_p
    handle.SDL_GetAudioDriver.argtypes = [ctypes.c_int]
    drivers = [handle.SDL_GetAudioDriver(i).decode() for i in range(handle.SDL_GetNumAudioDrivers())]
    for name in REQUIRED:
        if name not in drivers:
            errors.append(f'{lib.name} does not offer the {name} audio driver (has: {" ".join(drivers) or "none"})')
    return errors, drivers


def run(root):
    lib = find_library(Path(root))
    if lib is None:
        print(f'FAIL: no lib/{LIB} under {root}')
        return 1
    errors, drivers = check_library(lib)
    print(f'{lib}: audio drivers {" ".join(drivers)}')
    for error in errors:
        print('FAIL:', error)
    if not errors:
        print('PASS: pulseaudio, pipewire and alsa offered; no libpulse/libpipewire in DT_NEEDED')
    return 1 if errors else 0


def control():
    """Synthetic libraries: one good, one per defect. Needs a C compiler, nothing else."""
    template = '''
#include <stddef.h>
static const char *drivers[] = {%s};
int SDL_GetNumAudioDrivers(void) { return (int)(sizeof(drivers) / sizeof(drivers[0])); }
const char *SDL_GetAudioDriver(int i) { return drivers[i]; }
%s
'''
    good = '"pulseaudio", "alsa", "jack", "pipewire", "disk", "dummy"'
    cases = [
        ('complete', good, False, True),
        ('no pipewire', '"pulseaudio", "alsa", "jack", "disk", "dummy"', False, False),
        ('only alsa, jack, disk, dummy (the v0.2.1 defect)', '"alsa", "jack", "disk", "dummy"', False, False),
        ('no alsa', '"pulseaudio", "pipewire", "disk", "dummy"', False, False),
        ('libpulse in DT_NEEDED', good, True, False),
    ]
    with tempfile.TemporaryDirectory(prefix='opendana-sdl-audio.') as temp:
        temp = Path(temp)
        stub = temp / 'stub.c'
        stub.write_text('int pa_stub(void) { return 0; }\n')
        pulse = temp / 'libpulse.so.0'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wl,-soname,libpulse.so.0', '-o', str(pulse), str(stub)], check=True)
        failed = False
        for n, (label, driver_list, link_pulse, should_pass) in enumerate(cases):
            root = temp / f'case{n}'
            (root / 'lib').mkdir(parents=True)
            source = root / 'lib.c'
            source.write_text(template % (driver_list, 'int pa_use(void) { extern int pa_stub(void); return pa_stub(); }'
                                          if link_pulse else ''))
            cmd = ['cc', '-shared', '-fPIC', f'-Wl,-soname,{LIB}', '-o', str(root / 'lib' / LIB), str(source)]
            if link_pulse:
                shutil.copy(pulse, root / 'lib' / pulse.name)
                cmd += [str(pulse), '-Wl,--no-as-needed', '-Wl,-rpath,$ORIGIN']
            subprocess.run(cmd, check=True)
            errors, _ = check_library(root / 'lib' / LIB)
            ok = (not errors) == should_pass
            print(f'{"ok" if ok else "FAIL"}: control "{label}": {"accepted" if not errors else "rejected"}'
                  f'{"" if not errors else " (" + errors[0] + ")"}')
            failed |= not ok
    if failed:
        return 1
    print(f'linux-package-sdl-audio: PASS (1 positive, {len(cases) - 1} negative controls)')
    return 0


if __name__ == '__main__':
    if sys.argv[1:] == ['--control']:
        sys.exit(control())
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(run(sys.argv[1]))
