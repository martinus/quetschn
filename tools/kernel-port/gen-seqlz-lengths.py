#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Generates the code lengths of seqlz's tables, part of its format, from the
# counts of the symbols on the pages they were trained on. Prints
# seqlz_lengths.h, from which gen_seqlz_tables builds the codes and decode
# tables when the kernel is built.
#
#   gen-seqlz-lengths.py <counts file>
#
# The counts file has comment lines starting with '#', then "token", "ll" and
# "ml", each followed by the number of symbols and how often each occurred, 8
# times "lit", each followed by 257 and the number of pages that literal table
# codes, then how often each byte occurred as a literal on them, and "own",
# followed by 19 and how often each symbol of the description of a block's own
# literal table occurred. Numbers may go over several lines.
#
# Each table is a length-limited prefix code, built by package-merge, with
# every count plus 1. Only the most frequent tokens get a code; the others are
# sent as the escape, the last symbol of the token table, and the 12 bits of
# the token.
#
# Before it writes them, it checks the lengths as gen_seqlz_tables does, and
# fails if one does not pass: every table a complete prefix code, and a code
# for every symbol the encoder writes.

import sys

TOKEN_BITS = 11     # the longest code of a token
VALUE_BITS = 8      # the longest code of a length value
LIT_BITS = 10       # the longest code of a literal
ESCAPE_BITS = 12    # the bits of a token after the escape
LIT_SETS = 8
TOKENS = 3072
OWN_BITS = 8        # the longest code of a symbol of a block's own table
OWN_SYMBOLS = 19


def code_lengths(counts, max_bits):
    """The optimal code lengths of at most max_bits bits, by package-merge
    (Larmore and Hirschberg).

    The lists of the levels from max_bits up to 1 each hold the symbols and
    the pairs of the level below, sorted by weight, a symbol before a pair of
    the same weight. A symbol's length is how often it is among the 2n - 2
    lightest items of the top list, counted through the pairs."""
    n = len(counts)
    order = sorted(range(n), key=lambda s: counts[s])
    leaves = [(counts[s], s, None) for s in order]
    levels = [leaves]
    for _ in range(1, max_bits):
        below = levels[-1]
        pairs = [(below[i][0] + below[i + 1][0], i, i + 1)
                 for i in range(0, len(below) - 1, 2)]
        merged, a, b = [], 0, 0
        while a < len(leaves) or b < len(pairs):
            if b == len(pairs) or (a < len(leaves) and
                                   not pairs[b][0] < leaves[a][0]):
                merged.append(leaves[a])
                a += 1
            else:
                merged.append(pairs[b])
                b += 1
        levels.append(merged)
    lengths = [0] * n
    for i in range(2 * n - 2):
        stack = [(len(levels) - 1, i)]
        while stack:
            level, index = stack.pop()
            _, left, right = levels[level][index]
            if right is None:
                lengths[left] += 1
            else:
                stack += [(level - 1, left), (level - 1, right)]
    return lengths


def token_lengths(counts, block_bits):
    """Codes for the k most frequent tokens and the escape, with k in steps
    of 64 as it gives the fewest bits. A token and its offset take at most 31
    bits, also when escaped, which limits the escape's code."""
    max_escape = 31 - block_bits - ESCAPE_BITS
    order = sorted(range(len(counts)), key=lambda s: -counts[s])
    most = min(len(counts), (1 << TOKEN_BITS) - 1)
    best, best_bits = None, 0.0
    k = 64
    while k <= most:
        kept = [counts[order[i]] for i in range(k)]
        escaped = 1.0 + sum(counts[order[i]] for i in range(k, len(order)))
        lengths_k = code_lengths(kept + [escaped], TOKEN_BITS)
        if lengths_k[-1] <= max_escape:
            bits = (escaped - 1.0) * (lengths_k[-1] + ESCAPE_BITS)
            lengths = [0] * (len(counts) + 1)
            for i in range(k):
                bits += counts[order[i]] * lengths_k[i]
                lengths[order[i]] = lengths_k[i]
            lengths[-1] = lengths_k[-1]
            if best is None or bits < best_bits:
                best, best_bits = lengths, bits
        k = k + 64 if k + 64 <= most or k == most else most
    return best


def check(name, lengths, max_bits, every):
    """lengths as gen_seqlz_tables takes them: no code longer than max_bits,
    the codes filling the decode table of 2^max_bits entries exactly, and with
    every a code for every symbol"""
    if max(lengths) > max_bits:
        sys.exit(f"{name}: a code is longer than {max_bits} bits")
    if sum(1 << (max_bits - n) for n in lengths if n) != 1 << max_bits:
        sys.exit(f"{name}: the codes do not fill the decode table exactly")
    if every and not all(lengths):
        sys.exit(f"{name}: a symbol has no code")


