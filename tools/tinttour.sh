#!/bin/bash
# tinttour.sh: forces world tints in the first hallway and compares the native renderer (added lighting on
# and off) with the legacy renderer.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
"$BASH" tools/devfresh.sh >/dev/null 2>&1
python3 tools/dev.py send "renderer native; wait 200; god; bloom 0; vignette 0; sharpen 0; tp -39.6 1 -246; wait 20; face -39.6 -230; wait 150" --timeout 120 </dev/null >/dev/null || { python3 tools/dev.py status; exit 3; }
for t in "ff2020 1" "20ff20 1" "3040ff 1" "ff2020 0.5" "ffc040 1"; do
  n=$(echo $t | tr ' ' '_')
  python3 tools/dev.py send "tint $t; wait 150; renderer legacy; wait 5; shot tt_${n}_leg; renderer native; dlights 0; ao 0; wait 5; shot tt_${n}_off; dlights 2; ao 1; wait 8; shot tt_${n}_on" --timeout 120 </dev/null >/dev/null || { python3 tools/dev.py status; exit 3; }
  echo "== tint $t"; python3 tools/tintcmp.py devrun/tt_${n}_leg.png devrun/tt_${n}_off.png devrun/tt_${n}_on.png
done
