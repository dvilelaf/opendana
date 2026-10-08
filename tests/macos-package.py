#!/usr/bin/env python3
"""C6.70: inspect the contents of the macOS package (what the .dmg really holds).

    macos-package.py OpenDana.app COMMIT SDL2_PIN [UNSIGNED_DIR]

Closed inventory (the app, its Info.plist and icon, the two Mach-O files, the fonts and icon the
first-run screen loads, README/LICENSE/NOTICE and the licence bundle with every notice that
applies and the corresponding sources), then, without any Apple tool so that it runs anywhere:
  * opendana and libSDL2-2.0.0.dylib are universal Mach-O files with an x86_64 and an arm64 slice,
    both with a code signature (ad hoc);
  * opendana runs through the relative run path @executable_path/../Frameworks and needs
    @rpath/libSDL2-2.0.0.dylib; the dylib's install name is that one;
  * opendana carries "(commit COMMIT)", i.e. what `--version` prints;
  * the licence bundle verifies against its own SHA256SUMS and BUILD-IDENTITY.json binds the SDL2
    pin and, with UNSIGNED_DIR, the unsigned binaries the VM returned; the shipped (ad hoc signed)
    files have exactly the sections of those unsigned files, byte for byte (signing only adds a
    load command and signature data);
  * the Info.plist names the executable, the icon and the version; no ROM, no imported content.
The checks that need Apple tools (codesign verification, the signed files giving the bound bytes back
once the signature is removed) run on the VM in packaging/macos-remote-build.sh phase 2.
"""
import hashlib
import json
import plistlib
import struct
import sys
from pathlib import Path

DOC = 'Contents/Resources/doc'
LB = DOC + '/license-bundle'
CPU_X86_64, CPU_ARM64 = 0x01000007, 0x0100000C
LC_ID_DYLIB, LC_LOAD_DYLIB, LC_RPATH, LC_CODE_SIGNATURE = 0x0D, 0x0C, 0x8000001C, 0x1D
LC_SEGMENT_64 = 0x19
LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB = 0x80000018, 0x8000001F, 0x80000023


def fail(message):
    sys.exit('error: macos-package: ' + message)


def expected_files():
    files = ['Contents/Info.plist', 'Contents/MacOS/opendana', 'Contents/Frameworks/libSDL2-2.0.0.dylib',
             'Contents/Resources/opendana.icns', DOC + '/README.md', DOC + '/LICENSE', DOC + '/NOTICE',
             'Contents/_CodeSignature/CodeResources',
             # the first-run screen's fonts (with their licence) and icon, loaded at run time
             'Contents/share/opendana/fonts/Inter-Regular.ttf', 'Contents/share/opendana/fonts/Inter-SemiBold.ttf',
             'Contents/share/opendana/fonts/OFL.txt', 'Contents/share/opendana/ui/opendana-128.png']
    for name in ('README.txt', 'SOURCES.json', 'BUILD-IDENTITY.json', 'INPUT-MANIFEST.json', 'RELINK.txt', 'SHA256SUMS'):
        files.append(LB + '/' + name)
    for name in ('application', 'SDL2', 'SDL2-embedded', 'cJSON', 'stb_image', 'stb_image_write', 'stb_truetype', 'Inter'):
        files.append(LB + '/notices/' + name + '.txt')
    for name in ('application', 'SDL2', 'cJSON', 'stb_image', 'stb_image_write', 'stb_truetype', 'Inter',
                 'relink-materials'):
        files.append(LB + '/sources/' + name + '.tar.xz')
    return sorted(files)


def slices(data):
    """[(cputype, offset, size)] of a Mach-O (thin or fat), little-endian thin slices only."""
    magic = struct.unpack_from('>I', data, 0)[0]
    if magic == 0xCAFEBABE:                       # fat, 32-bit offsets, big-endian header
        count = struct.unpack_from('>I', data, 4)[0]
        out = []
        for i in range(count):
            cpu, _sub, off, size, _align = struct.unpack_from('>IIIII', data, 8 + 20 * i)
            out.append((cpu, off, size))
        return out
    if struct.unpack_from('<I', data, 0)[0] == 0xFEEDFACF:
        return [(struct.unpack_from('<I', data, 4)[0], 0, len(data))]
    fail('not a Mach-O file')


