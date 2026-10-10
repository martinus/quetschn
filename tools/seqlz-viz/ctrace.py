#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""ctrace.py <dir>: the trace of seqlz's compressor for compress.html, from the pages in dir and their
compressed forms <page>.fast and <page>.lit (compress.c), to <dir>/ctrace.json. The matcher of
src/page_lz.h and the encoder of src/seqlz_compress.c written again step by step, and checked: the bytes
they give must be the ones compress.c wrote. Needs the Python packages lz4 and zstandard for the sizes
of the same pages in those two."""

import base64
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "seqlz-ref"))
import lz4.block
import seqlz_ref as ref
import zstandard

P = 4096
HASH_BITS = 12
M64 = (1 << 64) - 1
RAW_BITS = [0, 4, 8, 12, 5, 9]
LIT_CODED_MIN = 51


def inverse(codes):
    """symbol -> (code, length) from seqlz_ref's {(length, code): symbol}"""
    return {s: (c, n) for (n, c), s in codes.items()}


TOK, LL, ML, LITS = ref.load_tables(12)
TOK, LL, ML = inverse(TOK), inverse(LL), inverse(ML)
LITS = [inverse(t) for t in LITS]
ESCAPE = 3072


def load32(b, p):
    return int.from_bytes(b[p : p + 4], "little")


def load64(b, p):
    return int.from_bytes(b[p : p + 8], "little")


def hash5(v):
    return (((v << 24) & M64) * 889523592379 & M64) >> (64 - HASH_BITS)


def count(src, p, q):
    n = 0
    while p + n < P and src[p + n] == src[q + n]:
        n += 1
    return n


def match_page(src):
    """src/page_lz.h's seqlz_match_page(): the sequences, and for each what the matcher did"""
    limit = P - 8
    table = [0] * (1 << HASH_BITS)
    pos, anchor, last = 1, 0, 1
    seqs = []
    v = load64(src, pos)
    h = hash5(v)
    cand = table[h]
    probe0 = pos
    while pos < limit:
        cur = v & 0xFFFFFFFF
        rep_hit = load32(src, pos - last) == cur
        cand_hit = load32(src, cand) == cur
        table[h] = pos
        if not (rep_hit or cand_hit):
            pos += 1
            v = load64(src, pos)
            h = hash5(v)
            cand = table[h]
            continue
        probe = pos
        m = pos - last if rep_hit else cand
        while pos > anchor and m > 0 and src[pos - 1] == src[m - 1]:
            pos -= 1
            m -= 1
        ml = 4 + count(src, pos + 4, m + 4)
        seqs.append(
            {
                "a": anchor,
                "p0": probe0,
                "hp": probe,
                "hit": "both" if rep_hit and cand_hit else "rep" if rep_hit else "cand",
                "cand": cand,
                "slot": h,
                "prev": last,
                "d": pos,
                "ml": ml,
                "off": pos - m,
            }
        )
        last = pos - m
        pos += ml
        anchor = pos
        if pos < limit:
            table[hash5(load64(src, pos - 2))] = pos - 2
            v = load64(src, pos)
            h = hash5(v)
            cand = table[h]
            probe0 = pos
    seqs.append({"a": anchor, "p0": probe0 if anchor < limit else None, "hp": None, "d": P, "ml": 0, "off": 0})
    return seqs


