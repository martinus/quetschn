#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""The codecs on the cores of a phone: memory against the time to write a page and to read it cold, one
panel per core and per direction, a row per core with its write and read side by side, from the log of
tools/phone-cores.sh.

    tools/phone-cores.sh <android build dir> <corpus base> "7 4 0" >cores.log
    ONCE=1 tools/phone-cores.sh <android build dir> <corpus base without same-filled pages> "7 4 0" >once.log
    tools/plot-cores.py --title "Pixel 9a (Tensor G4)" --pages "19 775 pages of a Mi 9T zram session" \\
        --cpu "7=Cortex-X4, cpu7" --cpu "4=Cortex-A720, cpu4" --cpu "0=Cortex-A520, cpu0" \\
        --sizes cores.log --out docs/plots/pixel-cores.svg once.log

A log of ONCE=1, pages seen once, has no sizes: --sizes takes them from a log of the harness, and the
read is the --flush pass. The panels' clocks are those of the --sizes log, ONCE=1 does not print them.

Each point is the mean over the rounds of the codec's mean time per page; the clock each panel names is
the highest clock its runs ended at. The dashed line joins the codecs with the lowest memory + lambda *
time for some lambda, for that direction alone. Needs matplotlib and the Noto Sans font.
"""

import argparse
import importlib.util
import re
import statistics as st
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

spec = importlib.util.spec_from_file_location("plot_codecs", Path(__file__).resolve().parent / "plot-codecs.py")
pc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc)

CODECS = ["lz4", "lzo-rle", "zstd", "seqlz-1", "seqlz-2", "seqlz-3"]
LABEL = {"lz4": "lz4", "lzo-rle": "lzo-rle", "zstd": "zstd 3", "seqlz-1": "seqlz-1", "seqlz-2": "seqlz-2", "seqlz-3": "seqlz-3"}
# a marker per codec, so that the legend alone tells them apart; the colours of plot-codecs.py
MARK = {"lz4": "o", "lzo-rle": "s", "zstd": "D", "seqlz-1": "p", "seqlz-2": "v", "seqlz-3": "^"}
DIRECTIONS = [("compress", "write", "write: compress, µs per page"),
              ("decompress cold", "read", "read: decompress cold, µs per page")]


def parse(path):
    """(cpu, codec): the runs, each with the bytes per page, the mean times in µs and the end clock"""
    runs, cur, passes = {}, None, 0
    for line in open(path):
        m = re.match(r"== cpu(\d+) round\d+ (\S+)", line)
        if m:
            cur, passes = (m[1], m[2]), 0
            runs.setdefault(cur, []).append({})
            continue
        # ONCE=1: --compress, then --flush, then --cold, each a line with the mean
        m = re.match(r"\S+: (compress|cold decode) per page.* mean (\d+) ns", line)
        if m and cur:
            key = "compress" if m[1] == "compress" else ("decompress cold" if passes == 1 else "after 2 MiB")
            runs[cur][-1][key] = int(m[2]) / 1000
            passes += 1
            continue
        m = re.match(r"STATE end cpu\d+=(\d+)", line)
        if m and cur:
            runs[cur][-1]["mhz"] = int(m[1]) // 1000
        m = re.match(r"zsmalloc cost .*?([\d.]+) bytes/page", line)
        if m and cur:
            runs[cur][-1]["b"] = float(m[1])
        m = re.match(r"(compress|decompress cold)\s+(.*)", line)
        if m and cur:
            runs[cur][-1][m[1]] = float(m[2].split()[-1]) / 1000
    return runs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--title", required=True, help="the phone, for the chart's title")
    ap.add_argument("--pages", required=True, help="what the pages are, for the footer")
    ap.add_argument("--date", default="", help="when it was measured, for the footer")
    ap.add_argument("--cpu", action="append", required=True, metavar="N=NAME", help="a row: the CPU and its name")
    ap.add_argument("--sizes", help="a harness log of the same pages, for a log of ONCE=1: the sizes and the clocks")
    ap.add_argument("--out", action="append", required=True)
    ap.add_argument("log")
    a = ap.parse_args()
    runs = parse(a.log)
    once = a.sizes is not None
    if once:
        sizes = parse(a.sizes)
        for k, v in runs.items():
            for x in v:
                x["b"] = sizes[k][0]["b"]
                x["mhz"] = max(y.get("mhz", 0) for y in sizes[k])
    cpus = [c.split("=", 1) for c in a.cpu]
    plt.rcParams["font.family"] = ["Noto Sans", "DejaVu Sans"]
    plt.rcParams["xtick.labelsize"] = 13
    plt.rcParams["ytick.labelsize"] = 13
    # a row per core, write and read side by side; the height in inches, so that the title and the
    # footer keep their room whatever the number of rows
    height = 5.6 * len(cpus) + 2.2
    fig, axes = plt.subplots(len(cpus), len(DIRECTIONS), figsize=(16, height), dpi=120, sharey=True, squeeze=False)
    for col, (key, short, xlabel) in enumerate(DIRECTIONS):
        for row, (cpu, title) in enumerate(cpus):
            mhz = max(x.get("mhz", 0) for c in CODECS for x in runs[(cpu, c)])
            ax = axes[row][col]
            pts = []
            for c in CODECS:
                rr = runs[(cpu, c)]
                t = st.mean(x[key] for x in rr)
                y = rr[0]['b'] / 4096 * 100
                pts.append((t, y, c))
                ax.scatter([t], [y], s=150, color=pc.STYLE[c][0], marker=MARK[c], edgecolors="white", linewidths=1.2,
                           zorder=3, label=f"{LABEL[c]}: {t:.2f} µs, {y:.1f}%")
            h = pc.hull(pts)
            ax.plot([p[0] for p in h], [p[1] for p in h], color="#999999", lw=1.5, zorder=1, ls="--")
            ax.legend(loc="lower right", ncol=2, fontsize=12, frameon=True, framealpha=0.95, edgecolor="#dddddd",
                      handletextpad=0.3, columnspacing=1.2, borderpad=0.6, labelspacing=0.5)
            ax.set_xlim(0, max(p[0] for p in pts) * 1.06)
            ax.set_ylim(0, 36)
            ax.set_title(f"{title}, {mhz} MHz: {short}", fontsize=15, fontweight="bold", pad=8)
            ax.set_xlabel(xlabel, fontsize=13)
            ax.grid(color="#eeeeee")
            for s in ("top", "right"): ax.spines[s].set_visible(False)
    for row in range(len(cpus)):
        axes[row][0].set_ylabel("memory per page, % of 4 KiB", fontsize=14)
    fig.suptitle(f"{a.title}: seqlz's levels against lz4, lzo-rle and zstd 3, writes and reads, lower left is better",
                 fontsize=17, y=1 - 0.3 / height)
    fig.text(0.5, 0.01, "Userspace harness, each codec alone in its own process pinned to one core"
             f"{', each page timed once per pass over all pages, the median of 3 passes' if once else ''}, the mean over the rounds.\n"
             f"{a.pages}{', ' + a.date if a.date else ''}. Cold: the compressed page flushed from the caches before the decode. "
             "Each panel has its own time scale.\n"
             "Clocks not fixed (no root); each panel names the clock its runs ended at. "
             "Dashed: the codecs with the lowest memory + lambda × time for some lambda.",
             ha="center", va="bottom", fontsize=11, color="#444444", linespacing=1.35)
    fig.subplots_adjust(left=0.06, right=0.99, bottom=1.45 / height, top=1 - 1.05 / height, wspace=0.08,
                        hspace=0.42)
    for o in a.out:
        fig.savefig(o, facecolor="white", metadata={"Date": None} if o.endswith(".svg") else None)


if __name__ == "__main__":
    main()