def load_commands(data, off):
    if struct.unpack_from('<I', data, off)[0] != 0xFEEDFACF:
        fail('slice is not a little-endian 64-bit Mach-O')
    ncmds = struct.unpack_from('<I', data, off + 16)[0]
    pos = off + 32
    commands = []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, pos)
        commands.append((cmd, data[pos:pos + size]))
        pos += size
    return commands


def cstring(blob, offset):
    end = blob.index(b'\0', offset)
    return blob[offset:end].decode()


def describe(path):
    data = path.read_bytes()
    result = {}
    for cpu, off, _size in slices(data):
        loads, rpaths, ident, signed = [], [], None, False
        for cmd, blob in load_commands(data, off):
            if cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB):
                loads.append(cstring(blob, struct.unpack_from('<I', blob, 8)[0]))
            elif cmd == LC_ID_DYLIB:
                ident = cstring(blob, struct.unpack_from('<I', blob, 8)[0])
            elif cmd == LC_RPATH:
                rpaths.append(cstring(blob, struct.unpack_from('<I', blob, 8)[0]))
            elif cmd == LC_CODE_SIGNATURE:
                signed = True
        result[cpu] = {'loads': loads, 'rpaths': rpaths, 'id': ident, 'signed': signed}
    return data, result


def sections(data):
    """{cputype: {(segment, section): bytes}} of every file-backed section of a Mach-O."""
    result = {}
    for cpu, off, _size in slices(data):
        found = {}
        for cmd, blob in load_commands(data, off):
            if cmd != LC_SEGMENT_64:
                continue
            segname = blob[8:24].rstrip(b'\0').decode()
            nsects = struct.unpack_from('<I', blob, 64)[0]
            for i in range(nsects):
                sec = blob[72 + 80 * i:72 + 80 * (i + 1)]
                sectname = sec[:16].rstrip(b'\0').decode()
                size = struct.unpack_from('<Q', sec, 40)[0]
                offset = struct.unpack_from('<I', sec, 48)[0]
                flags = struct.unpack_from('<I', sec, 64)[0]
                if flags & 0xFF in (0x1, 0xC, 0x12):        # zerofill, gb-zerofill, thread-local zerofill
                    found[(segname, sectname)] = size
                else:
                    found[(segname, sectname)] = data[off + offset:off + offset + size]
        result[cpu] = found
    return result


