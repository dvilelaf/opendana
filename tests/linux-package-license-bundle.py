#!/usr/bin/env python3
"""C6.9/C6.30: controls of packaging/license-bundle.sh with own synthetic materials.

Engineering gate only, NOT licensing acceptance. Positive controls: complete tar
and windows manifests pass and write a bundle whose sources keep their real
extension and are stored once. Negative controls (each must be rejected, with a
message naming the material): missing/tampered/unsafe/symlinked materials,
version mismatch, unsafe archives, compression bomb, missing component, wrong
artifact, STALE APPLICATION SOURCE (archive or stamp of another commit, binary
without the commit), source declared not shipped outside its allow-list, and an
existing bundle is never replaced.
"""
import copy
import gzip
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
OTHER = 'fedcba9876543210fedcba9876543210fedcba98'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    with tempfile.TemporaryDirectory(prefix='opendana-license-bundle-test.') as tmp:
        root = Path(tmp).resolve()   # C6.70: macOS TMPDIR is under /var, a symlink the gate rejects

        def file(name, data):
            (root / name).write_bytes(data)
            return {'path': name, 'sha256': digest(data)}

        def archive(name, version='1.0', unsafe=None, commit=None, stamp=None, compress='xz'):
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode='w', format=tarfile.PAX_FORMAT,
                              pax_headers={'comment': commit} if commit else None) as tar:
                members = [('source/VERSION', version.encode()), ('source/own.c', b'/* own fixture */')]
                if stamp:
                    members.append(('source/packaging/source-commit.txt', (stamp + '\n').encode()))
                for path, data in members:
                    info = tarfile.TarInfo(path); info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
                if unsafe:
                    info = tarfile.TarInfo(unsafe)
                    if unsafe == 'source/link':
                        info.type = tarfile.SYMTYPE; info.linkname = '/etc/passwd'
                    if unsafe == 'source/hard':
                        info.type = tarfile.LNKTYPE; info.linkname = '../outside'
                    tar.addfile(info)
            data = stream.getvalue()
            data = lzma.compress(data) if compress == 'xz' else gzip.compress(data) if compress == 'gz' else data
            return file(name, data)

        shared_sdl = archive('SDL2.tar.xz')
        components = []
        for name in BASE:
            source = shared_sdl if name.startswith('SDL2') else (
                archive(name + '.tar.xz', commit=COMMIT, stamp=COMMIT) if name == 'application'
                else archive(name + '.tar.xz'))
            components.append({'name': name, 'version': '1.0', 'license': 'OWN-test-only',
                               'notice': file(name + '.txt', b'Own synthetic notice; NOT upstream'),
                               'source': source, 'version_member': 'source/VERSION', 'version_text': '1.0'})
        app_binary = b'OWN application opendana 1.0 (commit ' + COMMIT.encode() + b')'
        artifacts = {'application': file('application.bin', app_binary)['sha256'],
                     'SDL2': file('SDL2.bin', b'OWN SDL2')['sha256']}
        identity = {'artifacts': artifacts, 'versions': {c['name']: c['version'] for c in components},
                    'toolchain': 'own fixture tool 1.0', 'flags': 'own fixture flags',
                    'linkage': {'SDL2': 'shared-substitutable'}}
        manifest = {'schema': 1, 'application_commit': COMMIT, 'components': components,
                    'identity': file('identity.json', json.dumps(identity).encode()),
                    'relink': file('RELINK.txt', b'Own fixture only. Replace the shared SDL in lib/.'),
                    'relink_materials': archive('relink.tar.gz', compress='gz')}
        mingw = {'name': 'mingw-w64-runtime', 'version': '8.0.0-1', 'license': 'OWN-test-only',
                 'notice': file('mingw.txt', b'Own synthetic mingw notice'),
                 'source': {'not_shipped': 'Own fixture: these licences only require notices for binaries.'},
                 'version_text': '8.0.0-1', 'version_kind': 'declared'}

        def with_identity(data, versions=None, artifacts_=None):
            ident = copy.deepcopy(identity)
            if versions:
                ident['versions'].update(versions)
            if artifacts_:
                ident['artifacts'].update(artifacts_)
            data['identity'] = file('identity-%s.json' % digest(json.dumps(ident).encode())[:8],
                                    json.dumps(ident).encode())
            return data

        windows = with_identity(copy.deepcopy(manifest) | {'components': components + [mingw]},
                                versions={'mingw-w64-runtime': '8.0.0-1'})

        def run(data, expected, mode='tar', output=None, pin=None, artifacts_=None, needle=None):
            raw = json.dumps(data).encode(); (root / 'manifest.json').write_bytes(raw)
            args = ['bash', str(SCRIPT), '--manifest', str(root / 'manifest.json'),
                    '--sha256', pin or digest(raw), '--mode', mode]
            for name, path in (artifacts_ or {'application': 'application.bin', 'SDL2': 'SDL2.bin'}).items():
                args += ['--artifact', name + '=' + str(root / path)]
            if output:
                args += ['--output', str(root / output)]
            result = subprocess.run(args, capture_output=True, text=True)
            print(('ACCEPT ' if result.returncode == 0 else 'REJECT ') + (result.stdout + result.stderr).strip())
            assert (result.returncode == 0) == expected, result
            if needle:
                assert needle in result.stderr, (needle, result.stderr)
            return result

        # Positive controls.
        run(manifest, True)
        run(windows, True, mode='windows')
        run(manifest, True, mode='macos')          # C6.70: the macOS set is the TAR set
        run(manifest, True, output='bundle-tar')
        bundle = root / 'bundle-tar'
        names = sorted(p.relative_to(bundle).as_posix() for p in bundle.rglob('*') if p.is_file())
        assert 'sources/SDL2.tar.xz' in names and 'sources/application.tar.xz' in names, names
        assert 'sources/relink-materials.tar.gz' in names, names
        assert not any(n.startswith('sources/SDL2-embedded') for n in names), 'shared source stored twice'
        sources = json.loads((bundle / 'SOURCES.json').read_text())
        assert sources['application_commit'] == COMMIT
        assert next(c for c in sources['components'] if c['name'] == 'SDL2-embedded')['source'] == 'sources/SDL2.tar.xz'
        assert COMMIT in (bundle / 'README.txt').read_text()
        seal = {n: digest((bundle / n).read_bytes()) for n in names}
        run(windows, True, mode='windows', output='bundle-win')
        win_sources = json.loads((root / 'bundle-win/SOURCES.json').read_text())
        assert next(c for c in win_sources['components'] if c['name'] == 'mingw-w64-runtime')['source'] is None

        run(manifest, True, mode='macos', output='bundle-mac')
        mac_readme = (root / 'bundle-mac/README.txt').read_text()
        assert 'Contents/Frameworks/libSDL2-2.0.0.dylib' in mac_readme and 'libSDL2-2.0.so.0' not in mac_readme

        # Named mismatches (PUB-19).
        bad = copy.deepcopy(manifest); bad['components'][3]['notice']['sha256'] = '0' * 64
        run(bad, False, needle='SHA-256 mismatch for notice of cJSON (cJSON.txt): expected ' + '0' * 64)
        run(manifest, False, pin='0' * 64, needle='SHA-256 mismatch for the license manifest')
        run(manifest, False, pin='not-a-hash', needle='invalid SHA-256 pin')
        bad = copy.deepcopy(manifest); bad['components'][0]['source']['path'] = 'absent.tar.xz'
        run(bad, False, needle='missing local material')

        # Stale application source (PUB-5): archive of another commit, stamp of another
        # commit, manifest without a commit, binary built from another commit.
        for kwargs, needle in [({'commit': OTHER, 'stamp': COMMIT}, 'the archive is of commit ' + OTHER),
                               ({'commit': COMMIT, 'stamp': OTHER}, 'source-commit.txt says ' + OTHER),
                               ({'commit': None, 'stamp': COMMIT}, 'the archive is of commit None')]:
            bad = copy.deepcopy(manifest)
            bad['components'][0]['source'] = archive('stale.tar.xz', **kwargs)
            run(bad, False, needle=needle)
        bad = copy.deepcopy(manifest); del bad['application_commit']
        run(bad, False, needle='application_commit')
        stale_binary = b'OWN application opendana 1.0 (commit ' + OTHER.encode() + b')'
        file('stale.bin', stale_binary)
        bad = with_identity(copy.deepcopy(manifest), artifacts_={'application': digest(stale_binary)})
        run(bad, False, artifacts_={'application': 'stale.bin', 'SDL2': 'SDL2.bin'},
            needle='does not contain "(commit ' + COMMIT)

        # Source not shipped: only the allow-listed component, only in windows mode.
        run(windows, False, mode='tar', needle='the source of mingw-w64-runtime must be shipped')
        bad = copy.deepcopy(manifest)
        bad['components'][3]['source'] = {'not_shipped': 'Own fixture: trying to skip a source that is required.'}
        run(bad, False, needle='the source of cJSON must be shipped')
        bad = copy.deepcopy(windows); bad['components'][-1]['source'] = {'not_shipped': 'short'}
        run(bad, False, mode='windows', needle='too short')
        bad = copy.deepcopy(windows); bad['components'] = components
        run(with_identity(bad), False, mode='windows', needle='required')

        # Inherited C6.9 controls.
        bad = copy.deepcopy(manifest); bad['components'][1]['version'] = '9.9'
        run(bad, False, needle='version')
        bad = copy.deepcopy(manifest); bad['components'][1]['notice']['path'] = '../escape'
        run(bad, False, needle='unsafe')
        (root / 'linked.txt').symlink_to(root / 'SDL2.txt')
        bad = copy.deepcopy(manifest); bad['components'][1]['notice']['path'] = 'linked.txt'
        run(bad, False, needle='symlink')
        bad = copy.deepcopy(manifest); bad['components'][3]['source'] = archive('wrong-version.tar.xz', '2.0')
        run(bad, False, needle='version proof')
        for path in ['../escape', '/absolute', 'source/link', 'source/hard', 'source/own.c']:
            bad = copy.deepcopy(manifest); bad['relink_materials'] = archive('unsafe.tar.gz', unsafe=path, compress='gz')
            run(bad, False)
        stream = io.BytesIO()
        with gzip.GzipFile(fileobj=stream, mode='wb') as compressed:
            for _ in range(129):
                compressed.write(b'\0' * (1024 * 1024))
        bad = copy.deepcopy(manifest); bad['relink_materials'] = file('compression-bomb.tar.gz', stream.getvalue())
        run(bad, False, needle='bounded decompressed size')
        bad = copy.deepcopy(manifest); bad['components'] = bad['components'][:-1]
        run(bad, False, needle='component inventory mismatch')
        run(manifest, False, artifacts_={'application': 'application.bin', 'SDL2': 'linked.txt'})
        file('wrong-sdl.bin', b'wrong binary')
        run(manifest, False, artifacts_={'application': 'application.bin', 'SDL2': 'wrong-sdl.bin'},
            needle='SHA-256 mismatch for artifact SDL2')
        run(manifest, False, output='bundle-tar', needle='refusing to replace')
        assert seal == {n: digest((bundle / n).read_bytes()) for n in names}, 'existing bundle changed'
        print('PASS: license-bundle gate: 3 positive controls, all negative controls rejected '
              '(named mismatches, stale source/commit, source-not-shipped allow-list, C6.9 set)')


if __name__ == '__main__':
    main()
