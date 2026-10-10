#!/usr/bin/env python3
"""Reader for EA IronMonkey "SBIN" binary databases (layouts.xml.sb, stringdata, ...).

Format (from im::serialization::Database in the game):
  "SBIN" u32 version, then chunks: tag(4) size(4) fnv-hash(4) data, 4-byte aligned.
  STRU  6-byte struct defs  (u16 name+1, u16 first field, u16 field count)
  FIEL  8-byte field defs   (u16 name+1, u16 type, u16 offset, u16 extra)
  OHDR  u32 per object: low 3 bits kind (0 struct, 1 root/unstructured, 2 array), >>3 = DATA offset
  DATA  object bodies. kind 0: u16 struct index, data. kind 2: u16 elem type, u16 struct, u32 count, data
  STRS  u32 count, then (u32 len, bytes) strings; string fields store index+1 (0 = none)
"""
import struct
import sys

TYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 2, 5: 4, 6: 4, 7: 8, 8: 8, 9: 1, 10: 4, 11: 8, 12: 2, 13: 2, 15: 4, 17: 4,
             18: 4, 19: 4, 20: 2}
HDR_SIZE = {0: 2, 1: 4, 2: 8}


class SBin:
    def __init__(self, data):
        self.d = data
        self.c = {}
        o = 8
        while o + 12 <= len(data):
            tag = data[o:o + 4].decode()
            size, h = struct.unpack_from("<II", data, o + 4)
            self.c[tag] = (o + 12, size, h)
            o = (o + 12 + size + 3) & ~3
        off, size, _ = self.c["STRS"]
        n = struct.unpack_from("<I", data, off)[0]
        p = off + 4
        self.strs = []
        for _ in range(n):
            ln = struct.unpack_from("<I", data, p)[0]
            self.strs.append(data[p + 4:p + 4 + ln].decode("utf-8", "replace"))
            p += 4 + ln
        off, size, _ = self.c["STRU"]
        self.structs = [struct.unpack_from("<3H", data, off + 6 * i) for i in range(size // 6)]
        off, size, _ = self.c["FIEL"]
        self.fields = [struct.unpack_from("<4H", data, off + 8 * i) for i in range(size // 8)]
        off, size, _ = self.c["OHDR"]
        self.ohdr = list(struct.unpack_from("<%dI" % (size // 4), data, off))
        self.doff = self.c["DATA"][0]

    def s(self, idx1):
        return self.strs[idx1 - 1] if 0 < idx1 <= len(self.strs) else None

    def struct_name(self, i):
        return self.s(self.structs[i][0])

    def struct_fields(self, i):
        _, first, count = self.structs[i]
        return [(self.s(self.fields[f][0]), self.fields[f][1], self.fields[f][2]) for f in range(first, first + count)]

    def obj(self, idx):
        h = self.ohdr[idx]
        return h & 7, self.doff + (h >> 3)

    def read_struct(self, si, p):
        out = {}
        for name, t, off in self.struct_fields(si):
            q = p + off
            if t == 13:
                out[name] = self.s(struct.unpack_from("<H", self.d, q)[0])
            elif t == 10:
                out[name] = struct.unpack_from("<f", self.d, q)[0]
            elif t == 5:
                out[name] = struct.unpack_from("<i", self.d, q)[0]
            elif t == 9:
                out[name] = self.d[q]
            elif t == 17:
                out[name] = ("array", struct.unpack_from("<i", self.d, q)[0])
            else:
                out[name] = ("t%d" % t, self.d[q:q + TYPE_SIZE.get(t, 4)].hex())
        return out

    def struct_size(self, si):
        return max(off + TYPE_SIZE.get(t, 4) for _, t, off in self.struct_fields(si))

    def read_array(self, idx):
        kind, p = self.obj(idx)
        assert kind == 2, kind
        et, si, n = struct.unpack_from("<HHI", self.d, p)
        p += 8
        if et == 16:
            sz = (self.struct_size(si) + 3) & ~3
            return [self.read_struct(si, p + i * sz) for i in range(n)]
        if et == 13:
            return [self.s(v) for v in struct.unpack_from("<%dH" % n, self.d, p)]
        return ("elem type %d" % et, n)

    def read_object(self, idx):
        kind, p = self.obj(idx)
        if kind == 0:
            si = struct.unpack_from("<H", self.d, p)[0]
            return self.struct_name(si), self.read_struct(si, p + 2)
        if kind == 2:
            return "array", self.read_array(idx)
        return "kind%d" % kind, None


def main():
    db = SBin(open(sys.argv[1], "rb").read())
    want = sys.argv[2] if len(sys.argv) > 2 else None
    for i in range(len(db.ohdr)):
        kind, _ = db.obj(i)
        if kind != 0:
            continue
        name, o = db.read_object(i)
        if name != "Layout" or (want and o["layoutId"] != want):
            continue
        print("Layout #%d %s" % (i, o["layoutId"]))
        if want:
            for e in db.read_array(o["entities"][1]):
                verts = db.read_array(e["vertices"][1]) if e["vertices"][1] >= 0 else []
                vs = " ".join("(%g,%g)" % (v["x"], v["y"]) for v in verts)
                print("  %-28s type=%-14s str=%-22s layout=%-22s img=%s w=%g h=%g flags=%d vis=%d %s" % (
                    e["entityName"], e["type"], e["stringId"], e["layout"], e["image"], e["width"], e["height"],
                    e["flags"], e["isVisible"], vs))


if __name__ == "__main__":
    main()