class Bits:
    """the bitstream, most significant bit first"""

    def __init__(self):
        self.bits = []

    def put(self, v, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((v >> i) & 1)

    def code(self, c):
        self.put(*c)
        return format(c[0], f"0{c[1]}b")

    def bytes(self):
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            byte = 0
            for k, b in enumerate(self.bits[i : i + 8]):
                byte |= b << (7 - k)
            out.append(byte)
        return bytes(out)


def value(bits, codes, v):
    if v < 16:
        sym, xb, x = v, 0, 0
    else:
        xb = v.bit_length() - 1
        sym, x = 12 + xb, v - (1 << xb)
    p = len(bits.bits)
    code = bits.code(codes[sym])
    bits.put(x, xb)
    return {"p": p, "code": code, "sym": sym, "xl": xb, "x": x, "v": v}


def encode(src, seqs):
    """src/seqlz_compress.c's encoder: the page with the literals as they are, and per sequence its fields"""
    bits = Bits()
    lits = bytearray()
    last = 1
    for i, s in enumerate(seqs):
        is_last = i == len(seqs) - 1
        ll, ml, off = s["d"] - s["a"], s["ml"], s["off"]
        s["ll"], s["li"] = ll, len(lits)
        lits += src[s["a"] : s["d"]]
        if is_last or off == last:
            cls = 0
        else:
            aligned = off >= 16 and off % 8 == 0
            cls = 1 + (off >= 16) + (off >= 256) + 2 * aligned
        t = min(ll, 15) + 16 * (0 if is_last else min(ml - 4, 31)) + 512 * cls
        s.update(cls=cls, t=t, tp=len(bits.bits))
        if t in TOK:
            s["tc"] = bits.code(TOK[t])
        else:
            s["tc"] = bits.code(TOK[ESCAPE])
            s["ep"] = len(bits.bits)
            bits.put(t, 12)
        s["rl"] = RAW_BITS[cls]
        s["rp"] = len(bits.bits)
        s["raw"] = off >> 3 if cls >= 4 else off if cls else 0
        bits.put(s["raw"], s["rl"])
        if ll >= 15:
            s["llv"] = value(bits, LL, ll - 15)
        if not is_last:
            if ml - 4 >= 31:
                s["mlv"] = value(bits, ML, ml - 35)
            last = off
        s["end"] = len(bits.bits)
    n = len(lits)
    return bytes([n & 255, n >> 8]) + bytes(lits) + bits.bytes(), bytes(lits), len(bits.bits)


def code_literals(raw, lits):
    """src/seqlz_compress.c's code_literals(): the cost in every table, and the page with coded literals if
    that pays"""
    n = len(lits)
    cost = [[sum(LITS[t][lits[k]][1] for k in range(j, n, 8)) for j in range(8)] for t in range(8)]
    totals = [sum(c) for c in cost]
    best = min(range(8), key=lambda t: (totals[t], t))
    sizes = [(b + 7) // 8 for b in cost[best]]
    coded = sum(sizes)
    allbits = 0
    for z in sizes:
        allbits |= z
    width = max(5, (allbits | 16).bit_length())
    header = 3 + width
    info = {"cost": cost, "set": best, "sizes": sizes, "coded": coded, "width": width, "limit": n - n // 16}
    info["pays"] = coded + LIT_CODED_MIN < n - n // 16
    if not info["pays"]:
        return raw, info
    body = raw[2 + n :]
    packed = 0
    for j, z in enumerate(sizes):
        packed |= z << (j * width)
    out = bytearray([n & 255, (n >> 8) | 0x80, best | (width - 5) << 3])
    out += packed.to_bytes(width, "little")
    starts = [header]
    for j in range(8):
        b = Bits()
        for k in range(j, n, 8):
            b.code(LITS[best][lits[k]])
        sb = b.bytes()
        assert len(sb) == sizes[j]
        out += sb
        starts.append(starts[-1] + len(sb))
    info["starts"] = starts
    out += body
    return bytes(out), info


def trace(pg, fast, lit, name):
    seqs = match_page(pg)
    raw, lits, nbits = encode(pg, seqs)
    assert raw == fast, name + ": the matcher and encoder here do not write what compress.c wrote"
    coded, info = code_literals(raw, lits)
    assert coded == lit, name + ": the literals are not coded as compress.c coded them"
    slots = bytearray()
    for p in range(P - 7):
        slots += hash5(load64(pg, p)).to_bytes(2, "little")
    return {
        "page": base64.b64encode(pg).decode(),
        "raw": base64.b64encode(raw).decode(),
        "lit": base64.b64encode(coded).decode(),
        "slots": base64.b64encode(bytes(slots)).decode(),
        "n": len(lits),
        "nbits": nbits,
        "seqs": seqs,
        "coding": info,
    }


DIR = Path(sys.argv[1])
PAGES = [
    (
        "heap",
        "Heap of a program",
        "A page of a small C++ program's heap: a hash map of the words of docs/format.md, each a struct with a string, two counters, a pointer to the one before and a double. malloc's chunk headers, pointers, and short strings.",
    ),
    (
        "text",
        "Source code",
        "4 KiB of src/seqlz_decompress.c, the decoder's bit reader: text, as a program holds it in memory after reading a file.",
    ),
    (
        "relro",
        "Relocated pointer table",
        "A page of libstdc++ in a running program, after the dynamic linker wrote the addresses into it: a table of a byte and a pointer, 16 bytes per entry.",
    ),
]
pages = []
for key, title, desc in PAGES:
    pg = (DIR / f"{key}.page").read_bytes()
    t = trace(pg, (DIR / f"{key}.page.fast").read_bytes(), (DIR / f"{key}.page.lit").read_bytes(), key)
    t.update(
        key=key,
        title=title,
        desc=desc,
        lz4=len(lz4.block.compress(pg, store_size=False)),
        zstd=len(zstandard.ZstdCompressor(level=3, write_checksum=False).compress(pg)),
    )
    pages.append(t)

tok_len = [TOK[s][1] if s in TOK else 0 for s in range(3073)]
lit_len = [LITS[t][b][1] for t in range(8) for b in range(256)]
data = {"pages": pages, "tok_len": tok_len, "lit_len": lit_len}
(DIR / "ctrace.json").write_text(json.dumps(data, separators=(",", ":")))
for p in pages:
    c = p["coding"]
    print(
        p["key"],
        "seqs",
        len(p["seqs"]),
        "lits",
        p["n"],
        "raw",
        len(base64.b64decode(p["raw"])),
        "lit",
        len(base64.b64decode(p["lit"])),
        "table",
        c["set"],
        "coded" if c["pays"] else "raw",
    )
