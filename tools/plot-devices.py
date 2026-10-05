#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""The zram codecs on a PC and on a phone, in the kernel: memory against latency, one row per CPU.

A row is the log of tools/zram-vm/run.sh, zram in a VM on the PC, or of zramphone, zram in the
phone's own kernel (docs/explored-designs.md, "In the phone's own kernel"). Panels: cold read, warm
read, write, and the score of PLAN.md §1.1. Each row has its own time axis, the CPUs are far apart.

    tools/plot-devices.py --row "PC=vm.log" --row "Phone, big core=cpu7.log" \\
        --row "Phone, little core=cpu2.log" --out devices.svg

Needs matplotlib and the Noto Sans font.
"""

import argparse
import importlib.util
import itertools
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("plot_codecs", here / "plot-codecs.py")
plot_codecs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot_codecs)

# zramphone's names: the variant modules register seqlz-<name> and seqlz-<name>-lit, and the phone's
# 4.14 has lzo where newer kernels have lzo-rle
PHONE_NAMES = {"lzo": "lzo-rle"}


def parse_phone(path):
    codecs = {}
    for line in Path(path).read_text().splitlines():
        m = re.match(r"RESULT (\S+) +(mm_stat|write|r0K|r\d+K) +(.*)", line)
        if not m:
            continue
        algo, what, rest = m.groups()
        name = PHONE_NAMES.get(algo) or re.sub(r"^seqlz-[^-]+(-lit)?$", r"seqlz-fast\1", algo)
        c = codecs.setdefault(name, {})
        if what == "mm_stat":
            f = rest.split()
            c["orig"], c["mem"] = float(f[0]), float(f[2])
        else:
            key = {"write": "write", "r0K": "warm"}.get(what, "cold")
            c[key] = {k: float(v) for k, v in re.findall(r"(p50|p99|mean) (\d+)", rest)}
    return codecs


def parse(path):
    text = Path(path).read_text()
    return parse_phone(path) if re.search(r"RESULT \S+ +r0K", text) else plot_codecs.parse(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--row", action="append", required=True, metavar="TITLE=LOG", help="a row: its title and its log")
    ap.add_argument(
        "--out", action="append", required=True, help="output file, .png or .svg, can be given more than once"
    )
    ap.add_argument("--reads-per-write", type=float, default=0.34, help="r of the score, default 0.34 (PLAN.md §1.1)")
    ap.add_argument("--note", default="", help="the footer's lines about what was measured, \\n between lines")
    args = ap.parse_args()

    r = args.reads_per_write
    rows = []
    for row in args.row:
        title, sep, path = row.partition("=")
        if not sep:
            raise SystemExit(f"--row {row}: TITLE=LOG")
        rows.append((title, parse(path)))
    names = [n for n in plot_codecs.STYLE if any(n in codecs for _, codecs in rows)]

    columns = [("Read, cold", "cold"), ("Read, warm", "warm"), ("Write", "write")]
    plt.rcParams.update({"font.family": "Noto Sans", "font.size": 10})
    head, foot, row_h = 0.8, 1.0, 3.9
    height = head + foot + row_h * len(rows)
    fig, axes = plt.subplots(len(rows), 4, figsize=(19, height), squeeze=False)
    for row, (title, codecs) in enumerate(rows):
        mem = {n: c["mem"] / c["orig"] * 100 for n, c in codecs.items()}
        for col, (what, key) in enumerate(columns):
            ax = axes[row][col]
            for name in names:
                color, marker = plot_codecs.STYLE[name]
                p50, p99, mean = (codecs[name][key][k] / 1000 for k in ("p50", "p99", "mean"))
                main_codec = name in plot_codecs.MAIN
                ax.plot([p50, p99], [mem[name]] * 2, color=color, lw=2.6 if main_codec else 1.6, alpha=0.9, zorder=2)
                ax.plot(p99, mem[name], marker=marker, ms=7, mfc="white", mec=color, mew=1.6, zorder=3)
                ax.plot(
                    p50,
                    mem[name],
                    marker=marker,
                    ms=10 if main_codec else 8,
                    color=color,
                    mec="white",
                    mew=0.8,
                    zorder=4,
                )
                ax.plot([mean] * 2, [mem[name] - 1.2, mem[name] + 1.2], color=color, lw=2.2, zorder=4)
            ax.set_xlim(0, max(codecs[n][key]["p99"] for n in names) / 1000 * 1.08)
            ax.set_xlabel(f"{what.lower()}: µs per page (filled p50, bar mean, open p99)")
            if row == 0:
                ax.set_title(what, loc="left", fontweight="bold")
        ax = axes[row][3]
        pts = [((codecs[n]["write"]["mean"] + r * codecs[n]["cold"]["mean"]) / 1000, mem[n], n) for n in names]
        for t, m, n in pts:
            color, marker = plot_codecs.STYLE[n]
            ax.plot(t, m, marker=marker, ms=9, color=color, mec="white", mew=0.8, zorder=4)
        h = plot_codecs.hull(pts)
        ax.plot([p[0] for p in h], [p[1] for p in h], "--", color="#999999", lw=1.2, zorder=1)
        for (t1, m1, _), (t2, m2, _) in itertools.pairwise(h):
            rate = (m1 - m2) / 100 * 4096 / (t2 - t1)
            ax.annotate(
                f"{rate:.0f} B/µs" if rate >= 10 else f"{rate:.1f} B/µs",
                ((t1 + t2) / 2, (m1 + m2) / 2),
                textcoords="offset points",
                xytext=(4, 6),
                fontsize=8,
                color="#666666",
            )
        ax.set_xlim(0, max(p[0] for p in pts) * 1.12)
        ax.set_xlabel(
            f"time per page written: write + {r:g} × cold read,\nmeans, µs (dashed: best for some exchange rate)"
        )
        if row == 0:
            ax.set_title("Score", loc="left", fontweight="bold")
        axes[row][0].set_ylabel(f"{title}\nmemory, % of the pages", fontsize=10)
        for ax in axes[row]:
            ax.set_ylim(0, 45)
            ax.yaxis.set_major_formatter(plt.FuncFormatter(lambda v, _: f"{v:.0f}%"))
            ax.grid(color="#e5e5e5")
            ax.set_axisbelow(True)
            for side in ("top", "right"):
                ax.spines[side].set_visible(False)
        lines = ["codec            memory  ratio"] + [f"{n:<16} {mem[n]:5.1f}%  {100 / mem[n]:.2f}:1" for n in names]
        axes[row][0].text(
            0.03,
            0.04,
            "\n".join(lines),
            transform=axes[row][0].transAxes,
            family="monospace",
            fontsize=7.5,
            va="bottom",
            color="#444444",
        )

    handles = [
        Line2D([0], [0], color=plot_codecs.STYLE[n][0], marker=plot_codecs.STYLE[n][1], lw=2, ms=8, label=n)
        for n in names
    ]
    fig.legend(
        handles=handles, loc="upper center", ncol=len(names), frameon=False, bbox_to_anchor=(0.5, 1 - 0.42 / height)
    )
    fig.suptitle(
        "zram codecs on a PC and a phone, in the kernel: memory against latency, lower left is better",
        fontsize=15,
        fontweight="bold",
        y=1 - 0.05 / height,
    )
    fig.text(
        0.01,
        0.01,
        args.note.replace("\\n", "\n")
        + f"\nEach row has its own time axis. Score: PLAN.md §1.1, {r:g} reads per write; B/µs: bytes saved per page for each "
        "µs more, the exchange rate at which the faster codec starts to win.",
        fontsize=8.5,
        color="#555555",
        va="bottom",
    )
    fig.tight_layout(rect=(0, foot / height, 1, 1 - head / height))
    for out in args.out:
        fig.savefig(out, dpi=150)


if __name__ == "__main__":
    main()
