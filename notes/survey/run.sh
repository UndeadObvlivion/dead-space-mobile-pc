export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd /c/Users/Dillon/Documents/deadspace-port
for m in 01_gov_01 01_gov_02 01_gov_03 01_gov_04 02_mines_01 02_mines_02 02_mines_03 03_water_01 03_water_02 03_water_03 04_food_01 04_food_02 04_food_03 05_tube_01 05_tube_03 06_titan_01 06_titan_reactor_01; do
  sleep 3
  python3 tools/dev.py start --fresh --level $m || { echo "$m: no start"; continue; }
  python3 tools/dev.py send "wait 1500; tap 1280 1340; wait 400; tap 2292 1332; wait 300; press Space; wait 600; shot $m; dump 0 all" --timeout 240 > notes/survey/$m.txt 2>&1
  cp devrun/$m.png notes/survey/ 2>/dev/null
  grep -q CRASH devrun/game.log && echo "$m: CRASH"
  echo "$m: $(tail -1 notes/survey/$m.txt)"
  python3 tools/dev.py stop >/dev/null  # stops only the dev instance, never the user's game
done
echo ALLDONE
