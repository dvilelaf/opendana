#!/usr/bin/env bash
# C6.9/C6.30: strictly local, hash-pinned corresponding-materials gate. No downloads.
#
# Modes: tar (Linux TAR), windows (Windows ZIP), macos (macOS .dmg, C6.70: the TAR set; the
# artifacts are bound WITHOUT their ad hoc code signature), appimage (AppImage publication, C6.50:
# the TAR set plus the Type-2 runtime and its statically linked libraries, each with its
# notice, exact version proved in its own source archive and corresponding source; the
# runtime binary is bound by SHA-256 and the LGPL-2.1 libfuse must contain the full licence
# text and relink instructions).
# C6.30 additions:
#   * the manifest names the application commit ("application_commit"); the
#     application source archive must be the `git archive` of that commit (pax
#     comment and packaging/source-commit.txt) and the application artifact must
#     contain "(commit <sha>)", i.e. what `opendana --version` prints. Stale source fails.
#   * every SHA-256 mismatch names the material, the expected and the actual hash;
#   * archives keep their real extension in the bundle (sources/<name>.tar.xz),
#     and a source shared by two components is stored once;
#   * windows mode requires the mingw-w64-runtime component, the only one that
#     may declare its source as not shipped (permissive licences, see notice).
set -euo pipefail
exec python3 - "$@" <<'PY'
import argparse
import bz2
import gzip
import hashlib
import io
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import sys
import tarfile
import tempfile

MAX_ARCHIVE = 64 * 1024 * 1024
MAX_UNPACKED = 128 * 1024 * 1024
MAX_TOTAL = 256 * 1024 * 1024
MAX_EXPANDED = 512 * 1024 * 1024
MAX_TEXT = 1024 * 1024
BASE = {'application', 'SDL2', 'SDL2-embedded', 'cJSON', 'stb_image', 'stb_image_write',
        'stb_truetype', 'Inter'}
RUNTIME = {'type2-runtime', 'libfuse', 'squashfuse', 'musl', 'zstd', 'zlib', 'mimalloc'}
WINDOWS = {'mingw-w64-runtime'}
# Components whose licences do not require shipping their source; they must say why.
SOURCE_NOT_SHIPPED_OK = {'mingw-w64-runtime'}


def archive_suffix(data):
    return ('.tar.xz' if data[:6] == b'\xfd7zXZ\x00' else '.tar.gz' if data[:2] == b'\x1f\x8b'
            else '.tar.bz2' if data[:3] == b'BZh' else '.tar')


def archive_commit(data, prefix_file='packaging/source-commit.txt'):
    """(pax comment commit, stamped commit) of a git-archive tarball."""
    decompressor = (gzip.open if data[:2] == b'\x1f\x8b' else
                    bz2.BZ2File if data[:3] == b'BZh' else
                    lzma.LZMAFile if data[:6] == b'\xfd7zXZ\x00' else None)
    if decompressor:
        with decompressor(io.BytesIO(data), 'rb') as stream:
            data = stream.read(MAX_UNPACKED + 1)
    stamped = None
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as archive:
        comment = archive.pax_headers.get('comment')
        for item in archive:
            parts = item.name.split('/', 1)
            if item.isfile() and len(parts) == 2 and parts[1] == prefix_file:
                stamped = archive.extractfile(item).read(128).decode('ascii', 'replace').strip()
                break
    return comment, stamped


def fail(message):
    raise ValueError(message)


def safe_path(name):
    if not isinstance(name, str) or not name or '\\' in name or '\x00' in name:
        fail('unsafe material/archive path')
    path = PurePosixPath(name)
    if path.is_absolute() or any(p in ('', '.', '..') for p in name.split('/')):
        fail('unsafe material/archive path: ' + name)
    return path


def no_symlinks(path):
    path = Path(os.path.abspath(path))
    for parent in [*reversed(path.parents), path]:
        if parent.is_symlink():
            fail('symlink path forbidden: ' + str(parent))
    return path


def read_file(path, limit):
    path = no_symlinks(path)
    try:
        # O_NOFOLLOW also closes the final-component symlink race.
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except OSError:
        fail('missing local material: ' + str(path))
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
            fail('material must be a nonempty bounded regular file: ' + str(path))
        data = stream.read(limit + 1)
    if not 0 < len(data) <= limit:
        fail('material exceeds size bound')
    return data


def hashed(data, expected, label):
    if not isinstance(expected, str) or not re.fullmatch('[0-9a-f]{64}', expected):
        fail('invalid SHA-256 pin for ' + label)
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected:
        fail('SHA-256 mismatch for %s: expected %s, got %s' % (label, expected, actual))
    return actual


