#!/usr/bin/env python3
"""C6.9/C6.30: inspect an unpacked Linux package, relocated to a private temp dir.

Closed inventory (binary, bundled SDL, README/LICENSE/NOTICE and the licence
bundle with every notice and corresponding source), the bundle's own SHA256SUMS,
the notice texts that must be present, RUNPATH/SONAME/ldd resolution inside the
package and, with --commit, the commit binding (--version and SOURCES.json).
--appdir inspects an extracted AppImage (usr/, AppRun, desktop, icon; C6.50: the
runtime components' notices and sources; no test marker allowed). Inventory is not the ROM auditor; --version is a loader
smoke test, never evidence that import or a game route works.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

DOC = 'share/doc/opendana'
LB = DOC + '/license-bundle'
EXPECTED = {'bin/opendana', 'lib/libSDL2-2.0.so.0', DOC + '/README.md', DOC + '/LICENSE', DOC + '/NOTICE'} | {
    LB + '/' + n for n in ['README.txt', 'SOURCES.json', 'BUILD-IDENTITY.json', 'INPUT-MANIFEST.json',
                           'RELINK.txt', 'SHA256SUMS']} | {
    LB + '/notices/%s.txt' % n for n in ['application', 'SDL2', 'SDL2-embedded', 'cJSON', 'stb_image',
                                         'stb_image_write', 'stb_truetype', 'Inter']} | {
    LB + '/sources/%s.tar.xz' % n for n in ['application', 'SDL2', 'cJSON', 'stb_image', 'stb_image_write',
                                            'stb_truetype', 'Inter', 'relink-materials']} | {
    # C6.48: the first-run screen's fonts (with their licence) and icon, loaded at run time
    'share/opendana/fonts/Inter-Regular.ttf', 'share/opendana/fonts/Inter-SemiBold.ttf',
    'share/opendana/fonts/OFL.txt', 'share/opendana/ui/opendana-128.png'} | {
    # C6.42/C6.44 freedesktop icons (this list had not been updated for them)
    'share/icons/hicolor/%s/apps/opendana.png' % s
    for s in ['16x16', '24x24', '32x32', '48x48', '64x64', '128x128', '256x256', '512x512']} | {
    'share/icons/hicolor/scalable/apps/opendana.svg'}
# C6.50: the AppImage bundle adds the Type-2 runtime and its statically linked libraries.
RUNTIME_COMPONENTS = {'type2-runtime': 'tar.gz', 'libfuse': 'tar.xz', 'squashfuse': 'tar.gz', 'zstd': 'tar.xz',
                      'zlib': 'tar.xz', 'mimalloc': 'tar.gz', 'musl': 'tar.gz'}
NOTICE_TEXTS = {'SDL2-embedded': ['Sun Microsystems', 'Signal 11 Software', 'Adrien Descamps', 'Jamey Sharp',
                                  'Copyright 2007 Red Hat', 'wayland-protocols', 'Khronos Group'],
                'application': ['GPL-3.0-or-later'], 'cJSON': ['Dave Gamble'], 'SDL2': ['Sam Lantinga'],
                'Inter': ['SIL OPEN FONT LICENSE Version 1.1', 'Inter Project Authors'],
                'stb_truetype': ['Sean Barrett']}


def inspect(root, env, appdir=False, commit=None):
    errors = []
    expected = set(EXPECTED)
    runtime_root = root / 'usr' if appdir else root
    if appdir:
        expected = {'usr/' + name for name in expected}
        expected |= {'AppRun', 'opendana.desktop', 'opendana.png', '.DirIcon'}
        expected |= {'usr/%s/notices/%s.txt' % (LB, n) for n in RUNTIME_COMPONENTS}
        expected |= {'usr/%s/sources/%s.%s' % (LB, n, ext) for n, ext in RUNTIME_COMPONENTS.items()}
    actual = {str(p.relative_to(root)) for p in root.rglob('*') if p.is_file() or p.is_symlink()}
    for name in sorted(actual):
        p = root / name
        if not p.resolve().is_relative_to(root.resolve()):
            errors.append(f'external symlink: {name}')
    if actual != expected:
        errors.append(f'inventory: missing={sorted(expected - actual)}, unexpected={sorted(actual - expected)}')
    bundle = runtime_root / LB
    sums = bundle / 'SHA256SUMS'
    if sums.is_file():
        for line in sums.read_text().splitlines():
            digest, name = line.split('  ', 1)
            target = bundle / name
            if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != digest:
                errors.append(f'licence bundle SHA256SUMS mismatch: {name}')
    texts_by_notice = dict(NOTICE_TEXTS)
    if appdir:
        texts_by_notice.update({'libfuse': ['GNU LESSER GENERAL PUBLIC LICENSE', 'Version 2.1, February 1999', 'BECAUSE IT IS LINKED STATICALLY'],
                                'type2-runtime': ['probonopd'], 'zstd': ['BSD License'], 'zlib': ['Jean-loup Gailly'],
                                'mimalloc': ['Daan Leijen'], 'musl': ['Rich Felker'], 'squashfuse': ['Dave']})
    for notice, texts in texts_by_notice.items():
        path = bundle / 'notices' / (notice + '.txt')
        content = path.read_text() if path.is_file() else ''
        errors += [f'notices/{notice}.txt lacks {t!r}' for t in texts if t not in content]
    licence = runtime_root / DOC / 'LICENSE'
    if not licence.is_file() or 'GNU GENERAL PUBLIC LICENSE' not in licence.read_text():
        errors.append('LICENSE is not the GPL text')
    if commit:
        sources = json.loads((bundle / 'SOURCES.json').read_text())
        if sources.get('application_commit') != commit:
            errors.append(f"SOURCES.json is of {sources.get('application_commit')}, not {commit}")
    binary = runtime_root / 'bin/opendana'
    if not binary.is_file():
        return errors + ['missing bin/opendana']
    dynamic = subprocess.run(['readelf', '-d', str(binary)], env=env, capture_output=True, text=True,
                             check=True).stdout
    needed = re.findall(r'\(NEEDED\).*\[([^]]+)\]', dynamic)
    rpaths = re.findall(r'\((?:RUNPATH|RPATH)\).*\[([^]]+)\]', dynamic)
    if rpaths != ['$ORIGIN/../lib']:
        errors.append(f'nonportable runtime path: {rpaths}')
    sdl = [n for n in needed if n.startswith('libSDL2')]
    if sdl != ['libSDL2-2.0.so.0']:
        errors.append(f'unexpected SDL DT_NEEDED: {sdl}')
    bundled = runtime_root / 'lib/libSDL2-2.0.so.0'
    if bundled.is_file():
        lib_dynamic = subprocess.run(['readelf', '-d', str(bundled)], env=env, capture_output=True,
                                     text=True, check=True).stdout
        if re.findall(r'\(SONAME\).*\[([^]]+)\]', lib_dynamic) != sdl:
            errors.append('bundled SDL SONAME does not match DT_NEEDED')
    result = subprocess.run(['ldd', str(binary)], env=env, capture_output=True, text=True)
    if result.returncode or 'not found' in result.stdout:
        errors.append('unresolved dependencies')
    match = re.search(r'libSDL2-2\.0\.so\.0\s+=>\s+(.+?)\s+\(', result.stdout)
    if not match or Path(match.group(1)).resolve() != bundled.resolve():
        errors.append('SDL resolution is not the bundled lib (host fallback)')
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--appdir', action='store_true', help='inspect an extracted AppImage')
    parser.add_argument('--version', action='store_true', help='execute only --version on this trusted binary')
    parser.add_argument('--commit', help='expected commit of the binary and its source archive')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='opendana-linux-package-check.') as temp:
        temp = Path(temp)
        relocated = temp / 'relocated'
        shutil.copytree(args.root, relocated, symlinks=True)
        home = temp / 'home'
        home.mkdir()
        env = os.environ.copy()
        env.update(LD_LIBRARY_PATH='', LD_PRELOAD='', LC_ALL='C', HOME=str(home),
                   XDG_CONFIG_HOME=str(home / 'config'), XDG_DATA_HOME=str(home / 'data'),
                   XDG_CACHE_HOME=str(home / 'cache'))
        errors = inspect(relocated, env, appdir=args.appdir, commit=args.commit)
        if (args.version or args.commit) and not errors:
            executable = relocated / 'AppRun' if args.appdir else relocated / 'bin/opendana'
            result = subprocess.run([str(executable), '--version'], cwd=temp, env=env, capture_output=True,
                                    text=True, timeout=15)
            print(result.stdout.strip(), f'(--version exit={result.returncode})')
            if result.returncode:
                errors.append('--version failed')
            if args.commit and not result.stdout.strip().endswith(f'(commit {args.commit})'):
                errors.append(f'--version is not of commit {args.commit}')
        for error in errors:
            print('FAIL:', error)
        if not errors:
            print('PASS: relocated layout/inventory/notices/ELF/SDL' + (' + commit' if args.commit else '')
                  + ' (not game acceptance)')
        return bool(errors)


if __name__ == '__main__':
    raise SystemExit(main())
