#!/usr/bin/env python3
"""Offline check: decode a dumped 16-byte/block payload as ETC2 RGBA8 (EAC alpha + ETC2 color)."""
import struct
import sys

sys.path.insert(0, "tools")
from bmp2png import write_png  # noqa: E402

ETC1_TBL = [[2, 8, -2, -8], [5, 17, -5, -17], [9, 29, -9, -29], [13, 42, -13, -42], [18, 60, -18, -60],
            [24, 80, -24, -80], [33, 106, -33, -106], [47, 183, -47, -183]]
ETC2_DIST = [3, 6, 11, 16, 23, 32, 41, 64]
EAC_TBL = [[-3, -6, -9, -15, 2, 5, 8, 14], [-3, -7, -10, -13, 2, 6, 9, 12], [-2, -5, -8, -13, 1, 4, 7, 12],
           [-2, -4, -6, -13, 1, 3, 5, 12], [-3, -6, -8, -12, 2, 5, 7, 11], [-3, -7, -9, -11, 2, 6, 8, 10],
           [-4, -7, -8, -11, 3, 6, 7, 10], [-3, -5, -8, -11, 2, 4, 7, 10], [-2, -6, -8, -10, 1, 5, 7, 9],
           [-2, -5, -8, -10, 1, 4, 7, 9], [-2, -4, -8, -10, 1, 3, 7, 9], [-2, -5, -7, -10, 1, 4, 6, 9],
           [-3, -4, -7, -10, 2, 3, 6, 9], [-1, -2, -3, -10, 0, 1, 2, 9], [-4, -6, -8, -9, 3, 5, 7, 8],
           [-3, -5, -7, -9, 2, 4, 6, 8]]


def clamp(v):
    return max(0, min(255, v))


def etc2_color(b):
    hi, lo = struct.unpack(">II", b)
    out = [[0, 0, 0] for _ in range(16)]  # index x*4+y

    def idx(i):
        return ((lo >> i) & 1) | (((lo >> (i + 16)) & 1) << 1)

    diff = (hi >> 1) & 1
    flip = hi & 1
    if diff:
        r, g, bb = (hi >> 27) & 31, (hi >> 19) & 31, (hi >> 11) & 31

        def s3(v):
            return v - 8 if v & 4 else v
        r2, g2, b2 = r + s3((hi >> 24) & 7), g + s3((hi >> 16) & 7), bb + s3((hi >> 8) & 7)
        if r2 < 0 or r2 > 31:  # T mode
            R1 = (((hi >> 27) & 3) << 2) | ((hi >> 24) & 3)
            G1, B1 = (hi >> 20) & 15, (hi >> 16) & 15
            R2, G2, B2 = (hi >> 12) & 15, (hi >> 8) & 15, (hi >> 4) & 15
            d = ETC2_DIST[(((hi >> 2) & 3) << 1) | (hi & 1)]
            c1 = [R1 * 17, G1 * 17, B1 * 17]
            c2 = [R2 * 17, G2 * 17, B2 * 17]
            paint = [c1, [clamp(v + d) for v in c2], c2, [clamp(v - d) for v in c2]]
            for i in range(16):
                out[i] = paint[idx(i)]
            return out
        if g2 < 0 or g2 > 31:  # H mode
            R1 = (hi >> 27) & 15
            G1 = (((hi >> 24) & 7) << 1) | ((hi >> 20) & 1)
            B1 = (((hi >> 19) & 1) << 3) | ((hi >> 15) & 7)
            R2, G2, B2 = (hi >> 11) & 15, (hi >> 7) & 15, (hi >> 3) & 15
            o1 = (R1 << 8) | (G1 << 4) | B1
            o2 = (R2 << 8) | (G2 << 4) | B2
            d = ETC2_DIST[(((hi >> 2) & 1) << 2) | ((hi & 1) << 1) | (1 if o1 >= o2 else 0)]
            c1 = [R1 * 17, G1 * 17, B1 * 17]
            c2 = [R2 * 17, G2 * 17, B2 * 17]
            paint = [[clamp(v + d) for v in c1], [clamp(v - d) for v in c1], [clamp(v + d) for v in c2],
                     [clamp(v - d) for v in c2]]
            for i in range(16):
                out[i] = paint[idx(i)]
            return out
        if b2 < 0 or b2 > 31:  # planar
            R0 = (hi >> 25) & 63
            G0 = (((hi >> 24) & 1) << 6) | ((hi >> 17) & 63)
            B0 = (((hi >> 16) & 1) << 5) | (((hi >> 11) & 3) << 3) | ((hi >> 7) & 7)
            RH = (((hi >> 2) & 31) << 1) | (hi & 1)
            GH, BH = (lo >> 25) & 127, (lo >> 19) & 63
            RV, GV, BV = (lo >> 13) & 63, (lo >> 6) & 127, lo & 63
            e6 = lambda v: (v << 2) | (v >> 4)
            e7 = lambda v: (v << 1) | (v >> 6)
            O = [e6(R0), e7(G0), e6(B0)]
            H = [e6(RH), e7(GH), e6(BH)]
            V = [e6(RV), e7(GV), e6(BV)]
            for x in range(4):
                for y in range(4):
                    out[x * 4 + y] = [clamp((x * (H[k] - O[k]) + y * (V[k] - O[k]) + 4 * O[k] + 2) >> 2) for k in range(3)]
            return out
        e5 = lambda v: (v << 3) | (v >> 2)
        base = [[e5(r), e5(g), e5(bb)], [e5(r2), e5(g2), e5(b2)]]
    else:
        e4 = lambda v: v * 17
        base = [[e4((hi >> 28) & 15), e4((hi >> 20) & 15), e4((hi >> 12) & 15)],
                [e4((hi >> 24) & 15), e4((hi >> 16) & 15), e4((hi >> 8) & 15)]]
    t = [(hi >> 5) & 7, (hi >> 2) & 7]
    for x in range(4):
        for y in range(4):
            sub = (y >= 2) if flip else (x >= 2)
            m = ETC1_TBL[t[sub]][idx(x * 4 + y)]
            out[x * 4 + y] = [clamp(v + m) for v in base[sub]]
    return out


def eac_alpha(b):
    base, mult, tbl = b[0], b[1] >> 4, b[1] & 15
    bits = int.from_bytes(b[2:8], "big")
    return [clamp(base + EAC_TBL[tbl][(bits >> (45 - 3 * i)) & 7] * mult) for i in range(16)]  # i = x*4+y


def main():
    path, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    d = open(path, "rb").read()
    rows = [bytearray(w * 3) for _ in range(h)]
    bw = w // 4
    for by in range(h // 4):
        for bx in range(bw):
            o = (by * bw + bx) * 16
            a = eac_alpha(d[o:o + 8])
            c = etc2_color(d[o + 8:o + 16])
            for x in range(4):
                for y in range(4):
                    i = x * 4 + y
                    al = a[i] / 255
                    px = bytes(int(c[i][k] * al + 60 * (1 - al)) for k in range(3))
                    X, Y = bx * 4 + x, by * 4 + y
                    rows[Y][X * 3:X * 3 + 3] = px
    write_png(sys.argv[4], w, h, rows)


if __name__ == "__main__":
    main()
