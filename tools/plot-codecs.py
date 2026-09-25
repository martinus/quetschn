#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""The chart of the zram codecs: memory against latency, from the logs of tools/zram-vm/run.sh.

One row per log, e.g. one per zram dump, and four panels per row: cold reads, warm reads and writes
(p50, mean and p99 per page), and the score of PLAN.md §1.1, memory against the time per page written,
with the codecs that have the lowest score for some exchange rate. The logs have only sizes and
timings, no page content.

    ALGOS=lz4,lzo-rle,zstd,seqlz-lit \\
        tools/zram-vm/run.sh <linux tree> <corpus> >first.log
    tools/plot-codecs.py --run "zram dump of 23 Sep=first.log" --run "zram dump of 24 Sep=second.log" \\
        --out codecs.png --out codecs.svg

bytelz and seqlz (seqlz-fast) are not in the default run: on x86-64 no exchange rate makes them the
best choice, they stay for arm64's little cores (docs/explored-designs.md). --skip leaves them out of a
log that has them.

Needs matplotlib and the Noto Sans font.
"""

import argparse
import math
import re
import subprocess
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

# zram's names of the backends, and what the chart calls them
NAMES = {
    "lz4": "lz4",
    "lzo-rle": "lzo-rle",
    "zstd": "zstd",
    "bytelz": "bytelz",
    "seqlz": "seqlz-fast",
    "seqlz-lit": "seqlz-fast-lit",
}
STYLE = {
    "lz4": ("#7f7f7f", "o"),
    "lzo-rle": ("#2b2b2b", "s"),
    "zstd": ("#8e44ad", "D"),
    "bytelz": ("#e67e22", "^"),
    "seqlz-fast": ("#1f6fd1", "o"),
    "seqlz-fast-lit": ("#17a2b8", "v"),
}
OTHER_COLORS = ["#2e9e44", "#c0392b", "#16a085", "#d35400", "#2c3e50"]
MAIN = ("seqlz-fast-lit",)
# what the footer says about a codec, if it is in the chart
ABOUT = {
    "seqlz-fast-lit": "seqlz-fast-lit: literals coded with one of 8 static tables per page, offsets in steps of 8 in their own classes (#40).",
}


def name_of(algo):
    """The chart's name of a zram device: primary+secondary, the primary recompressed with the other, is
    "primary + secondary"."""
    primary, plus, secondary = algo.partition("+")
    return f"{NAMES.get(primary, primary)} + {secondary}" if plus else NAMES.get(algo, algo)


def recompressed(name):
    return " + " in name


def parse(path, skip=()):
    """Per codec: orig and mem of mm_stat (after recompression where there is one), the write, warm and
    cold read times as p50 / p90 / p99 / mean in ns, and the recompression time per page."""
    codecs = {}
    for line in open(path, errors="replace"):
        at = line.find("RESULT ")
        if at < 0:
            continue
        algo, rest = line[at + 7 :].split(None, 1)
        if algo in skip:
            continue
        c = codecs.setdefault(name_of(algo), {})
        rest = rest.strip()
        times = {k: float(v) for k, v in re.findall(r"(p50|p90|p99|mean) (\d+)", rest)}
        if rest.startswith("mm_stat"):
            fields = rest.split()[3 if rest.startswith("mm_stat after") else 1 :]
            c["orig"], c["mem"] = float(fields[0]), float(fields[2])
        elif rest.startswith("write, other page before"):
            c["write"] = times
        elif rest.startswith("warm") and "prefetch 8" in rest:
            c["warm"] = times
        elif rest.startswith("flushed, other page first") and "prefetch 8" in rest:
            c["cold"] = times
        elif rest.startswith("recompress:"):
            c["recompress"] = float(rest.split()[1])
    for name, c in codecs.items():
        missing = [k for k in ("orig", "mem", "write", "warm", "cold") if k not in c]
        if missing or any("mean" not in c[k] for k in ("write", "warm", "cold")):
            raise SystemExit(f"{path}: {name} has no {', '.join(missing) or 'mean'}; a log from before the means?")
    return codecs


def hull(points):
    """The points (time, memory, name) with the lowest memory + lambda * time for some lambda >= 0: the
    lower left convex hull, from the fastest to the smallest."""
    h = []
    for t, m, n in sorted(points):
        if h and m >= h[-1][1]:
            continue
        while len(h) >= 2:
            (t1, m1, _), (t2, m2, _) = h[-2], h[-1]
            if (t2 - t1) * (m - m1) - (m2 - m1) * (t - t1) > 0:
                break
            h.pop()
        h.append((t, m, n))
    return h


def git_source():
    here = Path(__file__).resolve().parent
    try:
        return subprocess.run(["git", "-C", str(here), "describe", "--always", "--dirty"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def cpu_model():
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return "unknown CPU"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--run", action="append", required=True, metavar="TITLE=LOG",
                    help="a row: its title and the log of tools/zram-vm/run.sh")
    ap.add_argument("--out", action="append", required=True, help="output file, .png or .svg, can be given more than once")
    ap.add_argument("--reads-per-write", type=float, default=0.34, help="r of the score, default 0.34 (PLAN.md §1.1)")
    ap.add_argument("--clock", default="4.5 GHz", help="the fixed clock of the CPU the VM ran on, for the footer")
    ap.add_argument("--source", default=None, help="what was measured, for the footer; default git describe")
    ap.add_argument("--skip", default="", help="zram's names of codecs to leave out, comma separated, e.g. bytelz,seqlz")
    args = ap.parse_args()

    r = args.reads_per_write
    rows = []
    for run in args.run:
        title, sep, path = run.partition("=")
        if not sep:
            raise SystemExit(f"--run {run}: TITLE=LOG")
        rows.append((title, parse(path, set(filter(None, args.skip.split(","))))))
    others = iter(OTHER_COLORS * 4)
    for _, codecs in rows:
        for name in codecs:
            if name not in STYLE:
                STYLE[name] = (next(others), "X")
    names = [n for n in STYLE if any(n in codecs for _, codecs in rows)]

    columns = [("read, cold", "cold", 12), ("read, warm", "warm", 9), ("write", "write", 30)]
    plt.rcParams.update({"font.family": "Noto Sans", "font.size": 10})
    # in inches: title and legend above, the footer below, and each row
    head, foot, row_h = 0.8, 1.0, 4.4
    height = head + foot + row_h * len(rows)
    fig, axes = plt.subplots(len(rows), 4, figsize=(19, height), sharey=True, squeeze=False)

    for row, (title, codecs) in enumerate(rows):
        mem_pct = {n: c["mem"] / c["orig"] * 100 for n, c in codecs.items()}
        for col, (what, key, xmin) in enumerate(columns):
            ax = axes[row][col]
            for name, c in codecs.items():
                color, marker = STYLE[name]
                p50, p99, mean = (c[key][k] / 1000 for k in ("p50", "p99", "mean"))
                main_codec = name in MAIN
                ax.plot([p50, p99], [mem_pct[name]] * 2, color=color, lw=2.6 if main_codec else 1.6, alpha=0.9, zorder=2)
                ax.plot(p99, mem_pct[name], marker=marker, ms=7, mfc="white", mec=color, mew=1.6, zorder=3)
                ax.plot(p50, mem_pct[name], marker=marker, ms=10 if main_codec else 8, color=color, mec="white", mew=0.8,
                        zorder=4)
                ax.plot(mean, mem_pct[name], marker="|", ms=14, mew=2, color=color, zorder=5)
            top = max(c[key]["p99"] for c in codecs.values()) / 1000
            ax.set_xlim(0, max(xmin, math.ceil(top * 1.05)))
            ax.set_xlabel(f"{what}: latency per page, µs\n(filled p50, bar mean, open p99)")
            if row == 0:
                ax.set_title(what.capitalize(), fontsize=12, fontweight="bold", loc="left")
            if col == 0:
                ax.set_ylabel(f"{title}\n\nmemory used by zsmalloc,\nin % of the uncompressed pages")
                # the memory as a small table in the empty lower part, one line per codec, 11 points apart
                table = sorted(((m, n) for n, m in mem_pct.items()), reverse=True)
                lines = [(f"{'codec':<24}{'memory':>7}{'ratio':>9}", "#555555", "bold")]
                lines += [(f"{n:<24}{m:6.1f}%{100 / m:7.2f}:1", STYLE[n][0], "bold" if n in MAIN else "normal") for m, n in table]
                for k, (text, color, weight) in enumerate(lines):
                    ax.annotate(text, (0.03, 0.02), xycoords="axes fraction", textcoords="offset points",
                                xytext=(0, 11 * (len(lines) - 1 - k)), family="DejaVu Sans Mono", fontsize=8.8, color=color,
                                va="bottom", fontweight=weight)

        # the score: memory against the mean time per page written
        ax = axes[row][3]
        points = []
        for name, c in codecs.items():
            color, marker = STYLE[name]
            t = (c["write"]["mean"] + r * c["cold"]["mean"]) / 1000
            points.append((t, mem_pct[name], name))
            ax.plot(t, mem_pct[name], marker=marker, ms=10 if name in MAIN else 8, color=color, mec="white", mew=0.8,
                    zorder=4)
        # the recompressed ones are left out of the hull: their recompression is not in their time
        h = hull([p for p in points if not recompressed(p[2])])
        ax.plot([p[0] for p in h], [p[1] for p in h], color="#aaaaaa", lw=1.2, ls="--", zorder=1)
        for (t1, m1, _), (t2, m2, _) in zip(h, h[1:]):
            rate = (m1 - m2) / 100 * 4096 / (t2 - t1)
            ax.annotate(f"{rate:.0f} B/µs", ((t1 + t2) / 2, (m1 + m2) / 2), textcoords="offset points", xytext=(4, 4),
                        fontsize=8, color="#777777")
        for k, (t, m, name) in enumerate(p for p in points if recompressed(p[2]) and "recompress" in codecs[p[2]]):
            ax.annotate(f"+{codecs[name]['recompress'] / 1000:.0f} µs recompression per page, not counted", (t, m),
                        textcoords="offset points", xytext=(10, -14 - 12 * k), fontsize=8, color=STYLE[name][0])
        ax.set_xlim(0, max(20, math.ceil(max(p[0] for p in points) * 1.1)))
        ax.set_xlabel(f"time per page written: write + {r:g} × cold read,\nmeans, µs (dashed: best for some exchange rate)")
        if row == 0:
            ax.set_title("Score", fontsize=12, fontweight="bold", loc="left")

    for ax in axes.flat:
        ax.set_ylim(0, 50)
        ax.set_yticks(range(0, 51, 10))
        ax.set_yticklabels([f"{v}%" for v in range(0, 51, 10)])
        ax.grid(True, color="#e6e6e6", lw=0.8)
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)

    handles = [Line2D([0], [0], color=STYLE[n][0], marker=STYLE[n][1], lw=2, ms=8, label=n) for n in names]
    fig.legend(handles=handles, loc="upper center", ncol=len(names), frameon=False, bbox_to_anchor=(0.5, 1 - 0.42 / height))
    fig.suptitle("zram codecs: memory against latency, lower left is better", fontsize=15, fontweight="bold",
                 y=1 - 0.05 / height)
    about = " ".join([ABOUT[n] for n in names if n in ABOUT] + (
        ["A + B: written with A (so its writes are A's), then all pages recompressed with B by zram as idle pages, "
         "compacted, then read."] if any(recompressed(n) for n in names) else []))
    fig.text(
        0.01,
        0.01,
        f"Linux kernel in a VM, zram with zsmalloc, one device per codec, one boot per row ({args.source or git_source()}). "
        f"{cpu_model()}, one CPU at a fixed {args.clock}. Per page the median of 3 runs;\n"
        "each timed read and write comes after another page; cold: the compressed data flushed from the cache first. "
        + ("bytelz and the seqlz codecs" if "bytelz" in names else "The seqlz codecs")
        + " prefetch the compressed data in their zram backend, lz4, lzo-rle and zstd run as the kernel has them.\n"
        + (about + "\n" if about else "")
        + f"Score: PLAN.md §1.1, {r:g} reads per write; B/µs: bytes saved per page for each µs more, the exchange rate at which "
        "the faster codec starts to win. Ratio = uncompressed size / memory used by zsmalloc.",
        fontsize=8.5,
        color="#555555",
        va="bottom",
    )
    fig.tight_layout(rect=(0, foot / height, 1, 1 - head / height))
    for out in args.out:
        fig.savefig(out, dpi=150)


if __name__ == "__main__":
    main()
