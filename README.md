# quetschn

Fast compression for memory pages, built for Linux zram.

See [PLAN.md](PLAN.md) for the goal, the evidence behind it, and the plan.

## Build and test

```sh
cmake -S . -B build -G Ninja -DQUETSCHN_WERROR=ON
cmake --build build
./build/quetschn_test
```

`-DQUETSCHN_SANITIZE=ON` builds with ASan and UBSan. Formatting is checked with clang-format 21.

## Benchmarking the kernel's codecs

The harness builds zram's `lz4`, `lzo`, `lzo-rle` and `zstd` from a Linux source tree, with the
kernel's compiler flags. The kernel sources are compiled in place and not copied into this repository.

```sh
cmake -S . -B build -G Ninja -DQUETSCHN_KERNEL_TREE=$HOME/linux
cmake --build build
./build/quetschn-bench-lz4 --corpus corpus/desktop --cpu 2 --out lz4.tsv
./build/quetschn-bench-zstd --corpus corpus/desktop --cpu 2 --level -1
```

`--level` is zram's `algorithm_params` level: the acceleration for `lz4`, the level for `zstd`. Without
it each codec uses zram's default.

With a dictionary, trained on other programs than the ones it is measured on:

```sh
./build/quetschn-split-corpus --corpus corpus/desktop --train corpus/train --test corpus/test
zstd --train corpus/train.pages -B4096 --maxdict=64KB -o corpus/dict
./build/quetschn-bench-lz4 --corpus corpus/test --dict corpus/dict
```

`--out` writes one line per page. `quetschn-compare` pairs two such files from the same corpus:

```sh
./build/quetschn-bench-lzo-rle --corpus corpus/test --cpu 2 --out lzo-rle.tsv
./build/quetschn-bench-zstd --corpus corpus/test --cpu 2 --level -1 --out zstd-1.tsv
./build/quetschn-compare --baseline lzo-rle.tsv --candidate zstd-1.tsv
```

It prints how much Σ zsmalloc cost the candidate saves, how many pages get cheaper or more expensive,
and the difference of the latency percentiles. Every number has a 95% confidence interval from a
bootstrap that resamples pages, the same pages for both runs.

`quetschn-score` puts memory against time per page, the score of `PLAN.md` §1.1: bytes per page
against `compress + r * cold decompress`, the means, and which codecs have the lowest `bytes + lambda
* time` for some lambda. It reads the output of `quetschn-bench-*` with timing, or of
`tools/zram-vm/run.sh`, where recompression counts too. `r` is the reads per write,
`quetschn-swap-bursts` measures it on a running machine, and how large the bursts of swap-ins are:

```sh
./build/quetschn-bench-interleaved --codecs lz4,lzo-rle,zstd,seqlz-fast-lit --corpus corpus/test --cpu 2 >run.txt
./build/quetschn-score --reads-per-write 0.34 run.txt
./build/quetschn-swap-bursts --seconds 3600
```

`tools/plot-codecs.py` draws the chart of the codecs from the kernel VM: per dump memory against the
latency of cold reads, warm reads and writes, and against the score, with the codecs that have the
lowest score for some lambda. A device `a+b`, e.g. `seqlz-lit+zstd`, is written with `a` and then
recompressed by zram with `b`. One boot of `tools/zram-vm/run.sh` per row; it needs matplotlib and the
Noto Sans font:

```sh
ALGOS=lz4,lzo-rle,zstd,seqlz-lit tools/zram-vm/run.sh <linux tree> corpus/first >first.log
ALGOS=lz4,lzo-rle,zstd,seqlz-lit tools/zram-vm/run.sh <linux tree> corpus/second >second.log
tools/plot-codecs.py --run "first dump=first.log" --run "second dump=second.log" --out codecs.png --out codecs.svg
```

`quetschn-bench-spike-switch`, `-branchless`, `-zeroskip` and `-slots` run the decoder latency spike
of `PLAN.md` Phase 2b, `spike/wk64.h` describes its format.

For trying out designs, `tools/quick-bench.sh` is the fast benchmark: exact zsmalloc cost on the whole
corpus without timing, and latency on a fixed sample of 20 000 pages in 5 separate processes, all
compared with the first codec. `docs/explored-designs.md` says why, and when to run the full one.

