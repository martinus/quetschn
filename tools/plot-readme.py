#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""The README's chart: seqlz's three levels against lz4, lzo-rle and zstd 3 in zram, memory against the
time per page written, the mean over several zram dumps.

    ALGOS=lz4,lzo-rle,zstd,seqlz-1,seqlz-2,seqlz-3 BOOTS=3 MODE=swap KARGS=zram.zram_prefetch=8 \\
        tools/zram-vm/run.sh <linux tree> <corpus> >p3.log
    ./build/quetschn-score p3.log >p3.score
    tools/plot-readme.py --title "..." --note "..." --out docs/plots/levels-vm.svg p3.score p5.score ...

The inputs are the output of quetschn-score, one per dump: bytes per page and µs per page written,
write + r * read as docs/plan.md §1.1, here a swap-out and a cold swap-in, r as quetschn-score's header
says, all inputs with the same r. The chart shows the mean
over the dumps of both, memory as % of a 4 KiB page, and the hull of the means: the codecs that have
the lowest bytes + lambda * µs for some lambda, from the fastest to the smallest, each segment with its
lambda in bytes per µs. Needs matplotlib and the Noto Sans font.
"""

import argparse
import re

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# name in the logs: label, colour, marker, where the label goes (offset in points, alignment)
CODECS = {
    "lz4": ("lz4", "#7f7f7f", "o", (-10, 7), "right"),
    "lzo-rle": ("lzo-rle", "#2b2b2b", "s", (-10, -7), "right"),
    "zstd": ("zstd 3", "#8e44ad", "D", (0, 16), "center"),
    "seqlz-1": ("seqlz-1", "#9ecae9", "o", (10, 5), "left"),
    "seqlz-2": ("seqlz-2", "#3a87c8", "v", (-10, -10), "right"),
    "seqlz-3": ("seqlz-3", "#0b4f8a", "^", (0, -18), "center"),
}


def parse(path):
    """codec: (bytes per page, µs per page written) of a quetschn-score output, and its r"""
    rows, r = {}, None
    for line in open(path):
        m = re.match(r"time per page written = write \+ ([\d.]+) \* cold read", line)
        if m:
            r = float(m[1])
        m = re.match(r"(\S+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s", line)
        if m and m[1] in CODECS:
            rows[m[1]] = (float(m[2]), float(m[6]))
    if r is None:
        raise SystemExit(f"{path}: no line 'time per page written = write + r * cold read'")
    return rows, r


def hull(points):
    """the lower left convex hull of (µs, %) points, from the fastest to the smallest"""
    pts = sorted(points)
    lower = []
    for p in pts:
        while len(lower) >= 2 and (
            (lower[-1][0] - lower[-2][0]) * (p[1] - lower[-2][1]) - (lower[-1][1] - lower[-2][1]) * (p[0] - lower[-2][0])
        ) <= 0:
            lower.pop()
        lower.append(p)
    # up to the point with the least memory: past it a slower point is never better
    end = min(range(len(lower)), key=lambda i: (lower[i][1], lower[i][0]))
    return lower[: end + 1]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--title", required=True)
    ap.add_argument("--note", default="", help="the line below the chart: machine, pages, date")
    ap.add_argument("--out", action="append", required=True)
    ap.add_argument("scores", nargs="+")
    a = ap.parse_args()
    parsed = [parse(p) for p in a.scores]
    runs, rs = [p[0] for p in parsed], {p[1] for p in parsed}
    if len(rs) != 1:
        raise SystemExit(f"the scores have different r: {sorted(rs)}")
    r = rs.pop()
    mean = {}
    for c in CODECS:
        got = [r[c] for r in runs if c in r]
        if len(got) != len(runs):
            raise SystemExit(f"{c} is not in every score")
        mean[c] = (sum(g[1] for g in got) / len(got), sum(g[0] for g in got) / len(got) / 4096 * 100)

    plt.rcParams["font.family"] = "Noto Sans"
    fig, ax = plt.subplots(figsize=(7.2, 4.4), dpi=100)
    h = hull(list(mean.values()))
    ax.plot([p[0] for p in h], [p[1] for p in h], color="#9a9a9a", lw=1.2, ls="--", zorder=1)
    name = {v: CODECS[c][0] for c, v in mean.items()}
    steps = [f"{name[p]} to {name[q]} {(p[1] - q[1]) / 100 * 4096 / (q[0] - p[0]):.0f}" for p, q in zip(h, h[1:])]
    for c, (label, color, marker, off, ha) in CODECS.items():
        x, y = mean[c]
        ax.scatter([x], [y], s=72, color=color, marker=marker, edgecolors="white", linewidths=1.5, zorder=3)
        ax.annotate(f"{label}: {y:.1f}%, {x:.1f} µs", (x, y), xytext=off, textcoords="offset points",
                    fontsize=10, color="#222222", va="center", ha=ha)
    xmax = max(x for x, _ in mean.values()) * 1.15
    ymax = max(y for _, y in mean.values()) * 1.25
    ax.set_xlim(0, xmax)
    ax.set_ylim(0, ymax)
    ax.set_xlabel(f"time per page written, µs: swap-out + {r:g} × swap-in")
    ax.set_ylabel("memory, % of a 4 KiB page")
    ax.set_title(a.title, fontsize=12, loc="left")
    ax.grid(True, color="#e6e6e6", lw=0.8)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.text(0.98, 0.04, "lower left is better\ndashed: the hull, bytes per µs from one to the next:\n" + ", ".join(steps),
            transform=ax.transAxes, ha="right", fontsize=9, color="#666666", linespacing=1.4)
    if a.note:
        fig.text(0.01, 0.01, a.note, fontsize=8, color="#666666", va="bottom")
    fig.tight_layout(rect=(0, 0.07 if a.note else 0, 1, 1))
    for o in a.out:
        fig.savefig(o, facecolor="white", metadata={"Date": None} if o.endswith(".svg") else None)
    for c in CODECS:
        print(f"{c:8} {mean[c][1]:5.1f}% {mean[c][0]:6.2f} µs" + ("  hull" if mean[c] in h else ""))


if __name__ == "__main__":
    main()
