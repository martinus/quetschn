#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Generates seqlz's tables, the code lengths that are part of its format, from the counts of the symbols
# on the pages they were trained on. Prints the C file of both tables, or with --lengths or --lit-sets
# only that initializer.
#
#   gen-seqlz-tables.py [--lengths | --lit-sets] <counts file>
#
# The counts file has comment lines starting with '#', then "token", "ll" and "ml", each followed by the
# number of symbols and how often each occurred, and 8 times "lit", each followed by 257 and the number
# of pages that literal table codes, then how often each byte occurred as a literal on them. Numbers may
# go over several lines.
#
# Each table is a length-limited prefix code, built by package-merge, with every count plus 1. Only the
# most frequent tokens get a code; the others are sent as the escape, the last symbol of the token
# table, and the 12 bits of the token.

import sys

TOKEN_BITS = 11     # the longest code of a token
VALUE_BITS = 8      # the longest code of a length value
LIT_BITS = 10       # the longest code of a literal
ESCAPE_BITS = 12    # the bits of a token after the escape
LIT_SETS = 8
TOKENS = 3072


def code_lengths(counts, max_bits):
    """The optimal code lengths of at most max_bits bits, by package-merge (Larmore and Hirschberg).

    The lists of the levels from max_bits up to 1 each hold the symbols and the pairs of the level below,
    sorted by weight, a symbol before a pair of the same weight. A symbol's length is how often it is
    among the 2n - 2 lightest items of the top list, counted through the pairs."""
    n = len(counts)
    order = sorted(range(n), key=lambda s: counts[s])
    leaves = [(counts[s], s, None) for s in order]
    levels = [leaves]
    for _ in range(1, max_bits):
        below = levels[-1]
        pairs = [(below[i][0] + below[i + 1][0], i, i + 1) for i in range(0, len(below) - 1, 2)]
        merged, a, b = [], 0, 0
        while a < len(leaves) or b < len(pairs):
            if b == len(pairs) or (a < len(leaves) and not pairs[b][0] < leaves[a][0]):
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


def token_lengths(counts, page_bits):
    """Codes for the k most frequent tokens and the escape, with k in steps of 64 as it gives the fewest
    bits. A token and its offset take at most 31 bits, also when escaped, which limits the escape's
    code."""
    max_escape = 31 - page_bits - ESCAPE_BITS
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


def read_counts(path):
    words = []
    with open(path) as f:
        for line in f:
            if not line.startswith("#"):
                words += line.split()
    tables, pos = [], 0
    for name in ["token", "ll", "ml"] + ["lit"] * LIT_SETS:
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
    if len(tables[0]) != TOKENS or len(tables[1]) != len(tables[2]) or any(len(t) != 257 for t in tables[3:]):
        sys.exit(f"{path}: the tables have the wrong sizes")
    return tables


def initializer(name, lists, indent):
    """lists as C initializers of 16 numbers per line"""
    out = []
    for label, values in lists:
        rows = [", ".join(str(v) for v in values[i:i + 16]) for i in range(0, len(values), 16)]
        inner = indent + "\t"
        out.append(f"{indent}{label}{{\n" + ",\n".join(inner + r for r in rows) + f",\n{indent}}},")
    return f"{name} = {{\n" + "\n".join(out) + "\n};\n"


def lengths_initializer(tables):
    plus_one = [[c + 1.0 for c in t] for t in tables[:3]]
    # a length value's symbols: 16 of their own, then one per highest bit from 4 up to the page's bits
    page_bits = len(tables[1]) - 13
    return initializer("const struct seqlz_lengths seqlz_default_own", [
        (".token = ", token_lengths(plus_one[0], page_bits)),
        (".ll = ", code_lengths(plus_one[1], VALUE_BITS)),
        (".ml = ", code_lengths(plus_one[2], VALUE_BITS)),
    ], "\t")


def lit_sets_initializer(tables):
    # every byte gets a code, the encoder needs one; the first number of a literal table's counts is its
    # pages, not a byte
    sets = [code_lengths([c + 1.0 for c in t[1:]], LIT_BITS) for t in tables[3:]]
    return initializer("const u8 seqlz_lit_sets[SEQLZ_LIT_SETS][256]", [("", s) for s in sets], "\t")


def main():
    args = sys.argv[1:]
    what = args.pop(0) if args and args[0] in ("--lengths", "--lit-sets") else None
    if len(args) != 1:
        sys.exit("usage: gen-seqlz-tables.py [--lengths | --lit-sets] <counts file>")
    tables = read_counts(args[0])
    command = " ".join(sys.argv)
    head = (f"// SPDX-License-Identifier: GPL-2.0-only OR MIT\n"
            f"/*\n * Generated by:\n *\n *\t{command}\n *\n * Do not edit manually.\n */\n")
    if what == "--lengths":
        print(head + lengths_initializer(tables), end="")
    elif what == "--lit-sets":
        print(head + lit_sets_initializer(tables), end="")
    else:
        print(head + "/*\n * seqlz's tables, part of its format: the code lengths of the tokens and\n"
              " * the length values, and the 8 literal tables, one chosen per page, the most\n"
              " * used first.\n */\n"
              "#include <kunit/visibility.h>\n#include <linux/export.h>\n\n#include \"seqlz.h\"\n\n" +
              lengths_initializer(tables) + "EXPORT_SYMBOL_IF_KUNIT(seqlz_default_own);\n\n" +
              lit_sets_initializer(tables) + "EXPORT_SYMBOL_IF_KUNIT(seqlz_lit_sets);\n", end="")


if __name__ == "__main__":
    main()
