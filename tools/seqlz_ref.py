#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""seqlz_ref.py <compressed page>...: a decoder written from FORMAT.md alone, slow on purpose, to check
the spec against explore/seqlz.c. Prints per file "invalid" or "valid <sha256 of the page>".
--page-bits 14 for 16 KiB pages. The tables are read from the files FORMAT.md names, and checked
against the SHA-256 FORMAT.md gives for them."""

import argparse
import hashlib
import re
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


class Invalid(Exception):
    pass


def numbers(text):
    return [int(x) for x in re.findall(r"\b\d+\b", re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL))]


def spec_hashes():
    """the SHA-256 of each table, from FORMAT.md's table of tables"""
    return dict(re.findall(r"^\| `(\w+)` \| `([0-9a-f]{64})` \|", (REPO / "FORMAT.md").read_text(), re.MULTILINE))


def load_tables(page_bits):
    """canonical codes of TOK, LL, ML and the 8 literal tables, after checking the lengths' SHA-256"""
    explore = REPO / "explore"
    src = (explore / f"seqlz_default_tables_{4 if page_bits == 12 else 16}k.inc").read_text()

    def field(name):
        return numbers(re.search(r"\." + name + r"\s*=\s*\{([^}]*)\}", src).group(1))

    tables = {"TOK": field("token"), "LL": field("ll"), "ML": field("ml")}
    lit_src = (explore / "seqlz_lit_sets.c").read_text()
    lit = numbers(lit_src[lit_src.index("seqlz_lit_sets[SEQLZ_LIT_SETS][256] =") :].split("=", 1)[1])
    assert len(tables["TOK"]) == 3073 and len(tables["LL"]) == len(tables["ML"]) == 13 + page_bits
    assert len(lit) == 8 * 256
    hashes, size = spec_hashes(), f"{4 if page_bits == 12 else 16}k"
    for name, lengths in [*tables.items(), ("LIT", lit)]:
        key = f"{name}_{size}" if name != "LIT" else "LIT"
        if hashlib.sha256(bytes(lengths)).hexdigest() != hashes[key]:
            raise SystemExit(f"{key}: the table is not the one FORMAT.md names")
    return (
        canonical(tables["TOK"], 11),
        canonical(tables["LL"], 8),
        canonical(tables["ML"], 8),
        [canonical(lit[256 * k : 256 * (k + 1)], 10) for k in range(8)],
    )


def canonical(lengths, max_bits):
    """{(length, code): symbol}, FORMAT.md's "Prefix codes"; complete codes only"""
    assert max(lengths) <= max_bits
    assert sum(2.0**-n for n in lengths if n) == 1.0
    count = [0] * (max_bits + 1)
    for n in lengths:
        if n:
            count[n] += 1
    first, code = [0] * (max_bits + 1), 0
    for n in range(1, max_bits + 1):
        code = (code + count[n - 1]) << 1
        first[n] = code
    codes = {}
    for s, n in enumerate(lengths):
        if n:
            codes[(n, first[n])] = s
            first[n] += 1
    return codes


class Bits:
    """the bits of data from byte start on, 0 past its end: the sequences' bitstream least significant
    bit first in each byte, a literal stream most significant bit first"""

    def __init__(self, data, start=0, msb=False):
        self.data, self.pos, self.msb = data, 8 * start, msb

    def bit(self):
        i = self.pos
        self.pos += 1
        if i >= 8 * len(self.data):
            return 0
        return (self.data[i // 8] >> (7 - i % 8 if self.msb else i % 8)) & 1

    def read(self, n):
        return sum(self.bit() << k for k in range(n))

    def symbol(self, codes):
        code, length = 0, 0
        while (length, code) not in codes:
            code, length = code << 1 | self.bit(), length + 1
        return codes[(length, code)]


def decode(page, tables, page_bits):
    tok_codes, ll_codes, ml_codes, lit_codes = tables
    size = 1 << page_bits
    offset_bits = [0, 4, 8, page_bits, 5, page_bits - 3]
    if len(page) < 2:
        raise Invalid
    h = page[0] | page[1] << 8
    n = h & 0x7FFF
    if h & 0x8000:
        if len(page) < 19 or n > size or page[2] > 7:
            raise Invalid
        start = [19]
        for j in range(8):
            start.append(start[-1] + (page[3 + 2 * j] | page[4 + 2 * j] << 8))
        if start[8] > len(page):
            raise Invalid
        lits = [0] * n
        for j in range(8):
            r = Bits(page, start[j], msb=True)
            for k in range(j, n, 8):
                lits[k] = r.symbol(lit_codes[page[2]])
            if r.pos > 8 * start[j + 1]:
                raise Invalid
        bits = Bits(page[start[8] :])
    else:
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
        raw = bits.read(offset_bits[c])
        off = last if c == 0 else raw << (3 if c >= 4 else 0)
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
    tables = load_tables(args.page_bits)
    for f in args.files:
        try:
            print(f"{f} valid {hashlib.sha256(decode(Path(f).read_bytes(), tables, args.page_bits)).hexdigest()}")
        except Invalid:
            print(f"{f} invalid")


if __name__ == "__main__":
    main()
