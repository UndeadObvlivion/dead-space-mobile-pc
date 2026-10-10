#!/usr/bin/env python3
"""Disassemble a function (or address range) of the guest lib by symbol name.
usage: disas.py <symbol-substring|0xADDR> [count]"""
import sys, struct, subprocess
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
LIB = "apk/assets/guest_libs/libDeadSpace.so"
data = open(LIB, "rb").read()
# symbols via readelf
out = subprocess.run(["readelf", "-s", "-W", LIB], capture_output=True, text=True).stdout
syms = []
for line in out.splitlines():
    p = line.split()
    if len(p) >= 8 and p[3] == "FUNC" and p[6] != "UND":
        syms.append((int(p[1], 16), int(p[2]), p[7]))
def file_off(va):
    return va  # first PT_LOAD maps 1:1
q = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 80
if q.startswith("0x"):
    va = int(q, 16); thumb = va & 1; va &= ~1; name = q; size = n * 4
else:
    cands = [s for s in syms if q in s[2]]
    cands.sort(key=lambda s: len(s[2]))
    if not cands: sys.exit("no symbol")
    for c in cands[:8]: print("cand:", hex(c[0]), c[1], c[2])
    va, size, name = cands[0]; thumb = va & 1; va &= ~1
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB if thumb else CS_MODE_ARM)
print(f"== {name} @ {va:#x} size {size} {'thumb' if thumb else 'arm'}")
code = data[file_off(va): file_off(va) + min(size, n * 4)]
for i in md.disasm(code, va):
    print(f"{i.address:08x}: {i.mnemonic:8s} {i.op_str}")
