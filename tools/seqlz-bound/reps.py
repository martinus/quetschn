#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""reps.py <output of seqsrep>...: today's offset classes, then with a second repeat offset, then with a
second and a third, each as a class without bits in front of today's classes, priced on the sequences
of each dump as offsets.py does: the token with an 11-bit length-limited code and an escape, trained on
the other dumps, plus the offsets' raw bits. The matcher is today's, which looks for a match only at
the repeat offset and the hash table's entry: the matches at the other two offsets are the ones it
finds anyway. Prints how often a match has which offset, and bytes per page against today."""

import math
import sys

import numpy as np

from offsets import TODAY, train


def load(path):
    a = np.fromfile(path, dtype=np.uint16).reshape(-1, 6).astype(np.int64)
    lm, off, r1, r2, r3, first = a.T
    return lm, off, (r1, r2, r3), int(first.sum())


def price(dumps, reps):
    """bytes per page of each dump with reps more repeat offsets, its token codes trained on the others"""
    hists, raws = [], []
    for lm, off, r, _ in dumps:
        cls = np.where(off == r[0], 0, -1)
        raw = np.zeros(off.shape, np.int64)
        k = 1
        for extra in r[1 : 1 + reps]:
            cls[(cls < 0) & (off == extra)] = k
            k += 1
        for pred, bits in TODAY:
            m = (cls < 0) & pred(off)
            cls[m], raw[m] = k, bits
            k += 1
        assert (cls >= 0).all()
        hists.append(np.bincount(lm + 512 * cls, minlength=512 * k).astype(float))
        raws.append(raw.sum())
    out = []
    for i, d in enumerate(dumps):
        others = sum(h / h.sum() for j, h in enumerate(hists) if j != i)
        bits = train(others * 1e6, max(12, math.ceil(math.log2(512 * k))))
        out.append(((hists[i] * bits).sum() + raws[i]) / 8 / d[3])
    return np.array(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dumps = [load(p) for p in sys.argv[1:]]
    for (lm, off, r, n), p in zip(dumps, sys.argv[1:]):
        a, b = off == r[0], (off != r[0]) & (off == r[1])
        c = ~a & ~b & (off == r[2])
        print(f"{p}: {len(off) / n:.1f} matches per page, at repeat offset 1 {a.mean():.1%}, 2 {b.mean():.1%}, 3 {c.mean():.1%}")
    base = price(dumps, 0)
    print("today, tokens and offset bits, bytes per page: " + " ".join(f"{x:.1f}" for x in base))
    for reps in (1, 2):
        d = price(dumps, reps) - base
        print(f"{reps + 1} repeat offsets: " + " ".join(f"{x:+.2f}" for x in d) + f", mean {d.mean():+.2f} bytes per page")


if __name__ == "__main__":
    main()
