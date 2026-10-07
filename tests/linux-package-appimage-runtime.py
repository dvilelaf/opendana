#!/usr/bin/env python3
"""C6.30: controls of packaging/appimage-runtime-check.sh (never executes a runtime).

Positive control: a complete local ELF (/bin/true) carrying the AI\\x02 marker and
its exact SHA-256 is accepted (structurally; it is NOT a runtime and is never run).
Negative controls: missing file, wrong pin, invalid pin, no marker, AArch64
machine, truncated file, extra bytes appended (a whole AppImage passed as runtime).
"""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'packaging/appimage-runtime-check.sh'


def check(path, pin, expect, needle=None):
    result = subprocess.run(['bash', str(SCRIPT), str(path), pin], capture_output=True, text=True)
    print(('ACCEPT ' if result.returncode == 0 else 'REJECT ') + (result.stdout + result.stderr).strip())
    assert (result.returncode == 0) == expect, result
    if needle:
        assert needle in result.stderr, (needle, result.stderr)


def main():
    with tempfile.TemporaryDirectory(prefix='opendana-runtime-check.') as tmp:
        root = Path(tmp)
        good = bytearray(Path('/bin/true').read_bytes()); good[8:11] = b'AI\x02'

        def variant(name, data):
            path = root / name
            path.write_bytes(bytes(data))
            return path, hashlib.sha256(bytes(data)).hexdigest()

        path, pin = variant('good', good)
        check(path, pin, True)
        check(root / 'absent', pin, False, 'not found')
        check(path, '0' * 64, False, 'SHA-256 mismatch')
        check(path, 'nope', False, 'SHA-256 mismatch')
        nomark = bytearray(good); nomark[8:11] = b'\0\0\0'
        check(*variant('nomark', nomark), False, 'Type-2')
        arm = bytearray(good); struct.pack_into('<H', arm, 18, 183)
        check(*variant('aarch64', arm), False, 'x86_64')
        check(*variant('truncated', good[:len(good) // 2]), False, 'invalid/truncated')
        check(*variant('appended', good + b'hsqs' + b'\0' * 64), False, 'invalid/truncated')
    print('PASS: appimage-runtime-check: structural positive accepted, 7 negatives rejected')


if __name__ == '__main__':
    main()
