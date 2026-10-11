#!/bin/bash
# Starts dev mode on a fresh profile and gets to gameplay as fast as the game loads (about 10 s):
#   devfresh.sh [--level MAP] [--at "X Y Z"] [--renderer native|legacy] [-- extra game args]
# The game runs unthrottled ("turbo") until the player can move; --at teleports there afterwards.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
at=""; renderer="native"; args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --at) at="$2"; shift 2;;
    --renderer) renderer="$2"; shift 2;;
    *) args+=("$1"); shift;;
  esac
done
python3 tools/dev.py stop >/dev/null  # stops only the dev instance, never the user's game
python3 tools/dev.py start --fresh "${args[@]}" || exit 3
# the loading screen takes its "press any key" only once loading is done: keep pressing until the player has control
for i in $(seq 1 60); do
  python3 tools/dev.py send "turbo 6; press Space; waitfor notloading 1500; wait 5" --timeout 30 </dev/null >/dev/null
  [ $? -eq 3 ] && { python3 tools/dev.py status; exit 3; }  # (the game has exited)
  python3 tools/dev.py send "report" --timeout 30 </dev/null | grep -q "^player" && break
done
python3 tools/dev.py send "turbo 0; renderer $renderer; wait 20${at:+; tp $at; wait 20}; report" --timeout 60 </dev/null | grep -E "^state|^player"