def same_code(shipped, built, label):
    """Signing adds one load command and the signature data in __LINKEDIT; nothing else may differ."""
    a, b = sections(shipped), sections(built)
    if set(a) != set(b):
        fail('%s: the architectures differ from the bound build' % label)
    count = 0
    for cpu in a:
        if set(a[cpu]) != set(b[cpu]):
            fail('%s: the sections of %s differ from the bound build' % (label, hex(cpu)))
        for key in a[cpu]:
            if a[cpu][key] != b[cpu][key]:
                fail('%s: section %s,%s (%s) is not the bound build\'s' % (label, key[0], key[1], hex(cpu)))
            count += 1
    return count


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    app, commit, sdl_pin = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    unsigned = Path(sys.argv[4]) if len(sys.argv) == 5 else None
    if not app.is_dir() or app.name != 'OpenDana.app':
        fail('%s is not OpenDana.app' % app)
    have = []
    for path in sorted(app.rglob('*')):
        if path.is_symlink():
            fail('symbolic link in the package: %s' % path.relative_to(app))
        if path.is_file():
            have.append(path.relative_to(app).as_posix())
    want = expected_files()
    if have != want:
        for name in sorted(set(want) - set(have)):
            print('- missing: ' + name, file=sys.stderr)
        for name in sorted(set(have) - set(want)):
            print('+ unexpected: ' + name, file=sys.stderr)
        fail('inventory differs from the closed list')
    print('inventory: %d files, the closed list' % len(have))

    for rel, role in (('Contents/MacOS/opendana', 'exe'), ('Contents/Frameworks/libSDL2-2.0.0.dylib', 'lib')):
        data, info = describe(app / rel)
        if set(info) != {CPU_X86_64, CPU_ARM64}:
            fail('%s is not universal x86_64 + arm64 (slices %s)' % (rel, sorted(hex(c) for c in info)))
        for cpu, item in info.items():
            if not item['signed']:
                fail('%s slice %s has no code signature' % (rel, hex(cpu)))
            if role == 'exe':
                if item['rpaths'] != ['@executable_path/../Frameworks']:
                    fail('%s run path: %s' % (rel, item['rpaths']))
                if '@rpath/libSDL2-2.0.0.dylib' not in item['loads']:
                    fail('%s does not need @rpath/libSDL2-2.0.0.dylib: %s' % (rel, item['loads']))
                bad = [name for name in item['loads'] if not (name.startswith('/usr/lib/') or
                       name.startswith('/System/Library/') or name.startswith('@rpath/libSDL2'))]
                if bad:
                    fail('%s depends on something else: %s' % (rel, bad))
            else:
                if item['id'] != '@rpath/libSDL2-2.0.0.dylib':
                    fail('%s install name: %s' % (rel, item['id']))
        if role == 'exe':
            needle = ('(commit %s)' % commit).encode()
            if data.count(needle) < 2:
                fail('%s does not contain "(commit %s)" in both slices' % (rel, commit))
        for leak in (b'/Users/', b'/private/var/', b'/home/', b'/media/'):
            if leak in data:
                fail('%s embeds a machine path (%s)' % (rel, leak.decode()))
    print('Mach-O: opendana and libSDL2 are universal (x86_64 + arm64), signed, run path '
          '@executable_path/../Frameworks, commit %s' % commit[:12])

    plist = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    if plist.get('CFBundleExecutable') != 'opendana' or plist.get('CFBundleIconFile') != 'opendana' \
            or plist.get('CFBundlePackageType') != 'APPL' or not plist.get('CFBundleShortVersionString'):
        fail('Info.plist: %s' % plist)
    if (app / 'Contents/Resources/opendana.icns').read_bytes()[:4] != b'icns':
        fail('opendana.icns is not an icns file')
    if b'GNU GENERAL PUBLIC LICENSE' not in (app / DOC / 'LICENSE').read_bytes():
        fail('LICENSE is not the GPL text')

    bundle = app / LB
    for line in (bundle / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ', 1)
        if hashlib.sha256((bundle / name).read_bytes()).hexdigest() != digest:
            fail('licence bundle: %s does not match SHA256SUMS' % name)
    identity = json.loads((bundle / 'BUILD-IDENTITY.json').read_text())
    if identity['artifacts']['SDL2'] != sdl_pin:
        fail('BUILD-IDENTITY.json binds another SDL2 than the pinned one')
    if identity['application_commit'] != commit:
        fail('BUILD-IDENTITY.json is of another commit')
    if unsigned is not None:
        for key, name in (('application', 'opendana'), ('SDL2', 'libSDL2-2.0.0.dylib')):
            built = (unsigned / name).read_bytes()
            if hashlib.sha256(built).hexdigest() != identity['artifacts'][key]:
                fail('the bound %s is not the unsigned file of the build' % key)
            rel = 'Contents/MacOS/opendana' if key == 'application' else 'Contents/Frameworks/' + name
            n = same_code((app / rel).read_bytes(), built, name)
            print('%s: the shipped signed file has the %d sections of the bound unsigned build, byte for byte' % (name, n))
    sources = json.loads((bundle / 'SOURCES.json').read_text())
    if sources['application_commit'] != commit or sources['mode'] != 'macos':
        fail('SOURCES.json: %s / %s' % (sources['application_commit'], sources['mode']))
    print('licence bundle: SHA256SUMS verify; BUILD-IDENTITY binds the pinned SDL2 and commit %s' % commit[:12])
    print('macos-package: PASS')


if __name__ == '__main__':
    main()
