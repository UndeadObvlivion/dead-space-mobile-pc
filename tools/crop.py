#!/usr/bin/env python3
"""crop.py IN.bmp OUT.png X Y W H [SCALE]: crops a region of a 32-bit BMP screenshot to PNG (nearest upscaling)."""
import struct, sys, zlib
d = open(sys.argv[1], "rb").read()
off = struct.unpack_from("<I", d, 10)[0]; w, h = struct.unpack_from("<ii", d, 18); top_down = h < 0; h = abs(h)
x0, y0, cw, ch = map(int, sys.argv[3:7]); sc = int(sys.argv[7]) if len(sys.argv) > 7 else 1
rows = []
for y in range(y0, y0 + ch):
    sy = y if top_down else h - 1 - y
    line = bytearray()
    for x in range(x0, x0 + cw):
        i = off + (sy * w + x) * 4
        px = bytes((d[i + 2], d[i + 1], d[i]))
        line += px * sc
    for _ in range(sc): rows.append(b"\0" + bytes(line))
raw = b"".join(rows)
def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", cw * sc, ch * sc, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
