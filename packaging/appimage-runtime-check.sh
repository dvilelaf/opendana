#!/usr/bin/env bash
# C6.9/C6.30: structural guard of a local AppImage Type-2 runtime, never executed.
#   appimage-runtime-check.sh RUNTIME EXPECTED_SHA256
# Checks the pinned SHA-256, ELF64 little-endian x86_64 with the AI\x02 marker,
# and the complete header/tables/segments/entry point (rejects fake/truncated
# headers and whole AppImages passed as a raw runtime). It cannot authenticate
# code or settle licensing: the runtime's licence gate stays open
# so images built with it are local tests only.
set -euo pipefail
exec python3 - "$@" <<'PY'
import hashlib, pathlib, re, struct, sys
if len(sys.argv) != 3:
    sys.exit('usage: appimage-runtime-check.sh RUNTIME EXPECTED_SHA256')
path, expected = pathlib.Path(sys.argv[1]), sys.argv[2]
if not path.is_file():
    sys.exit('error: AppImage runtime not found: %s' % path)
data = path.read_bytes()
actual = hashlib.sha256(data).hexdigest()
if not re.fullmatch(r"[0-9a-fA-F]{64}", expected) or actual != expected.lower():
    sys.exit('error: AppImage runtime %s: SHA-256 mismatch (expected %s, got %s)' % (path, expected, actual))
if (len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01" or
        data[8:11] != b"AI\x02" or struct.unpack_from("<H", data, 16)[0] not in (2, 3) or
        struct.unpack_from("<H", data, 18)[0] != 62):
    sys.exit("error: runtime must be a Type-2 ELF64 little-endian x86_64 executable")


def reject():
    sys.exit("error: runtime has invalid/truncated ELF tables, segments or entry point")


_, _, version, entry, phoff, shoff, _, ehsize, phsize, phnum, shsize, shnum, names = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
if (version != 1 or ehsize != 64 or phsize != 56 or shsize != 64 or
        not phnum or not shnum or not 0 < names < shnum or
        phoff < 64 or shoff < 64 or phoff + phnum * 56 > len(data) or
        shoff + shnum * 64 > len(data)):
    reject()
extent = max(64, phoff + phnum * 56, shoff + shnum * 64)
entry_ok = False
for i in range(phnum):
    kind, flags, offset, address, _, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + i * 56)
    if offset + filesz > len(data) or (kind == 1 and memsz < filesz):
        reject()
    extent = max(extent, offset + filesz)
    if kind == 1 and flags & 1 and address <= entry < address + filesz:
        entry_ok = True
for i in range(shnum):
    _, kind, _, _, offset, size, _, _, _, _ = struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * 64)
    if kind != 8:  # SHT_NOBITS has no file-backed bytes.
        if offset + size > len(data):
            reject()
        extent = max(extent, offset + size)
    if i == names and kind != 3:  # Section-name table must be SHT_STRTAB.
        reject()
if not entry_ok or extent != len(data):
    reject()
print("local runtime SHA-256:", actual)
PY
