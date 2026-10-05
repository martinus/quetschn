#!/usr/bin/env python3
# summary of the app switching runs: per algorithm and run, launches of rounds 2-4, swap counters
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
        elif f[0] == 'battery_temp':
            cur['temp'] = int(f[1]) if len(f) > 1 else 0
        elif f[0].endswith(':'):
            cur[f[0][:-1]] = int(f[1])
        elif len(f) == 2:
            cur[f[0]] = int(f[1])
    return snaps

rows = []
for d in sorted(R.iterdir()):
    rep, algo = d.name.split('-', 1)
    L = [l.split() for l in (d / 'launches.txt').read_text().splitlines()]
    later = [x for x in L if int(x[0]) > 1]
    cold = sum(x[2] == 'COLD' for x in later)
    warmhot = [int(x[3]) for x in later if x[2] in ('WARM', 'HOT') and int(x[3]) > 0]
    hot = [int(x[3]) for x in later if x[2] == 'HOT' and int(x[3]) > 0]
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
                     'temp': b.get('temp', 0) / 10, 'minutes': (b['t'] - a['t']) / 60})
hdr = ['algo', 'rep', 'cold', 'warmhot', 'wh_med', 'wh_mean', 'hot_med', 'all_mean', 'all_med', 'pswpin', 'pswpout', 'majflt',
       'orig', 'compr', 'used', 'maxused', 'kswapd', 'kills', 'temp', 'minutes']
print(' '.join(f'{h:>9}' for h in hdr))
for r in sorted(rows, key=lambda r: (r['algo'], r['rep'])):
    print(' '.join(f'{r[h]:>9.1f}' if isinstance(r[h], float) else f'{r[h]:>9}' for h in hdr))
# the mean of the runs per algorithm; all_med and all_mean count the cold launches too, which the warm
# and hot ones alone hide: an app that survives makes a slow warm launch instead of a cold one
print()
for algo in sorted({r['algo'] for r in rows}):
    rs = [r for r in rows if r['algo'] == algo]
    m = {h: statistics.mean(r[h] for r in rs) for h in hdr[2:]}
    m['cold'] = sum(r['cold'] for r in rs)
    print(' '.join([f"{algo:>9}", f"{len(rs):>9}"] + [f'{m[h]:>9.1f}' if isinstance(m[h], float) else f'{m[h]:>9}' for h in hdr[2:]]))
