#!/usr/bin/env python3
"""Disassemble a guest address range with symbolized branch targets.
usage: disrange.py START END   (hex)"""
import bisect
import subprocess
import sys

from capstone import CS_ARCH_ARM, CS_MODE_ARM, Cs

LIB = "apk/assets/guest_libs/libDeadSpace.so"
data = open(LIB, "rb").read()
out = subprocess.run(["readelf", "-s", "-W", LIB], capture_output=True, text=True).stdout
syms = []
for line in out.splitlines():
    p = line.split()
    if len(p) >= 8 and p[3] == "FUNC" and p[6] != "UND":
        syms.append((int(p[1], 16) & ~1, p[7]))
syms.sort()
addrs = [s[0] for s in syms]
names = subprocess.run(["c++filt"], input="\n".join(s[1] for s in syms), capture_output=True,
                       text=True).stdout.split("\n")


def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    return names[i] + ("" if a == syms[i][0] else "+%#x" % (a - syms[i][0]))


s, e = int(sys.argv[1], 16), int(sys.argv[2], 16)
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
for i in md.disasm(data[s:e], s):
    extra = ""
    if i.mnemonic in ("bl", "blx") and i.op_str.startswith("#"):
        extra = "  ; " + name(int(i.op_str[1:], 16))[:110]
    print(f"{i.address:08x}: {i.mnemonic:8s} {i.op_str}{extra}")
