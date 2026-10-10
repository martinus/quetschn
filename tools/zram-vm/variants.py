#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""variants.py [--control <name>] <run.sh log>...: the codecs of a run.sh log with VARIANTS, each against
its base: the mean over the boots of swap-out, warm and flushed swap-in in us, and the difference to
seqlz for a raw variant, to seqlz-lit for a -lit one, in us and in %. With --control, the name of a
variant built from the same source as the base (default: same), whose difference is noise only: where
a code lands, which moved swap-ins by up to 1.2% with clang. A difference within the control's, or one
whose sign turns between gcc and clang, is that noise too (docs/measuring.md).
"""
import re
import sys

WHAT = ["swap-out, one call per page", "swap-in, warm", "swap-in, flushed"]


def load(path):
    seen = {}
    for line in open(path, errors="replace"):
        m = re.search(r"RESULT (\S+)\s+(.*?)\s*(?:n\s+\d+)?:.* mean (\d+) ns", line)
        if m:
            what = re.sub(r"\s+", " ", m[2])
            if what in WHAT:
                seen.setdefault(m[1], {}).setdefault(what, []).append(int(m[3]) / 1000)
    return seen


def main():
    args = sys.argv[1:]
    control = "same"
    if args[:1] == ["--control"]:
        control, args = args[1], args[2:]
    if not args:
        sys.exit(__doc__)
    for path in args:
        seen = load(path)
        compiler = re.search(r"CONFIG_CC_VERSION_TEXT=\"([^\"]*)\"", open(path, errors="replace").read())
        print(f"{path.split('/')[-1]}: {compiler[1] if compiler else 'compiler not in the log'}")
        boots = {len(v) for d in seen.values() for v in d.values()}
        if len(boots) > 1:
            print(f"  codecs with different numbers of boots, {sorted(boots)}: more codecs than one boot takes?")
        print(f"  {'codec':12s}" + "".join(f" | {w[:24]:>34s}" for w in WHAT))
        for codec, d in seen.items():
            base = "seqlz-lit" if codec.endswith("-lit") else "seqlz"
            row = f"  {codec:12s}"
            for w in WHAT:
                v = d[w]
                mean = sum(v) / len(v)
                cell = f"{mean:6.3f} ({min(v):.2f}-{max(v):.2f})"
                b = seen.get(base, {}).get(w)
                if b and codec not in ("seqlz", "seqlz-lit", "lz4"):
                    bm = sum(b) / len(b)
                    cell += f" {mean - bm:+.3f} {100 * (mean - bm) / bm:+.1f}%"
                row += f" | {cell:>34s}"
            if codec.split("-lit")[0] == control:
                row += "  <- control"
            print(row)
        if not any(c.split("-lit")[0] == control for c in seen):
            print(f"  no control '{control}': without it a difference cannot be told from where the code lands")


main()
