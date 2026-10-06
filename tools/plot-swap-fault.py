#!/usr/bin/env python3
# SPDX-License-Identifier: MIT OR GPL-2.0-only
"""The whole page fault of zram as swap: what the kernel takes and what the codec adds.

A row is the log of tools/zram-vm/run.sh with MODE=swap, zram in a VM on the PC, or of
tools/swap-fault/swap_fault.c on a phone. Per codec one bar, the mean over the pages: in grey the time
of a fault on a same-filled page, which zram stores without the codec, in the codec's colour what a
page that the codec decompresses (or compresses) takes more. Panels: swap-in with the compressed data
cold, swap-in warm, swap-out.

    tools/plot-swap-fault.py --row "PC=vm.log" --row "Phone, big core=cpu7.log" \\
        --row "Phone, little core=cpu2.log" --bytes "Phone, little core=cpu7.log" --out swap-fault.svg

--bytes takes a row's bytes per page from another log of the same pages, e.g. where other pages of the
system went into the device while it ran.

Needs matplotlib and the Noto Sans font.
"""

import argparse
import importlib.util
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("plot_codecs", here / "plot-codecs.py")
plot_codecs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plot_codecs)

# the phone's names: the module registers seqlz-<name> and seqlz-<name>-lit, and the phone's 4.14 has
# lzo where newer kernels have lzo-rle
ORDER = ["lz4", "lzo-rle", "zstd", "seqlz-fast", "seqlz-fast-lit"]
KERNEL = "#e2e2e2"


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
    ("Swap-in, compressed data cold", "swap-in, cold", "swap-in, flushed"),
    ("Swap-in, warm", "swap-in, warm", None),
    ("Swap-out, one page per call", "swap-out, one call per page", None),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--row", action="append", required=True, help="title=log")
    ap.add_argument("--bytes", action="append", default=[], help="title=log")
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

    plt.rcParams.update({"font.family": "Noto Sans", "font.size": 10})
    fig, axes = plt.subplots(len(rows), len(PANELS), figsize=(15, 3.1 * len(rows) + 1.4), squeeze=False)
    for i, (title, codecs) in enumerate(rows):
        names = [n for n in ORDER if n in codecs]
        for j, (panel, key, alt) in enumerate(PANELS):
            ax = axes[i][j]
            k = key if key in codecs[names[0]] else alt
            same = "swap-out, same-filled" if k.startswith("swap-out") else k + ", same-filled"
            base = codecs["lz4"][k] if "lz4" in codecs else None
            xmax = max(codecs[n][k] for n in names)
            for y, n in enumerate(reversed(names)):
                c = codecs[n]
                kernel = c[same]
                total = c[k]
                color = plot_codecs.STYLE[n][0]
                ax.barh(y, kernel, color=KERNEL, height=0.62, edgecolor="white", linewidth=1)
                ax.barh(y, total - kernel, left=kernel, color=color, height=0.62, edgecolor="white", linewidth=1)
                label = f"{total:.1f} µs"
                if base and n != "lz4":
                    lz4 = codecs["lz4"]
                    label += f", {100 * (total / base - 1):+.0f}% to lz4"
                    label += f"\n(the codec alone {100 * ((total - kernel) / (lz4[k] - lz4[same]) - 1):+.0f}%)"
                ax.text(total + xmax * 0.015, y, label, va="center", fontsize=8, color="#333333", linespacing=1.1)
            ax.set_yticks(range(len(names)))
            ax.set_yticklabels([f"{n}  {codecs[n]['bytes']:.0f} B" for n in reversed(names)] if j == 0 else [])
            ax.set_xlim(0, xmax * 1.55)
            ax.grid(axis="x", color="#e5e5e5")
            ax.set_axisbelow(True)
            for s in ("top", "right"):
                ax.spines[s].set_visible(False)
            if i == 0:
                ax.set_title(panel, loc="left", fontweight="bold")
            if i == len(rows) - 1:
                ax.set_xlabel("µs per page, mean")
        axes[i][0].set_ylabel(title, fontsize=10.5, fontweight="bold")
    fig.suptitle(
        "zram as swap, the whole page fault: the kernel's part is the same for every codec",
        x=0.01,
        ha="left",
        fontsize=14,
        fontweight="bold",
    )
    fig.text(
        0.01,
        0.005,
        "Grey: the same fault on a same-filled page, which zram stores without the codec. Colour: what the codec adds. "
        "Next to each codec its zsmalloc bytes per page.\n"
        "PC: Ryzen 9 7950X at 4.5 GHz, Linux 7.3-rc1 in a VM, cold = the compressed data flushed. Phone: Mi 9T, its Linux 4.14, "
        "cold = 2 MiB of other data read first, lzo instead of lzo-rle. 20 000 pages of a desktop zram dump.",
        fontsize=8.5,
        color="#555555",
        va="bottom",
    )
    fig.tight_layout(rect=(0, 0.05, 1, 0.95))
    for out in args.out:
        fig.savefig(out)


if __name__ == "__main__":
    main()
