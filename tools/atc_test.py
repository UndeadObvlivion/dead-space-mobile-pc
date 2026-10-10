#!/usr/bin/env python3
"""Offline ATC decode experiment: decodes the top mip of an IM-M3G texture to PNG with
different interpretations, to check which one produces a clean image."""
import struct
import sys
import zlib

sys.path.insert(0, "tools")
from bmp2png import write_png  # noqa: E402

path = sys.argv[1]
d = open(path, "rb").read()
off = d.index(b"\x70\x55\x55\x00") + 4 if len(sys.argv) < 3 else int(sys.argv[2], 0)
W = H = int(sys.argv[3]) if len(sys.argv) > 3 else 2048
CROP = 256


def e5(v): return (v << 3) | (v >> 2)
def e6(v): return (v << 2) | (v >> 4)


def color_block(b, variant):
    c0 = b[0] | (b[1] << 8)
    c1 = b[2] | (b[3] << 8)
    a = [e5((c0 >> 10) & 31), e5((c0 >> 5) & 31), e5(c0 & 31)]
    z = [e5((c1 >> 11) & 31), e6((c1 >> 5) & 63), e5(c1 & 31)]
    if variant == "dxt":  # treat as plain DXT1 color block for comparison
        a = [e5((c0 >> 11) & 31), e6((c0 >> 5) & 63), e5(c0 & 31)]
        cols = [a, [(2 * a[k] + z[k]) // 3 for k in range(3)], [(a[k] + 2 * z[k]) // 3 for k in range(3)], z]
        cols = [cols[0], cols[3], cols[1], cols[2]]
    elif not (c0 & 0x8000):
        cols = [a, [(5 * a[k] + 3 * z[k]) // 8 for k in range(3)], [(3 * a[k] + 5 * z[k]) // 8 for k in range(3)], z]
    else:
        cols = [[0, 0, 0], [max(0, a[k] - z[k] // 4) for k in range(3)], a, z]
    idx = struct.unpack_from("<I", b, 4)[0]
    return [cols[(idx >> (2 * i)) & 3] for i in range(16)]


def decode(variant, color_first):
    bw = W // 4
    rows = [bytearray(CROP * 3) for _ in range(CROP)]
    for by in range(CROP // 4):
        for bx in range(CROP // 4):
            o = off + (by * bw + bx) * 16
            blk = d[o:o + 16]
            cb = blk[0:8] if color_first else blk[8:16]
            px = color_block(cb, variant)
            for i in range(16):
                x, y = bx * 4 + (i % 4), by * 4 + (i // 4)
                rows[y][x * 3:x * 3 + 3] = bytes(px[i])
    return rows


for variant in ("atc", "dxt"):
    for cf in (False, True):
        name = f"shots/atc_{variant}_{'colorfirst' if cf else 'alphafirst'}.png"
        write_png(name, CROP, CROP, decode(variant, cf))
        print(name)
