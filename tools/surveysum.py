#!/usr/bin/env python3
"""surveysum.py FILE...: per-level averages of tools/survey.sh output."""
import sys, re, collections
d = collections.defaultdict(list)
for f in sys.argv[1:]:
    for l in open(f, encoding="utf-8", errors="replace"):
        m = re.match(r'(\S+)\s+\d+\s+luma\s+([\d.]+) ->\s+([\d.]+)\s+clipped\s+([\d.]+)% ->\s+([\d.]+)%\s+black\s+([\d.]+)% ->\s+([\d.]+)%\s+lights\s+(\d+)', l)
        if m: d[m.group(1)].append([float(x) for x in m.groups()[1:]])
        elif l.strip() and "no such door" not in l: print('?', l.strip()[:100])
print('level                 stops  luma off->on    clipped off->on   black off->on   lights  worst clip rise')
for k, v in d.items():
    n = len(v); a = [sum(x[i] for x in v) / n for i in range(7)]
    print('%-20s %5d  %5.1f -> %5.1f  %4.1f%% -> %4.1f%%   %4.1f%% -> %4.1f%%  %5.0f   %+.1f' % (k, n, a[0], a[1], a[2], a[3], a[4], a[5], a[6], max(x[3] - x[2] for x in v)))
