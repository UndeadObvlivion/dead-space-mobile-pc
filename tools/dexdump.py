#!/usr/bin/env python3
"""Minimal Dalvik .dex disassembler (smali-like output), no dependencies.

usage: dexdump.py classes.dex [class-filter-substring] > out.txt
"""
import struct
import sys


def uleb(b, o):
    r = 0
    s = 0
    while True:
        x = b[o]
        o += 1
        r |= (x & 0x7F) << s
        s += 7
        if not x & 0x80:
            return r, o


def mutf8(b, o):
    _, o = uleb(b, o)
    e = b.index(0, o)
    return b[o:e].decode("utf-8", "replace")


# (name, format, index kind)
OPS = {}


def op(code, name, fmt, kind=None):
    OPS[code] = (name, fmt, kind)


_simple = [
    (0x00, "nop", "10x"), (0x01, "move", "12x"), (0x02, "move/from16", "22x"), (0x03, "move/16", "32x"),
    (0x04, "move-wide", "12x"), (0x05, "move-wide/from16", "22x"), (0x06, "move-wide/16", "32x"),
    (0x07, "move-object", "12x"), (0x08, "move-object/from16", "22x"), (0x09, "move-object/16", "32x"),
    (0x0A, "move-result", "11x"), (0x0B, "move-result-wide", "11x"), (0x0C, "move-result-object", "11x"),
    (0x0D, "move-exception", "11x"), (0x0E, "return-void", "10x"), (0x0F, "return", "11x"),
    (0x10, "return-wide", "11x"), (0x11, "return-object", "11x"), (0x12, "const/4", "11n"),
    (0x13, "const/16", "21s"), (0x14, "const", "31i"), (0x15, "const/high16", "21h"),
    (0x16, "const-wide/16", "21s"), (0x17, "const-wide/32", "31i"), (0x18, "const-wide", "51l"),
    (0x19, "const-wide/high16", "21h"), (0x1D, "monitor-enter", "11x"), (0x1E, "monitor-exit", "11x"),
    (0x21, "array-length", "12x"), (0x26, "fill-array-data", "31t"), (0x27, "throw", "11x"),
    (0x28, "goto", "10t"), (0x29, "goto/16", "20t"), (0x2A, "goto/32", "30t"),
    (0x2B, "packed-switch", "31t"), (0x2C, "sparse-switch", "31t"),
]
for c, n, f in _simple:
    op(c, n, f)
op(0x1A, "const-string", "21c", "string")
op(0x1B, "const-string/jumbo", "31c", "string")
op(0x1C, "const-class", "21c", "type")
op(0x1F, "check-cast", "21c", "type")
op(0x20, "instance-of", "22c", "type")
op(0x22, "new-instance", "21c", "type")
op(0x23, "new-array", "22c", "type")
op(0x24, "filled-new-array", "35c", "type")
op(0x25, "filled-new-array/range", "3rc", "type")
for i, n in enumerate(["cmpl-float", "cmpg-float", "cmpl-double", "cmpg-double", "cmp-long"]):
    op(0x2D + i, n, "23x")
for i, n in enumerate(["eq", "ne", "lt", "ge", "gt", "le"]):
    op(0x32 + i, "if-" + n, "22t")
    op(0x38 + i, "if-" + n + "z", "21t")
_t = ["", "-wide", "-object", "-boolean", "-byte", "-char", "-short"]
for i, s in enumerate(_t):
    op(0x44 + i, "aget" + s, "23x")
    op(0x4B + i, "aput" + s, "23x")
    op(0x52 + i, "iget" + s, "22c", "field")
    op(0x59 + i, "iput" + s, "22c", "field")
    op(0x60 + i, "sget" + s, "21c", "field")
    op(0x67 + i, "sput" + s, "21c", "field")
for i, n in enumerate(["virtual", "super", "direct", "static", "interface"]):
    op(0x6E + i, "invoke-" + n, "35c", "method")
    op(0x74 + i, "invoke-" + n + "/range", "3rc", "method")
_un = ["neg-int", "not-int", "neg-long", "not-long", "neg-float", "neg-double", "int-to-long", "int-to-float",
       "int-to-double", "long-to-int", "long-to-float", "long-to-double", "float-to-int", "float-to-long",
       "float-to-double", "double-to-int", "double-to-long", "double-to-float", "int-to-byte", "int-to-char",
       "int-to-short"]
for i, n in enumerate(_un):
    op(0x7B + i, n, "12x")
