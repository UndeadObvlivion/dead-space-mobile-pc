#!/bin/bash
# survey.sh LEVEL [COUNT]: lighting survey of a level as text. Stands in front of COUNT doors looking away from them (down the corridor or into the room) and, at each, prints the
# picture's statistics with the added lighting off (the game's own look) and on, and what the frame was lit
# with. A quarter-size shot of each stop goes to survey/LEVEL_N.png. DS_DEV=name runs it as its own instance.
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
cd "$(dirname "$0")/.."
L=$1; N=${2:-8}; D=${DS_DEV:-devrun}
mkdir -p survey
"$BASH" tools/devfresh.sh --level "$L" >/dev/null 2>&1
python3 tools/dev.py send "renderer native; god; bloom 0; vignette 0; wait 60" --timeout 120 </dev/null >/dev/null || { python3 tools/dev.py status; exit 3; }
for i in $(seq 0 $((N-1))); do
  out=$(python3 tools/dev.py send "goto GameObjectDoor $i 2.0 0 away; wait 150; dlights 0; ao 0; shadows 0; wait 5; stat; dlights 2; ao 1; shadows 2; wait 8; stat; report; shot sv_${L}_$i" --timeout 120 </dev/null) || { echo "$out" | tail -3; python3 tools/dev.py status; exit 3; }
  echo "$out" | grep -q "^at " || { echo "$L stop $i: no such door"; break; }
  cp "$D/sv_${L}_$i.png" "survey/${L}_$i.png" 2>/dev/null
  echo "$out" | python3 -c "
import sys,re
t=sys.stdin.read()
st=re.findall(r'luma ([\d.]+), saturation ([\d.]+), clipped ([\d.]+)%, near black ([\d.]+)%',t)
r=re.search(r'lights (\d+) \(spot (\d+), panel glow (\d+), fan (\d+), with live tile (\d+)\); kept maps (\d+) in view \((\d+) stale.*?fog draws (\d+)',t)
p=re.search(r'player ([-\d.]+) ([-\d.]+) ([-\d.]+)',t)
if len(st)==2 and r:
    (lo,so,co,bo),(ln,sn,cn,bn)=st
    print('$L %2d  luma %5s -> %5s  clipped %4s%% -> %4s%%  black %4s%% -> %4s%%  lights %3s (spot %s glow %s fan %s)  kept maps %3s (stale %s)  fog %s  at %s' % ($i,lo,ln,co,cn,bo,bn,r.group(1),r.group(2),r.group(3),r.group(4),r.group(6),r.group(7),r.group(8),' '.join(p.groups()) if p else '?'))
else: print('$L $i: could not read', t[-300:])
"
done
python3 tools/dev.py stop >/dev/null
