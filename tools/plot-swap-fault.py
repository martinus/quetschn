#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""The whole page fault of zram as swap: memory against time, the kernel's part and the codec's.

A row is the log of tools/zram-vm/run.sh with MODE=swap, zram in a VM on the PC, or of
tools/swap-fault/swap_fault.c on a phone. Per codec one line at its zsmalloc memory per page, in % of
the page, from 0 to its mean time per page: in grey the time of a fault on a same-filled page, which
zram stores without the codec, in the codec's colour what a page that the codec decompresses (or
compresses) takes more. Lower left is better. Panels: swap-in with the compressed data cold, swap-in
warm, swap-out; both swap-ins of a row share one time axis.

    tools/plot-swap-fault.py --row "PC=vm.log" --row "Phone, big core=cpu7.log" \\
        --row "Phone, little core=cpu2.log" --bytes "Phone, little core=cpu7.log" --out swap-fault.svg

--bytes takes a row's bytes per page from another log of the same pages, e.g. where other pages of the
system went into the device while it ran. --codecs picks the codecs, by default lz4, lzo-rle, zstd and
seqlz-fast-lit.

Needs matplotlib and the Noto Sans font.
"""

import argparse
import importlib.util
import math
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("plot_codecs", here / "plot-codecs.py")
plot_codecs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot_codecs)

# the phone's names: the module registers seqlz-<name> and seqlz-<name>-lit, and the phone's 4.14 has
# lzo where newer kernels have lzo-rle
ORDER = ["lz4", "lzo-rle", "zstd", "seqlz-fast", "seqlz-fast-lit"]
COLORS = {"lz4": "#15191e", "lzo-rle": "#7a838e", "zstd": "#eb6834", "seqlz-fast": "#1baf7a", "seqlz-fast-lit": "#2a78d6"}
KERNEL = "#c9ced5"


def name_of(algo):
    if algo == "lzo":
        return "lzo-rle"
    m = re.fullmatch(r"seqlz-\w+?(-lit)?", algo)
    if m and algo not in ("seqlz", "seqlz-lit"):
        return "seqlz-fast-lit" if m.group(1) else "seqlz-fast"
    return plot_codecs.NAMES.get(algo, algo)


def parse(path):
    """Per codec: the means in ns of each measurement, and zsmalloc's bytes per page that is not
    same-filled."""
    codecs, n_codec = {}, None
    for line in open(path, errors="replace"):
        at = line.find("RESULT ")
        if at < 0:
            continue
        rest = line[at + 7 :]
        m = re.match(r"swap: (\d+) pages, (\d+) of them same-filled", rest)
        if m:
            n_codec = int(m.group(1)) - int(m.group(2))
            continue
        m = re.match(r"(\S+) +mm_stat +(\d+) +(\d+) +(\d+)", rest)
        if m:
            codecs.setdefault(name_of(m.group(1)), {})["mem"] = int(m.group(4))
            continue
        m = re.match(r"(\S+) +(.+?) +n +\d+: p50 \d+ p90 \d+ p99 (\d+) mean (\d+) ns", rest)
        if m:
            c = codecs.setdefault(name_of(m.group(1)), {})
            c[m.group(2)] = int(m.group(4)) / 1000
            c[m.group(2) + " p99"] = int(m.group(3)) / 1000
    for c in codecs.values():
        c["bytes"] = c["mem"] / n_codec
    return codecs


PANELS = [
    ("Swap-in, compressed data cold", ("swap-in, cold", "swap-in, flushed")),
    ("Swap-in, warm", ("swap-in, warm",)),
    ("Swap-out, one page per call", ("swap-out, one call per page",)),
]


def kernel_key(k):
    return "swap-out, same-filled" if k.startswith("swap-out") else k + ", same-filled"


def place_labels(ax, fig, marks):
    """Each label at the nearest free place, in pixels: right of the dot, then above or below it, then
    left of it, clear of every line and of the labels placed before. A label away from its dot gets a
    thin leader."""
    fig.canvas.draw()
    to_px = ax.transData.transform
    x0, y0 = to_px((ax.get_xlim()[0], ax.get_ylim()[0]))
    x1, y1 = to_px((ax.get_xlim()[1], ax.get_ylim()[1]))
    boxes = []
    for m in marks:
        (lx, ly), (rx, _) = to_px((0, m["y"])), to_px((m["x"], m["y"]))
        boxes.append((lx, ly - 4, rx + 6, ly + 4))
    hit = lambda b: any(b[0] < o[2] and o[0] < b[2] and b[1] < o[3] and o[1] < b[3] for o in boxes)
    for m in marks:
        dx, dy = to_px((m["x"], m["y"]))
        w, h = len(m["text"]) * 6.3 + 4, 12
        cands = []
        for off in (0, 11, -11, 22, -22, 33, -33):
            cands.append((dx + 8, dy + off - h / 2))
            if off:
                cands.append((dx - w / 2, dy + off - h / 2))
            cands.append((dx - 8 - w, dy + off - h / 2))
        pos = next(
            (c for c in cands if c[0] >= x0 + 2 and c[0] + w <= x1 and c[1] >= y0 and c[1] + h <= y1 and not hit((c[0], c[1], c[0] + w, c[1] + h))),
            cands[0],
        )
        boxes.append((pos[0], pos[1], pos[0] + w, pos[1] + h))
        inv = ax.transData.inverted().transform
        tx, ty = inv((pos[0], pos[1] + h / 2))
        ax.text(tx, ty, m["text"], va="center", ha="left", fontsize=8.5, color="#15191e")
        if abs(pos[1] + h / 2 - dy) > 7:
            ex = min(max(dx, pos[0]), pos[0] + w)
            ey = pos[1] if pos[1] > dy else pos[1] + h
            ax.plot(*zip(inv((dx, dy)), inv((ex, ey))), color="#7a838e", lw=0.7, zorder=2)


def nice_max(v):
    raw = v / 5
    p = 10 ** math.floor(math.log10(raw))
    step = next(s * p for s in (1, 2, 2.5, 5, 10) if raw / p <= s)
    return math.ceil(v / step) * step


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--row", action="append", required=True, help="title=log")
    ap.add_argument("--bytes", action="append", default=[], help="title=log")
    ap.add_argument("--codecs", default="lz4,lzo-rle,zstd,seqlz-fast-lit")
    ap.add_argument("--out", action="append", required=True)
    args = ap.parse_args()
    rows = [(r.split("=", 1)[0], parse(r.split("=", 1)[1])) for r in args.row]
    for b in args.bytes:
        title, path = b.split("=", 1)
        other = parse(path)
        for t, codecs in rows:
            if t == title:
                for n, c in codecs.items():
                    c["bytes"] = other[n]["bytes"]
    wanted = args.codecs.split(",")

    plt.rcParams.update({"font.family": "Noto Sans", "font.size": 10})
    fig, axes = plt.subplots(len(rows), len(PANELS), figsize=(15, 3.9 * len(rows) + 1.3), squeeze=False)
    for i, (title, codecs) in enumerate(rows):
        names = [n for n in ORDER if n in codecs and n in wanted]
        keys = [next(k for k in ks if k in codecs[names[0]]) for _, ks in PANELS]
        # one time axis for both swap-ins of a row, mean and p99, so that they compare
        swap_in = max(codecs[n][k + s] for n in names for k in keys[:2] for s in ("", " p99"))
        swap_out = max(codecs[n][keys[2] + s] for n in names for s in ("", " p99"))
        for j, (panel, _) in enumerate(PANELS):
            ax, k = axes[i][j], keys[j]
            ax.set_xlim(0, nice_max(1.15 * (swap_out if j == 2 else swap_in)))
            ax.set_ylim(0, 50)
            lz4 = codecs["lz4"][k]
            ax.axvline(lz4, color="#7a838e", lw=1, ls=(0, (4, 4)), zorder=1)
            marks = []
            for n in sorted(names, key=lambda n: -codecs[n]["bytes"]):
                c = codecs[n]
                y = 100 * c["bytes"] / 4096
                kernel, total = c[kernel_key(k)], c[k]
                ax.plot([0, kernel], [y, y], color=KERNEL, lw=4.5, solid_capstyle="butt", zorder=3)
                ax.plot([kernel, total], [y, y], color=COLORS[n], lw=4.5, solid_capstyle="butt", zorder=3)
                ax.plot(total, y, "o", ms=7, color=COLORS[n], mec="white", mew=1.5, zorder=4)
                shown = "lzo" if n == "lzo-rle" and "Phone" in title else n
                text = f"{shown} {total:.1f}" if total >= 10 else f"{shown} {total:.2f}"
                if n != "lz4":
                    text += f", {round(100 * (total / lz4 - 1)):+d}%"
                marks.append({"x": total, "y": y, "text": text})
            ax.yaxis.set_major_formatter(matplotlib.ticker.PercentFormatter(decimals=0))
            ax.grid(color="#eceff2")
            ax.set_axisbelow(True)
            for s in ("top", "right"):
                ax.spines[s].set_visible(False)
            if i == 0:
                ax.set_title(panel, loc="left", fontweight="bold")
            if i == len(rows) - 1:
                ax.set_xlabel("µs per page, mean")
            place_labels(ax, fig, marks)
        axes[i][0].set_ylabel(f"{title}\nmemory, % of the page", fontsize=10)
    fig.suptitle(
        "zram as swap, the whole page fault: memory against time, the kernel's part in grey",
        x=0.01,
        ha="left",
        fontsize=14,
        fontweight="bold",
    )
    fig.text(
        0.01,
        0.005,
        "Grey: the same fault on a same-filled page, which zram stores without the codec. Colour: what the codec adds. "
        "Dashed: lz4's time; the percentages are against it. Lower left is better.\n"
        "PC: Ryzen 9 7950X at 4.5 GHz, Linux 7.3-rc1 in a VM, 20 000 pages of a desktop zram dump, cold = the compressed data flushed. "
        "Phone: Mi 9T, its Linux 4.14, 20 000 pages of a phone zram dump,\ncold = 2 MiB of other data read first, lzo instead of lzo-rle. "
        "seqlz prefetches the compressed data, as its backend does.",
        fontsize=8.5,
        color="#555555",
        va="bottom",
    )
    fig.tight_layout(rect=(0, 0.055, 1, 0.96))
    for out in args.out:
        fig.savefig(out)


if __name__ == "__main__":
    main()
