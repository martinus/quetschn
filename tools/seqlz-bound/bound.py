#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""bound.py <output of bound>...: per symbol kind, in bytes per page: the bits seqlz's codes take, the
bits of Huffman codes fitted to this dump (no length limit, no escape), and the entropy of the dump's
own symbol counts. Codes fitted to the dump minus the entropy is what a coder with fractional bits,
ANS, could take off with the same model; seqlz's codes minus the fitted ones is what the tables lose
by being fixed, trained on other pages, and limited to 11 or 10 bits."""

import heapq
import json
import math
import sys
from pathlib import Path


def entropy(c):
    n = sum(c)
    return sum(-x * math.log2(x / n) for x in c if x)


def huffman_bits(c):
    """bits of an optimal prefix code for counts c, no length limit"""
    h = [x for x in c if x]
    if len(h) < 2:
        return sum(h)
    heapq.heapify(h)
    total = 0
    while len(h) > 1:
        a, b = heapq.heappop(h), heapq.heappop(h)
        total += a + b
        heapq.heappush(h, a + b)
    return total


for path in sys.argv[1:]:
    d = json.loads(Path(path).read_text())
    P, b = d["pages"], d["bits"]

    def per(x, pages=P):
        """bytes per page"""
        return x / 8 / pages

    rows = []
    tok_seqlz = b["tok"] + b["esc"]
    rows.append(("tokens", tok_seqlz, huffman_bits(d["tok"]), entropy(d["tok"])))
    rows.append(("ll value codes", b["llc"], huffman_bits(d["ll"]), entropy(d["ll"])))
    rows.append(("ml value codes", b["mlc"], huffman_bits(d["ml"]), entropy(d["ml"])))
    lit_h = sum(huffman_bits(x) for x in d["lit"])
    lit_e = sum(entropy(x) for x in d["lit"])
    rows.append(("coded literals", b["lit"], lit_h, lit_e))
    rows.append(("raw literals", b["rawlit"], huffman_bits(d["raw"]), entropy(d["raw"])))
    print(
        f"== {path}: {P} pages kept compressed, {d['huge']} stored raw by zram, {d['coded_pages']} with coded literals"
    )
    print(f"   {d['bytes'] / P:.1f} bytes per page; {d['esc'] / P:.1f} escaped tokens per page")
    print(f"   {'kind':16} {'seqlz':>8} {'fitted':>8} {'entropy':>8} {'tables lose':>12} {'ANS could':>10}")
    for name, s, h, e in rows:
        print(f"   {name:16} {per(s):8.1f} {per(h):8.1f} {per(e):8.1f} {per(s - h):12.1f} {per(h - e):10.1f}")
    for c, rb in ((1, 4), (2, 8), (3, 12), (4, 5), (5, 9)):
        h = d[f"off{c}"]
        n = sum(h)
        print(
            f"   offsets class {c}  {per(n * rb):8.1f} {'':>8} {per(entropy(h)):8.1f} {'':>12} {'':>10}   {n / P:.1f} per page, {rb} bits"
        )
    for name, key in (("length extras", None), ("header", "hdr"), ("fill bits", "pad")):
        v = b["llx"] + b["mlx"] if key is None else b[key]
        print(f"   {name:16} {per(v):8.1f}   (not coded)")
