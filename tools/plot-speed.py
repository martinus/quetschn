#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""The chart of docs/seqlz.md: seqlz-fast-lit against zram's codecs, in a hot loop and in the kernel.

Four panels: the cycles per page of a hot loop, from perf, and the write, cold read and memory of the
kernel VM of tools/zram-vm/run.sh, one log per zram dump. The hot loop is a TSV with a line per codec:
the name, compress cycles, decode cycles and decode instructions per page. Measured with the benchmark,
the counts of a run with 30 loops minus one with 10, divided by 20 loops and the pages, e.g.

    perf stat -x, -e cycles:u -- quetschn-bench-interleaved --codecs lz4 --corpus <sample> \\
        --decode-loop 30 --cpu 2 [--compress]

    tools/plot-speed.py --hot hot.tsv --run "23rd Sep=first.log" --run "24th Sep=second.log" \\
        --out docs/plots/seqlz-speed.svg

Needs matplotlib and the Noto Sans font.
"""

import argparse
import importlib.util
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("plot_codecs", here / "plot-codecs.py")
plot_codecs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot_codecs)

CODECS = ["lz4", "lzo-rle", "zstd", "seqlz-fast-lit"]
LABEL = {"zstd": "zstd 3"}


def lighter(color, f=0.55):
    rgb = [int(color[i : i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{int(c + (255 - c) * f):02x}" for c in rgb)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--hot", required=True, help="the hot loop, TSV: codec, compress cycles, decode cycles, decode instructions")
    ap.add_argument("--hot-title", default="Hot loop, 23rd Sep", help="the hot loop panel's title")
    ap.add_argument("--run", action="append", required=True, metavar="TITLE=LOG", help="two: a dump's title and its VM log")
    ap.add_argument("--out", action="append", required=True, help="output file, .png or .svg, can be given more than once")
    ap.add_argument("--clock", default="4.5 GHz", help="the fixed clock, for the footer")
    ap.add_argument("--source", default=None, help="what was measured, for the footer; default git describe")
    args = ap.parse_args()

    hot = {}
    for line in Path(args.hot).read_text().splitlines():
        f = line.split()
        if f and not f[0].startswith("#"):
            hot[plot_codecs.NAMES.get(f[0], f[0])] = tuple(float(x) for x in f[1:4])
    runs = []
    for run in args.run:
        title, sep, path = run.partition("=")
        if not sep:
            raise SystemExit(f"--run {run}: TITLE=LOG")
        runs.append((title, plot_codecs.parse(path)))
    if len(runs) != 2:
        raise SystemExit("two --run, one per dump")

    plt.rcParams.update({"font.family": "Noto Sans", "font.size": 10})
    fig, axes = plt.subplots(1, 4, figsize=(19, 4.9))
    ys = {n: len(CODECS) - 1 - i for i, n in enumerate(CODECS)}
    color = {n: plot_codecs.STYLE[n][0] for n in CODECS}

    ax = axes[0]
    for n in CODECS:
        c, d, ins = hot[n]
        y = ys[n]
        ax.barh(y + 0.18, c / 1000, height=0.34, color=color[n])
        ax.barh(y - 0.18, d / 1000, height=0.34, color=lighter(color[n]))
        ax.text(c / 1000 + 1, y + 0.18, f"{c / 1000:.1f}k compress", va="center", fontsize=8.5)
        ax.text(d / 1000 + 1, y - 0.18, f"{d / 1000:.1f}k decode, {ins / 1000:.1f}k instructions", va="center", fontsize=8.5)
    ax.set_xlim(0, 80)
    ax.set_xlabel("thousand cycles per page, page in L3")
    ax.set_title(args.hot_title, loc="left", fontweight="bold")

    for ax, key, title, xmax in [(axes[1], "write", "Kernel: write", 26), (axes[2], "cold", "Kernel: cold read", 11)]:
        for n in CODECS:
            means = []
            for k, (_, codecs) in enumerate(runs):
                t = codecs[n][key]
                y = ys[n] + (0.15 if k == 0 else -0.15)
                ax.plot([t["mean"] / 1000, t["p99"] / 1000], [y, y], color=color[n], lw=1.8)
                ax.plot(t["mean"] / 1000, y, "o", color=color[n], ms=7)
                ax.plot(t["p99"] / 1000, y, "o", mfc="white", mec=color[n], mew=1.6, ms=7)
                means.append(t["mean"] / 1000)
            p99 = max(codecs[n][key]["p99"] for _, codecs in runs) / 1000
            ax.text(p99 + 0.03 * xmax, ys[n], f"{means[0]:.2f} / {means[1]:.2f}", va="center", fontsize=8.5)
        ax.set_xlim(0, xmax)
        ax.set_xlabel(f"µs per page: filled the mean, open p99\nupper {runs[0][0]}, lower {runs[1][0]}; the numbers: means")
        ax.set_title(title, loc="left", fontweight="bold")

    ax = axes[3]
    for n in CODECS:
        for k, (_, codecs) in enumerate(runs):
            c = codecs[n]
            mem = c["mem"] / (c["orig"] / 4096)
            y = ys[n] + (0.18 if k == 0 else -0.18)
            ax.barh(y, mem, height=0.34, color=color[n] if k == 0 else lighter(color[n]))
            ax.text(mem + 25, y, f"{mem:.0f}", va="center", fontsize=8.5)
    ax.set_xlim(0, 2000)
    ax.set_xlabel(f"zsmalloc bytes per stored page\nupper {runs[0][0]}, lower {runs[1][0]}")
    ax.set_title("Kernel: memory", loc="left", fontweight="bold")

    for ax in axes:
        ax.set_yticks([ys[n] for n in CODECS], [LABEL.get(n, n) for n in CODECS])
        ax.set_ylim(-0.6, len(CODECS) - 0.4)
        ax.grid(axis="x", color="#e5e5e5")
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
    fig.suptitle("seqlz-fast-lit against zram's codecs: close to lz4's time, close to zstd's memory", fontsize=13,
                 fontweight="bold")
    fig.text(
        0.01,
        0.01,
        f"Hot loop: perf over 2000 pages of the first dump, the median of 5 processes, {plot_codecs.cpu_model()} at a fixed "
        f"{args.clock}; darker bar compress, lighter decode.\nKernel: tools/zram-vm/run.sh, 20 000 pages per dump, one boot "
        f"per dump with all codecs, {args.source or plot_codecs.git_source()}; cold read: compressed data flushed and another "
        "page read first, the seqlz backend prefetches the compressed data.",
        fontsize=8,
        color="#555555",
        va="bottom",
    )
    fig.tight_layout(rect=(0, 0.1, 1, 0.94))
    for out in args.out:
        fig.savefig(out)


if __name__ == "__main__":
    main()
