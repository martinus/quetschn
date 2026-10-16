#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""The README's chart: seqlz's three levels against lz4, lzo-rle and zstd 3 in zram, memory against the
time per page written, the mean over several zram dumps.

    ALGOS=lz4,lzo-rle,zstd,seqlz-1,seqlz-2,seqlz-3 BOOTS=3 MODE=swap KARGS=zram.zram_prefetch=8 \\
        tools/zram-vm/run.sh <linux tree> <corpus> >p3.log
    ./build/quetschn-score p3.log >p3.score
    tools/plot-readme.py --title "..." --note "..." --out docs/plots/levels-vm.svg p3.score p5.score ...

The inputs are the output of quetschn-score, one per dump: bytes per page and µs per page written,
write + 0.34 * read as docs/plan.md §1.1, here a swap-out and a cold swap-in. The chart shows the mean
over the dumps of both, memory as % of a 4 KiB page. Needs matplotlib and the Noto Sans font.
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
LEVELS = ["seqlz-1", "seqlz-2", "seqlz-3"]


def parse(path):
    """codec: (bytes per page, µs per page written) of a quetschn-score output"""
    rows = {}
    for line in open(path):
        m = re.match(r"(\S+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s", line)
        if m and m[1] in CODECS:
            rows[m[1]] = (float(m[2]), float(m[6]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--title", required=True)
    ap.add_argument("--note", default="", help="the line below the chart: machine, pages, date")
    ap.add_argument("--out", action="append", required=True)
    ap.add_argument("scores", nargs="+")
    a = ap.parse_args()
    runs = [parse(p) for p in a.scores]
    mean = {}
    for c in CODECS:
        got = [r[c] for r in runs if c in r]
        if len(got) != len(runs):
            raise SystemExit(f"{c} is not in every score")
        mean[c] = (sum(g[1] for g in got) / len(got), sum(g[0] for g in got) / len(got) / 4096 * 100)

    plt.rcParams["font.family"] = "Noto Sans"
    fig, ax = plt.subplots(figsize=(7.2, 4.4), dpi=100)
    ax.plot([mean[c][0] for c in LEVELS], [mean[c][1] for c in LEVELS], color="#3a87c8", lw=1.5, zorder=1)
    for c, (label, color, marker, off, ha) in CODECS.items():
        x, y = mean[c]
        ax.scatter([x], [y], s=72, color=color, marker=marker, edgecolors="white", linewidths=1.5, zorder=3)
        ax.annotate(f"{label}: {y:.1f}%, {x:.1f} µs", (x, y), xytext=off, textcoords="offset points",
                    fontsize=10, color="#222222", va="center", ha=ha)
    xmax = max(x for x, _ in mean.values()) * 1.15
    ymax = max(y for _, y in mean.values()) * 1.25
    ax.set_xlim(0, xmax)
    ax.set_ylim(0, ymax)
    ax.set_xlabel("time per page written, µs: swap-out + 0.34 × swap-in")
    ax.set_ylabel("memory, % of a 4 KiB page")
    ax.set_title(a.title, fontsize=12, loc="left")
    ax.grid(True, color="#e6e6e6", lw=0.8)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.text(0.98, 0.04, "lower left is better", transform=ax.transAxes, ha="right", fontsize=9, color="#666666")
    if a.note:
        fig.text(0.01, 0.01, a.note, fontsize=8, color="#666666", va="bottom")
    fig.tight_layout(rect=(0, 0.07 if a.note else 0, 1, 1))
    for o in a.out:
        fig.savefig(o, facecolor="white", metadata={"Date": None} if o.endswith(".svg") else None)
    for c in CODECS:
        print(f"{c:8} {mean[c][1]:5.1f}% {mean[c][0]:6.2f} µs")


if __name__ == "__main__":
    main()
