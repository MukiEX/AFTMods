#!/usr/bin/env python3
"""Rebuild res/ft_shaders.farc from the files in res/ft_shaders/.

DivaGL loads its shaders from ft_shaders.farc (an embedded resource), not from the folder, so
run this after editing a shader:  python3 tools/pack_ft_shaders.py
Format: FArC, big-endian header (name, offset, compressed size, size), gzip entries, each
entry's data aligned to 16 bytes.
"""
import gzip
import os
import struct
import sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'res')
src = os.path.join(root, 'ft_shaders')
out = os.path.join(root, 'ft_shaders.farc')

names = sorted(n for n in os.listdir(src) if os.path.isfile(os.path.join(src, n)))
blobs = []
for n in names:
    raw = open(os.path.join(src, n), 'rb').read()
    blobs.append((n, raw, gzip.compress(raw, 9, mtime=0)))

header_size = 4 + sum(len(n.encode()) + 1 + 12 for n, _, _ in blobs)  # alignment + entries
pos = (8 + header_size + 15) & ~15
entries, data = b'', b''
for n, raw, comp in blobs:
    entries += n.encode() + b'\0' + struct.pack('>III', pos, len(comp), len(raw))
    pad = (-len(comp)) & 15
    data += comp + b'\0' * pad
    pos += len(comp) + pad

head = b'FArC' + struct.pack('>II', header_size, 1) + entries
head += b'\0' * (((8 + header_size + 15) & ~15) - len(head))
open(out, 'wb').write(head + data)
print('%s: %d shaders' % (os.path.relpath(out), len(names)), file=sys.stderr)