def text(value, label):
    if not isinstance(value, str) or not value.strip() or len(value) > 4096:
        fail('missing/invalid ' + label)
    if value.strip().lower() in ('unknown', 'pending', 'unresolved', 'todo'):
        fail('unresolved ' + label)
    return value


def inspect_archive(data, member=None, expected=None):
    seen = set(); total = 0; proof = None
    # Bound decompression BEFORE tarfile interprets GNU/PAX extension sizes.
    # A malicious extension header must not trigger an unbounded allocation.
    decompressor = (gzip.open if data[:2] == b'\x1f\x8b' else
                    bz2.BZ2File if data[:3] == b'BZh' else
                    lzma.LZMAFile if data[:6] == b'\xfd7zXZ\x00' else None)
    if decompressor:
        with decompressor(io.BytesIO(data), 'rb') as stream:
            data = stream.read(MAX_UNPACKED + 1)
    if len(data) > MAX_UNPACKED:
        fail('archive exceeds bounded decompressed size')
    # Never extract: paths/links/devices cannot write outside a staging directory.
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:') as archive:
        for item in archive:
            name = item.name.rstrip('/') if item.isdir() else item.name
            safe_path(name)
            if name in seen or len(seen) >= 20000:
                fail('duplicate/excessive archive members')
            seen.add(name)
            if not (item.isfile() or item.isdir()) or item.size < 0:
                fail('archive links/devices/special members forbidden')
            total += item.size
            if total > MAX_EXPANDED or item.size > MAX_ARCHIVE:
                fail('archive exceeds expanded size bound')
            if item.isfile():
                stream = archive.extractfile(item)
                count = 0; chunks = []
                while True:
                    chunk = stream.read(65536)
                    if not chunk:
                        break
                    count += len(chunk)
                    if name == member:
                        if count > MAX_TEXT:
                            fail('version member too large')
                        chunks.append(chunk)
                if count != item.size:
                    fail('truncated archive member')
                if name == member:
                    proof = b''.join(chunks).decode('utf-8')
    if not seen:
        fail('empty source/material archive')
    if member is not None and (proof is None or (expected is not None and proof.strip() != expected)):
        fail('source version proof mismatch/missing: ' + member)
    return proof


