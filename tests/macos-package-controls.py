#!/usr/bin/env python3
"""C6.70: positive and negative controls of tests/macos-package.py (no Apple tool, no build).

A synthetic OpenDana.app (tiny hand-made universal Mach-O files, a licence bundle that verifies) is
accepted; then one defect at a time is injected and the check must refuse it: a missing or an
unexpected file, a symbolic link, a thin binary, a missing arm64 slice, no run path, a wrong run path,
no code signature, a wrong install name, another commit, another SDL2 pin, a corrupted bundle file,
the unsigned binary that is not the bound one, a machine path inside a binary.
"""
import hashlib
import importlib.util
import json
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / 'tests/macos-package.py'
COMMIT = '0123456789abcdef0123456789abcdef01234567'
CPU_X86_64, CPU_ARM64 = 0x01000007, 0x0100000C
LC_ID_DYLIB, LC_LOAD_DYLIB, LC_RPATH, LC_CODE_SIGNATURE = 0x0D, 0x0C, 0x8000001C, 0x1D


def command(cmd, text=None, extra=b''):
    if cmd == LC_CODE_SIGNATURE:
        return struct.pack('<IIII', cmd, 16, 0, 0)
    body = text.encode() + b'\0'
    size = (12 + len(body) + 7) // 8 * 8 + 0
    payload = struct.pack('<III', cmd, size, 12) + body
    return payload + b'\0' * (size - len(payload))


def segment(offset, size):
    """LC_SEGMENT_64 __TEXT with one file-backed section __text at `offset`."""
    seg = struct.pack('<II16sQQQQIIII', 0x19, 72 + 80, b'__TEXT', 0, 0x1000, 0, 0x1000, 5, 5, 1, 0)
    sect = struct.pack('<16s16sQQIIIIIIII', b'__text', b'__TEXT', 0x100, size, offset, 4, 0, 0, 0x80000400, 0, 0, 0)
    return seg + sect


def thin(cpu, commands, tail=b'', text=b'machine code of the fixture'):
    commands = list(commands)
    offset = 32 + sum(len(c) for c in commands) + 152
    commands.append(segment(offset, len(text)))
    blob = b''.join(commands)
    header = struct.pack('<IIIIIIII', 0xFEEDFACF, cpu, 3, 2, len(commands), len(blob), 0, 0)
    return header + blob + text + tail


def fat(slices):
    out, offset = [], 4096
    entries = b''
    for data in slices:
        cpu = struct.unpack_from('<I', data, 4)[0]
        entries += struct.pack('>IIIII', cpu, 3, offset, len(data), 12)
        out.append((offset, data))
        offset += (len(data) + 4095) // 4096 * 4096
    blob = struct.pack('>II', 0xCAFEBABE, len(slices)) + entries
    blob += b'\0' * (4096 - len(blob))
    for off, data in out:
        blob += b'\0' * (off - len(blob)) + data
    return blob


def exe(rpath='@executable_path/../Frameworks', signed=True, arm=True, commit=COMMIT, leak=b'',
        text=b'machine code of the fixture'):
    def one(cpu):
        cmds = [command(LC_LOAD_DYLIB, '/usr/lib/libSystem.B.dylib'),
                command(LC_LOAD_DYLIB, '@rpath/libSDL2-2.0.0.dylib')]
        if rpath:
            cmds.append(command(LC_RPATH, rpath))
        if signed:
            cmds.append(command(LC_CODE_SIGNATURE))
        return thin(cpu, cmds, ('opendana 0.2.0 (commit %s)' % commit).encode() + leak, text=text)
    return fat([one(CPU_X86_64)] + ([one(CPU_ARM64)] if arm else []))


def lib(ident='@rpath/libSDL2-2.0.0.dylib', signed=True, text=b'machine code of the fixture'):
    def one(cpu):
        cmds = [command(LC_ID_DYLIB, ident)]
        if signed:
            cmds.append(command(LC_CODE_SIGNATURE))
        return thin(cpu, cmds, text=text)
    return fat([one(CPU_X86_64), one(CPU_ARM64)])


