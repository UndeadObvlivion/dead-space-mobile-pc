#!/bin/bash
# tonecheck.sh LEVEL [DOORS...]: tone of the native renderer at the user's maximum settings against the
# legacy renderer (the game's own look) at a few doors: dark / middle / bright levels and contrast.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
L=$1; shift; DOORS=${@:-0 3 4 7}; D=${DS_DEV:-devrun}
"$BASH" tools/devfresh.sh --level "$L" -- --msaa 8 >/dev/null
C="god; dlights 3; shadows 3; ao 3; specular 3; envlight 3; bloom 3; sharpen 3; vignette 1; lightshadows 1"
for i in $DOORS; do C="$C; goto GameObjectDoor $i 2.0 0 away; wait 150; shot tc${i}_max; renderer legacy; wait 6; shot tc${i}_leg; renderer native; wait 4"; done
python3 tools/dev.py send "$C" --timeout 400 </dev/null >/dev/null || { python3 tools/dev.py status; exit 3; }
for i in $DOORS; do python3 tools/pngtone.py $D/tc${i}_max.png $D/tc${i}_leg.png | cut -c1-98 | sed "s/^/$L /"; done
python3 tools/dev.py stop >/dev/null
