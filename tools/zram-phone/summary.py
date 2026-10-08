#!/usr/bin/env python3
"""The table of a tools/zram-phone/run.sh log: per core and codec the mean over the rounds and the range.

    summary.py <log.txt>

Times are zramphone's means over the pages, in us: write, warm read (r0K), cold read (r2048K). With
COUNTS=1 also the kernel instructions per page of the whole run, writes and reads together. A run whose
core was not at the clock of CORES before or after it, e.g. held lower by the thermal limit, is left out
of the mean and the range; the column "core" has the number of such runs, "memory" the runs whose
memory's devfreq frequencies changed, which happens all the time without BUS=1 and is only noted.
"""
import collections
import re
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
times = collections.defaultdict(list)
run = []  # the values of the current run, until its STATE after says whether it counts
core_changed = collections.Counter()
mem_changed = collections.Counter()
temps = []
setup = ""
fixed = {}
cur = None
before = {}
pages = 0
for line in open(sys.argv[1], errors="replace"):
    line = line.strip()
    if line.startswith("SETUP"):
        setup = line
        fixed = dict(x.split(":") for x in re.search(r"cores (.*?) codecs", line)[1].split())
    m = re.match(r"RUN round (\d+) cpu (\d+) codec (\S+)", line)
    if m:
        cur = (m[2], m[3])
        run = []
        continue
    m = re.match(r"STATE (\w+) (.*)", line)
    if m and cur:
        kv = dict(x.split("=") for x in m[2].split())
        if m[1] == "before":
            before = kv
        elif m[1] == "after":
            core = f"cpu{cur[0]}"
            if before.get(core) != fixed.get(cur[0]) or kv.get(core) != fixed.get(cur[0]):
                core_changed[cur] += 1
            else:
                for k, v in run:
                    times[k].append(v)
            run = []
            if any(before.get(k) != v for k, v in kv.items() if k.startswith("cpu") and "-" in k and "step" not in k):
                mem_changed[cur] += 1
            temps += [int(v) / 1000 for k, v in kv.items() if k in ("cpu-0-max-step", "cpu-1-max-step")]
        continue
    m = re.match(r"RESULT pages (\d+)", line)
    if m:
        pages = int(m[1])
    m = re.match(r"RESULT (\S+) +(write|r0K|r2048K) .*mean (\d+)", line)
    if m and cur:
        run.append((cur + ({"write": "write", "r0K": "warm", "r2048K": "cold"}[m[2]],), int(m[3]) / 1000))
    m = re.match(r"([\d,]+)\s+instructions", line)
    if m and cur and pages:
        run.append((cur + ("instr",), int(m[1].replace(",", "")) / pages))

print(setup)
cores = sorted({k[0] for k in times}, key=int)
codecs = list(dict.fromkeys(k[1] for k in times))
cols = ["write", "warm", "cold"] + (["instr"] if any(k[2] == "instr" for k in times) else [])
for c in cores:
    print(f"\ncpu{c}, mean (min to max) over the rounds whose core clock held")
    print(f"{'codec':18s}" + "".join(f"{x:>24s}" for x in cols) + "  core memory")
    for a in codecs:
        row = f"{a:18s}"
        for x in cols:
            v = times.get((c, a, x), [])
            fmt = "{:.0f}" if x == "instr" else "{:.2f}"
            cell = f"{fmt.format(sum(v) / len(v))} ({fmt.format(min(v))} to {fmt.format(max(v))})" if v else "-"
            row += f"{cell:>24s}"
        print(row + f"  {core_changed[(c, a)]:4d} {mem_changed[(c, a)]:6d}")
if temps:
    print(f"\ncluster temperatures after the runs: {min(temps):.1f} to {max(temps):.1f} C")
