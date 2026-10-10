#!/usr/bin/env python3
"""framediff.py A.bmp B.bmp [--out diff.png]: compares two screenshots.
Prints the share of pixels differing by more than 8 and 64 (max channel delta), and optionally
writes a half-scale image where differing pixels are highlighted."""
import struct, sys, zlib
def load(p):
    d = open(p, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]; w, h = struct.unpack_from("<ii", d, 18)
    return w, abs(h), d[off:]
a, b = sys.argv[1], sys.argv[2]
out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
wa, ha, A = load(a); wb, hb, B = load(b)
assert (wa, ha) == (wb, hb), "sizes differ"
n = wa * ha; d8 = d64 = 0; rows = []
for y in range(ha):
    row = bytearray(); row.append(0)
    for x in range(wa):
        i = (y * wa + x) * 4
        d = max(abs(A[i] - B[i]), abs(A[i + 1] - B[i + 1]), abs(A[i + 2] - B[i + 2]))
        if d > 8: d8 += 1
        if d > 64: d64 += 1
        if out and y % 2 == 0 and x % 2 == 0:
            g = A[i + 1] // 3
            row += bytes((255, g, g) if d > 64 else (255, 160 + g // 2, 0) if d > 8 else (g, g, g))
    if out and y % 2 == 0: rows.append(bytes(row))
print(f"{a} vs {b}: {100 * d8 / n:.2f}% differ (>8), {100 * d64 / n:.2f}% strongly (>64)")
if out:
    ow, oh = wa // 2, ha // 2
    raw = b"".join(r[:1 + ow * 3] for r in rows[:oh])
    def chunk(t, data):
        return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(out, "wb").write(png); print("wrote", out)
