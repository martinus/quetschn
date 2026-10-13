#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""attribute.py <base> <map1> <map2>: gives the pages of a corpus from quetschn-import-raw their process
and mapping. <base>.tsv has the page's offset in the zram dump as its address; swap offset n is page n
of the dump. A slot counts only where swapmap saw the same owners before and after the dd; others keep
pid 0. A slot shared by several processes, e.g. zygote and the apps forked from it, goes to the one
that is not zygote, by name the smallest. Rewrites <base>.tsv in place and prints the pages by process."""
import collections
import sys


def load(path):
    owners = collections.defaultdict(set)
    with open(path, errors="replace") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) >= 5:
                owners[int(p[0])].add((p[2], p[1], p[3], p[4]))
    return owners


def main():
    base, m1, m2 = sys.argv[1:4]
    a, b = load(m1), load(m2)
    rows, by = [], collections.Counter()
    with open(base + ".tsv") as f:
        header = [next(f), next(f)]
        for line in f:
            p = line.rstrip("\n").split("\t")
            slot = int(p[3], 16) // 4096
            o = a.get(slot)
            if o and o == b.get(slot):
                name, pid, addr, mapping = min(o, key=lambda t: (t[0].startswith("zygote"), t))
                p[1], p[2], p[3], p[8] = pid, name, addr, mapping
            else:
                p[2] = "?"
            by[p[2]] += 1
            rows.append("\t".join(p) + "\n")
    with open(base + ".tsv", "w") as f:
        f.writelines(header + rows)
    print(f"{len(rows)} pages, {len(rows) - by['?']} with a process, {len(by) - 1} processes")
    for name, n in by.most_common(25):
        print(f"{n:8} {name}")


main()
