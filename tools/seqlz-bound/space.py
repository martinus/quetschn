#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""space.py <output of seqs>...: a search over layouts of seqlz's offset classes, priced as offsets.py
prices them. A layout is a set of offsets without bits (from 1, 2, 4 and 8), which today's decoder
cannot send, and classes of b bits that send the offset divided by 2^s, b from 1 and s from 0 to 3, the
class of 12 bits always in. Each offset goes to the class with the fewest bits that holds it.

For 6, 7 and 8 classes and every set of offsets without bits, a greedy search from today's layout: it
swaps one class with bits for another as long as that saves more than 0.02 bytes per page. Prints the
best of each search, bytes per page and escapes per page against today, as the searches finish. Takes
about 4 s per layout and hours in all; the searches run in parallel, one per CPU."""

import itertools
import math
import os
import sys
from multiprocessing import Pool

import numpy as np

from offsets import load, train

POOL = [(b, s) for s in range(4) for b in range(1, 13 - s)]
TODAY = ((), [(1, 1), (5, 3), (8, 0), (8, 3), (12, 0)])
dumps = []


def price(layout):
    """bytes and escapes per page of each dump, its codes trained on the others; None if an offset has
    no class"""
    fixed, bits = layout
    classes = [(v, 0, 0) for v in fixed] + [(0, b, s) for b, s in bits]
    symbols = 512 * (1 + len(classes))
    hists, raws = [], []
    for lm, off, rep, _ in dumps:
        cost = np.full((len(classes), len(off)), 99, np.int64)
        for i, (v, b, s) in enumerate(classes):
            ok = off == v if v else (off % (1 << s) == 0) & ((off >> s) < (1 << b))
            cost[i][ok] = b
        k = cost.argmin(0)
        raw = cost[k, np.arange(len(off))]
        if (raw[~rep] == 99).any():
            return None
        cls = np.where(rep, 0, k + 1)
        hists.append(np.bincount(lm + 512 * cls, minlength=symbols).astype(float))
        raws.append(np.where(rep, 0, raw).sum())
    out, esc = [], []
    for i, d in enumerate(dumps):
        others = sum(h / h.sum() for j, h in enumerate(hists) if j != i)
        tok = train(others * 1e6, max(12, math.ceil(math.log2(symbols))))
        out.append(((hists[i] * tok).sum() + raws[i]) / 8 / d[3])
        esc.append((hists[i] * (tok > 11)).sum() / d[3])
    return np.mean(out), np.mean(esc)


def name(layout):
    fixed, bits = layout
    b = ", ".join(
        f"{n}b" + (f"<<{s}" if s else "")
        for n, s in sorted(bits, key=lambda x: (x[1], x[0]))
    )
    return "+".join(map(str, fixed)) + " | " + b if fixed else b


def search(job):
    """the best layout from today's classes with bits, cut or filled to n, by single swaps"""
    fixed, n = job
    cur = sorted(set(TODAY[1][: n - 1] + [(12, 0)]))
    cur += [p for p in POOL if p not in cur][: n - len(cur)]
    best = price((fixed, cur))
    improved = n > 1
    while improved:
        improved = False
        for out in [c for c in cur if c != (12, 0)]:
            for inn in [p for p in POOL if p not in cur]:
                cand = sorted([c for c in cur if c != out] + [inn])
                r = price((fixed, cand))
                if r and r[0] < best[0] - 0.02:
                    cur, best, improved = cand, r, True
                    break
            if improved:
                break
    return (fixed, cur), best


def init(paths):
    dumps.extend(load(p) for p in paths)


def main():
    init(sys.argv[1:])
    jobs = []
    for classes in (6, 7, 8):
        for k in range(5):
            for fixed in itertools.combinations((1, 2, 4, 8), k):
                if classes - 1 - k >= 1:
                    jobs.append((fixed, classes - 1 - k))
    today = price(TODAY)
    print(
        f"today: {today[0]:.2f} bytes, {today[1]:.2f} escapes per page, {name(TODAY)}",
        flush=True,
    )
    with Pool(os.cpu_count(), init, (sys.argv[1:],)) as p:
        for layout, (b, e) in p.imap_unordered(search, jobs):
            print(
                f"{1 + len(layout[0]) + len(layout[1])} classes {b - today[0]:+7.2f} bytes "
                f"{e - today[1]:+6.2f} escapes  {name(layout)}",
                flush=True,
            )


if __name__ == "__main__":
    main()
