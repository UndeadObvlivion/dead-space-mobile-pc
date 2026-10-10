#!/usr/bin/env python3
"""montage.py OUT.png COLS A.png B.png ...: grid of half-size dev screenshots (unfiltered RGB PNGs of equal size)."""
import struct, sys, zlib
def load(p):
    d = open(p, "rb").read(); pos = 8; w = h = 0; idat = b""
    while pos < len(d):
        n, t = struct.unpack(">I4s", d[pos:pos + 8]); c = d[pos + 8:pos + 8 + n]
        if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
        if t == b"IDAT": idat += c
        pos += 12 + n
    raw = zlib.decompress(idat); st = w * 3 + 1
    return w // 2, h // 2, [b"".join(raw[y * st + 1 + x * 3: y * st + 4 + x * 3] for x in range(0, w - 1, 2)) for y in range(0, h - 1, 2)]
cols = int(sys.argv[2]); imgs = [load(p) for p in sys.argv[3:]]
w, h = imgs[0][0], imgs[0][1]; rows = []
for r in range(0, len(imgs), cols):
    grp = imgs[r:r + cols]
    for y in range(h):
        rows.append(b"\0" + b"".join(g[2][y] for g in grp) + b"\0" * (3 * w * (cols - len(grp))))
def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
open(sys.argv[1], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * cols, len(rows), 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
