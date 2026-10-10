#!/bin/bash
# Starts dev mode on a temporary copy of dist/userdata (the user's save) and continues Profile 1.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
python3 tools/dev.py stop >/dev/null  # stops only the dev instance, never the user's game
rm -rf savecopy; cp -r dist/userdata savecopy; rm -f savecopy/port_settings.ini
python3 tools/dev.py start --data savecopy "$@" && python3 tools/dev.py send "wait 1600; tap 1280 1340; wait 200; tap 160 1292; wait 350; tap 600 800; wait 100; tap 2300 1320; wait 1050; tap 1280 1340; wait 300" --timeout 300
