export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd /c/Users/Dillon/Documents/deadspace-port
for m in 01_gov_01 01_gov_02 01_gov_03 01_gov_04 02_mines_01 02_mines_02 02_mines_03 03_water_01 03_water_02 03_water_03 04_food_01 04_food_02 04_food_03 05_tube_01 05_tube_03 06_titan_01 06_titan_reactor_01; do
  bash tools/devfresh.sh --level $m >/dev/null 2>&1
  echo "== $m"
  python3 tools/dev.py send "audit 0.6" --timeout 300 2>&1
  grep -q CRASH devrun/game.log && echo "$m: CRASH"
done
python3 tools/dev.py stop >/dev/null
echo ALLDONE
