#!/usr/bin/env python3
"""pngtone.py A.png [B.png ...]: tone of dev shots: luma percentiles (5/25/50/75/95), contrast (p95 - p5), mean saturation."""
import struct, sys, zlib
for p in sys.argv[1:]:
    d = open(p, "rb").read(); pos = 8; w = h = 0; idat = b""
    while pos < len(d):
        n, t = struct.unpack(">I4s", d[pos:pos + 8]); c = d[pos + 8:pos + 8 + n]
        if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
        if t == b"IDAT": idat += c
        pos += 12 + n
    raw = zlib.decompress(idat); st = w * 3 + 1; ls = []; sat = 0.0
    for y in range(0, h, 2):
        row = raw[y * st + 1:(y + 1) * st]
        for x in range(0, w * 3, 6):
            r, g, b = row[x], row[x + 1], row[x + 2]; mx = max(r, g, b)
            ls.append(0.299 * r + 0.587 * g + 0.114 * b); sat += (mx - min(r, g, b)) / mx if mx else 0
    ls.sort(); q = lambda f: ls[int(f * (len(ls) - 1))]
    print("%-22s p5 %5.1f  p25 %5.1f  p50 %5.1f  p75 %5.1f  p95 %5.1f  contrast %5.1f  saturation %.2f" % (p.split("/")[-1], q(.05), q(.25), q(.5), q(.75), q(.95), q(.95) - q(.05), sat / len(ls)))
