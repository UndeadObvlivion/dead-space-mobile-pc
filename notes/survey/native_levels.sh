#!/bin/bash
# Runs every level on the native renderer (default settings: 16x anisotropic, reversed-Z), walks
# forward, turns, takes a screenshot, and reports crashes and GL errors per level.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd /c/Users/Dillon/Documents/deadspace-port
for m in 01_gov_01 01_gov_02 01_gov_03 01_gov_04 02_mines_01 02_mines_02 02_mines_03 03_water_01 03_water_02 03_water_03 04_food_01 04_food_02 04_food_03 05_tube_01 05_tube_03 06_titan_01 06_titan_reactor_01; do
  rm -rf "nl_$m" shots/frame_3050.bmp
  mkdir "nl_$m"
  printf 'renderer = native\n' > "nl_$m/port_settings.ini"
  timeout 300 ./build/deadspace.exe --assets dist/gamedata/assets --lib dist/gamedata/libDeadSpace.so --data "nl_$m" --level "$m" --log "notes/testlogs/nl_$m.log" --taps 1600:1280:1340,2000:2292:1332 --input "2300:Space:1,2301:Space:0,2700:W:1,2900:W:0,2950:MOUSE:600;0" --shots 3050 --shot-dir shots --exit-frame 3060 >/dev/null 2>&1
  st=ok
  grep -q CRASH "notes/testlogs/nl_$m.log" && st=CRASH
  if [ -f shots/frame_3050.bmp ]; then mv shots/frame_3050.bmp "shots/nl_$m.bmp"; else st="$st no-screenshot"; fi
  errs=$(grep -c "GL error" "notes/testlogs/nl_$m.log")
  filt=$(grep -c "16x anisotropic" "notes/testlogs/nl_$m.log")
  echo "$m: $st, GL errors: $errs, filtering on: $filt"
  rm -rf "nl_$m"
done
echo ALLDONE
