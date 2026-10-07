# quetschn

Fast compression for memory pages, built for Linux zram.

The codec is `seqlz-fast-lit`: LZ sequences from a greedy matcher, Huffman coded with static tables,
one of 8 literal tables per page. It needs about as little memory as `zstd` and swaps in close to
`lz4`. On a Xiaomi Mi 9T, in its own kernel, with 20 000 pages from the phone's zram, the whole page
fault per page:

| | bytes per page | swap-in warm, A76 / A55 | swap-out, A76 / A55 |
| --- | --- | --- | --- |
| `lz4` | 1300 | 5.3 / 16.2 µs | 10.3 / 31.2 µs |
| `lzo` | 1216 | 6.4 / 17.4 µs | 11.2 / 33.9 µs |
| `zstd` 3 | 898 | 16.9 / 47.5 µs | 34.7 / 133 µs |
| `seqlz-fast-lit` | 935 | 6.0 / 17.9 µs | 12.4 / 38.8 µs |

The same on the PC and on the phone, memory against time:
[docs/plots/swap-fault-2026-10-07.svg](docs/plots/swap-fault-2026-10-07.svg).

- [PLAN.md](PLAN.md): the goal, the evidence behind it, the plan, and where the project stands (§9).
- [docs/seqlz.md](docs/seqlz.md): the codec, and why each choice was made.
- [FORMAT.md](FORMAT.md): the compressed bytes, fixed for 4 KiB pages.
- [docs/explored-designs.md](docs/explored-designs.md): every design that was measured, kept or not,
  with the numbers.

## Build and test

```sh
cmake -S . -B build -G Ninja -DQUETSCHN_WERROR=ON
cmake --build build
./build/quetschn_test
```

`-DQUETSCHN_SANITIZE=ON` builds with ASan and UBSan. Formatting is checked with clang-format 21.
`-DQUETSCHN_PAGE_BITS=14` builds seqlz for 16 KiB pages, whose format is not fixed yet.

## Checking seqlz

Two decoders are written from FORMAT.md alone, slow on purpose, and checked against `seqlz_decode()`:
`tools/seqlz_ref.py`, and `tools/seqlz_ref.c`, which the tests and the fuzzers use.

The fuzz targets in `fuzz/` take any input: the decoder alone, the roundtrip, and the decoder against
`tools/seqlz_ref.c`. CI runs each for a minute with libFuzzer, ASan and UBSan. A change to the format
or the codec gets a longer run before it is merged: 1 billion inputs without a crash, the gate of
PLAN.md's Phase 4.

```sh
fuzz/smoke.sh 60 build-fuzz                 # the CI run, needs clang
AFL=$HOME/AFLplusplus fuzz/afl.sh out 30    # 30 AFL++ instances in the background, a third per target
```

- `tools/seqlz-bound/`: per kind of field the bits a dump's pages take, next to their entropy.
- `tools/seqlz-worst/`: `quetschn-seqlz-worst` counts the instructions of every page of a corpus and
  writes made-up pages that are slow; `cost_fuzz.c` searches for slower ones, also for `lz4`, `lzo-rle`
  and `zstd`. The slowest pages found are in `tools/seqlz-worst/pages/`.
- `quetschn-seqlz-train` trains the tables; `explore/seqlz_default_tables.c` says on which pages.

## Measuring in the kernel

The numbers that count come from a kernel. `tools/zram-vm/run.sh` builds a kernel from a Linux tree
with seqlz as a zram backend, boots it in a VM pinned to CPU 2, and measures zram's reads and writes.
With `MODE=swap` it measures the whole page fault of a swap-out and a swap-in instead; a fault on a
same-filled page, which zram stores without the codec, is the kernel's part. `KARGS` adds to the
kernel command line, `zram.zram_prefetch=8` gives seqlz its backend's prefetch:

```sh
ALGOS=lz4,lzo-rle,zstd,seqlz-lit tools/zram-vm/run.sh <linux tree> corpus/first >reads.log
KARGS=zram.zram_prefetch=8 MODE=swap ALGOS=lz4,lzo-rle,zstd,seqlz-lit \
    tools/zram-vm/run.sh <linux tree> corpus/first >swap.log
```

`tools/swap-fault/swap_fault.c` does the swap measurement on a running Linux as root, e.g. a rooted
phone, with zram devices that are not in use. `tools/plot-swap-fault.py` draws both, memory against
time, `tools/plot-codecs.py` the reads and writes of the VM:

```sh
swap_fault corpus/first.pages 2 1 lz4 lzo zstd >cpu2.log    # pages, CPU, first zram index, codecs
tools/plot-swap-fault.py --row "PC=swap.log" --row "Phone, little core=cpu2.log" --out swap-fault.svg
tools/plot-codecs.py --run "first dump=reads.log" --out codecs.svg
```

