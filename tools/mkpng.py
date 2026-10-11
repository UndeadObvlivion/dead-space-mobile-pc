#!/usr/bin/env python3
"""mkpng.py OUT.png W H R G B: a flat-colour RGB PNG (for texture pack tests)."""
import struct, sys, zlib
out, w, h, r, g, b = sys.argv[1], *map(int, sys.argv[2:7])
raw = (b"\0" + bytes([r, g, b]) * w) * h
def chunk(t, c): return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
