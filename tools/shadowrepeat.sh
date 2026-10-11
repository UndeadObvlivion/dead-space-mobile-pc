#!/bin/bash
# shadowrepeat.sh: does a place look the same however the player got there? At three spots of chapter 1's
# second hallway the brightness of the right wall and of the left wall (regions without the player) is
# measured standing still: twice in a row (the noise of idle animation and camera sway), then again after
# a visit to the far end of the hallway. Stable shadows give "after" within the noise. (Run after
# tools/hallwalk.sh, which opens the door.)
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
R="stat 0.62 0.15 1.0 0.95; stat 0.0 0.15 0.2 0.95"
for z in -221 -213 -205; do
  out=$(python3 tools/dev.py send "tp -39.6 1 -226; wait 30; tp -39.6 1 $z; wait 10; face -39.6 -190; wait 150; $R; wait 150; $R; tp -39.6 1 -202; wait 20; face -39.6 -240; wait 150; tp -39.6 1 $z; wait 10; face -39.6 -190; wait 150; $R" --timeout 200 </dev/null) || { python3 tools/dev.py status; exit 3; }
  echo "$out" | grep -o "luma [0-9.]*" | tr '\n' ' ' | awk -v z=$z '{printf "z %s  right wall: first %s again %s after %s   left wall: first %s again %s after %s\n", z, $2, $6, $10, $4, $8, $12}'
done
