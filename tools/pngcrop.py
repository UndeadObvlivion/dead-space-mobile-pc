#!/usr/bin/env python3
"""pngcrop.py IN.png OUT.png X Y W H [SCALE]: crops a region of a dev screenshot (unfiltered RGB PNG)."""
import struct, sys, zlib
d = open(sys.argv[1], "rb").read(); pos = 8; w = h = 0; idat = b""
while pos < len(d):
    n, t = struct.unpack(">I4s", d[pos:pos + 8]); c = d[pos + 8:pos + 8 + n]
    if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
    if t == b"IDAT": idat += c
    pos += 12 + n
raw = zlib.decompress(idat); stride = w * 3 + 1
x0, y0, cw, ch = map(int, sys.argv[3:7]); sc = int(sys.argv[7]) if len(sys.argv) > 7 else 1
rows = []
for y in range(y0, y0 + ch):
    line = raw[y * stride + 1 + x0 * 3: y * stride + 1 + (x0 + cw) * 3]
    if sc > 1: line = b"".join(line[i:i + 3] * sc for i in range(0, len(line), 3))
    rows += [b"\0" + line] * sc
def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", cw * sc, ch * sc, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))
