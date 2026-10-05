#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""seqlz_ref.py <compressed page>...: a decoder written from FORMAT.md alone, slow on purpose, to check
the spec against explore/seqlz.c. Prints per file "invalid" or "valid <sha256 of the page>".
--page-bits 14 for 16 KiB pages. The tables are read from the C files FORMAT.md names."""
import argparse
import hashlib
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


class Invalid(Exception):
    pass


def numbers(text):
    return [int(x) for x in re.findall(r"\b\d+\b", text)]


def load_tables(page_bits):
    explore = REPO / "explore"
    src = (explore / ("seqlz_default_tables.c" if page_bits == 12 else "seqlz_default_tables_16k.inc")).read_text()
    if page_bits == 12:
        src = src.split("#else", 1)[1]
    len_symbols = 13 + page_bits

    def field(name):
        body = re.search(r"\." + name + r"\s*=\s*\{([^}]*)\}", src).group(1)
        return numbers(body)

    tok, ll, ml = field("token"), field("ll"), field("ml")
    assert len(tok) == 3073 and len(ll) == len_symbols and len(ml) == len_symbols
    lit_src = (explore / "seqlz_lit_sets.c").read_text()
    lit_src = lit_src[lit_src.index("seqlz_lit_sets[SEQLZ_LIT_SETS][256] =") :]
    lit = numbers(lit_src.split("=", 1)[1])
    assert len(lit) == 8 * 256
    return tok, ll, ml, [lit[256 * k : 256 * (k + 1)] for k in range(8)]


def canonical(lengths, max_bits):
    """{(length, code): symbol}, FORMAT.md's "Prefix codes"; complete codes only."""
    assert max(lengths) <= max_bits
    assert sum(2.0 ** -l for l in lengths if l) == 1.0
    count = [0] * (max_bits + 1)
    for l in lengths:
        if l:
            count[l] += 1
    nxt, code = [0] * (max_bits + 1), 0
    for l in range(1, max_bits + 1):
        code = (code + count[l - 1]) << 1
        nxt[l] = code
    codes = {}
    for s, l in enumerate(lengths):
        if l:
            codes[(l, nxt[l])] = s
            nxt[l] += 1
    return codes


class Bits:
    """the sequences' bitstream: least significant bit first in each byte, 0 past the end"""

    def __init__(self, data):
        self.data, self.pos = data, 0

    def bit(self):
        i = self.pos
        self.pos += 1
        return (self.data[i // 8] >> (i % 8)) & 1 if i < 8 * len(self.data) else 0

    def read(self, n):
        return sum(self.bit() << k for k in range(n))

    def symbol(self, codes):
        code, length = 0, 0
        while True:
            code, length = code << 1 | self.bit(), length + 1
            if (length, code) in codes:
                return codes[(length, code)]


class MsbBits:
    """a literal stream: most significant bit first; past its end the page's next bytes, then 0, which
    the check of the stream's size rejects"""

    def __init__(self, data, start):
        self.data, self.pos = data, 8 * start

    def bit(self):
        i = self.pos
        self.pos += 1
        return (self.data[i // 8] >> (7 - i % 8)) & 1 if i < 8 * len(self.data) else 0

    def symbol(self, codes):
        code, length = 0, 0
        while True:
            code, length = code << 1 | self.bit(), length + 1
            if (length, code) in codes:
                return codes[(length, code)]


def decode(page, tables, page_bits):
    tok_codes, ll_codes, ml_codes, lit_codes = tables
    size = 1 << page_bits
    raw_bits = [0, 4, 8, 12, 5, 9] if page_bits == 12 else [0, 4, 8, 14, 5, 11]
    shift = [0, 0, 0, 0, 3, 3]
    if len(page) < 2:
        raise Invalid
    h = page[0] | page[1] << 8
    if h & 0x8000:
        n = h & 0x7FFF
        if len(page) < 19 or n > size or page[2] > 7:
            raise Invalid
        s = [page[3 + 2 * j] | page[4 + 2 * j] << 8 for j in range(8)]
        if 19 + sum(s) > len(page):
            raise Invalid
        lits = [0] * n
        for j in range(8):
            r = MsbBits(page, 19 + sum(s[:j]))
            for k in range(j, n, 8):
                lits[k] = r.symbol(lit_codes[page[2]])
            if r.pos - 8 * (19 + sum(s[:j])) > 8 * s[j]:
                raise Invalid
        bits = Bits(page[19 + sum(s) :])
    else:
        n = h
        if 2 + n > len(page):
            raise Invalid
        lits = list(page[2 : 2 + n])
        bits = Bits(page[2 + n :])

    def value(codes):
        sym = bits.symbol(codes)
        if sym < 16:
            return sym
        b = sym - 12
        return (1 << b) + bits.read(b)

    out, used, last = bytearray(), 0, 1
    while True:
        t = bits.symbol(tok_codes)
        if t == 3072:
            t = bits.read(12)
            if t >= 3072:
                raise Invalid
        ll, mlf, c = t & 15, (t >> 4) & 31, t >> 9
        raw = bits.read(raw_bits[c])
        off = last if c == 0 else raw << shift[c]
        if ll == 15:
            ll = 15 + value(ll_codes)
        if ll > n - used or ll > size - len(out):
            raise Invalid
        out += bytes(lits[used : used + ll])
        used += ll
        if len(out) == size:
            break
        ml = mlf + 4
        if mlf == 31:
            ml = 35 + value(ml_codes)
        last = off
        if off == 0 or off > len(out) or ml > size - len(out):
            raise Invalid
        for _ in range(ml):
            out.append(out[-off])
    if used != n or bits.pos > 8 * len(bits.data):
        raise Invalid
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--page-bits", type=int, default=12, choices=[12, 14])
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    tok, ll, ml, lit = load_tables(args.page_bits)
    tables = (canonical(tok, 11), canonical(ll, 8), canonical(ml, 8), [canonical(x, 10) for x in lit])
    for f in args.files:
        try:
            print(f"{f} valid {hashlib.sha256(decode(Path(f).read_bytes(), tables, args.page_bits)).hexdigest()}")
        except Invalid:
            print(f"{f} invalid")


if __name__ == "__main__":
    main()
