#!/usr/bin/env python3
"""Drive a running game in dev mode (see src/devmode.cpp for the command list).

  dev.py start [--data DIR] [--fresh] [--level MAP] [-- extra game args]
      launches build/deadspace.exe with --dev devrun (log: devrun/game.log) and waits until ready
  dev.py send "cmd; cmd; ..." [--timeout S]
      runs the commands and prints their output once they have all finished
  dev.py stop
"""
import os
import io
import random
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEV = os.path.join(ROOT, "devrun")
OUT = os.path.join(DEV, "out.txt")
CMD = os.path.join(DEV, "cmd.txt")
PIDFILE = os.path.join(DEV, "pid.txt")


def out_size():
    return os.path.getsize(OUT) if os.path.exists(OUT) else 0


def read_from(pos):
    with open(OUT, "r", encoding="utf-8", errors="replace") as f:
        f.seek(pos)
        return f.read()


def send(cmds, timeout=120.0):
    tag = "%08x" % random.getrandbits(32)
    deadline = time.time() + 10
    while os.path.exists(CMD):  # previous batch not picked up yet
        if time.time() > deadline:
            sys.exit("game is not reading commands (is it running?)")
        time.sleep(0.05)
    pos = out_size()
    tmp = CMD + ".tmp"
    with open(tmp, "w") as f:
        f.write(cmds.replace(";", "\n") + "\nsync " + tag + "\n")
    os.replace(tmp, CMD)
    deadline = time.time() + timeout
    while time.time() < deadline:
        text = read_from(pos)
        if "done " + tag in text:
            lines = [l for l in text.splitlines() if l != "done " + tag]
            print("\n".join(lines))
            return
        time.sleep(0.1)
    print(read_from(pos))
    sys.exit("timed out")


def start(args):
    data, level, extra, fresh = None, None, [], False
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--data":
            data = args[i + 1]; i += 2
        elif a == "--level":
            level = args[i + 1]; i += 2
        elif a == "--fresh":
            fresh = True; i += 1
        elif a == "--":
            extra = args[i + 1:]; break
        else:
            extra.append(a); i += 1
    data = data or os.path.join(ROOT, "freshdata")
    if fresh and os.path.isdir(data):
        shutil.rmtree(data)
    os.makedirs(DEV, exist_ok=True)
    if os.path.exists(OUT):
        os.remove(OUT)
    cmd = [os.path.join(ROOT, "build", "deadspace.exe"), "--assets", os.path.join(ROOT, "dist/gamedata/assets"),
           "--lib", os.path.join(ROOT, "dist/gamedata/libDeadSpace.so"), "--data", data, "--dev", DEV,
           "--log", os.path.join(DEV, "game.log")] + (["--level", level] if level else []) + extra
    flags = 0x00000008 | 0x00000200 if os.name == "nt" else 0  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    proc = subprocess.Popen(cmd, cwd=ROOT, creationflags=flags, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    with open(PIDFILE, "w") as f:  # "dev.py stop" only ever stops this instance
        f.write(str(proc.pid))
    deadline = time.time() + 60
    while time.time() < deadline:
        if os.path.exists(OUT) and "ready" in read_from(0):
            print("started")
            return
        time.sleep(0.2)
    sys.exit("game did not start")


def pid_alive(pid):
    r = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid, "/NH"], capture_output=True, text=True)
    return str(pid) in r.stdout


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    op = sys.argv[1]
    if op == "start":
        start(sys.argv[2:])
    elif op == "send":
        timeout = 120.0
        if "--timeout" in sys.argv:
            timeout = float(sys.argv[sys.argv.index("--timeout") + 1])
        send(sys.argv[2], timeout)
    elif op == "stop":
        if os.path.exists(OUT):
            with open(CMD, "w") as f:
                f.write("quit\n")
        # then make sure the dev instance (by PID; never any other copy of the game) has exited
        if os.path.exists(PIDFILE):
            pid = int(open(PIDFILE).read().strip() or 0)
            deadline = time.time() + 5
            while pid and time.time() < deadline and pid_alive(pid):
                time.sleep(0.2)
            if pid and pid_alive(pid):
                subprocess.run(["taskkill", "/F", "/PID", str(pid)], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            os.remove(PIDFILE)
        print("stopped")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
