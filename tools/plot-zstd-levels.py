#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""seqlz-fast-lit against zstd at every level: memory against time, from logs of tools/zram-vm/run.sh with
MODE=swap and zstd's levels as zram's algorithm_params, e.g. ALGOS=lz4,seqlz-lit,zstd:-1,zstd:3.

One row per corpus, from one or more logs of it; a codec in several logs gets the mean of them. Three
panels: the swap-out with one page per call, the swap-in with the compressed data cold, and the time per
page written of the score, docs/plan.md §1.1, all means per page. zstd's levels are one line, in the
order of the levels. The numbers come from quetschn-score, so they are the score's: the corpus's pages
without the same-filled ones the swap mode adds. Lower left is better.

    ALGOS=lz4,seqlz-lit,zstd:-5,zstd:-1,zstd:1,zstd:3 MODE=swap KARGS=zram.zram_prefetch=8 \\
        tools/zram-vm/run.sh <linux tree> <corpus> >a.log
    tools/plot-zstd-levels.py --row "Desktop=a.log,b.log" --out zstd-levels.svg

Needs matplotlib, the Noto Sans font, and a build with quetschn-score.
"""

import argparse
import importlib.util
import math
import subprocess
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("plot_codecs", HERE / "plot-codecs.py")
plot_codecs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot_codecs)

ZSTD = plot_codecs.STYLE["zstd"][0]
# memory in % of the page: where the labels of levels close to another one go, in a row
LABEL_ROW = 14.0
OTHERS = {"lz4": "lz4", "lzo-rle": "lzo-rle", "seqlz": "seqlz-fast", "seqlz-lit": "seqlz-fast-lit"}


def score_rows(score, log, r):
    """bytes per page, write and read in us, per zram device of a log, from quetschn-score"""
    out = subprocess.run([score, "--reads-per-write", str(r), log], capture_output=True, text=True, check=True).stdout
    rows = {}
    for line in out.splitlines()[3:]:
        f = line.split()
        if len(f) != 6:
            break
        rows[f[0]] = (float(f[1]), float(f[2]), float(f[3]))
    return rows


def mean_rows(score, logs, r):
    sums = {}
    for log in logs:
        for name, v in score_rows(score, log, r).items():
            sums.setdefault(name, []).append(v)
    return {n: tuple(sum(x) / len(vs) for x in zip(*vs)) for n, vs in sums.items()}


def spread(xs, gap, lo, hi):
    """Positions for labels of points at xs: in the same order, at least gap apart, within [lo, hi], each as
    close to its point as that allows."""
    order = sorted(range(len(xs)), key=lambda k: xs[k])
    pos = [xs[k] for k in order]
    for k in range(1, len(pos)):
        pos[k] = max(pos[k], pos[k - 1] + gap)
    pos[-1] = min(pos[-1], hi)
    for k in range(len(pos) - 2, -1, -1):
        pos[k] = min(pos[k], pos[k + 1] - gap)
    shift = max(0.0, lo - pos[0])
    out = [0.0] * len(xs)
    for k, i in enumerate(order):
        out[i] = pos[k] + shift
    return out


def level(name):
    return int(name.split(":", 1)[1]) if name.startswith("zstd:") else 3


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--row", action="append", required=True, metavar="TITLE=LOG[,LOG...]",
                    help="a row: its title and the logs of one corpus")
    ap.add_argument("--out", action="append", required=True, help="output file, .png or .svg, can be given more than once")
    ap.add_argument("--score", default=str(HERE.parent / "build" / "quetschn-score"), help="the quetschn-score binary")
    ap.add_argument("--reads-per-write", type=float, default=0.34, help="r of the score, default 0.34 (docs/plan.md §1.1)")
    ap.add_argument("--max-us", type=float, default=60,
                    help="where the time axes of swap-out and score end; slower levels are arrows at the edge")
    ap.add_argument("--clock", default="4.5 GHz", help="the fixed clock of the CPU the VM ran on, for the footer")
    ap.add_argument("--source", default=None, help="what was measured, for the footer; default git describe")
    args = ap.parse_args()

    r = args.reads_per_write
    rows = []
    for row in args.row:
        title, sep, logs = row.partition("=")
        if not sep:
            raise SystemExit(f"--row {row}: TITLE=LOG[,LOG...]")
        rows.append((title, mean_rows(args.score, logs.split(","), r)))

    panels = [("Swap-out, one page per call", lambda w, rd: w), ("Swap-in, compressed data cold", lambda w, rd: rd),
              (f"Time per page written: swap-out + {r:g} × swap-in", lambda w, rd: w + r * rd)]
    plt.rcParams.update({"font.family": "Noto Sans", "font.size": 10})
    head, foot, row_h = 0.8, 0.9, 4.6
    height = head + foot + row_h * len(rows)
    fig, axes = plt.subplots(len(rows), 3, figsize=(17, height), squeeze=False)
    top_mem = max(b for _, codecs in rows for b, _, _ in codecs.values()) / 4096 * 100
    ymax = 5 * math.ceil(top_mem * 1.1 / 5)

    for i, (title, codecs) in enumerate(rows):
        zstd = sorted((n for n in codecs if n.startswith("zstd")), key=level)
        for j, (what, time) in enumerate(panels):
            ax = axes[i][j]
            pts = {n: (time(w, rd), b / 4096 * 100) for n, (b, w, rd) in codecs.items()}
            # the swap-in panel shows all, the others end at --max-us: the slow levels would leave the rest in a
            # corner
            limit = None if j == 1 else args.max_us
            shown = [n for n in zstd if limit is None or pts[n][0] <= limit]
            right = limit or max(t for t, _ in pts.values()) * 1.08
            right = right if limit else math.ceil(right)
            ax.plot([pts[n][0] for n in shown], [pts[n][1] for n in shown], color=ZSTD, lw=1.6, marker="D", ms=6,
                    mec="white", mew=0.6, zorder=3, label="zstd, by level")
            # the levels that stand alone get their label next to them; the ones close to another one get
            # theirs in a row in the empty part below, apart, with a thin line to the point
            def crowded(n):
                return any(abs(pts[n][0] - pts[m][0]) / right < 0.06 and abs(pts[n][1] - pts[m][1]) < 3.0
                           for m in shown if m != n)

            low = [n for n in shown if crowded(n)]
            for n in shown:
                if n not in low:
                    ax.annotate(str(level(n)), pts[n], textcoords="offset points", xytext=(6, 4), fontsize=10,
                                color=ZSTD)
            xs = spread([pts[n][0] for n in low], right * 0.055, right * 0.03, right * 0.97)
            for n, x in zip(low, xs):
                ax.annotate(str(level(n)), pts[n], xytext=(x, LABEL_ROW), textcoords="data", ha="center", va="top",
                            fontsize=10, color=ZSTD,
                            arrowprops=dict(arrowstyle="-", color=ZSTD, lw=0.6, alpha=0.6, shrinkA=1, shrinkB=4))
            if low:
                ax.annotate("zstd level", ((min(xs) + max(xs)) / 2, LABEL_ROW - 3.2), ha="center", va="top",
                            fontsize=8.5, color=ZSTD)
            beyond = [n for n in zstd if n not in shown]
            if beyond:
                ax.plot([pts[shown[-1]][0], limit], [pts[shown[-1]][1], pts[beyond[0]][1]], color=ZSTD, lw=1.6, ls=":",
                        zorder=3)
                ax.plot([limit] * len(beyond), [pts[n][1] for n in beyond], marker=">", ms=7, color=ZSTD, ls="none",
                        zorder=3, clip_on=False)
                text = "\n".join(f"level {level(n)}: {pts[n][0]:.0f} µs" for n in beyond)
                ax.annotate("off the axis:\n" + text, (limit, pts[beyond[0]][1]), textcoords="offset points",
                            xytext=(-8, 18), ha="right", va="bottom", fontsize=8.5, color=ZSTD)
            for n, label in OTHERS.items():
                if n in pts:
                    color, marker = plot_codecs.STYLE[label]
                    main_codec = label in plot_codecs.MAIN
                    ax.plot(*pts[n], marker=marker, ms=11 if main_codec else 8, color=color, mec="white", mew=0.8,
                            ls="none", zorder=4, label=label)
                    ax.annotate(label, pts[n], textcoords="offset points", xytext=(-4, -24), fontsize=9, color=color,
                                fontweight="bold" if main_codec else "normal")
            if j == 2:
                h = [p for p in plot_codecs.hull([(t, m, n) for n, (t, m) in pts.items()]) if p[0] <= limit]
                ax.plot([p[0] for p in h], [p[1] for p in h], color="#aaaaaa", lw=1.2, ls="--", zorder=1,
                        label="best for some exchange rate")
            ax.set_xlim(0, right)
            ax.set_ylim(0, ymax)
            ax.set_yticks(range(0, ymax + 1, 5))
            ax.set_yticklabels([f"{v}%" for v in range(0, ymax + 1, 5)])
            ax.set_xlabel(f"{what.lower()}: mean per page, µs")
            if i == 0:
                ax.set_title(what, fontsize=12, fontweight="bold", loc="left")
            if j == 0:
                ax.set_ylabel(f"{title}\n\nmemory used by zsmalloc,\nin % of the uncompressed pages")
            ax.grid(True, color="#e6e6e6", lw=0.8)
            ax.set_axisbelow(True)
            for side in ("top", "right"):
                ax.spines[side].set_visible(False)

    handles, labels = axes[0][2].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=len(labels), frameon=False, bbox_to_anchor=(0.5, 1 - 0.42 / height))
    fig.suptitle("seqlz-fast-lit against zstd at each level, in zram as swap: memory against time, lower left is better",
                 fontsize=14, fontweight="bold", y=1 - 0.05 / height)
    fig.text(
        0.01,
        0.01,
        f"Linux kernel in a VM, zram as the only swap, zsmalloc, one device per codec ({args.source or plot_codecs.git_source()}). "
        f"{plot_codecs.cpu_model()}, one CPU at a fixed {args.clock}. The levels are zram's algorithm_params, the numbers next to\n"
        f"zstd's points; levels slower than {args.max_us:g} µs are arrows at the edge of the swap-out and score panels. Per page the median of 3 runs, then the mean over the pages; a codec in several boots gets the mean of them. "
        "Swap-in cold: the compressed data flushed from the cache before the fault.\n"
        f"seqlz-fast-lit prefetches the compressed data in its zram backend. Time per page written: the score of docs/plan.md §1.1, "
        f"{r:g} reads per write, with the time of the whole fault, which includes the kernel's part, the same for every codec.",
        fontsize=8.5,
        color="#555555",
        va="bottom",
    )
    fig.tight_layout(rect=(0, foot / height, 1, 1 - head / height))
    for out in args.out:
        fig.savefig(out, dpi=150)


if __name__ == "__main__":
    main()
