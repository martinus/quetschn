#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""offsets.py <output of seqs>...: layouts of seqlz's offset classes, priced on the sequences of each
dump: the token (ll, ml - 4 and the class) with an 11-bit length-limited Huffman code and an escape,
trained on the other dumps, plus the raw bits of the offsets. Prints bytes per page, and the difference
to today's layout. Needs numpy and at least two dumps."""

import math
import sys

import numpy as np


def load(path):
    a = np.fromfile(path, dtype=np.uint16).reshape(-1, 4).astype(np.int64)
    lm, off, last, first = a.T
    return lm, off, (off == last) | (off == 0), int(first.sum())


def classify(layout, off, rep):
    """layout: (predicate, raw bits) per class from 1 on, the first that matches wins; 0 repeats"""
    cls = np.where(rep, 0, -1)
    raw = np.zeros(off.shape, np.int64)
    for k, (pred, bits) in enumerate(layout, 1):
        m = (cls < 0) & pred(off)
        cls[m] = k
        raw[m] = bits
    assert (cls >= 0).all(), "an offset without a class"
    return cls, raw


def limited_lengths(counts, limit):
    """package-merge: optimal code lengths of at most limit bits"""
    syms = sorted((c, (i,)) for i, c in enumerate(counts) if c > 0)
    if len(syms) == 1:
        return {syms[0][1][0]: 1}
    items = list(syms)
    for _ in range(limit - 1):
        pairs = [
            (items[i][0] + items[i + 1][0], items[i][1] + items[i + 1][1])
            for i in range(0, len(items) - 1, 2)
        ]
        items = sorted(syms + pairs, key=lambda x: x[0])
    lengths = {}
    for _, s in items[: 2 * len(syms) - 2]:
        for i in s:
            lengths[i] = lengths.get(i, 0) + 1
    return lengths


def train(hist, esc_bits, limit=11):
    """bits per token: codes for the most frequent tokens, the others escaped, as many coded as cost the
    least on the training counts"""
    order = np.argsort(-hist)
    used = int((hist > 0).sum())
    best = None
    for k in sorted(
        {min(used, k) for k in (256, 384, 512, 640, 768, 1024, 1536, 2047)}
    ):
        coded = order[:k]
        esc = hist[order[k:]].sum()
        lengths = limited_lengths(
            list(hist[coded]) + [max(esc, 1e-9 * hist.sum())], limit
        )
        bits = np.full(len(hist), float(lengths[k] + esc_bits))
        bits[coded] = [lengths[j] for j in range(k)]
        cost = (hist * bits).sum()
        if best is None or cost < best[0]:
            best = (cost, bits)
    return best[1]


def price(layout, dumps):
    """bytes per page of tokens and raw offset bits of each dump"""
    symbols = 512 * (len(layout) + 1)
    hists, raws = [], []
    for lm, off, rep, _ in dumps:
        cls, raw = classify(layout, off, rep)
        hists.append(np.bincount(lm + 512 * cls, minlength=symbols).astype(float))
        raws.append(raw.sum())
    out = []
    for i, d in enumerate(dumps):
        others = sum(h / h.sum() for j, h in enumerate(hists) if j != i)
        bits = train(others * 1e6, max(12, math.ceil(math.log2(symbols))))
        out.append(((hists[i] * bits).sum() + raws[i]) / 8 / d[3])
    return out


def below(a, b=None):
    return (lambda o: (o >= a) & (o < b)) if b else (lambda o: o < a)


def x8(a, b):
    return lambda o: (o >= a) & (o < b) & (o % 8 == 0)


def x4(a, b):
    return lambda o: (o >= a) & (o < b) & (o % 4 == 0)


def eq(v):
    return lambda o: o == v


def either(a, b):
    return lambda o: (o == a) | (o == b)


TODAY = [
    (x8(16, 256), 5),
    (x8(256, 4096), 9),
    (below(16), 4),
    (below(256), 8),
    (below(4096), 12),
]
SEVEN = [
    (eq(2), 0),
    (eq(8), 0),
    (x8(16, 256), 5),
    (x8(256, 4096), 9),
    (below(256), 8),
    (below(4096), 12),
]
LAYOUTS = {
    "today: 1-15, 16-255, 256-4095, multiples of 8 16-255, 256-4095": TODAY,
    "offsets 2 and 8 as classes, 8 classes": [(eq(2), 0), (eq(8), 0)] + TODAY,
    "2 and 8 in one class with 1 raw bit, 7 classes": [(either(2, 8), 1)] + TODAY,
    "2 and 8 as classes, the rest of 1 to 15 in 8 bits, 7 classes": SEVEN,
    "... and multiples of 8 from 256 to 2047 in 8 bits, 8 classes": SEVEN[:3]
    + [(x8(256, 2048), 8)]
    + SEVEN[3:],
    "multiples of 4 as classes of their own, 8 classes": TODAY[:2]
    + [(x4(16, 256), 5), (x4(256, 4096), 9)]
    + TODAY[2:],
}

dumps = [load(p) for p in sys.argv[1:]]
base = None
for name, layout in LAYOUTS.items():
    r = price(layout, dumps)
    base = base or r
    print(
        f"{name:66} "
        + " ".join(f"{x:7.1f}" for x in r)
        + "   "
        + " ".join(f"{x - b:+5.1f}" for x, b in zip(r, base)),
        flush=True,
    )
