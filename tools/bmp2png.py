#!/usr/bin/env python3
"""Convert 24/32-bit BMP screenshots to PNG (no third-party packages).
usage: bmp2png.py file.bmp [...] [--scale N]   (N = integer downscale factor)"""
import struct
import sys
import zlib


def read_bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    comp = struct.unpack_from("<I", d, 30)[0]
    bottom_up = h > 0
    h = abs(h)
    stride = ((w * bpp // 8) + 3) & ~3
    masks = None
    if comp == 3:  # BI_BITFIELDS
        masks = struct.unpack_from("<III", d, 54)
    rows = []
    for y in range(h):
        sy = h - 1 - y if bottom_up else y
        row = d[off + sy * stride: off + sy * stride + w * bpp // 8]
        out = bytearray()
        for x in range(w):
            if bpp == 32:
                v = struct.unpack_from("<I", row, x * 4)[0]
                if masks:
                    def ch(m):
                        if not m:
                            return 255
                        s = (m & -m).bit_length() - 1
                        return (v & m) >> s
                    out += bytes((ch(masks[0]), ch(masks[1]), ch(masks[2])))
                else:
                    out += bytes((row[x * 4 + 2], row[x * 4 + 1], row[x * 4]))
            else:
                out += bytes((row[x * 3 + 2], row[x * 3 + 1], row[x * 3]))
        rows.append(out)
    return w, h, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main():
    args = sys.argv[1:]
    scale = 1
    if "--scale" in args:
        i = args.index("--scale")
        scale = int(args[i + 1])
        del args[i:i + 2]
    for p in args:
        w, h, rows = read_bmp(p)
        if scale > 1:
            rows = [bytes(b for x in range(0, w - w % scale, scale) for b in r[x * 3:x * 3 + 3])
                    for r in rows[::scale]]
            w, h = w // scale, len(rows)
        out = p[:-4] + ".png"
        write_png(out, w, h, rows)
        print(out)


if __name__ == "__main__":
    main()
