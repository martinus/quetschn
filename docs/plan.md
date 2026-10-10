# The plan

*Or how to get a new compressor into zram, which has worked once and failed once.*

quetschn is a compression codec for memory pages, aimed at the Linux kernel's zram module: 4 KiB pages
first, and 16 KiB pages as a parameter from the start (§3.5). This document is the goal, the evidence
behind it, the phases with their gates, and where the project stands.

> [!NOTE]
> **Where it stands, 9th October 2026.** The codec, `seqlz-fast-lit`, its format and its fuzzing are
> done. In the phone's own kernel it stores pages 28% smaller than `lz4`, and swaps them in 11% to
> 13% slower warm and 17% to 25% slower cold. The question to the zram maintainers is
> [sent](https://lore.kernel.org/linux-mm/CAAFOosa2WLf--T17cujJu5CUsN12NNjgTitOvno2LY7jAr1gfw@mail.gmail.com/), the answer decides what comes next. The details
> are in [§9](#9-where-the-project-stands-and-the-next-actions).

Last verified against mainline `v7.3-rc1-324-g986c24e0fe44` (`986c24e0fe44`) on 22nd September
2026. zram's multi-page compression and its backend interface were checked again on 7th October 2026
(§3.4, §3.5), and against Sergey Senozhatsky's series of 5th October 2026, which is not merged yet, on
8th October 2026 (§3.2, §3.3, §3.4).

**Contents**

1. [Goal and success criteria](#1-goal-and-success-criteria)
2. [Precedent: what gets a compressor into zram](#2-precedent-what-gets-a-compressor-into-zram)
3. [The technical ground truth](#3-the-technical-ground-truth-verified-against-mainline)
4. [Constraints](#4-constraints)
5. [Phases](#5-phases)
6. [Repository layout](#6-repository-layout)
7. [What is deferred](#7-what-is-deferred)
8. [Risks, and what to do about each](#8-risks-and-what-to-do-about-each)
9. [Where the project stands, and the next actions](#9-where-the-project-stands-and-the-next-actions)

---

## 1. Goal and success criteria

**The goal is a codec merged into mainline Linux as a zram backend**, chosen because it is measurably
better than what is there already.

Better than every alternative on every axis is not possible, so it is not the target. `zstd` stores
pages smallest and will keep doing so: no small, scalar codec without allocation beats an entropy
coder with a trained dictionary on ratio. The target is the fast corner of the Pareto front, as these
criteria:

| | bar | why this bar | status |
| --- | --- | --- | --- |
| C1 | ≥ 8% less Σ zsmalloc cost (§3.1) than the better of `lzo-rle` and `lz4`, on the held-out test corpus (§5.3), same-filled pages excluded, 95% bootstrap confidence interval excluding 0 | `lzo-rle` is zram's default, `lz4` the common choice on Android. A gain of 1% does not pay for a new backend | met: 24% and 21% less than `lzo-rle` on two desktop dumps, 28% less than `lz4` on the phone |
| C2 | lower p99 decompression latency than `lz4`, cold cache, on x86-64 **and** on an arm64 little core, 95% bootstrap confidence interval excluding 0, latency statistic of §5.2 | decompression runs in the page fault (§3.2), and on a phone often on a little core | not met, see R10 |
| C3 | p99 compression latency within 1.2× of `lz4`, same statistic as C2 | zram compresses more often than it decompresses | at the bar: 1.21 and 1.19 times `lz4` |
| C4 | beats `lz4` **with a trained dictionary**, not only bare `lz4` | zram supports dictionaries, and Honor made `lz4` with a dictionary more than 50% faster in March 2026 (`f0f6f7871430`) | met: 20.4% less than `lz4` with a dictionary |
| C5 | per-CPU workspace ≤ `lz4`'s 16 416 B | `lz4` needs 16 416 B per CPU, `lzo` 16 384 B, `842` 61 440 B (§3.3) | met with 4 KiB pages: 8 KiB per CPU from `kmalloc()`, 16 KiB with the contexts split, against `lz4`'s 20 KiB from `vmalloc()`. With 16 KiB pages 32 KiB, as much as `lz4`, and 48 KiB with the split (§3.3) |
| C6 | the decompressor is fuzz-safe and bounded in time for any input | this is what stopped the last new codec (§2.2) | met: 3.2 billion fuzz inputs, the worst case measured |

C1 and C2 together were meant to be the merge argument: *strictly better than the current default and
the current fast option, on memory and on tail latency, with less per-CPU memory.* C2 did not hold, so
the argument is now memory at a small cost in time (R10).

**Merged is not the same as on phones.** This plan ends when the backend is in mainline. Android
kernels are built from `gki_defconfig`, and turning a new backend on there is a separate decision of
the Android kernel team, made after the mainline merge. Phase 7 has to plan for it.

**Not a goal:** beating `zstd` level 3 on ratio. If quetschn lands as `lz4`-class latency at
`zstd -1`-class ratio, that is a win.

### 1.1 The score: memory against time, not bars

C1 to C3 are bars for the merge argument, and they stay the numbers to report there. As a target for
design choices they are cliffs: a design that saves 4% of memory fails because its p99 is 1 µs above
`lz4`'s, without asking what 1 µs is worth or how often the page is read (#37). So designs are chosen
by a score instead:

- **bytes**: zsmalloc memory per stored page, `mem_used_total / pages` in the VM of Phase 5, after
  recompression where the device recompresses.
- **time**: per page written to zram, `write + r * read + b * recompression`, each the **mean** over the
  pages, in µs. The mean, because it is the total time spent on the pages, and a burst of n swap-ins,
  e.g. after switching back to an app, waits for n decompressions. p99 per page stays a guard against
  pathological pages (C6), not a target.
- **score**: `bytes + lambda * time`, lower is better, with lambda in bytes per µs.

`r` is the number of reads per write. On a desktop with zram as its only swap it was 0.34: 3 146 179
pages swapped in against 9 210 320 swapped out in 27.5 days (`pswpin` and `pswpout` of
`/proc/vmstat`). That is one machine; a phone swaps differently and needs its own number. `b` weighs
the time of recompression, which zram runs on idle pages when told to, against the time of reads and
writes, which a task waits for or kswapd spends. `b = 1` counts every µs the same. A recompression on
an otherwise idle CPU costs energy and not latency, which would argue for less. The choice of `b`
decides whether recompression pays at all, so every result states it.

lambda is not fixed. `quetschn-score` reads the logs of `tools/zram-vm/run.sh` or of
`quetschn-bench-*` and prints the codecs that have the lowest score for some lambda, the lower left
convex hull of (time, bytes), with the exchange rate between neighbours: the lambda at which the
faster one starts to win. A design is worth keeping if it is on the hull at a lambda that matters.

**`seqlz-fast-lit` has the lowest score for any lambda from 3.1 to 155 bytes per µs on the first
desktop dump and from 10.3 to 211 on the second**, with the times of a swap-out and a swap-in in the VM
of Phase 5, `r = 0.34`, `b = 1`, the tables of 7 October
([explored-designs.md, "The score with the swap times"](explored-designs.md#the-score-with-the-swap-times-seqlz-fast-lits-range-ends-at-155-and-211-bytes-per-µs-instead-of-200-and-279)).
The hull is `lzo-rle`, `seqlz-fast`, `seqlz-fast-lit` and `zstd` 3, with `lz4` next to `lzo-rle` within
the noise. From `seqlz-fast-lit` to `zstd` 3 it is 3.1 and 10.3 bytes per µs, from `seqlz-fast` to
`seqlz-fast-lit` 155 and 211. Whoever runs `lz4` or `lzo-rle` instead of `zstd` says that lambda is above
3 to 10 for them. With the times of zram's read benchmark, which decodes every page several times, the
range went up to 200 and 279
([explored-designs.md, "The designs by the score"](explored-designs.md#the-designs-by-the-score) has the
older tables). Recompression is not pursued:
with `zstd` it pays only where the CPU time of an idle machine counts for less, and it makes each read
of a recompressed page 2.3 µs slower. `seqlz-opt` is removed
([explored-designs.md, "Recompression, measured, not pursued"](explored-designs.md#recompression-measured-not-pursued)).

**Levels 3 and 4 (#139) were measured and are not kept.** The same format with a matcher that searches
a hash chain and prices each match by the tables: level 3 stores 2.7% to 4.3% less than
`seqlz-fast-lit` on four dumps, as much as `zstd` 3 on two of them, for about twice the write time. In
the VM it follows `seqlz-fast-lit` on the hull at 4.2 and 8.3 bytes per µs on two dumps and is off it
on the other two, also with a literal table of each page's own, which made it 2.5% to 3.4% smaller
([explored-designs.md](explored-designs.md#levels-3-and-4-a-hash-chain-priced-by-the-tables-as-small-as-zstd-3-on-two-of-four-dumps-and-faster-to-write-and-read-not-kept),
[and the own table](explored-designs.md#a-literal-table-of-the-pages-own-at-levels-3-and-4-25-to-34-less-a-second-literal-path-in-the-format-not-kept)).

**The time in the score is the codec's alone**, which is what the codecs differ in. A task waits for
the whole page fault: on the PC 1.9 µs of it are the kernel's for every codec, and `lz4`'s
decompression is 1.4 µs of a cold swap-in of 3.5 µs, `seqlz-fast-lit`'s 2.0 µs of 4.1 µs. So a codec
40% slower than `lz4` makes a swap-in about 20% slower
([explored-designs.md, "The whole page fault"](explored-designs.md#the-whole-page-fault-the-kernels-part-is-the-same-for-every-codec-the-gap-to-lz4-about-halves)).
In the fault the gap between the two decoders is 0.28 µs larger than in zram's read benchmark with
`O_DIRECT`, which the hull above counts now: the swap times have the kernel's part in them too, the
same for every codec, which moves no exchange rate. The read benchmark decodes every page several
times, which trains the branch predictor on it: in a fault `seqlz-fast-lit` mispredicts about 110
branches per page more, and cold caches add 0.12 µs ([explored-designs.md, "The decoder in a fault"](explored-designs.md#the-decoder-in-a-fault-028-µs-slower-than-in-zrams-read-benchmark-warm-caches-give-back-012)).

Not measured yet: how large the bursts of swap-ins are, and `r` on a phone. `quetschn-swap-bursts`
samples `pswpin` every 10 ms and groups the swap-ins into bursts. 9 minutes on the development machine
saw 1 swap-in: with 56 GB free it does not swap. It needs a machine under memory pressure, a phone
best.

C5 and C6 stay hard limits. Per-CPU memory is taken from every CPU whether it swaps or not, and an
unsafe decoder is not a trade-off. C1 to C4 are reported for the merge argument.

---

## 2. Precedent: what gets a compressor into zram

Two cases, both verified.

### 2.1 The success: lzo-rle (Dave Rodgman, ARM, merged March 2019, `5ee4014af99f`)

What the merged commit message contains. Quotes are verbatim, the rest is paraphrased:

- A real corpus: *"captured some memory via /dev/fmem from a Chromebook with many tabs open which is
  starting to swap, and then split this into 4178 4k pages"*, with all-zero pages excluded as zram
  does.
- A distribution, not an average: *"the data is VERY bimodal: 44% of pages in this dataset contain 5%
  or fewer zeros, and 44% contain over 90% zeros"*.
- Measurements on **both** arm64 and x86-64.
- A *weighted roundtrip throughput*, weighted because zram compresses more than it decompresses.
- Regression analysis: *"0.1% (3/4178) of cases had a regression > 1 standard deviation, of which the
  largest was 4.6% (1.2 standard deviations)"*.

It took from November 2018 to March 2019 and reached **v5** of the patch series. That was an
increment to an existing codec, from an ARM engineer, as paid work.

That commit message is the template. The plan below is built to produce exactly that kind of evidence.

### 2.2 The failure: zBeWalgo (Benjamin Warnke, 2018, reached v7, never merged)

The same problem statement as quetschn: *"ZRAM compresses each page individually. As a result the
compression algorithm is forced to use a very small sliding window. None of the available compression
algorithms is designed to achieve high compression ratios with small inputs."* It did not get in.

Eric Biggers's objections are the checklist quetschn has to pass before the first patch is sent:

1. **No rigorous comparison to zstd.** *"This still isn't a valid excuse for not comparing it to
   Zstandard."*
2. **No formal format specification.** The established codecs had a spec and a tested userspace
   library before they went into the kernel.
3. **Not fuzz-safe.** *"You cannot just ignore fuzz-safety in the decompressor either."* He asked for
   a userspace port fuzzed with afl.
4. **No demonstrated roundtrip correctness.**
5. **An unstable format**, which needed an explicit opt-in.

Separately, Minchan Kim hit a kernel panic (`BUG: sleeping function called from invalid context`) and
wrote *"Unfortunately, I don't have the time to look into."* Maintainer time is the scarcest resource
in this project, so every patch has to be clean on the first read.

One obstacle from 2018 is gone. zBeWalgo had to go through the crypto API, but since 2024 zram has its
own backend API (`917a59e81c34`, "zram: introduce custom comp backends API"). So quetschn touches only
`lib/` and `drivers/block/zram/`, and the format-stability objection is weaker: zram data does not
survive a reboot.

---

## 3. The technical ground truth (verified against mainline)

Five facts shape the codec and the benchmark. Public compression benchmarks miss most of them.

### 3.1 zram does not pay for bytes, it pays for zsmalloc size classes

`zram_write_page()` calls `zs_malloc(pool, comp_len, ...)`, and zsmalloc rounds the size up to a class
(`mm/zsmalloc.c`):

```
ZS_ALIGN            = 8
ZS_HANDLE_SIZE      = 8          /* added to the request before class lookup */
CLASS_BITS          = 8
ZS_SIZE_CLASS_DELTA = PAGE_SIZE >> CLASS_BITS   = 16 bytes on 4 KiB pages
ZS_MIN_ALLOC_SIZE   = 32
ZS_SIZE_CLASSES     = 255
```

Two more details decide the real cost:

- `zs_create_pool()` walks the classes from the largest to the smallest and **merges** a class into
  the larger one before it when both have the same `(pages_per_zspage, objs_per_zspage)`. An object
  that maps to a merged class gets the slot size of the larger class.
- `calculate_zspage_chain_size()` picks `pages_per_zspage`, 1 to `CONFIG_ZSMALLOC_CHAIN_SIZE`, with
  the least tail waste, but the waste is often not 0. A class really costs
  `pages_per_zspage × PAGE_SIZE / objs_per_zspage` bytes per object, which is more than its slot size.

The cost of a page compressed to `comp_len` bytes, 64-bit, 4 KiB pages,
`CONFIG_ZSMALLOC_CHAIN_SIZE=8`, the default. The numbers come from `bench/zsmalloc_cost.cpp`, a port of
the merge loop and of `calculate_zspage_chain_size()` at `986c24e0fe44`. It reproduces the huge class
watermark for all 13 chain sizes and the class listings in `Documentation/mm/zsmalloc.rst`, except one
row of the listing for chain size 16 that contradicts the kernel code (see
`test/zsmalloc_cost_test.cpp`). `huge_class_size` depends on the config, so the harness has to read it
from `/sys/kernel/debug/zsmalloc/` or compute it, never hardcode it:

| comp_len | class slot | pages / objs per zspage | cost per page |
| ---: | ---: | ---: | ---: |
| 100 | 112 | 7 / 256 | 112.0 |
| 111 | 128 | 1 / 32 | 128.0 |
| 1024 | 1056 | 8 / 31 | 1057.0 |
| 2048 | 2176 | 8 / 15 | 2184.5 |
| 3255 | 3264 | 4 / 5 | 3276.8 |
| **3624** | **3632** | 8 / 9 | **3640.9** |
| **3625** | huge | 1 / 1 | **4096.0** |
| 4000 | huge | 1 / 1 | 4096.0 |

After merging, the 255 nominal classes collapse into 119 distinct ones. The step between them grows
with the size:

| size range | mean step | max step |
| --- | ---: | ---: |
| 0 - 1 KiB | 17 B | 32 B |
| 1 - 2 KiB | 28 B | 128 B |
| 2 - 3 KiB | 68 B | 144 B |
| 3 KiB - cliff | 93 B | 144 B |

Which means that:

- **Saving less than one class step on a page usually saves nothing.** Below 1 KiB that is about 16
  bytes, above 2 KiB it is 68 to 144 bytes. A small format header is free for large pages. That leaves
  room in the design: a mode per page, padding for a faster decoder, and explicit length fields cost
  almost nothing.
- **There is a cliff at `huge_class_size`, 3625 bytes.** Above it, `zram_write_page()` calls
  `write_incompressible_page()` and stores all 4096 bytes. Moving one page from 3625 to 3600 bytes saves
  455 bytes, 18 times the 25 bytes of compression it took. The cliff is the largest step, but every
  class boundary is a small cliff: a page a few bytes above a boundary pays the full step. *Pages just
  above a class boundary are worth extra compression effort, the ones just above the cliff most of
  all.* No general-purpose codec knows this.
- **Two effects decide how much memory the classes really use.** The **tail waste** inside a zspage is
  fixed per class and is already in the cost column above, so a userspace model gets it exactly.
  **Fragmentation** from partly filled zspages depends on the size distribution and on the allocation
  history, so two codecs with the same Σ cost can still use different amounts of memory. Only a real
  zram run measures it (Phase 5). On the development machine the difference was large: for the pages
  of the first zram dump (Phase 1) the model gives 604 MB, and zram's `mem_used_total` was 954 MB. Some
  minutes later zram had compacted (`pages_compacted` from 641 229 to 725 853) and used 628 MB for 3%
  more pages, close to the model. Fragmentation comes and goes, and at its worst it is larger than the
  differences between the codecs. On a new device, without frees, the model is exact
  ([explored-designs.md](explored-designs.md#zrams-memory-against-the-model-exact-on-a-new-device-σ-zsmalloc-cost-035-to-047-below-it)).

So the ratio metric of this project is **Σ zsmalloc cost**, the last column, plus the **number of pages
over the cliff**. Not the mean compression ratio.

### 3.2 Decompression is a tail-latency problem, not a throughput problem

`zcomp_decompress()` runs in the swap-in path. In August 2026 an RFC of Sergey Senozhatsky reported
that since `2efa9e9eb4db` ("zram: permit preemption with active compression stream") the zram stream
mutex became *the top lock contributing to Android UI frame drops, surpassing `mmap_lock`*. The
proposed fix adds an `async` flag per backend and brings `preempt_disable()` back for synchronous
backends on `!PREEMPT_RT`.

On 5th October 2026 Sergey posted a series that takes another path,
[PATCH 00/10 "zram: redesign zcomp and rework backends"](https://patchwork.kernel.org/series/1179613/).
Its patch 9, suggested by Barry Song, splits each per-CPU stream into a write stream for compression
and a read stream for decompression, each with its own mutex. So a reader never waits for a writer
that was preempted while it held the stream. Sergey measured it with fio, `zstd` level 12 and
`preempt=full`: on 1 CPU the read p99.99 went from 387 974 µs to 53.5 µs, on 24 CPUs the read IOPS
from 354k to 997k and the read p99.99 from 5669 µs to 137 µs. The rules below hold for both fixes.

What follows for the design:

- quetschn has to be a **synchronous backend**: no sleeping, no allocation, no page faults in
  compress or decompress, and correct with preemption disabled.
- The metric is the **latency distribution per page**, p50, p99, p99.9 and max, not MB/s.
- **A bound on the worst-case runtime** is a feature to advertise.
- Branch mispredictions dominate at 4 KiB inputs, and an LZ loop over literals and matches has
  branches that depend on the data. A word-granular format with control flow that barely depends on
  the data could win p99 even where it ties at p50. That was the most likely source of a C2 win, and
  an unverified assumption: on an in-order little core (Cortex-A53, A55, A520) the balance between
  branch misses and instruction count is different. The spike of Phase 2b tested it, and the win came
  from somewhere else.
- **Cold caches.** In a real page fault the compressed source and the destination page are both cold.
  A benchmark that loops over one page in L1 measures the wrong thing. The harness needs a cold-cache
  mode (§5.2), which alone may reorder the existing codecs.

With the wait for the lock gone, the decoder's own time is a larger part of what a task waits for in a
swap-in. That makes next action 2 in §9, fewer data-dependent branches in the decoder, and R10 matter
more.

### 3.3 Per-CPU workspace is real memory on a phone

`zcomp_strm_init()` already allocates 3 pages per CPU, `local_copy` of 4 KiB and `buffer` of 8 KiB.
On top of that:

| backend | compress workspace per CPU |
| --- | --- |
| `lzo` / `lzo-rle` | `LZO1X_1_MEM_COMPRESS` = 8192 × 2 = **16 384 B** |
| `lz4` | `LZ4_MEM_COMPRESS` = ((1<<11)+4) × 8 = **16 416 B** |
| `842` | `SW842_MEM_COMPRESS` = 0xf000 = **61 440 B** |
| `zstd` | `zstd_cctx_workspace_bound()`, `kvzalloc`'d, far larger |
| **quetschn** | **≤ 16 416 B, less is a bonus** |

On a phone with 16 cores, every 4 KiB below `lz4` saves 64 KiB, resident for good. That is small, but
it is a free line in the cover letter. The target was 4 KiB at first. It is `lz4`'s 16 KiB since 23rd
September 2026: 8 points less memory on the compressed pages are worth far more than a few KiB per
CPU, and a budget of 4 KiB costs speed or ratio in the matcher (`seqlz-fast` has a hash table of 8 KiB,
see [explored-designs.md](explored-designs.md)).

The split into read and write streams of October 2026 (§3.2) keeps the 3 pages: `buffer`, 2 pages,
goes with the write stream, `local_copy`, 1 page, with the read stream. A backend gets two contexts
too, one from `create_cctx` for compression and an optional one from `create_dctx` for decompression.
`seqlz-fast-lit` splits without extra memory asked for, because compression uses only the hash table
and decompression only the scratch for the coded literals (`struct seqlz_state` and `SEQLZ_SCRATCH` in
[`src/seqlz.h`](../src/seqlz.h)).

What counts is what `kmalloc()` and `vmalloc()` hand out, not what is asked for. `kmalloc()` rounds up
to a power of 2 up to `KMALLOC_MAX_CACHE_SIZE`, 2 pages; `vmalloc()` to whole pages. Without the split,
the backend's one context per CPU is a union of the hash table and the scratch: zram holds the stream's
mutex for either, and neither keeps anything from one page to the next
([`tools/kernel-port/backend_seqlz.c`](../tools/kernel-port/backend_seqlz.c)). With the split the two are
separate allocations, and the scratch, 16 bytes more than a page, takes the next larger cache:

| page size | hash table | literal scratch | one context, the union | split contexts |
| --- | ---: | ---: | ---: | ---: |
| 4 KiB | 8192 B | 4112 B | 8192 B asked, 8192 B allocated | 12 304 B asked, 16 384 B allocated |
| 16 KiB | 16 384 B | 16 400 B | 16 400 B asked, 32 768 B allocated | 32 784 B asked, 49 152 B allocated |

`lz4` asks `vmalloc()` for 16 416 B, which takes 5 pages, 20 480 B, with 4 KiB pages, and 2 pages,
32 768 B, with 16 KiB pages. `lzo-rle` asks
`kzalloc()` for 16 384 B. At level 1 the split has no decompression context. A scratch of 4096 B would
save the second 4 KiB of the split; the decoder's copies of 16 bytes read past the last literal, which
is what the 16 bytes are for.

The tables are global, not per CPU: 43 280 B of `__ro_after_init`, which is `PROGBITS`, so they are
zeros in the image until `seqlz_init()` builds them, in `vmlinux` or in the module. `SEQLZ` is built in
even with `ZRAM=m`, because the bool `ZRAM_BACKEND_SEQLZ` selects it, as `ZRAM_BACKEND_LZ4` selects
`LZ4_COMPRESS`. Both are for the cover letter, or for tables built at compile time (#153).

`seqlz-fast` needs no decompression context. The harness (`bench/kernel_codecs/zram_codec.h`) and the
kernel's backend (`tools/kernel-port/backend_seqlz.c`) have the two contexts since #102, the
backend for a tree with the series; at level 1 it has no decompression context.
`lz4`'s 16 416 B are for compression only, and after
patch 5 of the series `lz4` has no decompression context at all. So C5 has a question now that it did
not have with one context: is the limit for the compression context, for each context, or for the
sum? With 16 KiB pages the compression context alone, 16 KiB allocated, is below `lz4`'s 32 KiB, the
sum of the split contexts, 48 KiB, is not. This is not decided yet, and next action 4 in §9 depends on it.

### 3.4 The integration surface is small

Adding a backend is a contained diff:

- `lib/quetschn/` and `include/linux/quetschn.h`: the codec.
- `drivers/block/zram/backend_quetschn.{c,h}`: about 150 lines, modelled on `backend_lz4.c`. With
  the October series (§3.2) it has `create_cctx` and `create_dctx` instead of `create_ctx`.
- One entry in the `backends[]` array in `drivers/block/zram/zcomp.c`.
- `drivers/block/zram/Kconfig` and `Makefile`, `MAINTAINERS`, `Documentation/admin-guide/blockdev/zram.rst`.

`struct zcomp_params` carries `dict`, `dict_sz` and `level`, so a dictionary is possible and has to be
considered from the start (C4).

This may change. In March 2026 Sergey Senozhatsky wrote that the zcomp API is to be deleted
"sometime this year" in favour of the acomp crypto API
([his reply to RFC v2 "zram: Allow zcomps to manage streams"](https://lore.kernel.org/r/aa9tz-EIh7kOF3RM@google.com)). At
`986c24e0fe44` the zcomp backends are still there. With acomp, a new codec comes as a crypto algorithm
in `crypto/` plus the codec in `lib/`, and Herbert Xu's subsystem reviews it too (R11).

Since then the evidence points the other way. The October series of §3.2 redesigns zcomp instead of
removing it, and nothing in its 10 patches moves zram to acomp. On 30th September 2026 Qualcomm posted
a new zcomp backend, `qpace-lz4` (R12). acomp stays the other possible form.

### 3.5 Page size is not always 4 KiB

Android supports kernels with 16 KiB pages on arm64, and since November 2025 Google Play requires apps
that target Android 15 or newer to work on them. The phones this plan targets will more and more run
with 16 KiB pages. zram compresses one `PAGE_SIZE` page at a time, and zsmalloc scales with it:
`ZS_SIZE_CLASS_DELTA = PAGE_SIZE >> CLASS_BITS` is 64 bytes on 16 KiB pages. The model of §3.1 gives:

| | 4 KiB pages | 16 KiB pages |
| --- | ---: | ---: |
| class delta | 16 B | 64 B |
| distinct classes after merging | 119 | 121 |
| `huge_class_size` | 3625 | 14 553 |
| mean class step in the top quarter below the cliff | 93 B | 371 B |

Which means that:

- `PAGE_SIZE` is a parameter of the format, of the cost model and of the harness from the start. A
  codec tuned by hand for 4096-byte inputs, e.g. with 12-bit offsets baked into the format, is a dead
  end.
- The corpus needs 16 KiB pages too (Phase 1). Four adjacent 4 KiB pages are a first approximation,
  but real kernels with 16 KiB pages allocate differently.
- Larger inputs help `lz4` and `zstd` more than a word model, because their match window gets 4 times
  the history. quetschn's advantage may shrink on 16 KiB pages. That has to be measured, not assumed.

The same argument holds for multi-page compression in zram, e.g. whole large folios (mTHP) compressed
as one unit. Tangquan Zheng (OPPO) proposed it in March 2024, "mTHP-friendly compression in zsmalloc
and zram based on multi-pages". At `986c24e0fe44` nothing of that kind is merged in
`drivers/block/zram/` or `mm/zsmalloc.c`. What is merged is the swap-in of large folios from zram,
page by page, and in 2026 no newer version of the series was on the lists. Neighbouring swap slots
compressed as blocks of 16 KiB give `zstd` 3 13% to 22% less memory than `seqlz-fast-lit` on 4 KiB
pages ([explored-designs.md, "The ideas of #29 and #31"](explored-designs.md#the-ideas-of-29-and-31-measured)).
It is tracked as R9.

---

## 4. Constraints

### Kernel code rules

These are not negotiable:

- Plain C, `-std=gnu11` (kernel `Makefile:813`), no libc, no floating point, no VLAs.
- No SIMD. `kernel_neon_begin()` and `kernel_fpu_begin()` cost time and are not allowed in every
  context. All compressors in the kernel are scalar, so the SSE work on memlz does not transfer.
- Stack frames well under `CONFIG_FRAME_WARN`, 2048 on 64-bit. The target is below 256 B.
- Big-endian has to work, zram still gets big-endian fixes (`a8b5875741d4`, September 2026). Use
  `get_unaligned_le64()` from `<linux/unaligned.h>`, never type punning.
- An SPDX identifier in every file, and `scripts/checkpatch.pl --strict` clean.
- `Signed-off-by:` on every patch, the DCO.

### Project constraints

- **License: `MIT OR GPL-2.0-only`**, with the SPDX line in every file, as zstd does it. The MIT half
  lets userspace and other kernels use it, which is how a "this has users" argument gets built.
  Apache-2.0 is out, it is not compatible with GPL-2.0.
- **Time: 3 to 8 hours a week.** The whole path is 18 to 24 months. Every phase is sequenced so that it
  is worth publishing on its own, and a stall does not waste the work before it.
- **Hardware: x86-64, and an old phone from Phase 2 on.** arm64 numbers are required before
  submission: lzo-rle was merged on arm64 evidence first, and phones are the users. The old phone has
  an out-of-order big core and an in-order little core (Cortex-A76 and A55), which is what two boards
  would have given, on real phone silicon. It needs an unlockable bootloader, for root to read other
  apps' memory and to pin the CPU frequency, and Android 10 or newer. Phase 6 adds a current phone and
  16 KiB pages, and it **gates Phase 7**.

---

## 5. Phases

Each phase ends in an artifact and a gate. The durations are calendar weeks at 3 to 8 hours a week.

| phase | what | status |
| --- | --- | --- |
| [0](#phase-0-repository-foundation-2-weeks) | repository, license, CI | done |
| [1](#phase-1-corpus-tooling-4-weeks) | collecting pages | done for zram dumps of a desktop and of a phone |
| [2](#phase-2-benchmark-harness-and-baseline-8-weeks) | harness and baseline | done, the gate passed, not published yet |
| [2b](#phase-2b-decoder-latency-spike-2-weeks) | decoder latency spike | done on x86-64 |
| [3](#phase-3-page-analysis-and-design-exploration-12-weeks) | design exploration | done: `seqlz-fast-lit` |
| [4](#phase-4-reference-implementation-format-spec-fuzzing-20-weeks) | spec, reference decoders, fuzzing | done on 9th October 2026 |
| [5](#phase-5-kernel-port-and-validation-in-a-vm-16-weeks) | kernel port, validation in a VM | done, the gate passed on 8th October 2026 |
| [6](#phase-6-arm64-validation-a-hard-gate-before-phase-7) | arm64 | half: one phone, 4 KiB pages |
| [7](#phase-7-upstreaming-6-months-or-more-expect-v5) | upstreaming | the question to the maintainers is [sent](https://lore.kernel.org/linux-mm/CAAFOosa2WLf--T17cujJu5CUsN12NNjgTitOvno2LY7jAr1gfw@mail.gmail.com/), 9th October 2026 |

### Phase 0: repository foundation (2 weeks)

- [x] `LICENSE-MIT`, `LICENSE-GPL-2.0`, SPDX headers, `README.md`, `CLAUDE.md`.
- [x] `.gitignore` with `corpus/` and `*.pages` from the first commit. **Page dumps are never committed
  or published**, they contain keys, passwords and personal data.
- [x] CMake, C++20 for the tools and the harness, C11 for the codec.
- [x] CI from the first day: ASan and UBSan, gcc and clang, arm64, the same bytes on big-endian s390x
  under qemu and on 32-bit x86, the spec's table hashes, the links of the docs.
- [x] A CI job that compiles the codec with the kernel's flags
  (`-std=gnu11 -ffreestanding -nostdinc -Wframe-larger-than=256 -fno-builtin`), and `checkpatch.pl`:
  `kernel-port` builds the kernel's copy in a kernel tree with W=1 for x86-64, arm64, arm and s390, and
  runs checkpatch and kernel-doc on it.
- [x] [`CONTRIBUTING.md`](../CONTRIBUTING.md), with `Signed-off-by:` (DCO) as the kernel does it.
- [x] Get the old phone and root it: a Xiaomi Mi 9T, rooted on 3rd October 2026.
- [x] Check the employer rules (R8). Side projects are fine.

*Gate: CI green on an empty stub codec.* Passed.

### Phase 1: corpus tooling (4 weeks)

The corpus decides everything after it. Two collectors, because they answer different questions:

1. **Resident anonymous pages.** Walks `/proc/<pid>/maps`, reads anonymous private mappings with
   `process_vm_readv()`, writes pages. Cheap, broad, and biased.
2. **The pages that reclaim swaps out, the better one.** Rodgman sampled *resident* memory, but zram
   stores *cold, reclaimed* pages, which are a different distribution. Capture the real thing, the
   cheapest way first:
   - Read the zram device itself: `dd if=/dev/zram0 bs=4096`. zram decompresses every stored page on
     read, so this returns exactly the pages that reclaim put there, and free slots read as zeros,
     which the tools skip as same-filled anyway. It needs root, no kernel patch and no BPF, and works
     the same on a rooted phone. Honor collected the data for `f0f6f7871430` this way (their patch also
     sets `huge_class_size` to 0, for some reason; reading does not need that).
   - Swap to a plain block device in the VM instead of zram, zero it before `mkswap`, drive the VM into
     memory pressure, and read the swap device from the host. It holds exactly the pages that reclaim
     chose. The catches: freed slots keep stale pages, so snapshot while the workload is still under
     pressure, and reclaim behaves a bit differently with slow disk swap than with zram, which
     `swappiness` only partly corrects.
   - Only if those catches matter: a small BPF or kprobe tool on `zram_write_page()`, or a locally
     patched zram with a debugfs page dumper.

Both record per page: all zero, same-filled (zram handles these before any codec, in
`page_same_filled()`), zero density, byte entropy, and the source workload.

**Workloads**, scripted and reproducible in a VM: Firefox with a fixed list of URLs, a JVM service, a
Python and numpy job, a `make -j` kernel build, a GNOME session, PostgreSQL, Electron.

**Android pages without a phone.** Cuttlefish, the AOSP virtual device, runs on x86-64 with zram
enabled and runs real ART. ART's object layout barely depends on the architecture (compressed 32-bit
references either way), so an x86-64 Cuttlefish with a scripted mix of apps gives Android heap pages.
They do not replace phone data, but they show early whether Android pages look different from desktop
pages (R4).

**16 KiB pages.** An old phone runs 4 KiB pages. Only Pixel 8 and newer offer 16 KiB pages, as a
developer option, and a Raspberry Pi 5 runs them by default. Until one of them is available, four
adjacent 4 KiB pages of the same mapping are an approximation (§3.5).

**Two corpora**, for reproducibility:

- *private*: real dumps, which never leave the machine, for the headline numbers.
- *public*: made by the scripted VM workloads, free to share, so that others can reproduce the
  comparison without trusting anyone's private data.

Where it stands:

- [x] Collector 1, `quetschn-collect-resident` in `tools/collect/`. It finds resident pages with the
  `PAGEMAP_SCAN` ioctl (Linux 6.7 and newer) and falls back to `/proc/<pid>/pagemap` on older kernels,
  so it runs on the old phone too. Pages that are swapped already are counted but never read, because
  reading them would swap them back in. A first run on the development machine (Fedora, zram swap)
  found 1.8 million resident and 313 000 swapped pages.
- [x] Collector 2 with `dd`: `quetschn-import-raw` turns a `dd` of `/dev/zram0` into a corpus, see
  [measuring.md](measuring.md#pages-to-measure-on). The first dump of the development machine has
  460 923 pages that are not zero. It also checks the harness against the kernel: `lzo-rle` built in
  userspace reproduces zram's `mm_stat` for these pages to the last digit, 11 852 pages stored
  uncompressed (`huge_pages`), 5684 same-filled (`same_pages`) and 590 800 867 compressed bytes
  (`compr_data_size`).
- [x] Zram dumps: three of the desktop, two of the Mi 9T.
- [ ] The scripted VM workloads, and with them the public corpus.
- [ ] Cuttlefish pages.
- [x] Real 16 KiB pages: two zram dumps of the Android 17 emulator, and two of its image with 4 KiB
  pages ([explored-designs.md](explored-designs.md#android-17-in-the-emulator-the-4-kib-tables-fit-the-16-kib-ones-trained-again-26-smaller)).

*Gate: at least 500 000 pages from at least 6 kinds of workload, Cuttlefish included, with the
same-filled fraction measured and reported apart.* Not met as written: the numbers come from the
zram dumps, and the workloads were never scripted.

### Phase 2: benchmark harness and baseline (8 weeks)

The first public artifact, and worth doing whether a codec ever ships or not.

#### 5.1 Codecs under test

Built **from the kernel tree's own sources** (`lib/lzo`, `lib/lz4`, `lib/zstd`, `lib/842` from the
local clone), not from upstream releases. The kernel's `lz4` in particular lags behind upstream. So
the numbers predict the kernel's behaviour, which is what maintainers need, and which no existing
benchmark gives them.

`lzo`, `lzo-rle`, `lz4` (acceleration 1, 2 and 4), `lz4` with a trained dictionary, `lz4hc`, `zstd`
-1, 1, 2 and 3 with and without a trained dictionary, `deflate`, `842`. And as design references:
WKdm, WK4x4, memlz, LZAV.

Plus one system configuration, because it is the first alternative a maintainer will suggest: **`lz4`
as the primary codec, and `zstd` recompression of idle pages** with `CONFIG_ZRAM_MULTI_COMP`. Modelled
as `lz4` for the pages that are read back soon and `zstd` for the rest, with the idle fraction
measured in the workload. The cover letter has to say why quetschn is still worth it next to that.

**The compiler flags match the kernel's.** Every codec in the harness, quetschn too, is built with the
flags the kernel uses for `lib/`: `-O2 -fno-strict-aliasing`, and no SIMD registers
(`-mno-sse -mno-mmx -mno-avx -mno-sse2` on x86-64, `-mgeneral-regs-only` on arm64). Otherwise the
compiler vectorizes loops in userspace that it cannot vectorize in the kernel, and the userspace
numbers predict nothing. The exact flags come from a `make V=1` build of the local tree, not from this
list, see `cmake/kernel_codecs.cmake`.

#### 5.2 Metrics

All metrics leave out same-filled pages, because zram stores them before any codec runs
(`page_same_filled()`).

1. **Σ zsmalloc cost**, with the *real merged class table* and the cost per object including the
   zspage tail waste (§3.1). The ratio metric.
2. **Pages at or over `huge_class_size`**, 3625 B on 4 KiB pages: the cliff count.
3. **Decompression latency per page**: p50, p90, p99, p99.9 and max, **warm** and **cold**, with the
   source and the destination flushed between pages. Each page is timed several times and its latency
   is the **median** of those runs. Then the percentiles are taken across the pages. So p99 describes
   the slowest 1% of *pages*, which is a property of the data, and not the slowest 1% of
   *measurements*, which is mostly interrupts and timer noise.
4. **Compression latency per page**, with the same statistics.
5. **Weighted roundtrip**, with the ratio of compressions to decompressions measured from the actual
   workload rather than assumed: the time of the score of §1.1, from the means.
6. **Per-CPU workspace** in bytes.
7. **`perf` counters per page**: instructions, cycles, branch misses, LLC misses. They explain *why* a
   codec wins, and make the design argument checkable.

The timing is a loop per page (`rdtscp` and `lfence` on x86-64, the PMU's cycle counter through
`perf_event_open` on arm64, because `cntvct_el0` ticks at only 19 to 54 MHz on many boards), because
nanobench reports statistics over batches and not a distribution over pages. nanobench stays useful
for aggregate throughput. Pinned cores, fixed frequency, and `perf` counters where available.

**Hardware:** x86-64 and the big and the little core of the old phone, from the first published table
on.

#### 5.3 Statistical presentation

Copy the shape of the lzo-rle commit message, because it worked:

- per page **paired** differences against each baseline, not only aggregates;
- the number of pages that regress by more than 1σ, and the largest regression;
- the full distribution, a violin or CDF per codec, because the data is bimodal;
- results split by workload;
- 95% bootstrap confidence intervals, resampling pages, for every Σ cost and every difference of
  percentiles;
- a **train/test split by workload**. Dictionaries for `lz4` and `zstd` are trained on some workloads
  and measured on the others, and everything tuned for quetschn later (Phase 3) uses the same split. A
  dictionary trained on the pages it is measured on overstates C4, and so does a codec tuned on them.
  A zram dump has no process names, so there the split is by time: train on one dump, measure on a dump
  taken days later, and leave the pages that are in both out of the training side
  (`tools/bench-dict.sh`).

*Gate, go or no-go: headroom cannot be measured on a codec that does not exist yet, so the gate uses a
proxy. On the test corpus, does `zstd -1` without a dictionary need at least 12% less Σ zsmalloc cost
than the better of `lz4` with a dictionary and `lzo-rle`?* `zstd -1` stands in for "how much redundancy
is left that a fast codec could still find". It is not a strict bound, and quetschn will not get all
of it, so the gate asks for more than the 8% of C1. The 12% is a judgment call. If the proxy shows
less, go to §8.

**Passed:** on the first zram dump `zstd -1` needs 16.9% less than `lzo-rle`, which is better than
`lz4` with a dictionary there
([explored-designs.md, "The first runs with dictionaries"](explored-designs.md#the-first-runs-with-dictionaries-phases-0-to-2)).

Still to do:

- [ ] Publish the comparison on martin.ankerl.com and in the repository, whatever the outcome. A
  negative result would still have been the first public zram codec comparison with this method.
- [ ] **Post it to linux-mm**, without proposing a codec. Ask for criticism of the method, and ask
  directly whether the zram maintainers would consider a new backend at all, and which evidence they
  want. This is cheap, and it tests R3 about 18 months before a Phase 7 posting would. A clear "no"
  sends the project to the fallback of R2 before more work on the codec. A question about seqlz
  itself went to linux-mm on 9th October 2026 instead, [the thread](https://lore.kernel.org/linux-mm/CAAFOosa2WLf--T17cujJu5CUsN12NNjgTitOvno2LY7jAr1gfw@mail.gmail.com/), see
  [§9](#9-where-the-project-stands-and-the-next-actions).

### Phase 2b: decoder latency spike (2 weeks)

> [!NOTE]
> Done on x86-64. The spike's code was removed on 7th October 2026; the results stay here and in
> [explored-designs.md, "Word model"](explored-designs.md#word-model-wkdm-style-64-bit-words).

The gate of Phase 2 tests the headroom in ratio. It does not test C2, and C2 is the less likely of the
two: the kernel's `lz4` decodes a 4 KiB page mostly as `memcpy` of literals and matches, which is hard
to beat.

The spike is the smallest decoder of the design that §3.2 bets on: a WKdm-style format of 64-bit
words with a fixed tag layout, no LZ pass, no dictionary, and a throwaway encoder. Its cold p99 per
page (§5.2) is measured against the kernel's `lz4` on the test corpus, on x86-64 and on the phone's
in-order little core.

*Gate: the spike decoder's p99 is at or below `lz4`'s on both machines. If it is clearly slower, the
p99 argument of §3.2 does not hold, and Phase 3 starts from the fallback of §8 instead of from the word
model.*

**The x86-64 half passed.** The spike used 64-bit words with 2-bit tags (zero, exact match or a match
of the high 32 bits against a table of 16 recent words, literal), in four sections whose lengths follow
from the tags, so the decoder checks the length once and the loop has no bounds checks. All decoders
were built with the kernel's flags and `-O3`, like `lz4`, and decoded the same format.

The first three decoders were at parity with `lz4`: a `switch` on the tag, a branchless one that
selects with masks, and the `switch` with a fast path that writes a zero tag byte as 4 zero words at
once. The table update made the difference. Those decoders write each word to the table slot of its
hash, `slot_of(w)`, so the address of the store is known only after the table load that produced `w`,
and the next word's table load has to wait or guess. `spike-slots` takes the slot from the stream
instead: the stored index for exact and partial words, which the encoder wrote from the same hash, and
the hash of the stored word for literals. Then no store address depends on a table load, and an exact
match needs no store at all.

`quetschn-bench-interleaved` runs several codecs in one process, and each repetition runs every codec
once on the same page, so a drift of the CPU frequency hits all of them alike. On the 455 239 pages of
the first zram dump, Ryzen 9 7950X pinned to one core, `powersave` governor, median of 5 runs per page,
percentiles over the 344 955 pages that both store compressed:

| run | decoder | warm p99 | cold p50 | cold p99 | cold p99.9 |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | `lz4` | 2030 ns | 1360 ns | 2450 ns | 3180 ns |
| 1 | spike, `switch` | 1850 ns | 1630 ns | 2720 ns | 2970 ns |
| 1 | spike, zero fast path | 1820 ns | 1610 ns | 2810 ns | 3060 ns |
| 1 | `spike-slots` | 720 ns | 1390 ns | 1970 ns | 2160 ns |
| 2 | `spike-slots` | 710 ns | 1380 ns | 2160 ns | 2460 ns |
| 2 | `lz4` | 2030 ns | 1670 ns | 2890 ns | 3440 ns |

Run 2 has the order of run 1 reversed, because in one binary the code layout of one codec can shift
another. Paired over all pages, `spike-slots` is faster than `lz4` at cold p99 by 480 ns [470, 500] in
run 1 and by 820 ns [810, 830] in run 2, and at warm p99 by 1270 and 1280 ns. The absolute numbers of
`lz4` move between the two runs, the sign of the advantage does not. A branchless decoder in an
earlier, separate run was slower everywhere (cold p99 3690 ns), so control flow that does not depend
on the data is not what §3.2 hoped for. The win comes from a short dependency chain per word.

`lz4` is still better on pages that it compresses below 512 bytes, with 73% zero words: cold p50 780
against 840 ns in run 1. There `lz4` copies long matches while the spike still visits every tag.

The spike is not a codec: 55.7% Σ zsmalloc cost against 34.5% for `lz4`, and 24% of the pages stored
uncompressed. What it gave Phase 3: keep the store address of the table update independent of the
table load, give runs of zeros and repeats a path that costs per run and not per word, and measure
latency only interleaved. The arm64 half was not measured with the spike.

### Phase 3: page analysis and design exploration (12 weeks)

> [!NOTE]
> Done. The design is `seqlz-fast-lit`, described in [seqlz.md](seqlz.md). Every design that was
> measured, with its numbers and why it was kept or dropped, is in
> [explored-designs.md](explored-designs.md). Add to it before trying something new.

The questions, to answer with numbers from the corpus:

- Which fraction of pages is incompressible, e.g. compressed data or encrypted buffers? Those should
  be detected in a few hundred cycles and stored as they are.
- What is the structure of the 8-byte words? The distribution of distinct high 32-bit prefixes per
  page (pointer density), the autocorrelation at strides 8 and 16 (arrays of structs), the density of
  small integers.
- Where does `lz4` lose against `zstd`: in entropy coding, or in finding matches?
- How many pages sit within 256 bytes above the cliff, and what gets them back below it?
- How many pages sit a few bytes above a class boundary (§3.1), and how much Σ cost comes back if those
  pages get one more compression attempt?
- How often does a greedy parser get stuck in a chain of short matches on periodic data, as `lz4` with a
  dictionary does ([explored-designs.md](explored-designs.md#the-first-runs-with-dictionaries-phases-0-to-2))?
  A long match at a small multiple of the period is cheap to check for.
- Do the answers hold for Cuttlefish pages and for 16 KiB pages (§3.5)?

The candidate designs, in the order of their expected value:

1. **A word model with a mode per page.** WKdm's idea: 64-bit words, a larger dictionary of recent
   words, tags for zero, exact match, a match of the high bits with the low bits as literal, and
   literal. Control flow that barely depends on the data, for the p99 win of §3.2. The class step (16
   to 144 bytes, §3.1) pays for a mode header per page or per 512 bytes.
2. **A fast path for zeros and runs.** Rodgman's corpus was bimodal, 44% of pages with at most 5%
   zeros and 44% with at least 90%. Two separate modes beat one compromise. Check that our corpus is
   bimodal too first.
3. **A word model plus a short LZ pass** over the rest.
4. **Recovery at a class boundary**: when the fast path lands just above a class boundary, spend more
   compression time on that page only, e.g. a static Huffman or a small rANS stage. The encoder knows
   the class table, so it knows the target size, and compression time is less precious than
   decompression time. It pays most at `huge_class_size`, 455 bytes per rescued page, and 16 to 144
   bytes at every other boundary.
5. **Dictionary support** from the start, with `zcomp_params->dict` (C4). For seqlz it was measured and
   dropped: its tables are part of the format
   ([explored-designs.md, "The format written down"](explored-designs.md#the-format-written-down-one-set-of-tables-and-stream-sizes-that-hold)).

What happened: the gap between `lz4` and `zstd` turned out to be mostly how the sequences are coded
([explored-designs.md, "Where the ratio of `zstd` comes from"](explored-designs.md#where-the-ratio-of-zstd-comes-from)),
and the word models did not get to `lz4`'s memory. So the design became an LZ codec with Huffman coded
sequences and static tables, not one of the five above.

A slower sibling with a higher ratio, for recompression of idle pages with `CONFIG_ZRAM_MULTI_COMP`, is
a legitimate second target (working name `wuzl`), but only after the main codec lands.

*Gate: one design meets C1 against `lz4` with a dictionary at an equal or better p99, in a userspace
prototype, on the held-out test workloads, on x86-64 and on the phone's in-order little core.* C1
passed. The p99 part did not, and the score of §1.1 replaced it as the target.

### Phase 4: reference implementation, format spec, fuzzing (20 weeks)

This phase exists because of §2.2: everything Biggers asked for, before the first patch.

- [x] The codec, `src/seqlz_*.c`, `src/seqlz.h` and `src/seqlz_internal.h`: C11, freestanding, no libc, no allocation, a
  scratch buffer of at most `lz4`'s 16 416 B passed in by the caller. The page size is a parameter.
- [x] The tests with 4 KiB and with 16 KiB pages (§3.5), both in CI. Not the tests of the kernel codecs:
  zram's calls into them are built for 4 KiB pages.
- [x] **[The format](format.md)**: a byte-exact specification, and two deliberately slow reference
  decoders written from the spec alone, `tools/seqlz-ref/seqlz_ref.py` and `tools/seqlz-ref/seqlz_ref.c`.
  The fast decoder is tested against them.
- Fuzzing:
  - [x] the decompressor on any input: it must never read or write out of bounds and must always
    terminate, for truncated, damaged and adversarial input;
  - [x] the roundtrip of any page;
  - [x] the fast decoder against the reference decoder;
  - [x] continuous fuzzing in CI with ClusterFuzzLite, since 9th October 2026
    (`.github/workflows/cflite.yml`): once a day an hour each with ASan, UBSan and MSan, from the corpus
    of the runs before, and 10 minutes with ASan on a PR that changes the codec. OSS-Fuzz only takes
    projects with many users or importance for critical infrastructure, so a new codec will likely be
    turned down before it has users. Apply anyway, and again once there are users. *"We run under
    OSS-Fuzz"* would be a strong, checkable answer to objection 3, but the plan does not depend on it.
- [x] ASan and UBSan in CI, big-endian in CI.
- [x] MSan, since 9th October 2026: the fuzz targets, in CI and in ClusterFuzzLite. They mark the
  page, the scratch and the matcher's table as unwritten before every input, so a read of a byte the
  codec did not write for that input is reported. Not the doctest binary: libstdc++ is not built with
  MSan, and the first report is in its `std::map`, before any test runs.
- [x] A bound on the worst-case runtime, stated and measured: at most `PAGE / 4 + 1` sequences, and the
  slowest pages found cost 1.14 times the p99 of real pages to compress and 1.59 times to decode
  ([explored-designs.md, "The worst case"](explored-designs.md#the-worst-case-the-slowest-pages-found-cost-13-times-the-p99-of-real-ones-as-for-lz4)).

*Gate: 1e9 fuzz executions without a crash, spec and implementation agree on the whole corpus and the
fuzz corpus, frame size and workspace within budget.* Met with 1.7 billion AFL++ inputs on the tables
of 6th October and 1.5 billion libFuzzer inputs on the tables of 7th October. On 9th October AFL++ ran
on the codec of `5342a9b`, the tables of 7th October with #145 and #147: 12 instances for 2 hours,
3.65 billion inputs, 1.42 billion on the decoder, 1.94 billion on the decoder against the one in C
and 298 million on the roundtrip, nothing found.

### Phase 5: kernel port and validation in a VM (16 weeks)

- [x] `lib/quetschn/` and `include/linux/quetschn.h`, **free of any zram API dependency**. The zram
  backend API has changed again and again (the rewrite of 2024, the preemption series of 2025, the
  parameter handling of 2026). A thin `backend_quetschn.c` absorbs that. Done as `lib/seqlz/`,
  `include/linux/seqlz.h` with only `seqlz_compress()` and `seqlz_decompress()`, which
  `tools/kernel-port/port.py` writes into a kernel tree.
- [x] `backend_quetschn.c` modelled on `backend_lz4.c`, including the validation in `setup_params`
  (`7b0f677c7bd5`) and `pr_fmt` (`70922d5ef84a`): `backend_seqlz.c`.
- [x] A QEMU test rig: a VM with zram, reads, writes and the whole page fault, `tools/zram-vm/run.sh`.
  It measures the **actual** memory of `mm_stat`, which is where the fragmentation of §3.1 shows up and
  where the userspace cost model is confirmed or not.
- [x] Sustained swap thrash under memory pressure: `tools/kernel-port/stress.sh`, 4 CPUs swapping 1.5
  times the free memory to zram with seqlz for 15 minutes per boot, every page compared with what it
  should hold.
- [x] KUnit tests of `lib/seqlz/`: the tables against the hashes of the spec, its example, round trips,
  damaged pages, and pages made from the spec's rules that break one rule each. CI runs them in UML
  with KASAN and UBSan, `tools/kernel-port/kunit.sh`.
- [x] `tools/testing/selftests/zram/`, with new cases as needed: `stress.sh` runs them with seqlz.
  No new cases so far; the thrash covers what they don't.
- [x] Correct under `CONFIG_DEBUG_ATOMIC_SLEEP`, `PROVE_LOCKING`, KASAN, and with preemption disabled.
  Minchan's zBeWalgo panic was exactly this kind of bug. Also UBSan's bounds and shift checks.
  x86-64 has no `preempt=none` any more, its choices are `full` and `lazy`, and `stress.sh` boots
  both.

*Gate: a kernel with quetschn survives sustained swap thrash under KASAN, and `mm_stat` in the VM
confirms the userspace prediction within 2%.* Passed on 8th October 2026, kernel `986c24e0fe44`: 4
boots of `stress.sh` on the whole second phone dump, `preempt=full` and `lazy`, levels 1 and 2, 15
minutes each. 34.9 million pages swapped out and 33.6 million in, 37.2 million compared, none
different, no report from KASAN, lockdep, UBSan or `DEBUG_ATOMIC_SLEEP`, KUnit and the selftests
passed. `mem_used_total` on a new device is exactly the model's, the sum of the per-page costs 0.35 to
0.47% below it ([explored-designs.md](explored-designs.md#zrams-memory-against-the-model-exact-on-a-new-device-σ-zsmalloc-cost-035-to-047-below-it)).

### Phase 6: arm64 validation, a hard gate before Phase 7

No arm64 numbers, no patch. The old phone has given arm64 timings and phone pages since Phase 2, so this
phase does not start from zero.

- [x] The Mi 9T, Cortex-A76 and A55, 4 KiB pages, its own pages, in its own kernel as zram and as swap.
- [ ] A current phone that runs a kernel with 16 KiB pages (Pixel 8 or newer), rooted. Or ask Dave
  Rodgman (ARM, the author of lzo-rle), the Android kernel team or linux-mm for help with measuring.
  The publication of Phase 2 makes that ask reasonable.
- [ ] Pages from a current phone. Android pages may differ from desktop pages (ART's heap layout,
  another allocator), and a newer Android may differ from the old phone. The old phone already gives a
  first answer, so a surprise should be small. The design stays parameterised rather than tuned by hand
  to one corpus.

*Gate: C1 to C3 hold on arm64, on a big and on a little core, with 4 KiB and 16 KiB pages.*

### Phase 7: upstreaming (6 months or more, expect v5)

- The benchmark and the question about a new backend go out after Phase 2. Before the RFC, reply in
  that thread with the new numbers, so that the series does not arrive cold.
- Then an RFC series, with a cover letter modelled on `5ee4014af99f`: where the pages come from, their
  distribution, both architectures, paired regression analysis per page, the state of the fuzzing,
  and the workspace against the other codecs.
- The series: (1) `lib/quetschn` with its KUnit tests, the spec and `MAINTAINERS`, (2) the zram backend with Kconfig and
  Makefile, (3) documentation, (4) selftests. 4 KiB pages only: the format for 16 KiB pages is not
  fixed until a phone with 16 KiB pages has measured it (format.md, Status), and a second series adds
  it then.
- Who reads it: Sergey Senozhatsky and Minchan Kim (zram and zsmalloc, per `MAINTAINERS`), linux-mm,
  Andrew Morton (`lib/` goes through the mm tree), Eric Biggers (reviewed zBeWalgo and will review
  this), Dave Rodgman.
- A `MAINTAINERS` entry is a promise for years, and the maintainers will read it as one.
- After the merge: propose `CONFIG_ZRAM_BACKEND_QUETSCHN=y` for Android's `gki_defconfig`, with the
  phone numbers of Phase 6. Without that the codec is merged, but not on phones (§1). Vendors with
  hardware `lz4` (R12) will likely choose it, so the case for the default rests on the phones
  without it, and on memory for the ones with it.

---

## 6. Repository layout

The plan had the codec in `src/` and the kernel files in `kernel/`. The codec grew up in `explore/`,
next to the designs it beat. Those designs were removed on 7th October 2026, their results are in
[explored-designs.md](explored-designs.md) and their code is in git history before `57fb8fb`. Then
`explore/` became `src/`. The kernel port of Phase 5 will split it into a `lib/` part and a zram
backend.

```text
src/                        the codec, freestanding C
  seqlz.c, seqlz.h            encoder, decoder, the tables built from the code lengths
  page_lz.h                   the matcher
  seqlz_*tables*              the trained tables, part of the format
  zram_seqlz.c                seqlz as a zram backend calls it, for the harness
bench/                      the harness: per-page timing, zsmalloc's cost model, table training
  kernel_codecs/              zram's calls into lib/lz4, lib/lzo and zstd, built in userspace
cmake/kernel_codecs.cmake   builds those from QUETSCHN_KERNEL_TREE with the kernel's flags, sources never copied
test/                       doctest unit tests
fuzz/                       libFuzzer and AFL++ targets, the CI smoke run and the long runs
tools/                      see tools/README.md
  collect/                    page collectors and page statistics
  seqlz-ref/                  the reference decoders, written from docs/format.md alone
  zram-vm/                    the kernel VM: zram's reads and writes, or with MODE=swap the whole fault
  swap-fault/                 the whole page fault on a running Linux, e.g. the phone
  phone-apps/                 the app launch test on the phone
  seqlz-bound/, seqlz-worst/  bits per field against their entropy; the worst case
  seqlz-viz/                  the two pages that show seqlz step by step
docs/                       this plan, the format, how seqlz works, measuring, explored designs, plots
.github/workflows/ci.yml    gcc and clang, ASan and UBSan, arm64, s390x and 32-bit, fuzz smoke, format, docs
```

---

## 7. What is deferred

- `wuzl`, the sibling with a high ratio for recompression. A real opportunity with
  `CONFIG_ZRAM_MULTI_COMP`, but splitting the effort before the main codec lands would stall both.
- zswap. A different allocator, different constraints. Later.
  If quetschn ever is an acomp algorithm: zswap's load path without a lock (Usama Arif,
  [linux-mm PR #5153](https://github.com/linux-mm/linux-mm/pull/5153), patch 2) works only for
  synchronous algorithms that need no request context. Before such a port, check whether the
  scratch for the literals, from the crypto layer's per-CPU streams, counts as such a context.
- A generator of synthetic pages that are safe to share. Valuable, but a second project. The public
  corpus from the VM workloads (Phase 1) covers reproducibility for a fraction of the cost.
- Hardware compressors, e.g. 842 on POWER or the ones in phones. Out of scope as work. What hardware
  `lz4` on new Qualcomm phones does to the score is R12.

---

## 8. Risks, and what to do about each

| | risk | evidence | what to do |
| --- | --- | --- | --- |
| R1 | **No arm64 hardware.** Phones are the users, and lzo-rle was merged on arm64 data first. | a known constraint | An old rooted phone with a big and a little core is used from Phase 2 on. Phase 6 adds a current phone with 16 KiB pages and is a hard gate. Do not submit without it. |
| R2 | **Not enough headroom over `lz4` with a dictionary.** Nobody had measured it, and the project rested on it. | the gate of Phase 2: `zstd -1` needs 16.9% less than `lzo-rle` | Answered before any codec work. The fallback would have been the benchmark, and then *a targeted improvement of `lz4` or `lzo-rle` for page-sized inputs*. That is what lzo-rle was, and it is a much easier merge. |
| R3 | **The maintainers do not want another backend.** Each one is maintenance for good. | zBeWalgo reached v7 and was not merged | Ask before the kernel port. The question to Sergey Senozhatsky and Minchan Kim is [sent](https://lore.kernel.org/linux-mm/CAAFOosa2WLf--T17cujJu5CUsN12NNjgTitOvno2LY7jAr1gfw@mail.gmail.com/) (9th October 2026), with the phone's numbers, and not answered yet. A "no" sends the project to the fallback of R2. |
| R4 | **A codec tuned on desktop pages loses on Android pages.** Another heap layout, another allocator. | two zram dumps of the Mi 9T (Android 11): `seqlz-fast-lit` stores 28% less than `lz4` there, as on the desktop | The tables are trained on phone pages too, the phone counted 5 times, so that desktop pages do not cost the phone. On the Android 17 emulator they are within 0.6% of tables trained on its own pages, and its 16 KiB pages have tables of their own ([explored-designs.md](explored-designs.md#android-17-in-the-emulator-the-4-kib-tables-fit-the-16-kib-ones-trained-again-26-smaller)). Open: a current phone. |
| R5 | **The zram backend API changes.** The rewrite of 2024, the preemption series of 2025, parameter and naming changes in 2026. | `git log drivers/block/zram/`, and the split of zcomp into read and write streams posted in October 2026 (§3.2) | The codec has no kernel API dependency, `backend_quetschn.c` absorbs the changes. Rebase against mainline in CI. |
| R6 | **A fuzzing bug or a sleep in atomic context burns the maintainers' goodwill.** | Biggers's objection, Minchan's panic | Phase 4, and the gate of Phase 5 with KASAN and `DEBUG_ATOMIC_SLEEP`, exist for this. Continuous fuzzing with ClusterFuzzLite before the submission, OSS-Fuzz if it takes the project. |
| R7 | **Time.** 3 to 8 hours a week against a path of 18 to 24 months. | lzo-rle: 4 months, v5, paid work, an existing codec | Each phase can be published on its own. Phase 2 alone is worth it. |
| R8 | **Employer rules on open-source side projects**, especially kernel work with a `MAINTAINERS` entry. | checked on 23rd September 2026: side projects are fine | Resolved. |
| R9 | **The input size changes under the codec.** Kernels with 16 KiB pages on Android, or zram compressing multi-page folios as one unit. Larger inputs favour LZ codecs with a larger window. | §3.5. Multi-page compression was proposed in 2024, is not merged at `986c24e0fe44`, and had no newer version in 2026. `zstd` 3 on blocks of 16 KiB needs 13% to 22% less than `seqlz-fast-lit` on 4 KiB pages | `PAGE_SIZE` is a parameter of the format, the cost model and the harness. Watch the zram and mm lists for multi-page compression. If it comes back, measure `seqlz-fast-lit` against `zstd` on whole folios, in time per page too, before more work on the codec. |
| R10 | **No p99 decode win over `lz4`.** C2 rested on the argument about branch mispredictions in §3.2. | measured: `seqlz-fast-lit` decodes slower than `lz4`, 18% to 25% per swap-in, for 28% less memory | C2 is not met. The score of §1.1 replaced the bars as the target. The merge argument is memory at a small cost in time, not a faster read. |
| R11 | **zram's backend interface goes away.** The zcomp API was to be replaced by the acomp crypto API. A series of October 2026 redesigns the zcomp API instead of removing it. | Sergey Senozhatsky, March 2026 (§3.4), not done at `986c24e0fe44`. His series of October 2026 splits zcomp's streams (§3.2), and Qualcomm posted a new zcomp backend in September 2026 (§3.4) | The codec has no kernel API dependency. Ask in the question of R3 which form the maintainers want, and port to whatever exists then. |
| R12 | **Hardware `lz4` on new Qualcomm SoCs.** It makes `lz4` faster, and if it writes the bytes of software `lz4` it moves `lz4` along the time axis of the score (§1.1), not along the memory axis. | Qualcomm's QPaCE series of 30th September 2026, [series 1177021](https://patchwork.kernel.org/series/1177021/): a zcomp backend `qpace-lz4`, synchronous, the CPU polls the engine, no speed numbers. | The argument stays memory. As a what-if with the VM's swap times, `lz4` with its own time at 0 still leaves `seqlz-fast-lit` the lowest score from 3.1 to 73 and from 10.3 to 72 bytes per µs, instead of up to 155 and 211; on the phone's big core from 0.9 to 25. A faster `lz4` only lowers the upper end; to push `seqlz-fast-lit` off the hull it would need a time below 0 ([explored-designs.md](explored-designs.md#lz4-in-hardware-a-what-if-seqlz-fast-lit-stays-on-the-hull-up-to-73-and-72-bytes-per-µs-instead-of-155-and-211)). Measure on a device if one becomes available. Open: does `qpace-lz4` write the lz4 block format at the ratio of `LZ4_compress_fast()` with acceleration 1? If it stores more, the gap to `seqlz-fast-lit` is larger than 28%. And does it take a dictionary? zram's `lz4` does (C4), patch 4 ignores `params->dict`. |

---

## 9. Where the project stands, and the next actions

As of 9th October 2026:

- **Phases 0 to 3: done.** The harness, the collectors, zram dumps of the desktop and of the Mi 9T,
  and the design: `seqlz-fast-lit` ([seqlz.md](seqlz.md)), with every alternative that was measured in
  [explored-designs.md](explored-designs.md). Not done from them: the scripted VM workloads, and
  publishing the comparison.
- **Phase 4: done.** [The format](format.md), and two reference decoders written from it. The
  tables were trained again on 7th October, on swapped pages too. Fuzzing of the decoder, of the
  roundtrip, and of the decoder against the reference decoder in C: 1.7 billion inputs with AFL++ on the
  tables of 6th October, 1.5 billion with libFuzzer on the new ones, and 3.65 billion with AFL++ on the
  codec of 9th October, no difference found. ASan and UBSan
  in CI, big-endian on s390x in CI. The worst case is measured: against its own p99,
  `seqlz-fast-lit`'s slowest pages cost 1.14 times to compress and 1.59 times to decode, the least of
  it, `lz4`, `lzo-rle` and `zstd`. The tests run with 16 KiB pages too, in CI. MSan on the fuzz targets
  and continuous fuzzing with ClusterFuzzLite since 9th October.
- **Phase 5: done, the gate passed.** `tools/kernel-port/` writes `lib/seqlz/`, a minimal
  `include/linux/seqlz.h`, the zram backend and KUnit tests into a kernel tree; CI builds it for
  x86-64, arm64, arm and s390 and runs the KUnit tests. `stress.sh` swapped 34.9 million pages through
  it under KASAN, lockdep and UBSan without a report or a wrong page, the zram selftests pass with it,
  and `mm_stat` on a new device is exactly the model's memory
  ([explored-designs.md](explored-designs.md#zrams-memory-against-the-model-exact-on-a-new-device-σ-zsmalloc-cost-035-to-047-below-it)). The kernel VM of `tools/zram-vm/` measures reads, writes and the whole
  page fault.
- **Phase 6: half.** The Mi 9T, A76 and A55, 4 KiB pages, its own pages, in its own kernel as zram and
  as swap: done. Open: 16 KiB pages and a current phone. The 16 KiB tables are trained on the pages
  of the Android 17 emulator, and with 16 KiB pages `seqlz-fast-lit` needs twice `lz4`'s work memory
  per CPU, above C5.
- **Phase 7: the question to the maintainers is sent** (R3, R11), on 9th October 2026 to linux-mm,
  linux-block and linux-kernel, [the thread](https://lore.kernel.org/linux-mm/CAAFOosa2WLf--T17cujJu5CUsN12NNjgTitOvno2LY7jAr1gfw@mail.gmail.com/). Not answered yet.
- **Levels 3 and 4** (#139): a deeper matcher for the same format, 2.7% to 4.3% less for about twice
  the write time, measured and not kept.

Next, in this order:

1. The answer of Sergey Senozhatsky and Minchan Kim to [the question](https://lore.kernel.org/linux-mm/CAAFOosa2WLf--T17cujJu5CUsN12NNjgTitOvno2LY7jAr1gfw@mail.gmail.com/): a new algorithm at
   all, and as a zram backend or as an acomp algorithm (§3.4). The answer decides the form of
   Phase 5.
2. Fewer data-dependent branches in the decoder. In a fault it mispredicts about 110 branches per page
   that it gets right after a decode of the same page, which every benchmark before hid by decoding a
   page more than once ([explored-designs.md](explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)). The refill without its branch on
   out-of-order cores took 63 ns off a swap-in on x86-64
   ([explored-designs.md](explored-designs.md#the-refill-without-its-branch-every-second-fast-sequence-63-ns-less-per-swap-in-on-x86-64-and-the-a76-kept)). The fast path's condition
   misses once per length value, and four ways to cut the misses after it were slower
   ([explored-designs.md](explored-designs.md#the-fast-paths-condition-its-22-misses-per-page-are-the-length-values-four-ways-around-them-slower-not-kept)).
   One copy for every offset, without the branch on an offset below 8, was slower too
   ([explored-designs.md](explored-designs.md#the-offset-below-8-one-copy-for-every-offset-saves-12-misses-per-page-and-costs-more-not-kept)). The other choices between
   a branch and more work hold with `MODE=swap`
   ([explored-designs.md](explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained)).
   The score with the swap times is in §1.1.
3. A corpus from the Android emulator, Android 17, with 16 KiB pages and with 4 KiB pages: done. The
   4 KiB tables fit, within 0.6% of tables trained on its pages; the 16 KiB tables are trained on its
   16 KiB pages, 2.6% smaller on a second dump
   ([explored-designs.md](explored-designs.md#android-17-in-the-emulator-the-4-kib-tables-fit-the-16-kib-ones-trained-again-26-smaller)).
4. The work memory with 16 KiB pages: within C5, or a reason why not. Which context C5 limits, once
   zram splits them, is open (§3.3).
5. Phase 5 again if the maintainers want another form than a zram backend, e.g. an acomp algorithm:
   `lib/seqlz/` stays, the glue changes, and `stress.sh` runs again.
