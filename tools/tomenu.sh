#!/bin/bash
# Starts dev mode on a fresh profile and gets to the MAIN MENU (a fresh profile has none until a game
# exists: new game -> skip the intro -> pause -> quit to menu). Extra arguments go to the game.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
python3 tools/dev.py stop >/dev/null  # stops only the dev instance, never the user's game
python3 tools/dev.py start --fresh "$@" || exit 3
send() { python3 tools/dev.py send "$1" --timeout 60 </dev/null; [ $? -eq 3 ] && { python3 tools/dev.py status; exit 3; }; }
send "turbo 6; waitfor loading 5000; wait 30" >/dev/null
for i in 1 2 3 4 5 6 7 8; do  # the loading screen, until the difficulty prompt is up
  send "press Space; waitfor notloading 1500; wait 20; report" | grep -q "BTN_OK" && break
done
send "tapbtn BTN_OK; wait 60; waitfor loading 20000; press Space; waitfor notloading 3000; turbo 0; wait 120; press Space; wait 150" >/dev/null
send "press Escape; wait 60; tapbtn BTN_QUIT; wait 60; tapbtn BTN_OK; wait 300; waitfor button BTN_PLAY 30000; wait 90; report" | grep "^state"
