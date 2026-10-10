#!/usr/bin/env python3
"""List the call targets (bl/blx) of a guest function, symbolized."""
import sys, subprocess, bisect
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
LIB = "apk/assets/guest_libs/libDeadSpace.so"
data = open(LIB, "rb").read()
out = subprocess.run(["readelf", "-s", "-W", LIB], capture_output=True, text=True).stdout
syms = []
for line in out.splitlines():
    p = line.split()
    if len(p) >= 8 and p[3] == "FUNC" and p[6] != "UND":
        syms.append((int(p[1], 16) & ~1, int(p[2]), p[7], int(p[1], 16) & 1))
syms.sort()
addrs = [s[0] for s in syms]
dem = subprocess.run(["c++filt"], input="\n".join(s[2] for s in syms), capture_output=True, text=True).stdout.split("\n")
def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0: return hex(a)
    s = syms[i]
    return dem[i] + ("" if a == s[0] else "+%#x" % (a - s[0]))
q = sys.argv[1]
c = [s for s in syms if q in s[2]]
c.sort(key=lambda s: len(s[2]))
va, size, nm, thumb = c[0]
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
print("==", name(va))
for i in md.disasm(data[va:va+size], va):
    if i.mnemonic in ("bl", "blx") and i.op_str.startswith("#"):
        t = int(i.op_str[1:], 16)
        print(f"  {i.address:08x}: {i.mnemonic} {name(t)}")
