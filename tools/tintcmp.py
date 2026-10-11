#!/usr/bin/env python3
"""tintcmp.py REF.png A.png [B.png ...]: mean colour and saturation of each image, and how far A, B... are from REF."""
import struct, sys, zlib
def stats(p):
    d = open(p, "rb").read(); pos = 8; w = h = 0; idat = b""
    while pos < len(d):
        n, t = struct.unpack(">I4s", d[pos:pos + 8]); c = d[pos + 8:pos + 8 + n]
        if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
        if t == b"IDAT": idat += c
        pos += 12 + n
    raw = zlib.decompress(idat); st = w * 3 + 1; s = [0, 0, 0]; n = 0
    for y in range(40, h, 2):  # (skip the HUD strip at the top)
        row = raw[y * st + 1:(y + 1) * st]
        for x in range(0, w * 3, 6):
            s[0] += row[x]; s[1] += row[x + 1]; s[2] += row[x + 2]; n += 1
    m = [v / n for v in s]; mx = max(m); mn = min(m)
    return m, (mx - mn) / mx if mx else 0
ref, rs = stats(sys.argv[1])
print("%-14s mean %3.0f %3.0f %3.0f  saturation %.2f" % (sys.argv[1].split("/")[-1], *ref, rs))
for p in sys.argv[2:]:
    m, s = stats(p)
    print("%-14s mean %3.0f %3.0f %3.0f  saturation %.2f  (saturation %+.2f, brightness %+.0f%%)" % (p.split("/")[-1], *m, s, s - rs, 100 * (max(m) / max(ref) - 1) if max(ref) else 0))