_bin = []
for t, ops in (("int", ["add", "sub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr", "ushr"]),
               ("long", ["add", "sub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr", "ushr"]),
               ("float", ["add", "sub", "mul", "div", "rem"]),
               ("double", ["add", "sub", "mul", "div", "rem"])):
    _bin += ["%s-%s" % (o, t) for o in ops]
for i, n in enumerate(_bin):
    op(0x90 + i, n, "23x")
    op(0xB0 + i, n + "/2addr", "12x")
for i, n in enumerate(["add", "rsub", "mul", "div", "rem", "and", "or", "xor"]):
    op(0xD0 + i, n + "-int/lit16", "22s")
for i, n in enumerate(["add", "rsub", "mul", "div", "rem", "and", "or", "xor", "shl", "shr", "ushr"]):
    op(0xD8 + i, n + "-int/lit8", "22b")

FMT_LEN = {"10x": 1, "12x": 1, "11n": 1, "11x": 1, "10t": 1, "20t": 2, "22x": 2, "21t": 2, "21s": 2, "21h": 2,
           "21c": 2, "23x": 2, "22b": 2, "22t": 2, "22s": 2, "22c": 2, "30t": 3, "32x": 3, "31i": 3, "31t": 3,
           "31c": 3, "35c": 3, "3rc": 3, "51l": 5}


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def s8(x):
    return x - 0x100 if x & 0x80 else x


