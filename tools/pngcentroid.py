#!/usr/bin/env python3
"""pngcentroid.py A.png B.png: where B is brighter than A (centroid and extent of the gain, in image pixels). Dev shots."""
import struct, sys, zlib
def load(p):
    d = open(p, "rb").read(); pos = 8; w = h = 0; idat = b""
    while pos < len(d):
        n, t = struct.unpack(">I4s", d[pos:pos + 8]); c = d[pos + 8:pos + 8 + n]
        if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
        if t == b"IDAT": idat += c
        pos += 12 + n
    return w, h, zlib.decompress(idat)
w, h, a = load(sys.argv[1]); _, _, b = load(sys.argv[2])
st = w * 3 + 1; sx = sy = tot = 0; n = 0; x0 = y0 = 10**9; x1 = y1 = -1
for y in range(h):
    for x in range(w):
        i = y * st + 1 + x * 3
        g = max(0, b[i] + b[i + 1] + b[i + 2] - a[i] - a[i + 1] - a[i + 2])
        if g > 24:
            sx += x * g; sy += y * g; tot += g; n += 1
            x0 = min(x0, x); x1 = max(x1, x); y0 = min(y0, y); y1 = max(y1, y)
print("no gain" if not tot else "gain centroid %.0f,%.0f of %dx%d; %d px in x %d-%d y %d-%d" % (sx / tot, sy / tot, w, h, n, x0, x1, y0, y1))
