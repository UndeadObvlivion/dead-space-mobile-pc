#!/usr/bin/env python3
"""blowout.py A.png [B.png ...]: share of pixels that are clipped (a channel >= 250) and mean brightness. Dev shots."""
import struct, sys, zlib
for p in sys.argv[1:]:
    d = open(p, "rb").read(); pos = 8; w = h = 0; idat = b""
    while pos < len(d):
        n, t = struct.unpack(">I4s", d[pos:pos + 8]); c = d[pos + 8:pos + 8 + n]
        if t == b"IHDR": w, h = struct.unpack(">II", c[:8])
        if t == b"IDAT": idat += c
        pos += 12 + n
    raw = zlib.decompress(idat); st = w * 3 + 1; clip = tot = 0; n = 0
    for y in range(0, h, 2):
        row = raw[y * st + 1:(y + 1) * st]
        for x in range(0, w * 3, 6):
            m = max(row[x], row[x + 1], row[x + 2]); tot += m; n += 1; clip += m >= 250
    print("%-28s clipped %5.1f%%  mean %3d" % (p.split("/")[-1], 100.0 * clip / n, tot // n))