def read_counts(path):
    words = []
    with open(path) as f:
        for line in f:
            if not line.startswith("#"):
                words += line.split()
    tables, pos = [], 0
    for name in ["token", "ll", "ml"] + ["lit"] * LIT_SETS + ["own"]:
        if pos + 2 > len(words) or words[pos] != name:
            sys.exit(f"{path}: expected {name}")
        n = int(words[pos + 1])
        values = [float(w) for w in words[pos + 2:pos + 2 + n]]
        if len(values) != n or min(values) < 0:
            sys.exit(f"{path}: {name} needs {n} numbers of 0 or more")
        tables.append(values)
        pos += 2 + n
    if pos != len(words):
        sys.exit(f"{path}: more than the tables")
    if len(tables[0]) != TOKENS or len(tables[1]) != len(tables[2]) or \
            any(len(t) != 257 for t in tables[3:3 + LIT_SETS]) or \
            len(tables[-1]) != OWN_SYMBOLS:
        sys.exit(f"{path}: the tables have the wrong sizes")
    return tables


def rows(values, indent):
    """values as lines of a C initializer, 16 numbers each"""
    return "".join(f"{indent}" + ", ".join(str(v) for v in values[i:i + 16]) +
                   ",\n" for i in range(0, len(values), 16))


def initializer(name, lists):
    """lists as one C initializer, each in braces"""
    return f"{name} = {{\n" + "".join(
        f"\t{label}{{\n" + rows(values, "\t\t") + "\t},\n"
        for label, values in lists) + "};\n"


def lengths_initializer(tables, storage=""):
    plus_one = [[c + 1.0 for c in t] for t in tables[:3]]
    # a length value's symbols: 16 of their own, then one per highest bit
    # from 4 up to the block's bits minus 1, 12 + P of them, see
    # SEQLZ_LEN_SYMBOLS
    block_bits = len(tables[1]) - 12
    if block_bits not in (12, 14):
        sys.exit(f"ll: {len(tables[1])} symbols, which is no block size")
    token = token_lengths(plus_one[0], block_bits)
    ll = code_lengths(plus_one[1], VALUE_BITS)
    ml = code_lengths(plus_one[2], VALUE_BITS)
    # a token without a code is sent as the escape, which must have one that
    # leaves room for the offset
    check("token", token, TOKEN_BITS, False)
    if not 0 < token[-1] <= 31 - block_bits - ESCAPE_BITS:
        sys.exit("token: the escape has no code, or one too long")
    check("ll", ll, VALUE_BITS, True)
    check("ml", ml, VALUE_BITS, True)
    return initializer(
        f"{storage}const struct seqlz_lengths seqlz_default_lengths",
        [(".token = ", token), (".ll = ", ll), (".ml = ", ml)])


def lit_sets_initializer(tables, storage=""):
    # every byte gets a code, the encoder needs one; the first number of a
    # literal table's counts is its pages, not a byte
    sets = [code_lengths([c + 1.0 for c in t[1:]], LIT_BITS)
            for t in tables[3:3 + LIT_SETS]]
    for k, s in enumerate(sets):
        check(f"lit {k}", s, LIT_BITS, True)
    return initializer(
        f"{storage}const u8 seqlz_lit_sets[SEQLZ_LIT_SETS][256]",
        [("", s) for s in sets])


def own_initializer(tables, storage=""):
    # the code of a block's own literal table, see SEQLZ_LIT_OWN; every
    # symbol gets a code, so that the decoder's table has no gap
    own = code_lengths([c + 1.0 for c in tables[-1]], OWN_BITS)
    check("own", own, OWN_BITS, True)
    return (f"{storage}const u8 seqlz_own_lengths[SEQLZ_OWN_SYMBOLS] = {{\n" +
            rows(own, "\t") + "};\n")


def head():
    return ("/* SPDX-License-Identifier: GPL-2.0-only OR MIT */\n"
            f"/*\n * Generated by:\n *\n *\t{' '.join(sys.argv)}\n *\n"
            " * Do not edit manually.\n */\n")


def main_inc(what, tables):
    """the initializers of src/seqlz_default_tables_*.inc or
    src/seqlz_lit_sets_*.inc, for the repository's userspace build"""
    text = head().replace("/* SPDX-License-Identifier: GPL-2.0-only OR MIT */",
                          "// SPDX-License-Identifier: GPL-2.0-only OR MIT")
    if what == "--lengths":
        print(text + lengths_initializer(tables), end="")
    else:
        print(text + lit_sets_initializer(tables) + "\n" +
              own_initializer(tables), end="")


def main():
    args = sys.argv[1:]
    what = None
    if args and args[0] in ("--lengths", "--lit-sets"):
        what = args.pop(0)
    if len(args) != 1:
        sys.exit("usage: gen-seqlz-lengths.py <counts file>")
    tables = read_counts(args[0])
    if what:
        return main_inc(what, tables)
    print(head() +
          "#ifndef _LIB_SEQLZ_SEQLZ_LENGTHS_H\n"
          "#define _LIB_SEQLZ_SEQLZ_LENGTHS_H\n\n"
          "/*\n"
          " * The code lengths of seqlz's tables, part of its format: of\n"
          " * the tokens and the length values, of the 8 literal tables, one\n"
          " * chosen per block, the most used first, and of the code of a\n"
          " * block's own literal table. gen_seqlz_tables builds the codes\n"
          " * and decode tables from them.\n"
          " */\n"
          "#include \"seqlz_format.h\"\n\n" +
          lengths_initializer(tables, "static ") + "\n" +
          lit_sets_initializer(tables, "static ") + "\n" +
          own_initializer(tables, "static ") + "\n#endif\n", end="")


if __name__ == "__main__":
    main()
