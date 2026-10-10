#!/usr/bin/env python3
# perlaunch.py <results dir>: for the warm and hot launches after round 1, a least squares line of the
# launch time against the pages swapped in during the launch, per run and per algorithm
import os
import statistics as st
import sys
from collections import defaultdict


def fit(xs, ys):
    mx, my = st.mean(xs), st.mean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return b, my - b * mx

d = sys.argv[1]
per_algo = defaultdict(list)
print(f"{'run':22} {'n':>3} {'swpin med':>9} {'ms med':>6} {'us/page':>8} {'ms at 0':>7}")
for run in sorted(os.listdir(d)):
    f = os.path.join(d, run, "launches.txt")
    # run.sh keeps a spoiled run that it repeated under this name
    if not os.path.exists(f) or run.endswith("-spoiled"):
        continue
    pts = []
    with open(f) as fh:
        lines = fh.read().splitlines()
    for line in lines:
        p = line.split()
        if len(p) >= 6 and int(p[0]) > 1 and p[2] in ("HOT", "WARM") and int(p[3]) > 0:
            pts.append((int(p[4]), int(p[3])))
    if len(pts) < 5:
        continue
    xs, ys = zip(*pts)
    b, a = fit(xs, ys)
    per_algo[run.split("-", 1)[1]] += pts
    print(f"{run:22} {len(pts):3} {st.median(xs):9.0f} {st.median(ys):6.0f} {b * 1000:8.1f} {a:7.0f}")
print()
for algo, pts in sorted(per_algo.items()):
    xs, ys = zip(*pts)
    b, a = fit(xs, ys)
    print(f"{algo:22} {len(pts):3} {st.median(xs):9.0f} {st.median(ys):6.0f} {b * 1000:8.1f} {a:7.0f}")
