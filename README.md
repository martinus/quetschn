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

`--out` writes one line per page for paired comparisons. The `quetschn-bench-*` binaries contain
GPL-2.0-only kernel code, so they are GPL-2.0 works; they are for measuring, not for distribution.

## Collecting pages

```sh
mkdir -p corpus
./build/quetschn-collect-resident --out corpus/desktop --max-per-process 2000
```

This samples resident anonymous memory of all processes you may read into `corpus/desktop.pages`
and `corpus/desktop.tsv`. The pages contain whatever was in memory, including keys and passwords.
`corpus/` is in `.gitignore`; never commit or publish these files.

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