Fix the clock of the CPU the benchmarks run on first, cold latencies depend on it. On an AMD CPU with
`amd-pstate`, for CPU 2 at 4.5 GHz:

```sh
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost
sudo cpupower -c 2 frequency-set -g performance
echo 4500000 | sudo tee /sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq
echo 4500000 | sudo tee /sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq
```

```sh
tools/quick-bench.sh build corpus/test results lz4,lzo-rle,zstd:-1,spike-slots
```

`quetschn-lz-analysis --corpus <base>` estimates what `lz4hc`'s matches would cost with entropy coded
sequences and literals, and computes exactly what byte oriented formats would cost; `--codec seqlz`
takes the matches of `seqlz-fast`'s matcher instead, see `docs/explored-designs.md`. `quetschn-seqlz-train` trains the tables of the
seqlz prototype; `explore/seqlz_default_tables.c` says how the compiled-in ones were made.

For `perf`, `quetschn-bench-interleaved --codecs <codec> --corpus <base> --decode-loop <n>` compresses
every page once and then only decodes, n times, and prints warm decode percentiles per page; with
`--cold`, 2 MiB of other data are read and the page is flushed before each decode; with `--compress`
it times the compression instead, every page one after the other, so each comes from memory. The difference of
two runs with different n, e.g. with `perf stat -e cycles,instructions,branch-misses`, is the decoder
alone. `perf record -j any,u -e branch-misses` shows the mispredicted branches on Zen 4. `--out <file.tsv>`
writes the median time and compressed length of each page, to see which pages make the tail.

Latency comparisons between separate runs suffer from drift, e.g. of the CPU frequency.
`quetschn-bench-interleaved` has all codecs in one binary and runs each of them once per repetition on
the same page, in rotating order. `--out` is a directory then:

```sh
./build/quetschn-bench-interleaved --codecs lz4,spike-slots --corpus corpus/test --cpu 2 --out results
./build/quetschn-compare --baseline results/lz4.tsv --candidate results/spike-slots.tsv
```

The `quetschn-bench-*` binaries contain GPL-2.0-only kernel code, so they are GPL-2.0 works; they are
for measuring, not for distribution.

## Collecting pages

```sh
mkdir -p corpus
./build/quetschn-collect-resident --out corpus/desktop --max-per-process 2000
```

This samples resident anonymous memory of all processes you may read into `corpus/desktop.pages`
and `corpus/desktop.tsv`. The pages contain whatever was in memory, including keys and passwords.
`corpus/` is in `.gitignore`; never commit or publish these files.

The pages zram really holds, the ones reclaim swapped out, come from the zram device itself. zram
decompresses every page on read; the second `dd` runs as you and leaves free slots as holes:

```sh
mkdir -m 700 -p ~/quetschn-corpus
sudo dd if=/dev/zram0 bs=1M iflag=direct status=progress |
    dd of=$HOME/quetschn-corpus/zram0.raw bs=4096 iflag=fullblock conv=sparse
./build/quetschn-import-raw --in ~/quetschn-corpus/zram0.raw --out ~/quetschn-corpus/zram0
```

The dump has no process names, every page gets the name of the dump file. So a dump cannot be split
by process name; take a second dump some days later instead, train on one and measure on the other.
`tools/bench-dict.sh` does all of it: it trains a dictionary on the first dump without the pages
that are also in the second, then runs every codec with and without the dictionary on the second
dump and compares them with `quetschn-compare`:

```sh
CPU=2 tools/bench-dict.sh build ~/quetschn-corpus/zram0-mon ~/quetschn-corpus/zram0-thu \
    ~/quetschn-corpus/dict-mon-thu
```

## Licensing

quetschn is dual licensed under **`MIT OR GPL-2.0-only`**. You may use it under
either license, at your option.

- [LICENSE-MIT](LICENSE-MIT)
- [LICENSE-GPL-2.0](LICENSE-GPL-2.0)

The GPL-2.0-only option exists so the codec can be merged into the Linux kernel,
which requires GPL-2.0 compatibility. The MIT option exists so userspace and
non-GPL projects can use it too. This is the same approach zstd takes.

Every source file carries an SPDX identifier:

```c
// SPDX-License-Identifier: MIT OR GPL-2.0-only
```

Contributions are accepted under the same dual license. Sign off your commits
with `git commit -s` to certify the [Developer Certificate of
Origin](https://developercertificate.org/), as the Linux kernel requires.
