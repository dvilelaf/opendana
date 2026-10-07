#!/usr/bin/env python3
"""C6.42: pack PNG files into a multi-size .ico (PNG-compressed entries, Vista+).

usage: make-ico.py out.ico in16.png in24.png ...  (sizes read from the PNG IHDR)
"""
import struct
import sys

out, *pngs = sys.argv[1:]
entries, blobs, offset = [], [], 6 + 16 * len(pngs)
for p in pngs:
    data = open(p, 'rb').read()
    w, h = struct.unpack('>II', data[16:24])
    entries.append(struct.pack('<BBBBHHII', w % 256, h % 256, 0, 0, 1, 32, len(data), offset))
    blobs.append(data)
    offset += len(data)
with open(out, 'wb') as f:
    f.write(struct.pack('<HHH', 0, 1, len(pngs)) + b''.join(entries) + b''.join(blobs))
