#!/bin/bash
# hallwalk.sh [NAME]: the shadow walk-through of chapter 1's second hallway (through the first door to the
# far door) at maximum lighting and shadow settings on the native renderer; filmstrips devrun/NAME1..3.png.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
N=${1:-hw}; D=${DS_DEV:-devrun}
"$BASH" tools/devfresh.sh --at "-39.6 1 -227.4" >/dev/null
python3 tools/dev.py send "god; renderer native; dlights 3; shadows 3; ao 3; specular 3; envlight 3; lightshadows 1; bloom 0; vignette 0; face -39.6 -200; wait 90; press e; wait 180; key w 1; wait 120; report; burst ${N}1 16 14 6; report; burst ${N}2 16 14 6; report; burst ${N}3 16 14 6; pos; key w 0; report" --timeout 300 </dev/null | grep -E "^player|^render|E:" | cut -c1-200
