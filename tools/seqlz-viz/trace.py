#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""trace.py <dir>: the trace of seqlz's decoder for page.html, from the pages in dir and their
compressed forms <page>.fast and <page>.lit (compress.c), to <dir>/trace.json. Every field with its bit
position, decoded with tools/seqlz-ref/seqlz_ref.py's tables, and the C decoder's register refills and fast path
emulated. A position is byte * 8 + the bit's index in reading order, highest bit first in the
sequences' bitstream and in a literal stream. Needs the Python packages lz4 and zstandard for the
sizes of the same pages in those two."""

import base64
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "seqlz-ref"))
import lz4.block
import seqlz_ref as ref
import zstandard

P = 4096
tok_codes, ll_codes, ml_codes, lit_codes = ref.load_tables(12)
src = (ref.REPO / "src/seqlz_default_tables_4k.inc").read_text()


def field(name):
    return ref.numbers(re.search(r"\." + name + r"\s*=\s*\{([^}]*)\}", src).group(1))


TOK_LEN, LL_LEN, ML_LEN = field("token"), field("ll"), field("ml")
lit_src = (ref.REPO / "src/seqlz_lit_sets.c").read_text()
LIT_LEN = ref.numbers(lit_src[lit_src.index("seqlz_lit_sets[SEQLZ_LIT_SETS][256] =") :].split("=", 1)[1])


class Reader:
    """the bits of data from byte start on, 0 past its end; pos is where the next read starts"""

    def __init__(self, data, start):
        self.data, self.pos = data, 8 * start

    def bit(self):
        i = self.pos
        self.pos += 1
        if i >= 8 * len(self.data):
            return 0
        return (self.data[i // 8] >> (7 - i % 8)) & 1

    def read(self, n):
        v = 0
        for _ in range(n):
            v = v << 1 | self.bit()
        return v

    def symbol(self, codes):
        code, length, s = 0, 0, ""
        while (length, code) not in codes:
            b = self.bit()
            code, length, s = code << 1 | b, length + 1, s + str(b)
        return codes[(length, code)], s


def trace(page_bytes, comp, name):
    h = comp[0] | comp[1] << 8
    n = h & 0x7FFF
    out = {"comp": base64.b64encode(comp).decode(), "n": n}
    if h & 0x8000:
        out["layout"] = "coded"
        out["set"], w = comp[2] & 7, 5 + (comp[2] >> 3 & 7)
        packed = int.from_bytes(comp[3 : 3 + w], "little")
        sizes = [packed >> (j * w) & ((1 << w) - 1) for j in range(8)]
        out["width"] = w
        starts = [3 + w]
        for s in sizes:
            starts.append(starts[-1] + s)
        out["sizes"], out["starts"] = sizes, starts
        lit_rec = [None] * n
        for j in range(8):
            r = Reader(comp, starts[j])
            for k in range(j, n, 8):
                p0 = r.pos
                v, bits = r.symbol(lit_codes[comp[2] & 7])
                lit_rec[k] = [p0, len(bits), v, bits]
            assert r.pos <= 8 * starts[j + 1]
        lits = [x[2] for x in lit_rec]
        out["lits"] = lit_rec
        bs = starts[8]
        # the fast path needs 16 bytes of literals behind the cursor: the scratch has them always
        lit_limit = P
    else:
        out["layout"] = "raw"
        lits = list(comp[2 : 2 + n])
        bs = 2 + n
        lit_limit = len(comp) - 2 - 16
    out["bs"] = bs

    r = Reader(comp, bs)
    end = len(comp) - bs
    # the C decoder's bit reader: p bytes loaded, count bits in the register; a refill when fewer than
    # 23 are left for the next token and offset, and always before the escape's 12 bits and a length value
    st = {"p": 0, "count": 0}
    events = []

    def refill(why):
        before = st["count"]
        if end - st["p"] >= 8:
            st["p"] += (63 - st["count"]) >> 3
            st["count"] |= 56
        else:
            while 0 <= st["count"] <= 56 and st["p"] < end:
                st["p"] += 1
                st["count"] += 8
        events.append([why, before, st["count"]])

    def drop(k):
        st["count"] -= k

    def value(codes):
        p0 = r.pos
        sym, bits = r.symbol(codes)
        if sym < 16:
            v, xb, x = sym, 0, 0
        else:
            xb = sym - 12
            x = r.read(xb)
            v = (1 << xb) + x
        drop(len(bits) + xb)
        return {"p": p0, "cl": len(bits), "code": bits, "sym": sym, "xl": xb, "x": x, "v": v}

    seqs, outb, used, last = [], bytearray(), 0, 1
    if st["count"] < 23:
        refill("token")
    while True:
        seq = {"ev": events[:], "cnt": st["count"], "tp": r.pos}
        events.clear()
        t, bits = r.symbol(tok_codes)
        seq["tc"] = bits
        if t == 3072:
            drop(len(bits))
            refill("escape")
            seq["ev"] += events[:]
            events.clear()
            seq["ep"] = r.pos
            t = r.read(12)
            drop(12)
        else:
            drop(len(bits))
        ll, mlf, c = t & 15, (t >> 4) & 31, t >> 9
        ob = [0, 4, 8, 12, 5, 9][c]
        seq.update(t=t, cls=c, rp=r.pos, rl=ob)
        raw = r.read(ob)
        drop(ob)
        off = last if c == 0 else raw << (3 if c >= 4 else 0)
        seq["raw"], seq["off"] = raw, off
        lenflag = ll == 15 or mlf == 31
        d0 = len(outb)
        seq["d"], seq["li"] = d0, used
        seq["fast"] = not lenflag and d0 <= P - 64 and used <= lit_limit
        if ll == 15:
            refill("ll")
            seq["llv"] = value(ll_codes)
            ll = 15 + seq["llv"]["v"]
        outb += bytes(lits[used : used + ll])
        used += ll
        seq["ll"] = ll
        if len(outb) == P:
            seq["ml"] = 0
            seq["ev"] += events[:]
            events.clear()
            seqs.append(seq)
            break
        ml = mlf + 4
        if mlf == 31:
            refill("ml")
            seq["mlv"] = value(ml_codes)
            ml = 35 + seq["mlv"]["v"]
        last = off
        assert 0 < off <= len(outb) and ml <= P - len(outb)
        for _ in range(ml):
            outb.append(outb[-off])
        seq["ml"] = ml
        seq["ev"] += events[:]
        events.clear()
        if st["count"] < 23:
            refill("token")
        seqs.append(seq)
    assert bytes(outb) == page_bytes and used == n, name
    out["seqs"] = seqs
    return out


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
        "4 KiB of src/seqlz.c, the decoder's bit reader: text, as a program holds it in memory after reading a file.",
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
    entry = {
        "key": key,
        "title": title,
        "desc": desc,
        "page": base64.b64encode(pg).decode(),
        "lz4": len(lz4.block.compress(pg, store_size=False)),
        "zstd": len(zstandard.ZstdCompressor(level=3, write_checksum=False).compress(pg)),
        "variants": {},
    }
    for v in ["lit", "fast"]:
        entry["variants"][v] = trace(pg, (DIR / f"{key}.page.{v}").read_bytes(), key + v)
    pages.append(entry)

data = {"pages": pages, "tok_len": TOK_LEN, "ll_len": LL_LEN, "ml_len": ML_LEN, "lit_len": LIT_LEN}
(DIR / "trace.json").write_text(json.dumps(data, separators=(",", ":")))
for p in pages:
    for v, t in p["variants"].items():
        print(
            p["key"],
            v,
            t["layout"],
            len(base64.b64decode(t["comp"])),
            "seqs",
            len(t["seqs"]),
            "lits",
            t["n"],
            "fast",
            sum(s["fast"] for s in t["seqs"]),
            "esc",
            sum("ep" in s for s in t["seqs"]),
        )
