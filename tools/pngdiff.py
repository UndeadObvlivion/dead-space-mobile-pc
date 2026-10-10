#!/usr/bin/env python3
"""pngdiff.py A.png B.png: share of pixels differing (>8 / >64 max channel delta). Unfiltered RGB PNGs (dev shots)."""
import struct, sys, zlib
def load(p):
    d = open(p, "rb").read(); pos = 8; w = h = 0; idat = b""
    while pos < len(d):
        n = struct.unpack_from(">I", d, pos)[0]; t = d[pos+4:pos+8]; c = d[pos+8:pos+8+n]
        if t == b"IHDR": w, h = struct.unpack_from(">II", c)
        elif t == b"IDAT": idat += c
        pos += 12 + n
    raw = zlib.decompress(idat); st = w*3+1
    return w, h, b"".join(raw[y*st+1:(y+1)*st] for y in range(h))
wa, ha, A = load(sys.argv[1]); wb, hb, B = load(sys.argv[2]); n = wa*ha; d8 = d64 = 0
for i in range(0, n*3, 3):
    d = max(abs(A[i]-B[i]), abs(A[i+1]-B[i+1]), abs(A[i+2]-B[i+2]))
    d8 += d > 8; d64 += d > 64
print(f"{sys.argv[1].split('/')[-1]} vs {sys.argv[2].split('/')[-1]}: {100*d8/n:.2f}% differ, {100*d64/n:.2f}% strongly")
