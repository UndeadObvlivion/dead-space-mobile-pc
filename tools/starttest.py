#!/usr/bin/env python3
"""Start the dev build N times on a throwaway profile and report the starts that exit by themselves.
usage: starttest.py [N] [seconds] [-- extra game args]
Each instance is stopped by its own PID after `seconds`; an early exit prints its exit code and log tail."""
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = sys.argv[1:]
extra = []
if "--" in args:
    extra = args[args.index("--") + 1:]
    args = args[:args.index("--")]
n = int(args[0]) if args else 10
secs = float(args[1]) if len(args) > 1 else 4.0
data = os.path.join(ROOT, "starttest_data")
log = os.path.join(ROOT, "devrun", "starttest.log")
bad = 0
for i in range(n):
    shutil.rmtree(data, ignore_errors=True)
    cmd = [os.path.join(ROOT, "build", "deadspace.exe"), "--assets", os.path.join(ROOT, "dist/gamedata/assets"),
           "--lib", os.path.join(ROOT, "dist/gamedata/libDeadSpace.so"), "--data", data, "--log", log] + extra
    p = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    try:
        code = p.wait(timeout=secs)
        bad += 1
        tail = open(log, encoding="utf-8", errors="replace").read().splitlines()[-2:]
        print("start %d: EXITED by itself, code %d (0x%08x)\n    %s" % (i, code, code & 0xffffffff, "\n    ".join(tail)))
        shutil.copy(log, os.path.join(ROOT, "devrun", "starttest_fail%d.log" % i))
    except subprocess.TimeoutExpired:
        p.kill()  # this instance only
        p.wait()
shutil.rmtree(data, ignore_errors=True)
print("%d of %d starts exited by themselves" % (bad, n))
