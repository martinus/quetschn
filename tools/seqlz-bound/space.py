#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""space.py <output of seqs>...: a search over layouts of seqlz's offset classes, priced by offsets.py. A
layout is a set of offsets without bits (from 1, 2, 4 and 8), which today's decoder cannot send, and
classes of b bits that send the offset divided by 2^s, b from 1 and s from 0 to 3, the class of 12 bits
always in. Each offset goes to the class with the fewest bits that holds it.

For 6, 7 and 8 classes and every set of offsets without bits, a greedy search from today's layout: it
swaps one class with bits for another as long as that saves more than 0.02 bytes per page. Prints the
best of each search, bytes per page and escapes per page against today, as the searches finish. Takes
about 4 s per layout and hours in all; the searches run in parallel, one per CPU."""

import itertools
import os
import sys
from multiprocessing import Pool

import numpy as np

from offsets import eq, load, price_escapes

POOL = [(b, s) for s in range(4) for b in range(1, 13 - s)]
WHOLE = (12, 0)
TODAY = ((), [(1, 1), (5, 3), (8, 0), (8, 3), WHOLE])
dumps = []


def bits_class(b, s):
    return lambda o: (o % (1 << s) == 0) & ((o >> s) < (1 << b))


def price(layout):
    """mean bytes and escapes per page; offsets.py's first matching class is the one of fewest bits"""
    fixed, bits = layout
    classes = [(eq(v), 0) for v in fixed] + [
        (bits_class(b, s), b) for b, s in sorted(bits)
    ]
    out, esc = price_escapes(classes, dumps)
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
    cur = sorted(TODAY[1][:-1][: n - 1] + [WHOLE])
    cur += [p for p in POOL if p not in cur][: n - len(cur)]
    seen = {}

    def cost(layout):
        key = tuple(sorted(layout))
        if key not in seen:
            seen[key] = price((fixed, layout))
        return seen[key]

    best = cost(cur)
    while True:
        for out, inn in itertools.product([c for c in cur if c != WHOLE], POOL):
            cand = [c for c in cur if c != out] + [inn]
            if inn not in cur and cost(cand)[0] < best[0] - 0.02:
                cur, best = sorted(cand), cost(cand)
                break
        else:
            return (fixed, cur), best


def init(paths):
    dumps[:] = [load(p) for p in paths]


def main():
    init(sys.argv[1:])
    jobs = [
        (f, classes - 1 - k)
        for classes in (6, 7, 8)
        for k in range(5)
        for f in itertools.combinations((1, 2, 4, 8), k)
    ]
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
