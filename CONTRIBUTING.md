# Contributing

quetschn is meant to end up in the Linux kernel, so the codec follows the kernel's rules, and every
design decision needs a measurement. This file says how to build, test and measure a change, and what
a change needs before it is merged.

## Before you try a design

Read [docs/explored-designs.md](docs/explored-designs.md) first. About 60 designs and choices are
in there, with their numbers, and most of the obvious ideas were already tried. Add an entry for
everything you measure, in the same pull request as the code, also and especially when it did not
work.

A design is judged by how much closer it gets to the goal, `lz4`'s speed at `zstd`'s size, by the score
of [the plan, §1.1](docs/plan.md#11-the-score-memory-against-time-not-bars): memory per stored page
against the mean time per page written. Two limits are hard:

- **C5:** the work memory per CPU is at most `lz4`'s 16 416 bytes.
- **C6:** the decoder is safe and bounded in time for any input.

The p99 of reads and writes against `lz4` (C2 and C3) are guards, not gates. A design is not dropped
because its p99 misses the bar, and none is kept just because it meets it. A p99 that gets clearly
worse is a reason to find out which pages cause it.

## Build

```sh
cmake -S . -B build -G Ninja -DQUETSCHN_WERROR=ON
cmake --build build
```

| option | what it does |
| --- | --- |
| `-DQUETSCHN_WERROR=ON` | warnings are errors, as in CI |
| `-DQUETSCHN_SANITIZE=ON` | ASan and UBSan |
| `-DQUETSCHN_KERNEL_TREE=<linux>` | builds zram's `lz4`, `lzo` and `zstd` from a Linux tree with the kernel's flags, and with them `quetschn-bench-*` and the kernel codec tests |
| `-DQUETSCHN_PAGE_BITS=14` | seqlz for 16 KiB pages. The tests run with it, except the kernel codecs', so configure it without a kernel tree |
| `-DQUETSCHN_FUZZ=ON` | the fuzz targets, needs clang or AFL++'s `afl-clang-lto` |
| `-DQUETSCHN_ALIGN_FUNCTIONS=ON` | every kernel codec function aligned to 64 bytes, for A/B comparisons of two builds |

CI builds with the kernel tree at the commit `KERNEL_COMMIT` in
[`.github/workflows/ci.yml`](.github/workflows/ci.yml), and fetches only the files the codecs need.

## Test

```sh
./build/quetschn_test
ctest --test-dir build
```

With a kernel tree, `ctest` also checks that seqlz built with the kernel's flags still has its
prefetch instructions: clang drops `__builtin_prefetch` without a warning when the kernel builds x86-64
without SSE.

Every bug fix and every change of behavior gets a test that fails without the change.

## Format

```sh
pip install clang-format==21.1.8
clang-format --dry-run --Werror $(git ls-files '*.cpp' '*.h' '*.c')
```

Pinned, because two versions of clang-format never agree on every line.

## Rules for the codec in `src/`

The codec goes into the kernel as it is, so:

- Plain C, `-std=gnu11`, freestanding: no libc, no floating point, no VLAs, no allocation.
- No SIMD, and no inline assembly. A speedup has to come from code a C compiler keeps.
- Stack frames well under the kernel's limit, below 256 bytes as a target.
- Big-endian has to work: read multi-byte values with explicit little endian loads, never by type
  punning. CI checks that s390x writes the same bytes as x86-64.
- No sleeping and nothing that may fault: zram may call the codec with preemption disabled.

The tools and the harness are C++20 and have none of these limits.

## Changing the format

Nobody uses seqlz yet, so the format may still change, the tables included. A change that does needs,
in the same pull request:

1. [docs/format.md](docs/format.md) updated: the rules, the SHA-256 of the tables, the worked example,
   the status.
2. Both reference decoders, `tools/seqlz-ref/seqlz_ref.py` and `tools/seqlz-ref/seqlz_ref.c`,
   changed from docs/format.md alone, not from `src/`. They exist to catch a spec that says something
   other than the code.
3. A long fuzz run before the merge: 1 billion inputs without a crash, the gate of
   [Phase 4](docs/plan.md#phase-4-reference-implementation-format-spec-fuzzing-20-weeks).

`tools/seqlz-ref/check.sh` checks the first two points in seconds, and CI runs it: the hashes in
docs/format.md against the tables in `src/`, and pages that `src/` compressed decoded by
`seqlz_ref.py`.

```sh
fuzz/smoke.sh 60 build-fuzz                 # what CI runs, a minute per target, needs clang
AFL=$HOME/AFLplusplus fuzz/afl.sh out 30    # 30 AFL++ instances in the background, a third per target
```

`quetschn-seqlz-train` trains the tables, and `src/seqlz_default_tables.c` says on which pages. With
`--counts bench/seqlz_counts_4k.txt` (or `_16k.txt`) it also writes the counts the tables are built
from; the test `seqlz_train` fails until the counts in `bench/` give the tables in `src/`.

## Measuring

[docs/measuring.md](docs/measuring.md) has the commands. In short: fix the clock, let nothing else
run while a benchmark times, use `tools/quick-bench.sh` while you try things out, and the full run or
the kernel VM for the numbers that go into a document. Every number that is published names the
hardware, the corpus and how it was measured.

Page dumps contain keys, passwords and personal data. They are never committed, and never published.

## Commits and pull requests

Commit messages start with the area, then what changed: `seqlz: the tables trained again, on swapped
pages too`, `docs: the whole page fault with the tables of 7 October`. The body says why, how, and
what it measured.

Sign off every commit with `git commit -s`. That certifies the
[Developer Certificate of Origin](https://developercertificate.org/), as the Linux kernel requires.
Contributions are accepted under the project's license, `MIT OR GPL-2.0-only`, and every source file
starts with its SPDX line:

```c
// SPDX-License-Identifier: MIT OR GPL-2.0-only
```

CI has one required check, `ci-ok`. It needs these to pass: the format check, the tests with gcc and
clang on x86-64 and arm64, the fuzz smoke run with 4 KiB pages, with MSan and with 16 KiB pages, the
kernel port built for x86-64, arm64, 32-bit arm and s390 with its KUnit tests, the same compressed bytes
on x86-64, 32-bit x86 and big-endian s390x, and the docs: `tools/seqlz-ref/check.sh` and
`tools/check-links.py`, both quick to run before a push.
