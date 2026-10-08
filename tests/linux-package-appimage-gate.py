#!/usr/bin/env python3
"""C6.50: controls of the AppImage publication mode of packaging/license-bundle.sh.

Synthetic materials only (no network, no Docker, no ROM). The runtime components use
the real version-proof kinds of packaging/write-manifest.sh (regex, git-archive-commit,
literal). Positive control: a complete appimage manifest passes and writes a bundle
with the seven runtime notices and sources. Negative controls (each must be rejected
with a message that names the problem): a runtime binary with the wrong hash, a runtime
component missing, a missing notice, a libfuse notice without the LGPL-2.1 text, libfuse
not declared static, a RELINK.txt that does not explain the runtime, a version that the
source does not contain, a tag archive of another commit, and tar mode not accepting the
runtime artifact.
"""
import copy
import hashlib
import io
import json
import lzma
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'packaging/license-bundle.sh'
BASE = ['application', 'SDL2', 'SDL2-embedded', 'cJSON', 'stb_image', 'stb_image_write',
        'stb_truetype', 'Inter']
COMMIT = '0123456789abcdef0123456789abcdef01234567'
RT_COMMIT = '8f39b89e2ac31e1640b3d3f7e9a5108e6ce805fa'
LGPL = 'GNU LESSER GENERAL PUBLIC LICENSE\n Version 2.1, February 1999\n'
# name: (version, member, kind, regex, member text)
RUNTIME = {
    'type2-runtime': (RT_COMMIT, 'LICENSE', 'git-archive-commit', None, 'MIT own fixture'),
    'libfuse': ('3.15.0', 'meson.build', 'regex', r"project\('libfuse3'.*?version:\s*'(\d+)\.(\d+)\.(\d+)'",
                "project('libfuse3', ['c'], version: '3.15.0',\n meson_version: '>= 0.51')\n"),
    'squashfuse': ('0.5.2', 'configure.ac', 'regex', r"AC_INIT\(\[squashfuse\],\s*\[(\d+)\.(\d+)\.(\d+)\]",
                   'AC_INIT([squashfuse], [0.5.2], [nobody@example.org])\n'),
    'zstd': ('1.5.6', 'lib/zstd.h', 'regex',
             r"#define\s+ZSTD_VERSION_MAJOR\s+(\d+).*?#define\s+ZSTD_VERSION_MINOR\s+(\d+)"
             r".*?#define\s+ZSTD_VERSION_RELEASE\s+(\d+)",
             '#define ZSTD_VERSION_MAJOR    1\n#define ZSTD_VERSION_MINOR    5\n#define ZSTD_VERSION_RELEASE  6\n'),
    'zlib': ('1.3.2', 'zlib.h', 'regex', r'#define\s+ZLIB_VERSION\s+"(\d+)\.(\d+)\.(\d+)"',
             '#define ZLIB_VERSION "1.3.2"\n'),
    'mimalloc': ('2.1.7', 'cmake/mimalloc-config-version.cmake', 'regex',
                 r"set\(mi_version_major (\d+)\)\s*set\(mi_version_minor (\d+)\)\s*set\(mi_version_patch (\d+)\)",
                 'set(mi_version_major 2)\nset(mi_version_minor 1)\nset(mi_version_patch 7)\n'),
    'musl': ('1.2.5', 'VERSION', 'literal', None, '1.2.5\n'),
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    with tempfile.TemporaryDirectory(prefix='opendana-appimage-gate.') as tmp:
        root = Path(tmp).resolve()   # C6.70: macOS TMPDIR is under /var, a symlink the gate rejects

        def file(name, data):
            (root / name).write_bytes(data)
            return {'path': name, 'sha256': digest(data)}

        def archive(name, members, commit=None, top='src'):
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode='w', format=tarfile.PAX_FORMAT,
                              pax_headers={'comment': commit} if commit else None) as tar:
                for path, data in members.items():
                    info = tarfile.TarInfo(top + '/' + path); info.size = len(data.encode())
                    tar.addfile(info, io.BytesIO(data.encode()))
            return file(name, lzma.compress(stream.getvalue()))

        components = []
        for name in BASE:
            version = '1.0'
            members = {'VERSION': '1.0', 'packaging/source-commit.txt': COMMIT + '\n'}
            source = archive(name + '.tar.xz', members, commit=COMMIT if name == 'application' else None)
            components.append({'name': name, 'version': version, 'license': 'OWN-test-only',
                               'notice': file(name + '.txt', b'Own synthetic notice; NOT upstream'),
                               'source': source, 'version_member': 'src/VERSION', 'version_text': '1.0'})
        for name, (version, member, kind, regex, text) in RUNTIME.items():
            notice = b'Own synthetic runtime notice. ' + (LGPL.encode() if name == 'libfuse' else b'')
            entry = {'name': name, 'version': version, 'license': 'OWN-test-only',
                     'notice': file(name + '.txt', notice),
                     'source': archive(name + '.tar.xz', {member: text}, commit=RT_COMMIT if name == 'type2-runtime' else None),
                     'version_member': 'src/' + member, 'version_text': version, 'version_kind': kind}
            if regex:
                entry['version_regex'] = regex
            components.append(entry)
        app_binary = b'OWN application opendana 1.0 (commit ' + COMMIT.encode() + b')'
        file('application.bin', app_binary); file('SDL2.bin', b'OWN SDL2'); file('runtime.bin', b'OWN runtime')
        artifacts = {'application': digest(app_binary), 'SDL2': digest(b'OWN SDL2'),
                     'type2-runtime': digest(b'OWN runtime')}
        identity = {'artifacts': artifacts, 'versions': {c['name']: c['version'] for c in components},
                    'toolchain': 'own fixture tool 1.0', 'flags': 'own fixture flags',
                    'linkage': {'SDL2': 'shared-substitutable', 'libfuse': 'static'}}
        relink_text = b'Own fixture only. Rebuild the runtime with another libfuse: bash build.sh'
        manifest = {'schema': 1, 'application_commit': COMMIT, 'components': components,
                    'identity': file('identity.json', json.dumps(identity).encode()),
                    'relink': file('RELINK.txt', relink_text),
                    'relink_materials': archive('relink.tar.xz', {'build.sh': '#!/bin/sh\n'})}

        def variant(**changes):
            data = copy.deepcopy(manifest)
            ident = copy.deepcopy(identity)
            for key, value in changes.items():
                if key == 'identity_linkage':
                    ident['linkage'] = value
                elif key == 'drop':
                    data['components'] = [c for c in data['components'] if c['name'] != value]
                    ident['versions'].pop(value, None)
                    ident['artifacts'] = {k: v for k, v in ident['artifacts'].items() if k != value}
                elif key == 'relink':
                    data['relink'] = file('RELINK-%s.txt' % digest(value)[:6], value)
                else:
                    raise KeyError(key)
            data['identity'] = file('identity-%s.json' % digest(json.dumps(ident).encode())[:8],
                                    json.dumps(ident).encode())
            return data

        def run(data, expected, mode='appimage', output=None, runtime='runtime.bin', needle=None):
            raw = json.dumps(data).encode(); (root / 'manifest.json').write_bytes(raw)
            args = ['bash', str(SCRIPT), '--manifest', str(root / 'manifest.json'), '--sha256', digest(raw),
                    '--mode', mode, '--artifact', 'application=' + str(root / 'application.bin'),
                    '--artifact', 'SDL2=' + str(root / 'SDL2.bin')]
            if runtime:
                args += ['--artifact', 'type2-runtime=' + str(root / runtime)]
            if output:
                args += ['--output', str(root / output)]
            result = subprocess.run(args, capture_output=True, text=True)
            print(('ACCEPT ' if result.returncode == 0 else 'REJECT ') + (result.stdout + result.stderr).strip())
            assert (result.returncode == 0) == expected, result
            if needle:
                assert needle in result.stderr, (needle, result.stderr)

        # Positive control: complete manifest, bundle written with the seven runtime notices and sources.
        run(manifest, True, output='bundle')
        names = {p.relative_to(root / 'bundle').as_posix() for p in (root / 'bundle').rglob('*') if p.is_file()}
        for name in RUNTIME:
            assert 'notices/%s.txt' % name in names, name
            assert any(n.startswith('sources/%s.tar' % name) for n in names), name
        assert 'LESSER GENERAL PUBLIC LICENSE' in (root / 'bundle/notices/libfuse.txt').read_text()
        sources = json.loads((root / 'bundle/SOURCES.json').read_text())
        assert sources['mode'] == 'appimage' and len(sources['components']) == len(BASE) + len(RUNTIME)

        # Negative controls.
        file('wrong-runtime.bin', b'a runtime with another hash')
        run(manifest, False, runtime='wrong-runtime.bin', needle='SHA-256 mismatch for artifact type2-runtime')
        run(manifest, False, runtime=None, needle='all actual package artifacts must be bound', output='bundle-2')
        for missing in RUNTIME:
            run(variant(drop=missing), False, needle='component inventory mismatch')
        bad = copy.deepcopy(manifest)
        next(c for c in bad['components'] if c['name'] == 'zstd')['notice']['path'] = 'no-such-notice.txt'
        run(bad, False, needle='missing local material')
        bad = copy.deepcopy(manifest)
        fuse = next(c for c in bad['components'] if c['name'] == 'libfuse')
        fuse['notice'] = file('fuse-short.txt', b'LGPL, see the internet')
        run(bad, False, needle='libfuse notice must contain the full LGPL-2.1 text')
        run(variant(identity_linkage={'SDL2': 'shared-substitutable'}), False, needle='static libfuse')
        run(variant(relink=b'Own fixture only. Nothing about the runtime here.'), False,
            needle='must explain how to rebuild the runtime')
        bad = copy.deepcopy(manifest)
        zlib = next(c for c in bad['components'] if c['name'] == 'zlib')
        zlib['source'] = archive('zlib-wrong.tar.xz', {'zlib.h': '#define ZLIB_VERSION "1.2.11"\n'})
        run(bad, False, needle='zlib source version mismatch')
        bad = copy.deepcopy(manifest)
        t2 = next(c for c in bad['components'] if c['name'] == 'type2-runtime')
        t2['source'] = archive('type2-other.tar.xz', {'LICENSE': 'MIT own fixture'}, commit='f' * 40)
        run(bad, False, needle='is not the git archive of commit ' + RT_COMMIT)
        bad = copy.deepcopy(manifest)
        musl = next(c for c in bad['components'] if c['name'] == 'musl')
        musl['version_kind'] = 'unknown-kind'
        run(bad, False, needle='unsupported source version proof kind')
        run(manifest, False, mode='tar', needle='artifact identity inventory mismatch')
        print('PASS: appimage licence gate: 1 positive control, 17 negative controls rejected '
              '(wrong runtime hash, missing component/notice, libfuse LGPL text and static linkage, '
              'relink instructions, version proofs)')


if __name__ == '__main__':
    main()
