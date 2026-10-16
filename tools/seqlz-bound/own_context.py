#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""own_context.py <train.bin> <test.bin>...: bytes per description of a page's own literal table, with
OWN as it is and with OWN's code chosen by the symbol before, counted offline. The .bin files are
owndump's output. Every description is parsed with OWN (and checked against the reference decoder's
length), then priced with codes trained on train.bin, one per context, at most 8 bits like OWN, every
symbol that can come in a context counted once more so that it has a code. A run never follows a run
(the encoder writes one run for the gap) and a run of 256 to 511 is never valid, so neither gets a
code in the context models. Prints bytes per description, the byte padding included, per test file."""

import math
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "seqlz-ref"))
import seqlz_ref  # noqa: E402

NSYM, RUN0, MAXBITS = 19, 10, 8
OWN = seqlz_ref.load_tables(12)[-1]
OWN_LEN = [0] * NSYM
PEEK = [None] * 256
for (l, c), s in OWN.items():
    OWN_LEN[s] = l
    for low in range(1 << (8 - l)):
        PEEK[(c << (8 - l)) | low] = (s, l)


def parse(page):
    """the description's symbols, each with the number of bits after it"""
    at = 3 + 5 + ((page[2] >> 4) & 7)
    bits = "".join(format(b, "08b") for b in page[at : at + 600]) + "0" * 16
    pos, space, syms = 0, 0, []
    while space < 1024:
        s, l = PEEK[int(bits[pos : pos + 8], 2)]
        extra = s - RUN0 if s >= RUN0 else 0
        pos += l + extra
        if s < RUN0:
            space += 1024 >> (s + 1)
        syms.append((s, extra))
    assert space == 1024 and seqlz_ref.own_table(page, at, OWN)[1] == (pos + 7) // 8
    return syms


def load(name):
    data, i, out = open(name, "rb").read(), 0, []
    while i < len(data):
        n = data[i] | data[i + 1] << 8
        out.append(parse(data[i + 2 : i + 2 + n]))
        i += 2 + n
    return out


def package_merge(freq):
    """code lengths of at most MAXBITS bits for the symbols with freq > 0, a complete code"""
    syms = [s for s in range(len(freq)) if freq[s]]
    lens = [0] * len(freq)
    leaves = sorted((freq[s], [s]) for s in syms)
    level = list(leaves)
    for _ in range(MAXBITS - 1):
        pairs = [(level[i][0] + level[i + 1][0], level[i][1] + level[i + 1][1]) for i in range(0, len(level) - 1, 2)]
        level = sorted(leaves + pairs, key=lambda x: x[0])
    for _, group in level[: 2 * len(syms) - 2]:
        for s in group:
            lens[s] += 1
    assert sum(2.0 ** -lens[s] for s in syms) == 1
    return lens


MODELS = {
    "OWN as it is": None,
    "after a run or not, 2 codes": lambda p: p is not None and p >= RUN0,
    "the length before: 6 or less, 7, 8, 9, 10, or a run or the start, 6 codes": lambda p: (
        0 if p is None or p >= RUN0 else min(max(p + 1, 6), 10)
    ),
    "the symbol before, 12 codes": lambda p: -1 if p is None else (RUN0 if p >= RUN0 else p),
}


def train(tables, ctx):
    count = {}
    for syms in tables:
        prev = None
        for s, _ in syms:
            count.setdefault(ctx(prev), Counter())[s] += 1
            prev = s
    codes = {}
    for c, k in count.items():
        after_run = c == ctx(RUN0) and c not in (ctx(0), ctx(None))  # a context only a run leads to
        codes[c] = package_merge([0 if s == 18 or (after_run and s >= RUN0) else k[s] + 1 for s in range(NSYM)])
    return codes


def price(tables, ctx, codes):
    total = 0
    for syms in tables:
        bits, prev = 0, None
        for s, extra in syms:
            l = (codes[ctx(prev)] if ctx else OWN_LEN)[s]
            assert l, (prev, s)
            bits += l + extra
            prev = s
        total += (bits + 7) // 8
    return total / len(tables)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    train_set = load(sys.argv[1])
    tests = {os.path.basename(n): load(n) for n in sys.argv[2:]}
    print("model\t" + "\t".join(tests) + "\tmean")
    for name, ctx in MODELS.items():
        codes = train(train_set, ctx) if ctx else None
        row = [price(t, ctx, codes) for t in tests.values()]
        print(name + "\t" + "\t".join(f"{v:.2f}" for v in row) + f"\t{sum(row) / len(row):.2f}")
    print("descriptions\t" + "\t".join(str(len(t)) for t in tests.values()))


if __name__ == "__main__":
    main()
