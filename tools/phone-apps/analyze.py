#!/usr/bin/env python3
# summary of the app switching runs: per algorithm and run, launches of rounds 2-4, swap counters, and
# how much each number spreads between the runs of one algorithm
import re
import statistics
import sys
from pathlib import Path

R = Path(sys.argv[1])

def stats(path):
    snaps, cur = {}, None
    for l in path.read_text().splitlines():
        m = re.match(r'== (\S+) (\d+)', l)
        if m:
            cur = snaps.setdefault(m.group(1), {'t': int(m.group(2))})
            continue
        f = l.split()
        if not f or cur is None:
            continue
        if f[0] in ('mm_stat', 'io_stat'):
            cur[f[0]] = [int(x) for x in f[1:]]
        elif f[0] == 'kswapd':
            cur['kswapd'] = int(f[1]) + int(f[2])
        elif f[0] == 'STATE':
            continue
        elif f[0] == 'battery_temp':
            cur['temp'] = int(f[1]) if len(f) > 1 else 0
        elif f[0].endswith(':'):
            cur[f[0][:-1]] = int(f[1])
        elif len(f) == 2:
            cur[f[0]] = int(f[1])
    return snaps

rows = []
for d in sorted(R.iterdir()):
    # run.sh keeps a spoiled run that it repeated under this name
    if not d.is_dir() or d.name.endswith('-spoiled'):
        continue
    rep, algo = d.name.split('-', 1)
    L = [l.split() for l in (d / 'launches.txt').read_text().splitlines()]
    later = [x for x in L if int(x[0]) > 1]
    cold = sum(x[2] == 'COLD' for x in later)
    warmhot = [int(x[3]) for x in later if x[2] in ('WARM', 'HOT') and int(x[3]) > 0]
    hot = [int(x[3]) for x in later if x[2] == 'HOT' and int(x[3]) > 0]
    # A run is not counted where Android's limit on cached processes was set back to 32 after round 1,
    # which kills by count, or where a launch hung: both gave cold launches that had nothing to do with
    # the codec. swapbench.sh writes both to limit.txt.
    events = [l.split() for l in (d / 'limit.txt').read_text().splitlines()] if (d / 'limit.txt').exists() else []
    late = sum(e[-1] == 'reset' and int(e[0]) > 1 for e in events)
    hung = sum(e[-1] == 'hung' for e in events)
    s = stats(d / 'stats.txt')
    a, b = s['start'], s['end']
    mm = b['mm_stat']
    rows.append({'algo': algo, 'rep': rep, 'cold': cold, 'n': len(later), 'warmhot': len(warmhot),
                     'wh_med': statistics.median(warmhot) if warmhot else 0,
                     'wh_mean': statistics.mean(warmhot) if warmhot else 0,
                     'hot_med': statistics.median(hot) if hot else 0,
                     'all_mean': statistics.mean(int(x[3]) for x in later if int(x[3]) > 0),
                     'all_med': statistics.median(int(x[3]) for x in later if int(x[3]) > 0),
                     'pswpin': b['pswpin'] - a['pswpin'], 'pswpout': b['pswpout'] - a['pswpout'],
                     'majflt': b['pgmajfault'] - a['pgmajfault'],
                     'orig': mm[0] / 2**20, 'compr': mm[1] / 2**20, 'used': mm[2] / 2**20, 'maxused': mm[4] / 2**20,
                     'kswapd': (b.get('kswapd', 0) - a.get('kswapd', 0)) / 100,
                     'kills': int((d / 'kills.txt').read_text().split()[0]),
                     'temp': b.get('temp', 0) / 10, 'minutes': (b['t'] - a['t']) / 60,
                     'ok': 'no' if late or hung else 'yes'})
# zram's memory and the pages swapped in first: they spread least between the runs of one algorithm, see
# the spread below
hdr = ['algo', 'rep', 'used', 'pswpin', 'kswapd', 'cold', 'kills', 'warmhot', 'wh_med', 'wh_mean', 'hot_med', 'all_mean',
       'all_med', 'pswpout', 'majflt', 'orig', 'compr', 'maxused', 'temp', 'minutes', 'ok']
print(' '.join(f'{h:>9}' for h in hdr))
for r in sorted(rows, key=lambda r: (r['algo'], r['rep'])):
    print(' '.join(f'{r[h]:>9.1f}' if isinstance(r[h], float) else f'{r[h]:>9}' for h in hdr))
# the mean of the runs per algorithm, of the counted runs only; all_med and all_mean count the cold
# launches too, which the warm and hot ones alone hide: an app that survives makes a slow warm launch
# instead of a cold one
skipped = [f"{r['rep']}-{r['algo']}" for r in rows if r['ok'] != 'yes']
rows = [r for r in rows if r['ok'] == 'yes']
print()
if skipped:
    print('not counted, a limit reset after round 1 or a hung launch: ' + ' '.join(sorted(skipped)))
for algo in sorted({r['algo'] for r in rows}):
    rs = [r for r in rows if r['algo'] == algo]
    m = {h: statistics.mean(r[h] for r in rs) for h in hdr[2:-1]}
    m['cold'] = sum(r['cold'] for r in rs)
    print(' '.join([f"{algo:>9}", f"{len(rs):>9}"] + [f'{m[h]:>9.1f}' if isinstance(m[h], float) else f'{m[h]:>9}' for h in hdr[2:-1]]))
# The spread between the runs of one algorithm, pooled over the algorithms, in % of the mean, against the
# spread of the algorithms' means. F is the ratio of the two variances, as in a one-way ANOVA: the larger,
# the better the number tells the algorithms apart with this many runs. Chance alone gives an F above 3.5
# in 1 of 20 series of 5 algorithms with 3 runs each, above 5.1 with 3 algorithms of 3 runs, above 3.7
# with 3 algorithms of 6 runs.
groups = {}
for r in rows:
    groups.setdefault(r['algo'], []).append(r)
runs = sum(len(g) for g in groups.values())
if len(groups) > 1 and runs > len(groups):
    print()
    print(f"{'spread':>9} {'runs':>9} {'algos':>9} {'F':>9}")
    out = []
    for h in ['used', 'pswpin', 'kswapd', 'cold', 'kills', 'wh_med', 'wh_mean', 'all_mean', 'all_med']:
        mean = statistics.mean(r[h] for r in rows)
        within = sum((r[h] - statistics.mean(x[h] for x in g)) ** 2 for g in groups.values() for r in g) / (runs - len(groups))
        means = [statistics.mean(r[h] for r in g) for g in groups.values()]
        between = sum(len(g) * (statistics.mean(r[h] for r in g) - mean) ** 2 for g in groups.values()) / (len(groups) - 1)
        f = between / within if within else float('inf')
        out.append((f, h, within ** 0.5 / mean if mean else 0, statistics.pstdev(means) / mean if mean else 0))
    for f, h, w, b in sorted(out, reverse=True):
        print(f'{h:>9} {w:>9.1%} {b:>9.1%} {f:>9.1f}')