class Dex:
    def __init__(self, b):
        self.b = b
        h = struct.unpack_from("<8s4x20s20I", b, 0)
        (_, _, self.file_size, _, _, _, _, _, nstr, ostr, ntype, otype, nproto, oproto, nfield, ofield,
         nmeth, ometh, ncls, ocls, _, _) = h
        self.strings = [mutf8(b, struct.unpack_from("<I", b, ostr + 4 * i)[0]) for i in range(nstr)]
        self.types = [self.strings[struct.unpack_from("<I", b, otype + 4 * i)[0]] for i in range(ntype)]
        self.protos = []
        for i in range(nproto):
            _, ret, poff = struct.unpack_from("<III", b, oproto + 12 * i)
            self.protos.append((self.types[ret], self.typelist(poff)))
        self.fields = []
        for i in range(nfield):
            c, t, n = struct.unpack_from("<HHI", b, ofield + 8 * i)
            self.fields.append((self.types[c], self.types[t], self.strings[n]))
        self.methods = []
        for i in range(nmeth):
            c, p, n = struct.unpack_from("<HHI", b, ometh + 8 * i)
            self.methods.append((self.types[c], self.protos[p], self.strings[n]))
        self.classes = [struct.unpack_from("<8I", b, ocls + 32 * i) for i in range(ncls)]

    def typelist(self, off):
        if not off:
            return []
        n = struct.unpack_from("<I", self.b, off)[0]
        return [self.types[struct.unpack_from("<H", self.b, off + 4 + 2 * i)[0]] for i in range(n)]

    def mstr(self, i):
        c, (ret, params), n = self.methods[i]
        return "%s->%s(%s)%s" % (c, n, "".join(params), ret)

    def fstr(self, i):
        c, t, n = self.fields[i]
        return "%s->%s:%s" % (c, n, t)

    def idx(self, kind, i):
        if kind == "string":
            return repr(self.strings[i])
        if kind == "type":
            return self.types[i]
        if kind == "field":
            return self.fstr(i)
        if kind == "method":
            return self.mstr(i)
        return str(i)

    def disasm(self, off, out):
        regs, ins, outs, tries, dbg, n = struct.unpack_from("<HHHHII", self.b, off)
        out.append("    .registers %d (ins %d)" % (regs, ins))
        base = off + 16
        w = struct.unpack_from("<%dH" % n, self.b, base)
        pc = 0
        while pc < n:
            u = w[pc]
            o = u & 0xFF
            if o == 0 and u in (0x100, 0x200, 0x300):  # payloads
                if u == 0x100:
                    sz = w[pc + 1]
                    ln = 4 + sz * 2
                elif u == 0x200:
                    sz = w[pc + 1]
                    ln = 2 + sz * 4
                else:
                    es = w[pc + 1]
                    cnt = w[pc + 2] | (w[pc + 3] << 16)
                    ln = 4 + (es * cnt + 1) // 2
                out.append("    %04x: <payload %x len %d>" % (pc, u, ln))
                pc += ln
                continue
            if o not in OPS:
                out.append("    %04x: ?? %04x" % (pc, u))
                pc += 1
                continue
            name, fmt, kind = OPS[o]
            a = u >> 8
            L = FMT_LEN[fmt]
            x = w[pc:pc + L]
            if fmt == "10x":
                t = ""
            elif fmt == "12x":
                t = "v%d, v%d" % (a & 15, a >> 4)
            elif fmt == "11n":
                v = a >> 4
                t = "v%d, #%d" % (a & 15, v - 16 if v & 8 else v)
            elif fmt == "11x":
                t = "v%d" % a
            elif fmt == "10t":
                t = "%04x" % (pc + s8(a))
            elif fmt == "20t":
                t = "%04x" % (pc + s16(x[1]))
            elif fmt == "22x":
                t = "v%d, v%d" % (a, x[1])
            elif fmt == "21t":
                t = "v%d, %04x" % (a, pc + s16(x[1]))
            elif fmt == "21s":
                t = "v%d, #%d" % (a, s16(x[1]))
            elif fmt == "21h":
                t = "v%d, #0x%x0000" % (a, x[1])
            elif fmt == "21c":
                t = "v%d, %s" % (a, self.idx(kind, x[1]))
            elif fmt == "23x":
                t = "v%d, v%d, v%d" % (a, x[1] & 0xFF, x[1] >> 8)
            elif fmt == "22b":
                t = "v%d, v%d, #%d" % (a, x[1] & 0xFF, s8(x[1] >> 8))
            elif fmt == "22t":
                t = "v%d, v%d, %04x" % (a & 15, a >> 4, pc + s16(x[1]))
            elif fmt == "22s":
                t = "v%d, v%d, #%d" % (a & 15, a >> 4, s16(x[1]))
            elif fmt == "22c":
                t = "v%d, v%d, %s" % (a & 15, a >> 4, self.idx(kind, x[1]))
            elif fmt == "30t":
                v = x[1] | (x[2] << 16)
                t = "%04x" % ((pc + (v - (1 << 32) if v >> 31 else v)) & 0xFFFFFFFF)
            elif fmt == "32x":
                t = "v%d, v%d" % (x[1], x[2])
            elif fmt in ("31i", "31t", "31c"):
                v = x[1] | (x[2] << 16)
                if fmt == "31c":
                    t = "v%d, %s" % (a, self.idx(kind, v))
                elif fmt == "31t":
                    t = "v%d, %04x" % (a, pc + (v - (1 << 32) if v >> 31 else v))
                else:
                    t = "v%d, #%d" % (a, v - (1 << 32) if v >> 31 else v)
            elif fmt == "35c":
                cnt = a >> 4
                g = a & 15
                r = [x[2] & 15, (x[2] >> 4) & 15, (x[2] >> 8) & 15, x[2] >> 12, g][:cnt]
                t = "{%s}, %s" % (", ".join("v%d" % i for i in r), self.idx(kind, x[1]))
            elif fmt == "3rc":
                t = "{v%d .. v%d}, %s" % (x[2], x[2] + a - 1, self.idx(kind, x[1]))
            elif fmt == "51l":
                v = x[1] | (x[2] << 16) | (x[3] << 32) | (x[4] << 48)
                t = "v%d, #0x%x" % (a, v)
            else:
                t = "?"
            out.append("    %04x: %s %s" % (pc, name, t))
            pc += L

    def dump(self, filt=None):
        out = []
        for c in self.classes:
            cname = self.types[c[0]]
            if filt and filt not in cname:
                continue
            sup = self.types[c[2]] if c[2] != 0xFFFFFFFF else "-"
            out.append("\n.class %s extends %s  flags=%x" % (cname, sup, c[1]))
            if c[3]:
                out.append("  .implements " + " ".join(self.typelist(c[3])))
            if not c[6]:
                continue
            o = c[6]
            nsf, o = uleb(self.b, o)
            nif, o = uleb(self.b, o)
            ndm, o = uleb(self.b, o)
            nvm, o = uleb(self.b, o)
            for cnt, tag in ((nsf, "static"), (nif, "")):
                fi = 0
                for _ in range(cnt):
                    d, o = uleb(self.b, o)
                    fl, o = uleb(self.b, o)
                    fi += d
                    out.append("  .field %s %s  flags=%x" % (tag, self.fstr(fi), fl))
            for cnt in (ndm, nvm):
                mi = 0
                for _ in range(cnt):
                    d, o = uleb(self.b, o)
                    fl, o = uleb(self.b, o)
                    co, o = uleb(self.b, o)
                    mi += d
                    out.append("  .method %s  flags=%x%s" % (self.mstr(mi), fl, " native" if fl & 0x100 else ""))
                    if co:
                        self.disasm(co, out)
        return "\n".join(out)


if __name__ == "__main__":
    d = Dex(open(sys.argv[1], "rb").read())
    sys.stdout.reconfigure(encoding="utf-8")
    print(d.dump(sys.argv[2] if len(sys.argv) > 2 else None))