Fix the clock of the CPU first, the times depend on it. On an AMD CPU with `amd-pstate`, for CPU 2 at
4.5 GHz:

```sh
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost
sudo cpupower -c 2 frequency-set -g performance
echo 4500000 | sudo tee /sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq
echo 4500000 | sudo tee /sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq
```

## Measuring in userspace

The harness builds zram's `lz4`, `lzo`, `lzo-rle` and `zstd` from a Linux source tree with the
kernel's compiler flags, and seqlz with `lz4`'s. The kernel sources are compiled in place and not
copied into this repository. The `quetschn-bench-*` binaries contain GPL-2.0-only kernel code, so they
are GPL-2.0 works, for measuring, not for distribution.

```sh
cmake -S . -B build -G Ninja -DQUETSCHN_KERNEL_TREE=$HOME/linux
cmake --build build
./build/quetschn-bench-interleaved --codecs lz4,lzo-rle,zstd,seqlz-fast-lit --corpus corpus/test --cpu 2 --out results
./build/quetschn-compare --baseline results/lz4.tsv --candidate results/seqlz-fast-lit.tsv
```

`quetschn-bench-interleaved` runs all codecs in one binary, in turns on the same page, so that drift
of the clock hits all alike; between codecs the kernel VM is still the better judge. `--no-timing`
gives only the sizes. `quetschn-compare` pairs two runs page by page: the saving and the difference of
every latency percentile, with 95% confidence intervals from a bootstrap over the pages.
`tools/quick-bench.sh` is the fast benchmark for trying out a change: exact sizes on the whole corpus,
times on a sample of 20 000 pages.

```sh
tools/quick-bench.sh build corpus/test results lz4,lzo-rle,zstd,seqlz-fast-lit
```

`quetschn-score` puts memory against time per page, the score of PLAN.md §1.1, from the output of
`quetschn-bench-*` or of `tools/zram-vm/run.sh`. `r`, the reads per write, comes from
`quetschn-swap-bursts` on a running machine:

```sh
./build/quetschn-score --reads-per-write 0.34 run.txt
./build/quetschn-swap-bursts --seconds 3600
```

`--level` is zram's `algorithm_params` level, `--dict` a dictionary as zram takes one;
`tools/bench-dict.sh` trains one on one dump and measures every codec with and without it on another.
`quetschn-lz-analysis` prices the matches of `lz4hc` or of seqlz's matcher with other encodings.

For `perf`, `--decode-loop <n>` compresses every page once and then only decodes, n times; with
`--cold` the compressed page is flushed and 2 MiB of other data read before each decode, with
`--compress` it times the compression instead. The difference of two runs with different n, e.g. with
`perf stat -e cycles,instructions,branch-misses`, is the decoder alone.

## Measuring on an Android phone

For arm64 timings on a rooted Android phone, build with the Android NDK and run the binaries over
`adb`. The kernel tree is the same; its codecs get the flags of an arm64 kernel build:

```sh
cmake -S . -B build-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_static \
    -DQUETSCHN_KERNEL_TREE=$HOME/linux
cmake --build build-android
adb shell mkdir -p /data/local/tmp/quetschn
adb push build-android/quetschn-bench-interleaved corpus/test.pages corpus/test.tsv /data/local/tmp/quetschn/
```

On arm64 the harness counts cycles with `perf_event_open`, which needs root, or
`setprop security.perf_harden 0`. Fix the clock of the cluster first, e.g. for cpu2 of a Snapdragon 730
at 1804.8 MHz, as root:

```sh
cd /sys/devices/system/cpu/cpu2/cpufreq
echo performance > scaling_governor
echo 1804800 > scaling_max_freq
echo 1804800 > scaling_min_freq
```

To compare two builds, configure both with `-DQUETSCHN_ALIGN_FUNCTIONS=ON`: with the kernel's alignment
a change in one function moves the ones behind it, and on the phone's little core that alone moved
reads by 130 ns. `tools/phone-apps/` is the app launch test.

The pages are as private on the phone as anywhere else: delete them from `/data/local/tmp` afterwards.

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

The dump has no process names, so take a second dump some days later, train on one and measure on the
other.

## Licensing

quetschn is dual licensed under **`MIT OR GPL-2.0-only`**. You may use it under either license, at
your option.

- [LICENSE-MIT](LICENSE-MIT)
- [LICENSE-GPL-2.0](LICENSE-GPL-2.0)

The GPL-2.0-only option exists so the codec can be merged into the Linux kernel, which requires
GPL-2.0 compatibility. The MIT option exists so userspace and non-GPL projects can use it too. This is
the same approach zstd takes.

Every source file carries an SPDX identifier:

```c
// SPDX-License-Identifier: MIT OR GPL-2.0-only
```

Contributions are accepted under the same dual license. Sign off your commits with `git commit -s` to
certify the [Developer Certificate of Origin](https://developercertificate.org/), as the Linux kernel
requires.
