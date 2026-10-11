#!/bin/bash
# mkinstaller.sh: builds release/DeadSpaceMobilePCPort-Setup.exe from build/deadspace.exe.
# The installer holds only the port's own program; the game data comes from the user's APK at install time.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.." || exit 1
set -e
mkdir -p installer/payload release
strip -o installer/payload/deadspace.exe build/deadspace.exe
(cd installer && windres installer.rc -O coff -o payload/installer.res)
g++ -std=c++20 -O2 -municode -mwindows -static -Isrc installer/installer.cpp src/apk.cpp installer/payload/installer.res \
    -o release/DeadSpaceMobilePCPort-Setup.exe -lz -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -ladvapi32
strip release/DeadSpaceMobilePCPort-Setup.exe
ls -la release/DeadSpaceMobilePCPort-Setup.exe | awk '{print $5, $9}'
