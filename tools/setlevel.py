#!/usr/bin/env python3
"""Test helper: make a save profile start at another level.
usage: setlevel.py <userdata dir> <level id, e.g. 01_gov_02>
Renames the level in 00_LevelSave.sb (same-length ids only) and removes the checkpoint save."""
import os
import struct
import sys

def fnv1(b):
    h = 0x811c9dc5
    for x in b:
        h = ((h * 0x01000193) & 0xffffffff) ^ x
    return h

root = os.path.join(sys.argv[1], "data/data/com.ea.deadspace/files/var")
new = sys.argv[2].encode()
p = os.path.join(root, "00_LevelSave.sb")
d = bytearray(open(p, "rb").read())
o = 8
while o + 12 <= len(d):
    tag = bytes(d[o:o + 4]); size = struct.unpack_from("<I", d, o + 4)[0]
    if tag == b"STRS":
        body = d[o + 12:o + 12 + size]
        i = body.find(b"01_gov_01")
        assert i >= 0 and len(new) == 9, "unexpected save layout"
        body[i:i + 9] = new
        d[o + 12:o + 12 + size] = body
        struct.pack_into("<I", d, o + 8, fnv1(body))
    o = (o + 12 + size + 3) & ~3
open(p, "wb").write(d)
cp = os.path.join(root, "00_CheckpointSave.sb")
if os.path.exists(cp):
    os.remove(cp)
print("profile now starts at", new.decode())
