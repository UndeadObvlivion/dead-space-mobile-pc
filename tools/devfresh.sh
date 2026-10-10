#!/bin/bash
# Starts dev mode on a fresh profile and skips to gameplay (optionally --level MAP).
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
python3 tools/dev.py stop >/dev/null  # stops only the dev instance, never the user's game
python3 tools/dev.py start --fresh "$@" && python3 tools/dev.py send "wait 1500; tap 1280 1340; wait 400; tap 2292 1332; wait 300; press Space; wait 600" --timeout 300
