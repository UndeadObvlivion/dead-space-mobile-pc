#!/bin/bash
# lighttour.sh LEVEL [COUNT]: visits COUNT doors of the level and compares clipped pixels with the added
# lighting off (the game's own look) and on. Stops if the game dies.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
L=$1; N=${2:-8}
"$BASH" tools/devfresh.sh --level "$L" >/dev/null 2>&1
python3 tools/dev.py send "renderer native; wait 200; god" --timeout 120 </dev/null >/dev/null || { python3 tools/dev.py status; exit 3; }
for i in $(seq 0 $((N-1))); do
  python3 tools/dev.py send "goto GameObjectDoor $i; wait 150; dlights 0; bloom 0; wait 4; shot tour_${i}_off; dlights 2; bloom 1; wait 6; shot tour_${i}_on" --timeout 120 </dev/null >/dev/null || { python3 tools/dev.py status; exit 3; }
  python3 tools/blowout.py devrun/tour_${i}_off.png devrun/tour_${i}_on.png | tr '\n' ' '; echo
done
