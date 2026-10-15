#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""seqlz's levels against lz4 and zstd 3 on the Mi 9T: memory against the time per page written, one
panel per core.

    tools/plot-levels.py <tools/zram-phone/run.sh output> <sizes> <out.svg> [<out.png>]

The times are the means over the rounds that tools/zram-phone/run.sh prints, zram in the phone's own
kernel; time per page written is write + 0.34 * cold read, as docs/plan.md §1.1. <sizes> is the output
of quetschn-bench-interleaved --no-timing on the same pages: the zsmalloc cost per page, as % of 4 KiB.
A module's codecs seqlz-<build>-<level> are seqlz-<level> on the chart; with several builds, the last
one in the run's table wins.

Needs matplotlib and the Noto Sans font.
"""

import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

R = 0.34
# the chart's codecs: label, colour, marker, where the label goes in fractions of the axes
CODECS = {
    "lz4": ("lz4", "#7f7f7f", "o", (0.30, 0.95)),
    "zstd": ("zstd 3", "#8e44ad", "D", (0.62, 0.45)),
    "seqlz-1": ("seqlz-1: raw literals", "#1f6fd1", "o", (0.42, 0.82)),
    "seqlz-2": ("seqlz-2: fixed literal tables", "#17a2b8", "v", (0.42, 0.72)),
    "seqlz-3": ("seqlz-3: lazy, the page's own literal table", "#2e9e44", "s", (0.30, 0.30)),
}
CORES = [(7, "A76", 2208), (2, "A55", 1805)]


def name_of(codec):
    m = re.fullmatch(r"seqlz-(?:\w+-)?([0-9])", codec)
    return f"seqlz-{m[1]}" if m else codec


def hull(points):
    """the lower left convex hull, from the fastest to the smallest: the best codec for some exchange rate"""
    pts = sorted(points)
    best = [pts[0]]
    for p in pts[1:]:
        if p[1] >= best[-1][1]:
            continue
        while len(best) >= 2:
            (x1, y1), (x2, y2) = best[-2], best[-1]
            if (y2 - y1) * (p[0] - x1) >= (p[1] - y1) * (x2 - x1):
                best.pop()
            else:
                break
        best.append(p)
    return best


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    run, sizes, outs = sys.argv[1], sys.argv[2], sys.argv[3:]
    size, codec = {}, None
    for line in open(sizes):
        m = re.match(r"codec +(\S+?),", line)
        if m:
            codec = name_of(m[1])
        m = re.match(r"zsmalloc cost .*?([\d.]+) bytes/page", line)
        if m and codec:
            size[codec] = float(m[1]) / 4096 * 100
    times, cpu = {}, None
    for line in open(run):
        m = re.match(r"cpu(\d), mean", line)
        if m:
            cpu = int(m[1])
            continue
        m = re.match(r"(\S+)\s+([\d.]+) \([^)]*\)\s+([\d.]+) \([^)]*\)\s+([\d.]+) \(", line)
        if m and cpu is not None:
            times[(cpu, name_of(m[1]))] = (float(m[2]), float(m[3]), float(m[4]))

    plt.rcParams["font.family"] = ["Noto Sans", "DejaVu Sans"]
    fig, axes = plt.subplots(1, 2, figsize=(13, 5.6))
    for ax, (c, core, mhz) in zip(axes, CORES):
        pts = []
        for codec, (label, color, marker, at) in CODECS.items():
            if (c, codec) not in times or codec not in size:
                continue
            write, _, cold = times[(c, codec)]
            x, y = write + R * cold, size[codec]
            pts.append((x, y))
            ax.scatter([x], [y], color=color, marker=marker, s=70, zorder=3)
            ax.annotate(f"{label}: {x:.1f} µs, {y:.1f}%", (x, y), xytext=at, textcoords="axes fraction",
                        fontsize=8.5, color="#333333", va="center",
                        arrowprops=dict(arrowstyle="-", color="#aaaaaa", lw=0.7))
        h = hull(pts)
        ax.plot([p[0] for p in h], [p[1] for p in h], color="#bbbbbb", lw=1, zorder=1)
        ax.set_xlim(0, max(p[0] for p in pts) * 1.12)
        ax.set_ylim(0, max(p[1] for p in pts) * 1.15)
        ax.set_xlabel(f"time per page written: write + {R} × cold read, µs ({core}, {mhz} MHz)")
        ax.set_ylabel("memory per stored page, % of 4 KiB (zsmalloc)")
        ax.set_title(f"Mi 9T {core}, zram in the phone's kernel")
        ax.grid(color="#eeeeee")
    fig.suptitle("seqlz's levels against lz4 and zstd 3: memory against time, lower left is better")
    fig.text(0.5, 0.005, "Means over the rounds of tools/zram-phone/run.sh with BUS=1; sizes from "
             "quetschn-bench-interleaved on the same pages. Grey line: the codecs with the lowest score for "
             "some exchange rate.", ha="center", fontsize=8, color="#555555")
    fig.tight_layout(rect=(0, 0.03, 1, 0.95))
    for o in outs:
        fig.savefig(o)


if __name__ == "__main__":
    main()