def main():
    parser = argparse.ArgumentParser(description='Offline engineering gate; not legal certification')
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--mode', choices=['tar', 'windows', 'macos', 'appimage'], required=True)
    parser.add_argument('--artifact', action='append', default=[])
    parser.add_argument('--output', help='New bundle directory; existing output is never replaced')
    args = parser.parse_args()
    manifest_path = no_symlinks(args.manifest)
    raw = read_file(manifest_path, MAX_TEXT)
    hashed(raw, args.sha256, 'the license manifest ' + str(manifest_path))
    manifest = json.loads(raw)
    if manifest.get('schema') != 1:
        fail('unsupported license manifest schema')
    commit = manifest.get('application_commit')
    if not isinstance(commit, str) or not re.fullmatch('[0-9a-f]{40}', commit):
        fail('application_commit must be the 40-hex commit the application was built from')
    materials = {}; total = 0; stored = {}

    def material(record, destination, archive=False, label=None):
        nonlocal total
        name = safe_path(record['path'])
        label = label or destination
        data = read_file(manifest_path.parent / str(name), MAX_ARCHIVE if archive else MAX_TEXT)
        hashed(data, record['sha256'], '%s (%s)' % (label, name))
        if archive:
            destination += archive_suffix(data)
            digest = hashlib.sha256(data).hexdigest()
            if digest in stored:  # same source shared by two components: store once
                return data, stored[digest]
            stored[digest] = destination
        total += len(data)
        if total > MAX_TOTAL:
            fail('total materials exceed bound')
        if not archive:
            if not data.decode('utf-8').strip():
                fail('empty notice/instructions: ' + label)
        materials[destination] = data
        return data, destination

    components = manifest['components']
    if not isinstance(components, list) or len(components) > 32:
        fail('invalid component inventory')
    names = set(); sources = []; versions = {}
    for component in components:
        name = component['name']
        if not isinstance(name, str) or not re.fullmatch('[A-Za-z0-9_-]+', name) or name in names:
            fail('invalid/duplicate component name')
        names.add(name)
        version = text(component['version'], 'exact version of ' + name)
        versions[name] = version
        license_name = text(component['license'], 'license identifier of ' + name)
        notice, notice_name = material(component['notice'], 'notices/' + name + '.txt',
                                       label='notice of ' + name)
        entry = {'name': name, 'version': version, 'license': license_name,
                 'notice': notice_name, 'notice_sha256': hashlib.sha256(notice).hexdigest()}
        if isinstance(component.get('source'), dict) and 'not_shipped' in component['source']:
            reason = component['source']['not_shipped']
            if name not in SOURCE_NOT_SHIPPED_OK or args.mode != 'windows':
                fail('the source of %s must be shipped' % name)
            text(reason, 'reason why the source of %s is not shipped' % name)
            if len(reason) < 40:
                fail('the reason why the source of %s is not shipped is too short' % name)
            entry.update(source=None, source_not_shipped=reason, version_kind='declared')
            sources.append(entry)
            continue
        source, source_name = material(component['source'], 'sources/' + name, archive=True,
                                       label='source of ' + name)
        proof = str(safe_path(component['version_member']))
        version_text = text(component['version_text'], 'source version text of ' + name)
        if not re.search(r'(?<![A-Za-z0-9.])' + re.escape(version) + r'(?![A-Za-z0-9.])', version_text):
            fail('version absent from source version proof of ' + name)
        kind = component.get('version_kind', 'literal')
        source_text = inspect_archive(source, proof, version_text if kind == 'literal' else None)
        if kind == 'SDL2-header':
            parts = [re.search(r'^#define\s+SDL_' + key + r'\s+(\d+)\s*$', source_text, re.M)
                     for key in ('MAJOR_VERSION', 'MINOR_VERSION', 'PATCHLEVEL')]
            if not all(parts) or '.'.join(match.group(1) for match in parts) != version:
                fail('SDL source header version mismatch')
        elif kind == 'cJSON-header':
            parts = [re.search(r'^#define\s+CJSON_VERSION_' + key + r'\s+(\d+)\s*$', source_text, re.M)
                     for key in ('MAJOR', 'MINOR', 'PATCH')]
            if not all(parts) or '.'.join(match.group(1) for match in parts) != version:
                fail('cJSON source header version mismatch')
        elif kind == 'stb-header':
            match = re.search(r'stb_(?:image|image_write|truetype)(?:\.h)? - v([0-9.]+)', source_text[:512])
            if not match or match.group(1) != version:
                fail('stb source header version mismatch')
        elif kind == 'CMake-project':
            match = re.search(r'project\([^)]*\bVERSION\s+([0-9.]+)', source_text, re.I)
            if not match or match.group(1) != version:
                fail('application CMake source version mismatch')
        elif kind == 'regex':
            # C6.50: the version is whatever the (dot-joined) groups of version_regex match in
            # the proof member; a manifest cannot claim a version the source does not contain.
            pattern = component.get('version_regex')
            match = re.search(pattern, source_text, re.M | re.S) if isinstance(pattern, str) else None
            if not match or '.'.join(match.groups()) != version:
                fail('%s source version mismatch: %s does not contain %s' % (name, proof, version))
        elif kind == 'git-archive-commit':
            # C6.50: a hosting-service tag/commit archive records its commit in the pax header.
            if archive_commit(source)[0] != version:
                fail('%s source is not the git archive of commit %s' % (name, version))
        elif kind != 'literal':
            fail('unsupported source version proof kind')
        if name == 'application':
            comment, stamped = archive_commit(source)
            if comment != commit:
                fail('stale application source: the archive is of commit %s, the application of %s'
                     % (comment, commit))
            if stamped != commit:
                fail('stale application source: packaging/source-commit.txt says %s, expected %s'
                     % (stamped, commit))
            entry['commit'] = commit
        if args.mode == 'appimage' and name == 'libfuse':
            # LGPL-2.1, statically linked: the full licence text must travel with the notice.
            text_ = notice.decode('utf-8')
            if 'GNU LESSER GENERAL PUBLIC LICENSE' not in text_ or 'Version 2.1, February 1999' not in text_:
                fail('the libfuse notice must contain the full LGPL-2.1 text')
        entry.update(source=source_name, source_sha256=hashlib.sha256(source).hexdigest(),
                     version_member=proof, version_text=version_text, version_kind=kind)
        sources.append(entry)
    required = BASE | (RUNTIME if args.mode == 'appimage' else set()) | (WINDOWS if args.mode == 'windows' else set())
    if not required <= names:
        fail('component inventory mismatch: required ' + ','.join(sorted(required)))
    identity_data, _ = material(manifest['identity'], 'BUILD-IDENTITY.json', label='build identity')
    identity = json.loads(identity_data)
    if identity['versions'] != versions:
        fail('binary/source identity version mismatch')
    text(identity['toolchain'], 'exact toolchain'); text(identity['flags'], 'exact build flags')
    if identity['linkage'].get('SDL2') != 'shared-substitutable':
        fail('SDL2 shared replacement mechanism required')
    if args.mode == 'appimage' and identity['linkage'].get('libfuse') != 'static':
        fail('runtime static libfuse relink mechanism required')
    expected_artifacts = {'application', 'SDL2'} | ({'type2-runtime'} if args.mode == 'appimage' else set())
    if set(identity['artifacts']) != expected_artifacts:
        fail('artifact identity inventory mismatch')
    supplied = {}
    for artifact in args.artifact:
        name, path = artifact.split('=', 1)
        if name not in expected_artifacts or name in supplied:
            fail('invalid/duplicate actual artifact')
        supplied[name] = path
        data = read_file(path, MAX_ARCHIVE)
        hashed(data, identity['artifacts'][name], 'artifact %s (%s)' % (name, path))
        if name == 'application' and ('(commit %s)' % commit).encode() not in data:
            fail('the application artifact %s does not contain "(commit %s)": built from another '
                 'commit than its source archive' % (path, commit))
    # Check-only supports pre-build source validation; a written bundle ALWAYS binds all binaries.
    if args.output and set(supplied) != expected_artifacts:
        fail('all actual package artifacts must be bound before bundling')
    relink_text, _ = material(manifest['relink'], 'RELINK.txt', label='relink instructions')
    if args.mode == 'appimage' and (b'libfuse' not in relink_text or b'build.sh' not in relink_text):
        fail('the AppImage RELINK.txt must explain how to rebuild the runtime with another libfuse')
    relink, relink_name = material(manifest['relink_materials'], 'sources/relink-materials', archive=True,
                                   label='relink materials')
    inspect_archive(relink)
    materials['INPUT-MANIFEST.json'] = raw
    materials['SOURCES.json'] = (json.dumps({'schema': 1, 'mode': args.mode, 'components': sources,
        'application_commit': commit,
        'relink_materials': relink_name,
        'relink_materials_sha256': hashlib.sha256(relink).hexdigest(),
        'warning': 'Caller-attested engineering evidence, not legal certification or ROM audit'},
        indent=2, sort_keys=True) + '\n').encode()
    if args.mode == 'windows':
        shared = 'SDL2 is shared: replace bin/SDL2.dll with an ABI-compatible modified build.\n'
    elif args.mode == 'macos':
        shared = ('SDL2 is shared: replace OpenDana.app/Contents/Frameworks/libSDL2-2.0.0.dylib with an\n'
                  'ABI-compatible modified build and sign again (RELINK.txt); the application uses\n'
                  '@executable_path/../Frameworks.\n')
    else:
        shared = ('SDL2 is shared: replace lib/libSDL2-2.0.so.0 with an ABI-compatible modified build;\n'
                  'the application uses $ORIGIN/../lib.\n')
    if args.mode == 'appimage':
        shared += ('The AppImage runtime (the executable at the start of the file) statically links libfuse\n'
                   '(LGPL-2.1), squashfuse, zstd, zlib, mimalloc and musl. Their notices and sources are\n'
                   'here; RELINK.txt explains how to rebuild the runtime with a modified libfuse.\n')
    materials['README.txt'] = ('''Third-party notices and corresponding source materials
See notices/, SOURCES.json and BUILD-IDENTITY.json for exact supplied identities.
sources/application.tar.xz is the complete corresponding source of the game
(GPL-3.0-or-later): the git archive of commit ''' + commit + ''',
the commit printed by `opendana --version`.
See RELINK.txt and ''' + relink_name + ''' for build/relink instructions/materials.
''' + shared + '''No prohibition on modification/reverse engineering for debugging is added by
this bundle. The distributor must review completeness and applicable terms.
Hashes bind supplied bytes; they do not authenticate ownership, license correctness,
source correspondence, or compliance. No fictional written source offer is made.
''').encode()
    materials['SHA256SUMS'] = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                                     for name, data in sorted(materials.items())).encode()
    if args.output:
        output = no_symlinks(args.output)
        if output.exists():
            fail('refusing to replace existing license bundle')
        if not output.parent.is_dir():
            fail('output parent must already exist')
        stage = Path(tempfile.mkdtemp(prefix='.license-bundle.', dir=output.parent))
        try:
            for name, data in materials.items():
                target = stage / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            os.chmod(stage, 0o755)
            os.rename(stage, output)
        finally:
            if stage.exists():
                shutil.rmtree(stage)
    print('PASS: offline license/source inventory and SHA-256 gate (' + args.mode + ', commit ' + commit + ')')




try:
    main()
except (ValueError, KeyError, TypeError, OSError, EOFError, OverflowError, tarfile.TarError, UnicodeError) as error:
    sys.exit('error: license bundle: ' + str(error))
PY
