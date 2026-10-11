#!/bin/bash
# grabtest.sh ENEMY DIST: starts a fresh dev instance, spawns the enemy next to the player and presses E
# whenever the game waits for a struggle move. Stops at once if the game is no longer running.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
"$BASH" tools/devfresh.sh --level 02_mines_01 -- --give-weapons >/dev/null 2>&1
python3 tools/dev.py send "renderer native; wait 200; health 200; spawn $1 $2" --timeout 120 </dev/null >/dev/null || { python3 tools/dev.py status; exit 3; }
shot=0
for i in $(seq 1 45); do
  out=$(python3 tools/dev.py send "grab" --timeout 30 </dev/null) || { echo "$out"; exit 3; }
  s=$(echo "$out" | grep -o "state [0-9]*" | cut -d' ' -f2)
  case "$s" in
    1|5|7|100)
      if [ $shot -lt 2 ]; then python3 tools/dev.py send "shot grab_$1_$s" --timeout 30 </dev/null >/dev/null; shot=$((shot+1)); fi
      python3 tools/dev.py send "press E; wait 5; press E; wait 5; press E; wait 5; press E; wait 5" --timeout 30 </dev/null >/dev/null || { python3 tools/dev.py status; exit 3; };;
  esac
done
grep "grab state" devrun/game.log | cut -c1-60
python3 tools/dev.py status
