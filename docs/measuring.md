# Measuring

How to get the numbers that the other documents report: in a kernel, in userspace, and on a phone.
The numbers that count come from a kernel, because a codec's time depends on everything around it.
The userspace harness is faster to run and is for trying out a change.

- [Pages to measure on](#pages-to-measure-on)
- [Fix the clock first](#fix-the-clock-first)
- [In the kernel](#in-the-kernel)
- [In userspace](#in-userspace)
- [On an Android phone](#on-an-android-phone)
- [Rules that came from getting it wrong](#rules-that-came-from-getting-it-wrong)

## Pages to measure on

Everything here runs on a **corpus**: `<base>.pages` with the pages, 4096 bytes each, and
`<base>.tsv` with one line per page. The paths in the commands below are examples, e.g. `corpus/test`
is the corpus `corpus/test.pages` and `corpus/test.tsv`.

> [!CAUTION]
> The pages contain whatever was in memory, keys and passwords included. `corpus/` and `*.pages` are
> in `.gitignore`. Never commit or publish these files, and delete copies on other machines, e.g. a
> phone, after use.

**Resident pages.** This samples the resident anonymous memory of all processes you may read:

```sh
mkdir -p corpus
./build/quetschn-collect-resident --out corpus/desktop --max-per-process 2000
```

**The pages zram really holds**, the ones reclaim swapped out, come from the zram device itself. zram
decompresses every page on read. The second `dd` runs as you and leaves free slots as holes:

```sh
mkdir -m 700 -p ~/quetschn-corpus
sudo dd if=/dev/zram0 bs=1M iflag=direct status=progress |
    dd of=$HOME/quetschn-corpus/zram0.raw bs=4096 iflag=fullblock conv=sparse
./build/quetschn-import-raw --in ~/quetschn-corpus/zram0.raw --out ~/quetschn-corpus/zram0
```

A dump has no process names, so take a second dump some days later, train on one and measure on the
other. [The plan, §5.3](plan.md#53-statistical-presentation) says why the pages that are trained on
must not be the ones that are measured. `quetschn-split-corpus` splits a corpus into a training and a
test part, and `quetschn-sample-corpus --pages 20000` draws a fixed random sample, the same one for the same
`--seed`, e.g. for the quick benchmark.

## Fix the clock first

The times depend on the clock of the CPU, cold reads most of all: with boost on, two identical runs
differed by up to 520 ns. On an AMD CPU with `amd-pstate`, for CPU 2 at 4.5 GHz:

```sh
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost
sudo cpupower -c 2 frequency-set -g performance
echo 4500000 | sudo tee /sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq
echo 4500000 | sudo tee /sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq
```

Every benchmark prints the frequency range and the boost state in its header. A number that goes into
a document needs the minimum equal to the maximum and `boost: 0`. A reboot resets it.

## In the kernel

`tools/zram-vm/run.sh` builds a kernel from a Linux tree with seqlz as a zram backend, boots it in a
VM pinned to CPU 2, and measures zram's reads and writes. With `MODE=swap` it measures the whole page
fault of a swap-out and a swap-in instead. zram stores a same-filled page without the codec, so a fault
on such a page is the kernel's part. `KARGS` adds to the kernel command line, and
`zram.zram_prefetch=8` gives seqlz the prefetch that its backend has. `LLVM=1` builds the kernel with
clang, as Android does.

```sh
ALGOS=lz4,lzo-rle,zstd,seqlz-lit tools/zram-vm/run.sh <linux tree> corpus/first >reads.log
KARGS=zram.zram_prefetch=8 MODE=swap ALGOS=lz4,lzo-rle,zstd,seqlz-lit \
    tools/zram-vm/run.sh <linux tree> corpus/first >swap.log
```

`tools/swap-fault/swap_fault.c` does the swap measurement on a running Linux as root, e.g. a rooted
phone, with zram devices that are not in use. `tools/plot-swap-fault.py` draws both, memory against
time, and `tools/plot-codecs.py` the reads and writes of the VM:

```sh
swap_fault corpus/first.pages 2 1 lz4 lzo zstd >cpu2.log    # pages, CPU, first zram index, codecs
tools/plot-swap-fault.py --row "PC=swap.log" --row "Phone, little core=cpu2.log" --out swap-fault.svg
tools/plot-codecs.py --run "first dump=reads.log" --out codecs.svg
```

Some options are for finding where a fault's time goes, all in `KARGS`:

- `zram.zram_warm=1` to `7` warms something right before the timed decompression: seqlz's tables, its
  code, the destination page, or a decode of another or of the same page.
- `zram.zram_pmu=1` counts cycles, instructions, branch and cache misses around every timed
  `zcomp_decompress()`, in the guest. Each read of a counter exits to the host, about 77 µs, so the times
  of such a boot are useless, only the counts are not.
- `quetschn.decomp=1` times `zcomp_decompress()` alone in the read benchmark too, and
  `quetschn.cond=3` runs only its condition "flushed, other page first".

See [explored-designs.md, "The decoder in a fault"](explored-designs.md#the-decoder-in-a-fault-028-µs-slower-than-in-zrams-read-benchmark-warm-caches-give-back-012)
and ["The decoder in a fault, found"](explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides).

> [!TIP]
> A single boot can be off: `zstd` came out 18% slower in one boot with nothing changed. Compare
> codecs within one boot, and repeat a boot before you trust a difference of a few percent.

## In userspace

The harness builds zram's `lz4`, `lzo`, `lzo-rle` and `zstd` from a Linux source tree with the
kernel's compiler flags, and seqlz with `lz4`'s. The kernel sources are compiled in place and never
copied into this repository.

```sh
cmake -S . -B build -G Ninja -DQUETSCHN_KERNEL_TREE=$HOME/linux
cmake --build build
./build/quetschn-bench-interleaved --codecs lz4,lzo-rle,zstd,seqlz-fast-lit --corpus corpus/test --cpu 2 --out results
./build/quetschn-compare --baseline results/lz4.tsv --candidate results/seqlz-fast-lit.tsv
```

`quetschn-bench-interleaved` runs all codecs in one binary, in turns on the same page, so that drift
of the clock hits all of them alike. Between codecs the kernel VM is still the better judge.
`--no-timing` gives only the sizes. `quetschn-compare` pairs two runs page by page: the saving, and the
difference of every latency percentile, with 95% confidence intervals from a bootstrap over the pages.

**For trying out a change, use the quick benchmark.** It takes about 90 s: exact sizes on the whole
corpus, and the times of a sample of 20 000 pages in 5 separate processes, with the median and the
range of the 5. Run the full `quetschn-bench-interleaved` only to confirm a result that goes into a
document.

```sh
tools/quick-bench.sh build corpus/test results lz4,lzo-rle,zstd,seqlz-fast-lit
```

**The score.** `quetschn-score` puts memory against time per page, the score of
[the plan, §1.1](plan.md#11-the-score-memory-against-time-not-bars), from the output of
`quetschn-bench-*` or of `tools/zram-vm/run.sh`. `r`, the reads per write, comes from
`quetschn-swap-bursts` on a running machine:

```sh
./build/quetschn-score --reads-per-write 0.34 run.txt
./build/quetschn-swap-bursts --seconds 3600
```

**Dictionaries and levels.** `--level` is zram's `algorithm_params` level, `--dict` a dictionary as
zram takes one. `tools/bench-dict.sh` trains one on one dump and measures every codec with and
without it on another. `quetschn-lz-analysis` prices the matches of `lz4hc` or of seqlz's matcher
with other encodings.

**For `perf`.** `--decode-loop <n>` compresses every page once and then only decodes, n times. With
`--cold` the compressed page is flushed and 2 MiB of other data read before each decode, and with
`--compress` it times the compression instead. The difference of two runs with different n, e.g. with
`perf stat -e cycles,instructions,branch-misses`, is the decoder alone.

> [!NOTE]
> The `quetschn-bench-*` binaries contain GPL-2.0-only kernel code, so they are GPL-2.0 works. They
> are for measuring, not for distribution.

## On an Android phone

For arm64 timings on a rooted Android phone, build with the Android NDK and run the binaries over
`adb`. The kernel tree is the same, and its codecs get the flags of an arm64 kernel build:

```sh
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static \
    -DQUETSCHN_KERNEL_TREE=$HOME/linux
cmake --build build-android
adb shell mkdir -p /data/local/tmp/quetschn
adb push build-android/quetschn-bench-interleaved corpus/test.pages corpus/test.tsv /data/local/tmp/quetschn/
```

On arm64 the harness counts cycles with `perf_event_open`, which needs root or
`setprop security.perf_harden 0`. Fix the clock of the cluster first, e.g. for cpu2 of a Snapdragon 730
at 1804.8 MHz, as root:

```sh
cd /sys/devices/system/cpu/cpu2/cpufreq
echo performance > scaling_governor
echo 1804800 > scaling_max_freq
echo 1804800 > scaling_min_freq
```

To compare two builds, configure both with `-DQUETSCHN_ALIGN_FUNCTIONS=ON`. With the kernel's
alignment, a change in one function moves the ones behind it, and on the phone's little core that
alone moved reads by 130 ns. `tools/phone-apps/` is the app launch test: 25 apps, switched in turn,
with the same RAM given to zram for every codec.

The pages are as private on the phone as anywhere else: delete them from `/data/local/tmp` afterwards.

## Rules that came from getting it wrong

Each of these cost a wrong result first. The measurements behind them are in
[explored-designs.md, "How the numbers are measured"](explored-designs.md#how-the-numbers-are-measured).

- **Nothing else runs while a benchmark times.** A build or a test run during a benchmark gave numbers
  that the next clean run did not reproduce.
- **Interleave, always.** Separate runs drifted by 6%, as much as the effects measured.
- **Reverse the codec order** when a difference is below about 200 ns. All codecs are in one binary,
  and the code layout of one shifts the others.
- **Several processes.** One process's confidence interval covers which pages were sampled, not which
  physical pages its buffers got. Five identical runs gave `zstd -1` against `lz4` from +1800 to
  +4620 ns, with about ±80 ns each.
- **Decode each page once, for the decoder's time.** A decode of the same page just before trains the
  branch predictor: `seqlz-fast-lit` then decodes a page in a fault in 1.41 instead of 2.01 µs. The read
  benchmark of `tools/zram-vm/run.sh` reads every page 6 times in a row, the userspace harness takes
  the median of 5 decodes, so both make `seqlz` look faster than it is in a swap-in. `MODE=swap` decodes
  each page once per pass and is the one to trust ([explored-designs.md](explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
- **Compressions apart from decompressions.** `lz4hc` touches 256 KiB when it compresses, and moved the
  next codec's cold reads by 300 ns. Every repetition times all compressions first, then all
  decompressions.