def build(root, **kw):
    spec = importlib.util.spec_from_file_location('macos_package', CHECK)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    app = root / 'OpenDana.app'
    unsigned = root / 'unsigned'
    unsigned.mkdir()
    built_exe, built_lib = exe(signed=False), lib(signed=False)
    (unsigned / 'opendana').write_bytes(built_exe)
    (unsigned / 'libSDL2-2.0.0.dylib').write_bytes(built_lib)
    pin = hashlib.sha256(built_lib).hexdigest()
    bundle_files = {}
    for rel in module.expected_files():
        path = app / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        if rel == 'Contents/MacOS/opendana':
            data = exe(**kw.get('exe', {}))
        elif rel == 'Contents/Frameworks/libSDL2-2.0.0.dylib':
            data = lib(**kw.get('lib', {}))
        elif rel == 'Contents/Info.plist':
            data = plistlib.dumps({'CFBundleExecutable': 'opendana', 'CFBundleIconFile': 'opendana',
                                   'CFBundlePackageType': 'APPL', 'CFBundleShortVersionString': '0.2.0'})
        elif rel == 'Contents/Resources/opendana.icns':
            data = b'icns' + b'\0' * 8
        elif rel.endswith('/LICENSE'):
            data = b'GNU GENERAL PUBLIC LICENSE\n'
        else:
            data = ('fixture of %s\n' % rel).encode()
        path.write_bytes(data)
        if rel.startswith(module.LB + '/') and not rel.endswith(('SHA256SUMS', 'BUILD-IDENTITY.json', 'SOURCES.json')):
            bundle_files[rel[len(module.LB) + 1:]] = data
    bundle = app / module.LB
    identity = {'artifacts': {'application': hashlib.sha256(built_exe).hexdigest(), 'SDL2': pin},
                'application_commit': COMMIT}
    identity.update(kw.get('identity', {}))
    (bundle / 'BUILD-IDENTITY.json').write_text(json.dumps(identity))
    (bundle / 'SOURCES.json').write_text(json.dumps({'application_commit': COMMIT, 'mode': 'macos'}))
    sums = ''
    for name in sorted(p.relative_to(bundle).as_posix() for p in bundle.rglob('*') if p.is_file()):
        if name != 'SHA256SUMS':
            sums += hashlib.sha256((bundle / name).read_bytes()).hexdigest() + '  ' + name + '\n'
    (bundle / 'SHA256SUMS').write_text(sums)
    return app, pin, unsigned


def check(app, pin, unsigned, commit=COMMIT, expect=True, needle=None):
    result = subprocess.run([sys.executable, str(CHECK), str(app), commit, pin, str(unsigned)],
                            capture_output=True, text=True)
    print(('ACCEPT ' if result.returncode == 0 else 'REJECT ') + (result.stdout.strip().splitlines() or [''])[-1]
          + (result.stderr.strip() and ' ' + result.stderr.strip().splitlines()[-1]))
    assert (result.returncode == 0) == expect, result
    if needle:
        assert needle in result.stderr, (needle, result.stderr)


def variant(name, needle, mutate=None, commit=COMMIT, pin=None, **kw):
    with tempfile.TemporaryDirectory(prefix='opendana-macos-package-controls.') as tmp:
        app, real_pin, unsigned = build(Path(tmp), **kw)
        if mutate:
            mutate(app, unsigned)
        check(app, pin or real_pin, unsigned, commit=commit, expect=False, needle=needle)
    return name


def main():
    with tempfile.TemporaryDirectory(prefix='opendana-macos-package-controls.') as tmp:
        app, pin, unsigned = build(Path(tmp))
        check(app, pin, unsigned)

    def rm(rel):
        return lambda app, _u: (app / rel).unlink()

    def extra(app, _u):
        (app / 'Contents/Resources/extra.nes').write_bytes(b'x')

    def link(app, _u):
        (app / 'Contents/link').symlink_to('/etc/passwd')

    def corrupt(app, _u):
        (app / 'Contents/Resources/doc/license-bundle/RELINK.txt').write_text('changed\n')

    def other_unsigned(app, unsigned):
        (unsigned / 'opendana').write_bytes(b'another build')

    def swapped_code(app, _u):
        # a shipped executable whose code differs from the bound build (signature or not)
        (app / 'Contents/MacOS/opendana').write_bytes(exe(text=b'MACHINE CODE OF ANOTHER BUILD!!'))

    variant('missing file', 'inventory differs', rm('Contents/Resources/doc/license-bundle/notices/SDL2.txt'))
    variant('unexpected file', 'inventory differs', extra)
    variant('symbolic link', 'symbolic link', link)
    variant('thin binary', 'not universal', exe={'arm': False})
    variant('no run path', 'run path', exe={'rpath': None})
    variant('absolute run path', 'run path', exe={'rpath': '/Users/someone/lib'})
    variant('unsigned executable', 'no code signature', exe={'signed': False})
    variant('wrong install name', 'install name', lib={'ident': '/usr/local/lib/libSDL2.dylib'})
    variant('unsigned library', 'no code signature', lib={'signed': False})
    variant('other commit', 'does not contain', exe={'commit': 'f' * 40})
    variant('machine path in a binary', 'machine path', exe={'leak': b'/Users/builder/src/x.c'})
    variant('another SDL2 pin', 'another SDL2', pin='0' * 64)
    variant('identity of another commit', 'another commit', identity={'application_commit': 'f' * 40})
    variant('corrupted bundle file', 'does not match SHA256SUMS', corrupt)
    variant('unsigned binary that is not the bound one', 'not the unsigned file', other_unsigned)
    variant('shipped code differs from the bound build', 'is not the bound build', swapped_code)
    variant('shipped library code differs', 'is not the bound build',
            lambda app, _u: (app / 'Contents/Frameworks/libSDL2-2.0.0.dylib').write_bytes(lib(text=b'other library code....')))
    print('PASS: macos-package controls: 1 positive, 17 negatives rejected')


if __name__ == '__main__':
    main()
