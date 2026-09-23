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

The dump has no process names, every page gets the name of the dump file.

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
