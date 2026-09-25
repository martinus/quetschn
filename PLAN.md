# quetschn — project plan

A compression codec for memory pages, aimed at the Linux kernel's zram module. 4 KiB pages first,
16 KiB pages as a parameter from day one (§3.5).

Status: no code yet. This document is the plan, the evidence behind it, and the decision gates.

Last verified against mainline `v7.3-rc1-324-g986c24e0fe44` (`986c24e0fe44`) on 2026-09-22.

---

## 1. Goal and success criteria

**Goal:** a codec merged into mainline Linux as a zram backend, chosen because it is measurably better
than what is already there.

"Better than all alternatives" on every axis at once is not achievable and should not be the target.
zstd holds the ratio crown and will keep it; no small, allocation-free, scalar codec beats an entropy
coder with a trained dictionary. The achievable and sufficient target is to **dominate the fast corner
of the Pareto frontier**:

| Criterion | Bar | Why this bar |
| --- | --- | --- |
| C1 | ≥ 8% less Σ zsmalloc cost (§3.1) than the better of `lzo-rle` and `lz4`, on the held-out test corpus (§5.3), same-filled pages excluded, 95% bootstrap confidence interval excluding 0 | `lzo-rle` is the zram default; `lz4` is the common Android choice. A 1% gain does not pay for a new backend |
| C2 | Lower p99 decompression latency than `lz4`, cold cache, on x86-64 **and** an arm64 little core, 95% bootstrap confidence interval excluding 0. Latency statistic defined in §5.2 | decompression runs in the page-fault path (§3.2); on phones that often means a little core |
| C3 | p99 compression latency within 1.2× of `lz4`, same statistic as C2 | zram compresses more often than it decompresses |
| C4 | Beats `lz4` **with a trained dictionary**, not just bare `lz4` | zram supports dictionaries; Honor made dict-lz4 >50% faster in March 2026 (`f0f6f7871430`) |
| C5 | Per-CPU workspace ≤ `lz4`'s 16416 B | `lz4` needs 16416 B/CPU, `lzo` 16384 B/CPU, `842` 61440 B/CPU (§3.3) |
| C6 | Decompressor is fuzz-safe and bounded-time for arbitrary input | this is what killed the last new-codec attempt (§2.2) |

C1 and C2 together are the merge argument: *strictly dominates the current default and the current
fast option, on both memory and tail latency, with less per-CPU memory.* C1 has headroom only if the
Phase 2 gate passes, and C2 is plausible only if the decoder spike after Phase 2 passes (§5, Phase 2b).
If either fails, the project pivots (§8).

**Scope of "merged":** this plan ends when the backend is in mainline. That does not put it on phones.
Android kernels are built from `gki_defconfig`, and enabling a new backend there is a separate
decision by the Android kernel team, made after the mainline merge. Phase 7 has to plan for it.

**Explicit non-goal:** beating `zstd` level 3 on ratio. If quetschn lands as "lz4-class latency at
zstd-1-class ratio", that is a win.

### 1.1 The score: memory against time, not bars

C1 to C3 are bars for the merge argument, and they stay the numbers to report there. As a target for
design choices they are cliffs: a design that saves 4% of memory fails because its p99 is 1 us above
`lz4`'s, without asking what 1 us is worth or how often the page is read (#37). The designs are chosen
by a score instead:

- **bytes**: zsmalloc memory per stored page, `mem_used_total / pages` in the VM of §5 Phase 5, after
  recompression where the device recompresses.
- **time**: per page written to zram, `write + r * read + b * recompression`, each the **mean** over the
  pages, in us. The mean, because it is the total time spent on the pages, and a burst of n swap-ins,
  e.g. after switching back to an app, waits for n decompressions. p99 per page stays a guard against
  pathological pages (C6), not a target.
- **score**: `bytes + lambda * time`, lower is better, lambda in bytes per us.

`r` is the number of reads per write. On a desktop with zram as the only swap it was 0.34: 3 146 179
pages swapped in against 9 210 320 swapped out in 27.5 days (`pswpin` and `pswpout` of
`/proc/vmstat`). It is one machine; a phone swaps differently and needs its own number. `b` weighs the
time of recompression, which zram runs on idle pages when told to, against the time of reads and
writes, which a task waits for or kswapd spends. `b = 1` counts every us the same; a recompression on
an otherwise idle CPU costs energy and not latency, which would argue for less. The choice of `b` decides
whether recompression pays at all, so every result states it.

lambda is not fixed. `quetschn-score` reads the logs of `tools/zram-vm/run.sh` or of
`quetschn-bench-*` and prints the codecs that have the lowest score for some lambda, the lower left
convex hull of (time, bytes), with the exchange rate between neighbours: the lambda at which the
faster one starts to win. A design is worth keeping if it is on the hull at a lambda that matters.

First results, VM of §5 Phase 5, both dumps, `r = 0.34`, `b = 1` (docs/explored-designs.md, "The
designs by the score"): the hull is `lzo-rle`, `seqlz` (`seqlz-fast`), `seqlz-fast-lit`, `zstd` 3. From
`seqlz-fast-lit` to `zstd` 3 is 8 and 24 bytes per us, from `lzo-rle` to `seqlz-fast-lit` together
about 220 and 210, so `seqlz-fast-lit` has the lowest score for any lambda from 24 to about 150 bytes
per us on both dumps. Who runs `lz4` or `lzo-rle` instead of `zstd` says that lambda is above 8 to 24
for them. Recompression is not pursued: with `zstd` it pays only where the CPU time of an idle machine
counts for less, and makes each read of a recompressed page 2.3 us slower; `seqlz-opt` is removed
(docs/explored-designs.md, "Recompression, measured, not pursued").

Not measured yet: how large the bursts of swap-ins are, and `r` on a phone. `quetschn-swap-bursts`
samples `pswpin` every 10 ms and groups the swap-ins into bursts; 9 minutes on the development machine
saw 1 swap-in, with 56 GB free it does not swap. It needs a machine under memory pressure, a phone
best.

C5 and C6 stay hard limits: per-CPU memory is taken from every CPU whether it swaps or not, and an
unsafe decoder is not a trade-off. C1 to C4 are reported for the merge argument.

---

## 2. Precedent: what actually gets a compressor into zram

Two data points, both verified.

### 2.1 The success: lzo-rle (Dave Rodgman, ARM, merged March 2019, `5ee4014af99f`)

What the merged commit message contains. Italic text is quoted verbatim, the rest is paraphrased:

- A real corpus: *"captured some memory via /dev/fmem from a Chromebook with many tabs open which is
  starting to swap, and then split this into 4178 4k pages"*, with all-zero pages excluded as zram does.
- A distribution, not an average: *"the data is VERY bimodal: 44% of pages in this dataset contain 5%
  or fewer zeros, and 44% contain over 90% zeros"*.
- Measurements on **both** arm64 and x86-64.
- A *weighted roundtrip throughput* metric, weighted because zram compresses more than it decompresses.
- Explicit regression analysis: *"0.1% (3/4178) of cases had a regression > 1 standard deviation, of
  which the largest was 4.6% (1.2 standard deviations)"*.

It took from November 2018 to March 2019 and reached **v5** of the patch series — for a change that was
an increment to an existing codec, from an ARM engineer, doing it as paid work.

That commit message is the template. The plan below is built to produce exactly that kind of evidence.

### 2.2 The failure: zBeWalgo (Benjamin Warnke, 2018, reached v7, never merged)

Same problem statement as quetschn: *"ZRAM compresses each page individually. As a result the
compression algorithm is forced to use a very small sliding window. None of the available compression
algorithms is designed to achieve high compression ratios with small inputs."* It did not get in.

Eric Biggers's objections are the checklist quetschn must satisfy before the first patch is sent:

1. **No rigorous comparison to zstd.** *"This still isn't a valid excuse for not comparing it to
   Zstandard."*
2. **No formal format specification.** Established codecs had a spec and a tested userspace library
   before kernel inclusion.
3. **Not fuzz-safe.** *"You cannot just ignore fuzz-safety in the decompressor either."* He explicitly
   asked for a userspace port fuzzed with afl.
4. **No demonstrated roundtrip correctness guarantee.**
5. **Unstable format** needed an explicit opt-in mechanism.

Separately, Minchan Kim hit a kernel panic (`BUG: sleeping function called from invalid context`) and
wrote *"Unfortunately, I don't have the time to look into."* **Maintainer time is the scarcest
resource in this project.** Every patch must be clean on first read.

One obstacle from 2018 is gone: zBeWalgo had to go through the crypto API. Since 2024 zram has its own
backend API (`917a59e81c34`, "zram: introduce custom comp backends API"), so quetschn touches
`lib/` + `drivers/block/zram/` only, and the format-stability objection is weaker — zram data does not
survive a reboot.

---

## 3. The technical ground truth (verified against mainline)

These five facts shape the codec design and the benchmark. Most of them are routinely missed by
public compression benchmarks.

### 3.1 zram does not pay for bytes; it pays for zsmalloc buckets

`zram_write_page()` calls `zs_malloc(pool, comp_len, ...)`. zsmalloc quantizes
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

- `zs_create_pool()` walks the classes from largest to smallest and **merges** a class into the
  previous, larger one when both have the same `(pages_per_zspage, objs_per_zspage)`. An object
  that maps to a merged class gets the slot size of the larger class.
- `calculate_zspage_chain_size()` picks `pages_per_zspage` (1 to `CONFIG_ZSMALLOC_CHAIN_SIZE`) with the
  least tail waste, but the waste is often not 0. A class really costs
  `pages_per_zspage × PAGE_SIZE / objs_per_zspage` bytes per object, which is more than its slot size.

Computed cost for a page compressed to `comp_len` bytes (64-bit, 4 KiB pages,
`CONFIG_ZSMALLOC_CHAIN_SIZE=8`, the default). The numbers come from `bench/zsmalloc_cost.cpp`, a port
of the merge loop and of `calculate_zspage_chain_size()` at `986c24e0fe44`. It reproduces the huge
class watermark for all 13 chain sizes and the class listings in `Documentation/mm/zsmalloc.rst`,
except one row of the chain size 16 listing that contradicts the kernel code (see
`test/zsmalloc_cost_test.cpp`). `huge_class_size` depends on the config, so the
harness must read it from `/sys/kernel/debug/zsmalloc/` or recompute it, never hardcode it:

| comp_len | class slot | pages / objs per zspage | cost per page |
| --- | --- | --- | --- |
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
| --- | --- | --- |
| 0 - 1 KiB | 17 B | 32 B |
| 1 - 2 KiB | 28 B | 128 B |
| 2 - 3 KiB | 68 B | 144 B |
| 3 KiB - cliff | 93 B | 144 B |

Consequences:

- **Saving less than one class step on a page usually saves nothing.** Below 1 KiB that is ~16 bytes,
  above 2 KiB it is 68 to 144 bytes. A small format header is free for large pages. This gives real
  design freedom: richer per-page mode dispatch, alignment padding for a faster decoder, and explicit
  length fields cost nothing in practice.
- **There is a cliff at `huge_class_size` = 3625 bytes.** Above it, `zram_write_page()` calls
  `write_incompressible_page()` and stores the full 4096 bytes. Moving one page from 3625 to 3600
  saves 455 bytes, 18× more than the 25 bytes of compression it took. The cliff is the largest step,
  but every class boundary is a small cliff: a page that lands a few bytes above a boundary pays the
  full step. *Pages just above a class boundary are worth extra compression effort, and the ones just
  above the cliff most of all.* No general-purpose codec knows this.
- Two separate effects decide how much memory the classes really use. The **tail waste** inside a
  zspage is fixed per class and is already in the cost column above, so a userspace model gets it
  exactly. **Fragmentation** from partly filled zspages depends on the size distribution and on
  allocation history, so two codecs with the same Σ cost can still use different amounts of memory.
  Only a real zram run measures fragmentation (Phase 5). On the development machine the difference
  was large: for the pages of the first zram dump (Phase 1) the model gives 604 MB, zram's
  `mem_used_total` was 954 MB. Some minutes later zram had compacted (`pages_compacted` from 641 229
  to 725 853) and used 628 MB for 3% more pages, close to the model. Fragmentation comes and goes,
  and at its worst it is larger than the differences between codecs.

The primary ratio metric for this project is therefore **Σ zsmalloc cost** (the last column), plus
the **count of pages over the cliff**. Not mean compression ratio.

### 3.2 Decompression is a tail-latency problem, not a throughput problem

`zcomp_decompress()` runs in the swap-in path. In August 2026 an RFC (Sergey Senozhatsky) reported
that since `2efa9e9eb4db` ("zram: permit preemption with active compression stream") the zram stream
mutex became *the top lock contributing to Android UI frame drops, surpassing `mmap_lock`*. The
proposed fix adds an `async` flag per backend and restores `preempt_disable()` for synchronous
backends on `!PREEMPT_RT`.

Design constraints that follow:

- quetschn must be a **synchronous backend**: no sleeping, no allocation, no page faults in
  compress/decompress. It must remain correct with preemption disabled.
- The metric is the **per-page latency distribution** — p50, p99, p99.9, max — not aggregate MB/s.
- **Worst-case bounded runtime** is a feature to advertise, not an afterthought.
- Branch mispredictions dominate at 4 KiB inputs. An LZ literal/match loop has data-dependent,
  hard-to-predict branches. A word-granular format with near-data-independent control flow can win p99
  even where it ties on p50. This is the most likely source of a genuine C2 win. It is also an
  unverified assumption, and on an in-order little core (Cortex-A53/A55/A520) the balance between
  branch misses and instruction count is different. The Phase 2b spike tests it on both.
- **Cold caches.** In a real page fault both the compressed source and the destination page are cold.
  A benchmark that hot-loops one page out of L1 measures the wrong thing. The harness needs a
  cold-cache mode (§5.2) — this alone may reorder the existing codecs.

### 3.3 Per-CPU workspace is real memory on a phone

`zcomp_strm_init()` already allocates 3 pages per CPU (`local_copy` 4 KiB + `buffer` 8 KiB). On top:

| backend | compress workspace per CPU |
| --- | --- |
| `lzo` / `lzo-rle` | `LZO1X_1_MEM_COMPRESS` = 8192 × 2 = **16384 B** |
| `lz4` | `LZ4_MEM_COMPRESS` = ((1<<11)+4) × 8 = **16416 B** |
| `842` | `SW842_MEM_COMPRESS` = 0xf000 = **61440 B** |
| `zstd` | `zstd_cctx_workspace_bound()`, `kvzalloc`'d — far larger |
| **quetschn target** | **≤ 16416 B, less is a bonus** |

On a 16-core phone every 4 KiB less than `lz4` saves 64 KiB, permanently resident. Small, but it is a
free line in the cover letter and it is the kind of thing Android vendors notice. The target was 4 KiB
at first. It is `lz4`'s 16 KiB since 23rd September 2026: 8 points less memory on the compressed pages
are worth far more than a few KiB per CPU, and a 4 KiB budget costs speed or ratio in the matcher
(`seqlz-fast` has an 8 KiB hash table, `docs/explored-designs.md`).

### 3.4 The integration surface is small

Adding a backend is a contained diff:

- `lib/quetschn/` + `include/linux/quetschn.h` — the codec.
- `drivers/block/zram/backend_quetschn.{c,h}` — ~150 lines, modelled on `backend_lz4.c`.
- one entry in the `backends[]` array in `drivers/block/zram/zcomp.c`.
- `drivers/block/zram/Kconfig` + `Makefile`, `MAINTAINERS`, `Documentation/admin-guide/blockdev/zram.rst`.

`struct zcomp_params` carries `dict` / `dict_sz` / `level`, so **dictionary support is available and
should be designed in from the start** (see C4).

### 3.5 Page size is not always 4 KiB

Android supports 16 KiB page kernels on arm64, and since November 2025 Google Play requires apps that
target Android 15 or newer to work on them. The phones this plan targets will increasingly run with
16 KiB pages. zram compresses one `PAGE_SIZE` page at a time, and zsmalloc scales with it:
`ZS_SIZE_CLASS_DELTA = PAGE_SIZE >> CLASS_BITS` is 64 bytes on 16 KiB pages. The same model as §3.1 gives, for 16 KiB pages:

| | 4 KiB pages | 16 KiB pages |
| --- | --- | --- |
| class delta | 16 B | 64 B |
| distinct classes after merging | 119 | 121 |
| `huge_class_size` | 3625 | 14553 |
| mean class step in the top quarter below the cliff | 93 B | 371 B |

Which means that:

- `PAGE_SIZE` is a parameter of the format, of the cost model and of the harness from the start. A
  codec hand-tuned for 4096-byte inputs, e.g. with 12-bit offsets baked into the format, is a dead end.
- The corpus needs 16 KiB pages too (§5, Phase 1). Four adjacent 4 KiB pages are a first
  approximation, but real 16 KiB page kernels have different allocator behaviour.
- Larger inputs help `lz4` and `zstd` more than a word model, because their match window gets 4× more
  history. The quetschn advantage may shrink on 16 KiB pages. That has to be measured, not assumed.

The same argument applies to multi-page compression in zram, e.g. compressing whole large folios
(mTHP) as one unit. It has been proposed on the lists. At `986c24e0fe44`, `git log --grep=folio` on
`drivers/block/zram/` and `mm/zsmalloc.c` shows nothing of that kind merged. It is tracked as R9.

---

## 4. Constraints

### Kernel code rules (non-negotiable)

- Plain C, `-std=gnu11` (kernel `Makefile:813`), no libc, no floating point, no VLAs.
- No SIMD. `kernel_neon_begin()` / `kernel_fpu_begin()` cost time and are not allowed in all contexts.
  All existing kernel compressors are scalar. The prior SSE work on memlz does not transfer.
- Stack frames well under `CONFIG_FRAME_WARN` (2048 on 64-bit); target < 256 B.
- Big-endian must work — zram still gets big-endian fixes (`a8b5875741d4`, Sep 2026). Use
  `get_unaligned_le64()` from `<linux/unaligned.h>`; never type-pun.
- SPDX identifier in every file. `scripts/checkpatch.pl --strict` clean.
- Signed-off-by / DCO on every patch.

### Project constraints (confirmed with the maintainer)

- **License: `MIT OR GPL-2.0-only`**, SPDX line in every file. Same model zstd uses. The MIT half lets
  userspace and other kernels adopt it, which is how the "this has users" argument gets built.
  Apache-2.0 is excluded: not GPL-2.0 compatible.
- **Time budget: 3–8 h/week.** Realistic end-to-end timeline is **18–24 months**. Every phase below is
  sequenced to have standalone published value, so a stall does not waste the work before it.
- **Hardware today: x86-64 only.** This is the single largest schedule risk (§8, R1). arm64 numbers are
  a hard prerequisite for submission: lzo-rle was merged on arm64-first evidence, and phones are the
  users. An old phone is used from Phase 2 on, so that arm64 timings exist early. It has both an
  out-of-order big core and an in-order little core (e.g. Cortex-A76 + A55), which is what two separate
  boards would have given, but on real phone silicon. Requirements: an unlockable bootloader (root is
  needed to read other apps' memory for the corpus and to pin the CPU frequency, e.g. Pixel 3 to 7),
  and Android 10 or newer. Phase 6 then adds a current phone and 16 KiB pages, and still **gates
  Phase 7**.

---

## 5. Phases

Each phase ends in an artifact and a gate. Durations are calendar weeks at 3–8 h/week.

### Phase 0 — Repository foundation (2 weeks)

- `LICENSE-MIT`, `LICENSE-GPL-2.0`, SPDX headers, `README.md`, `CLAUDE.md` (from `martinus/ai` template).
- `.gitignore` with `corpus/` and `*.pages` excluded from the first commit. **Page dumps are never
  committed or published** — they contain keys, passwords and personal data.
- CMake + C++20 for tools and harness; the codec core stays C11.
- CI from day one, including a **kernel-compat job**: compile the core with kernel-like flags
  (`-std=gnu11 -ffreestanding -nostdinc -Wframe-larger-than=256 -fno-builtin`), plus `checkpatch.pl`,
  ASan/UBSan, and a big-endian cross-compile (`s390x` or `mips` via qemu-user).
- `CONTRIBUTING.md` requiring `Signed-off-by:` (DCO), matching kernel practice.
- Get the old phone (§4) and root it.
- ~~Check the employer rules (R8).~~ Done, side projects are fine.

*Gate: CI green on an empty stub codec.*

### Phase 1 — Corpus tooling (4 weeks)

The corpus decides everything downstream. Two collectors, because they answer different questions:

1. **Resident-anonymous sampler.** Walks `/proc/<pid>/maps`, reads anonymous private mappings via
   `process_vm_readv()`, writes pages. Cheap, broad, biased.
2. **Swap-path capture (the better one).** Rodgman sampled *resident* memory; zram actually stores
   *cold, reclaimed* pages, which are a different distribution. Capture the real thing, cheapest method
   first:
   - Read the zram device itself: `dd if=/dev/zram0 bs=4096`. zram decompresses every stored page on
     read, so this returns exactly the pages that reclaim put there, and slots that are free read as
     zeros, which the tools skip as same-filled anyway. Needs root, no kernel patch, no BPF, and works
     the same on a rooted phone. Honor collected the data for f0f6f7871430 this way (their patch also
     sets `huge_class_size` to 0, for some reason; reading does not need that).
   - Swap to a plain block device in the VM instead of zram, zero it before `mkswap`, drive the VM
     into memory pressure, then read the swap device from the host. It contains exactly the pages
     that reclaim chose. No kernel patch, no BPF. Caveats: slots freed during the run keep stale
     pages, so snapshot while the workload is still under pressure; and reclaim behaves a bit
     differently with slow disk swap than with zram, which `swappiness` only partly corrects.
   - Only if those caveats turn out to matter: a small BPF/kprobe tool on `zram_write_page()`, or a
     locally patched zram with a debugfs page dumper.

   **This is a methodological improvement over the published precedent, and the swap-device method
   makes it cheap.**

Both record, per page: all-zero, same-filled (zram handles these before any codec —
`page_same_filled()`), zero density, byte entropy, source workload.

Workload set, scripted and reproducible in a VM: Firefox with a fixed URL list, a JVM service, a
Python/numpy job, a `make -j` kernel build, a GNOME session, PostgreSQL, Electron.

**Android pages without a phone.** Cuttlefish, the AOSP virtual device, runs on x86-64 with zram
enabled and runs real ART. The ART object layout is mostly architecture-independent (compressed 32-bit
references either way), so an x86-64 Cuttlefish instance running a scripted app mix gives Android heap
pages a year before Phase 6. They are not a replacement for phone data, but they show early whether
Android pages look different from desktop pages (R4).

**16 KiB pages.** An old phone runs 4 KiB pages; only Pixel 8 and newer offer 16 KiB pages, as a
developer option. A Raspberry Pi 5 runs 16 KiB pages by default. Until one of them is available,
concatenate four adjacent 4 KiB pages of the same mapping as an approximation (§3.5).

**Collector 1 is done:** `quetschn-collect-resident` in `tools/collect/`. It finds resident pages with
the `PAGEMAP_SCAN` ioctl (Linux 6.7+) and falls back to reading `/proc/<pid>/pagemap` on older kernels,
so it runs on the old phone too. Pages that are already swapped are counted but never read, because
reading them would swap them back in. On the development machine (Fedora, zram swap) a first run found
1.8 million resident and 313 000 already swapped pages.

**Collector 2, the `dd` variant, is done:** `quetschn-import-raw` turns a `dd` of `/dev/zram0` into a
corpus (`README.md` has the commands). The first dump of the development machine has 460 923 pages
that are not zero. It also checks the harness against the kernel: `lzo-rle` built in userspace
reproduces zram's `mm_stat` for these pages to the last digit, 11 852 pages stored uncompressed
(`huge_pages`), 5684 same-filled (`same_pages`) and 590 800 867 compressed bytes (`compr_data_size`).

**Reproducibility strategy.** Two corpora:
- *private*: real dumps, never leave the machine, used for the headline numbers.
- *public*: generated by the scripted VM workloads above, redistributable, so third parties can
  reproduce the comparison without trusting anyone's private data.

*Gate: ≥ 500 000 pages across ≥ 6 workload types, including Cuttlefish; same-filled fraction measured
and reported separately.*

### Phase 2 — Benchmark harness and published baseline (8 weeks) — **first public artifact**

The most valuable thing this project can produce, independent of whether a codec ever ships.

#### 5.1 Codecs under test

Built **from the kernel tree's own sources** (`lib/lzo`, `lib/lz4`, `lib/zstd`, `lib/842` from the
local clone), not from upstream releases — the kernel's lz4 in particular lags upstream. This makes the
numbers directly predictive of kernel behaviour, which is exactly what maintainers need and what no
existing benchmark provides.

`lzo`, `lzo-rle`, `lz4` (acceleration 1/2/4), `lz4` + trained dict, `lz4hc`, `zstd` −1/1/2/3 with and
without trained dict, `deflate`, `842`. Plus, as design references: WKdm, WK4x4, memlz, LZAV.

Plus one system configuration, because it is the first alternative a maintainer will suggest:
**`lz4` as primary, `zstd` recompression of idle pages** via `CONFIG_ZRAM_MULTI_COMP`. Modelled as
`lz4` for the pages that get read back soon and `zstd` for the rest, using the idle fraction measured
in the workload. The cover letter has to explain why quetschn is still worth it next to that setup.

**Compiler flags match the kernel.** Every codec in the harness, quetschn included, is built with the
flags the kernel uses for `lib/`: `-O2 -fno-strict-aliasing`, and no SIMD registers
(`-mno-sse -mno-mmx -mno-avx -mno-sse2` on x86-64, `-mgeneral-regs-only` on arm64). Otherwise the
compiler auto-vectorizes loops in userspace that it cannot vectorize in the kernel, and the userspace
numbers are not predictive. The exact flags are taken from a `make V=1` build of the local tree, not
from this list.

#### 5.2 Metrics

All metrics exclude same-filled pages, because zram stores them before any codec runs
(`page_same_filled()`).

1. **Σ zsmalloc cost** using the *real merged class table* and the per-object cost including zspage
   tail waste (§3.1). This is the primary ratio metric.
2. **Pages at or over `huge_class_size`** (3625 B on 4 KiB pages), the cliff count.
3. **Per-page decompression latency**: p50 / p90 / p99 / p99.9 / max, in **both** warm-cache and
   **cold-cache** modes (flush the source and destination between pages). Each page is timed several
   times, and the page's latency is the **median** of those runs. The percentiles are then taken
   across pages. That way p99 describes the slowest 1% of *pages*, which is a property of the data,
   and not the slowest 1% of *measurements*, which is mostly interrupts and timer noise.
4. **Per-page compression latency**, same statistics.
5. **Weighted roundtrip**, with the compression:decompression ratio measured from the actual workload
   rather than assumed: the time of the score of §1.1, from the means.
6. **Per-CPU workspace bytes.**
7. **`perf` counters per page**: instructions, cycles, branch-misses, LLC-misses — these explain *why*
   a codec wins and make the eventual design argument credible.

Timing with a dedicated per-page loop (`rdtscp`/`lfence` on x86-64, the PMU cycle counter through
`perf_event_open` on arm64, because `cntvct_el0` ticks at only 19 to 54 MHz on many boards), because nanobench reports statistics over batches and not a distribution over pages. nanobench
stays useful for the aggregate throughput numbers. Pinned cores, fixed frequency, `perf` counters where
available.

**On which hardware.** x86-64 and the big and little core of the old phone, from the first published
table on.

#### 5.3 Statistical presentation

Copy the shape of the lzo-rle commit message, because it worked:

- per-page **paired** differences against each baseline, not just aggregates;
- count of pages regressing by > 1σ, and the largest regression;
- the full distribution (violin/CDF per codec), since the data is bimodal;
- results split by workload;
- 95% bootstrap confidence intervals, resampling pages, for every Σ cost and every percentile
  difference;
- a **train/test split by workload**. Dictionaries for `lz4` and `zstd` are trained on some workloads
  and measured on the others; everything tuned for quetschn later (Phase 3) uses the same split. A
  dictionary trained on the pages it is measured on overstates C4, and so does a tuned codec. A zram
  dump has no process names, so there the split is by time: train on one dump, measure on a dump
  taken days later, and leave the pages that are in both out of the training side
  (`tools/bench-dict.sh`).

*Gate (go/no-go): headroom cannot be measured on a codec that does not exist yet, so the gate uses
a proxy. On the test corpus, does `zstd -1` without a dictionary need ≥ 12% less Σ zsmalloc cost than
the better of `lz4`+dict and `lzo-rle`? `zstd -1` stands in for "how much redundancy is left that a
fast codec could still find". It is not a strict bound, but quetschn will not get all of it, so the
gate asks for more than C1's 8%. The 12% is a judgment call; revisit it when the first table exists.
If the proxy shows less, go to §8.*

Publish this on martin.ankerl.com and in the repo regardless of the outcome. A negative result is
still the first honest public zram codec comparison.

**Then post it to linux-mm**, without proposing a codec. Invite criticism of the methodology, and ask
directly whether the zram maintainers would consider a new backend at all, and what evidence they
would want. This is cheap and it tests R3 about 18 months earlier than a Phase 7 posting would. A
clear "no" redirects the project to the R2 fallback before any codec work.

### Phase 2b — Decoder latency spike (2 weeks)

The Phase 2 gate tests ratio headroom. It does not test C2, and C2 is the less likely of the two:
kernel `lz4` decode of a 4 KiB page is mostly `memcpy` of literals and matches, which is hard to beat.

Write the smallest decoder of the design that §3.2 bets on: a WKdm-style 64-bit-word format with a
fixed tag layout, no LZ pass, no dictionary, and a throwaway encoder. Measure its cold-cache p99 per
page (§5.2) against kernel `lz4` on the test corpus, on x86-64 and on the phone's in-order little core.

*Gate: the spike decoder's p99 is at or below `lz4`'s on both machines. If it is clearly slower, the
p99 argument of §3.2 does not hold, and Phase 3 starts from the §8 fallback instead of from the word
model.*

**x86-64 half: passed.** The spike is in `spike/`: 64-bit words, 2-bit tags (zero, exact match or
high-32-bits match against a 16-entry table of recent words, literal), four sections whose lengths
follow from the tags, so the decoder checks the length once and the loop has no bounds checks. All
decoders are built with the kernel's flags and `-O3`, like `lz4`, and decode the same format.

The first three decoders were at parity with `lz4`: a `switch` on the tag, a branchless one that
selects with masks, and the `switch` with a fast path that writes a zero tag byte as 4 zero words at
once. What made the difference is the table update. Those decoders write each word to the table slot
of its hash, `slot_of(w)`, so the address of the store is only known after the table load that
produced `w`, and the next word's table load has to wait or guess. `spike-slots` takes the slot from
the stream instead: the stored index for exact and partial words, which the encoder wrote from the
same hash, and the hash of the stored word for literals. No store address then depends on a table
load, and an exact match needs no store at all.

`quetschn-bench-interleaved` runs several codecs in one process, each repetition runs every codec
once on the same page, so a drift of the CPU frequency hits all of them alike. On the 455 239 pages of
the first zram dump, Ryzen 9 7950X pinned to one core, `powersave` governor, median of 5 runs per page,
percentiles over the 344 955 pages that both store compressed:

| run | decoder | warm p99 | cold p50 | cold p99 | cold p99.9 |
| --- | --- | --- | --- | --- | --- |
| 1 | `lz4` | 2030 ns | 1360 ns | 2450 ns | 3180 ns |
| 1 | spike, `switch` | 1850 ns | 1630 ns | 2720 ns | 2970 ns |
| 1 | spike, zero fast path | 1820 ns | 1610 ns | 2810 ns | 3060 ns |
| 1 | `spike-slots` | 720 ns | 1390 ns | 1970 ns | 2160 ns |
| 2 | `spike-slots` | 710 ns | 1380 ns | 2160 ns | 2460 ns |
| 2 | `lz4` | 2030 ns | 1670 ns | 2890 ns | 3440 ns |

Run 2 has the order of run 1 reversed, because in one binary the code layout of one codec can shift
another. Paired over all pages, `spike-slots` is faster than `lz4` at cold p99 by 480 ns [470, 500]
in run 1 and 820 ns [810, 830] in run 2, and at warm p99 by 1270 and 1280 ns. The absolute numbers
of `lz4` move between the two runs, the advantage does not change sign. A branchless decoder in an
earlier, separate run was slower everywhere (cold p99 3690 ns), so data-independent control flow
alone is not what §3.2 hoped for; the win comes from a short dependency chain per word.

Where `lz4` is still better: pages that it compresses below 512 bytes, 73% zero words, cold p50 780
against 840 ns in run 1. There `lz4` copies long matches while the spike still visits every tag.

The spike is not a codec: 55.7% Σ zsmalloc cost against 34.5% for `lz4`, 24% of the pages stored
uncompressed. For Phase 3 this means: keep the store address of the table update independent of the
table load, give runs of zeros and repeats a path that costs per run and not per word, and measure
latency only interleaved. The arm64 little core is the other half of this gate and is still open;
an in-order core may weigh the dependency chain differently.

### Phase 3 — Page analysis and design exploration (12 weeks)

Every design that has been measured, with its numbers and why it was kept or dropped, is in
[`docs/explored-designs.md`](docs/explored-designs.md). Add to it before trying something new.

Answer, with numbers from the corpus:

- What fraction of pages is genuinely incompressible (already-compressed data, encrypted buffers)?
  These should be detected in a few hundred cycles and bailed out on, not compressed slowly.
- What is the 8-byte-word structure? Distribution of distinct high-32-bit prefixes per page (pointer
  density), stride-8 and stride-16 autocorrelation (array-of-struct layout), small-integer density.
- Where does `lz4` lose relative to `zstd`? Entropy coding, or match finding?
- How many pages sit within 256 bytes above the cliff, and what recovers them?
- How many pages sit a few bytes above a class boundary (§3.1), and how much Σ cost comes back if
  those pages get one extra compression attempt?
- How often does a greedy parser get stuck in a chain of short matches on periodic data, like
  `lz4` with a dictionary does (§9, next action 6)? A long match at a small multiple of the period is
  cheap to check for.
- Do the answers hold for Cuttlefish pages and for 16 KiB pages (§3.5)?

Candidate designs to prototype in userspace, ranked by expected value:

1. **Word-granular model with mode dispatch.** WKdm's idea, modernised: 64-bit words, a larger
   recent-word dictionary, tags for zero / exact-match / high-bits-match+low-bits-literal / literal.
   Near-data-independent control flow → the p99 win of §3.2. The class step (16 to 144 bytes, §3.1)
   pays for a per-page or per-512-byte mode header for free.
2. **Zero/RLE fast path.** Rodgman's corpus was bimodal: 44% of pages ≤ 5% zeros, 44% ≥ 90% zeros.
   Two well-separated modes beat one compromise model. Verify the bimodality holds on our corpus first.
3. **Word model + short LZ pass** over the residual.
4. **Boundary-recovery mode**: when the fast path lands just above a class boundary, spend extra
   compression time (a static Huffman or a small rANS stage) on that page only. The encoder knows the
   class table, so it knows the target size. Compression time is less precious than decompression
   time. The payoff is largest at `huge_class_size`, 455 bytes per rescued page, and 16 to 144 bytes
   at every other boundary.
5. **Dictionary support** from the start, via `zcomp_params->dict` (C4).

A slower, higher-ratio sibling for `CONFIG_ZRAM_MULTI_COMP` recompression of idle pages is a legitimate
second target (working name `wuzl`) — but only after the primary codec lands. Do not split effort.

*Gate: one design meets C1 against `lz4`+dict at equal or better p99 latency, in a userspace
prototype, on the held-out test workloads, on x86-64 and the phone's in-order little core.*

### Phase 4 — Reference implementation, format spec, fuzzing (20 weeks)

This phase exists because of §2.2. Everything Biggers asked for, before the first patch.

- `quetschn.c` / `quetschn.h`: C11, freestanding, no libc, no allocation, scratch of at most `lz4`'s
  16416 B passed in by the caller. Page size is a parameter, and every test runs with 4 KiB and 16 KiB
  pages (§3.5).
- **`FORMAT.md`**: byte-exact format specification, plus an independent, deliberately slow reference
  decoder written from the spec alone. Differential-test the fast decoder against it.
- **Fuzzing** (AFL++ is already checked out at `~/gra/AFLplusplus`):
  - decompressor vs arbitrary input — must never read or write out of bounds, must always terminate,
    for truncated, corrupted and adversarial input;
  - roundtrip property fuzzing with structure-aware generators;
  - differential fuzzing fast decoder vs reference decoder;
  - continuous fuzzing in CI with ClusterFuzzLite (GitHub Actions). OSS-Fuzz only accepts projects
    with a significant user base or importance for critical infrastructure, so a new codec will likely
    be rejected before it has users. Apply anyway, and apply again once there is adoption. *"We run
    under OSS-Fuzz"* would be a strong, checkable answer to objection 3, but the plan does not depend
    on it.
- Sanitizers: ASan, UBSan, MSan in CI. Big-endian correctness tests.
- Worst-case runtime bound, stated and measured.

*Gate: 1e9 fuzz executions with no crash; spec and implementation agree on the full corpus plus fuzz
corpus; frame size and workspace within budget.*

### Phase 5 — Kernel port and in-VM validation (16 weeks)

- `lib/quetschn/` + `include/linux/quetschn.h`, kept **free of any zram API dependency** — the zram
  backend API has churned repeatedly (2024 rewrite, 2025 preemption series, 2026 param handling). A
  thin `backend_quetschn.c` adapter absorbs that churn.
- `backend_quetschn.c` modelled directly on `backend_lz4.c`, including `setup_params` validation
  (`7b0f677c7bd5`) and `pr_fmt` (`70922d5ef84a`).
- QEMU test rig: a VM with zram as swap, driven into memory pressure by a reproducible workload.
  Measure **actual** `mm_stat` memory used. This is where the fragmentation effect of §3.1 shows up
  and where the userspace cost model gets validated or falsified.
- Run `tools/testing/selftests/zram/`; add cases as needed.
- Correctness under `CONFIG_DEBUG_ATOMIC_SLEEP`, `PROVE_LOCKING`, KASAN, and with preemption disabled.
  Minchan's zBeWalgo panic was exactly this class of bug.

*Gate: a kernel build with quetschn survives sustained swap thrash under KASAN, and in-VM `mm_stat`
confirms the userspace prediction within 2%.*

### Phase 6 — arm64 validation (**hard gate before Phase 7**)

Unavoidable. No arm64 numbers, no patch. The old phone has produced arm64 timings and phone pages since
Phase 2, so this phase no longer starts from zero. What is still missing is a current phone and 16 KiB
pages. Options in descending order of value:

1. A current rooted Android phone that runs a 16 KiB page kernel (Pixel 8 or newer).
2. Ask Dave Rodgman (ARM, author of lzo-rle), the Android kernel team or the linux-mm list for help
   measuring. The Phase 2 publication makes this ask reasonable rather than presumptuous.

Also needed: a corpus from a current phone. Android page data may differ from desktop data (ART heap
layout, different allocator), and a newer Android version may differ from the old phone. The old phone
and Cuttlefish corpora already give a first answer to that, so a surprise here should be small. The Phase 3 design still stays parameterised rather than
hand-tuned to one corpus.

*Gate: C1–C3 hold on arm64, on both a big and a little core, with 4 KiB and 16 KiB pages.*

### Phase 7 — Upstreaming (6+ months, expect v5+)

- The benchmark and the question about a new backend were already posted after Phase 2. Before the
  RFC, reply in that thread with the updated numbers, so the series does not arrive cold.
- Then an RFC series. Cover letter modelled on `5ee4014af99f`: corpus provenance, distribution, both
  architectures, paired per-page regression analysis, fuzzing status, workspace comparison.
- Series shape: (1) `lib/quetschn` + spec + `MAINTAINERS`, (2) zram backend + Kconfig/Makefile,
  (3) documentation, (4) selftests.
- Audience: Sergey Senozhatsky and Minchan Kim (zram/zsmalloc maintainers, per `MAINTAINERS`),
  linux-mm, Andrew Morton (lib/ goes via the mm tree), Eric Biggers (reviewed zBeWalgo; will review
  this), Dave Rodgman.
- Commit to maintaining it. A `MAINTAINERS` entry is a multi-year promise and maintainers will read it
  as one.
- After the merge: propose `CONFIG_ZRAM_BACKEND_QUETSCHN=y` for Android's `gki_defconfig`, with the
  phone numbers from Phase 6. Without that, the codec is merged but not on phones (§1).

---

## 6. Repository layout

```
include/quetschn.h          C11, freestanding, no libc
src/quetschn.c              the codec core
src/quetschn_ref.c          independent reference decoder (from FORMAT.md only)
FORMAT.md                   byte-exact format specification
tools/collect/              corpus collectors (C++)
tools/analyze/              page statistics (C++)
bench/                      harness: per-page timing loop + kernel-sourced codecs
bench/zsmalloc_cost.*       zsmalloc cost model (§3.1)
bench/kernel_codecs/        zram's calls into lib/lz4 and lib/lzo, and the headers to build them in userspace
cmake/kernel_codecs.cmake   builds them from QUETSCHN_KERNEL_TREE with kernel flags; sources never copied
test/                       doctest unit tests
fuzz/                       AFL++ / libFuzzer targets
kernel/                     backend_quetschn.c + Kconfig/Makefile fragments + patch generator
results/                    published measurements (no raw pages, ever)
.github/workflows/          CI incl. kernel-flag build, checkpatch, big-endian, fuzz smoke
```

---

## 7. What is deliberately deferred

- `wuzl`, the high-ratio recompression sibling. Real opportunity via `CONFIG_ZRAM_MULTI_COMP`, but
  splitting effort before the primary codec lands is how projects like this die.
- zswap integration. Different allocator, different constraints. Later.
- The synthetic privacy-safe page generator. Genuinely valuable to the field, but it is a second
  project; the public VM-workload corpus (Phase 1) covers the reproducibility need at a fraction of the
  cost.
- Hardware-accelerator comparison (842 on POWER, phone hardware compressors). Out of scope.

---

## 8. Risks, and what to do about each

| # | Risk | Evidence | Mitigation / fallback |
| --- | --- | --- | --- |
| R1 | **No arm64 hardware.** Phones are the users; lzo-rle was merged on arm64-first data. | Confirmed constraint | An old rooted phone (big and little core) is used from Phase 2 on. Phase 6 adds a current phone with 16 KiB pages and is a hard gate. Do not submit without it. |
| R2 | **Insufficient headroom over lz4+dict.** Nobody has measured this. The whole project rests on an unverified assumption. | No measurement of `lz4`+dict on page data known to this plan | Phase 2 gate answers it before any codec work. Fallback: publish the benchmark, then pursue a *targeted improvement to lz4 or lzo-rle for page-sized inputs* — that is exactly what lzo-rle was, and it is a much easier merge. |
| R3 | **Maintainers do not want another backend.** Each one is permanent maintenance cost. | zBeWalgo reached v7 and died | Ask right after Phase 2, together with the benchmark posting, before any codec work. A "no" discovered early redirects to R2's fallback. |
| R4 | **Desktop-tuned codec loses on Android data.** Different heap layout, different allocator. | ART and bionic lay out the heap differently from glibc desktop processes; not measured yet | Cuttlefish pages in the Phase 1 corpus, so the difference is measured before Phase 3. Keep Phase 3 designs parameterised, not hand-tuned. Obtain real phone pages before freezing the format. |
| R5 | **zram backend API churn.** 2024 rewrite, 2025 preemption series, 2026 param and naming changes. | `git log drivers/block/zram/` | Codec core has zero kernel-API dependency; all churn is absorbed by `backend_quetschn.c`. Rebase against mainline in CI. |
| R6 | **Fuzz-safety or a sleeping-in-atomic bug burns maintainer goodwill.** | Biggers's objection; Minchan's panic | Phase 4 and the Phase 5 KASAN/`DEBUG_ATOMIC_SLEEP` gate exist for this. Continuous fuzzing with ClusterFuzzLite before submission; OSS-Fuzz if it accepts the project. |
| R7 | **Timeline.** 3–8 h/week against an 18–24 month path. | lzo-rle: 4 months, v5, paid work, existing codec | Each phase publishes independently. Phase 2 alone is a worthwhile public contribution. |
| R8 | **Employer rules on open-source side projects**, particularly kernel contributions with a `MAINTAINERS` entry. | Checked 2026-09-23: side projects are fine | Resolved. |
| R9 | **The input size changes under the codec.** 16 KiB page kernels on Android, or zram compressing multi-page folios as one unit. Larger inputs favour LZ codecs with a larger window. | §3.5; multi-page compression proposed on the lists, not merged at `986c24e0fe44` | `PAGE_SIZE` is a parameter of format, cost model and harness from Phase 0. Every table from Phase 2 on has a 16 KiB column. Watch the zram and mm lists for multi-page compression, and rerun the Phase 2 gate if it gets merged. |
| R10 | **No p99 decode win over `lz4`.** C2 rests on the branch-misprediction argument of §3.2, which is unmeasured. | Kernel `lz4` decode is mostly `memcpy` | Phase 2b spike measures it in 2 weeks, before Phase 3. If it fails, fall back to the R2 route. |

---

## 9. Immediate next actions

1. Get the old phone and root it (§4).
2. ~~Check the employer rules (R8).~~ Done.
3. Finish Phase 0: the kernel-flag build job, which needs the codec stub. Licenses, `README.md`,
   CMake, doctest and CI are done.
4. The zsmalloc cost model is done: `bench/zsmalloc_cost.cpp`, with `PAGE_SIZE` as a
   parameter. The first zram dump confirms `huge_class_size`: the harness stores exactly as many
   pages uncompressed as zram (Phase 1). Still open is a check of the other classes against
   `/sys/kernel/debug/zsmalloc/<pool>/classes`, which Fedora's kernel does not have.
5. Both collectors are done: `quetschn-collect-resident`, and `quetschn-import-raw` for a `dd` of the
   zram device. Next in Phase 1: a second zram dump some days later, to train a dictionary on
   swapped pages and measure it on other swapped pages (§5.3), then the scripted VM workloads.
6. The harness runs `lz4`, `lzo`, `lzo-rle` and `zstd` from the kernel tree with kernel flags, with
   and without dictionary: `quetschn-bench-<codec> [--level n] [--dict file]`, one binary per codec.
   `quetschn-split-corpus` splits a corpus by process name, so a dictionary is trained on programs it
   is not measured on (§5.3). `quetschn-compare` pairs two runs page by page, with bootstrap
   confidence intervals for the saving and for every latency percentile difference. Still missing:
   the arm64 flags from a real arm64 kernel build, and the PMU cycle counter on arm64.

   First run with dictionaries, only to shake out the harness. 61 043 resident pages of the
   development machine (the biased collector 1), split by process name: 72 names to train a 64 KiB
   dictionary with `zstd --train -B4096 --maxdict=64KB` (Honor's settings), 32 other names with 8937
   measured pages to test on. Ryzen 9 7950X pinned to one core, `powersave` governor so the frequency
   was not fixed, median of 5 runs per page, TSC resolution about 10 ns:

   | codec | Σ zsmalloc cost | per CPU | per device | decompress cold p50 / p99 |
   | --- | --- | --- | --- | --- |
   | `lz4` | 38.2% | 16 440 B | 0 | 1840 / 2940 ns |
   | `lz4` + dict | 36.6% | 16 472 B | 16 416 B | 1740 / 3010 ns |
   | `lzo-rle` | 36.0% | 16 384 B | 0 | 1820 / 3480 ns |
   | `lzo` | 35.2% | 16 384 B | 0 | 2240 / 3800 ns |
   | `zstd -1` | 30.2% | 169 728 B | 75 112 B | 3490 / 5120 ns |
   | `zstd -1` + dict | 29.5% | 153 344 B | 58 728 B | 3150 / 4820 ns |
   | `zstd 3` (zram default) | 27.6% | 186 112 B | 91 496 B | 4260 / 6570 ns |
   | `zstd 3` + dict | 27.3% | 186 112 B | 435 560 B | 4890 / 7820 ns |

   The dictionary saves `lz4` 4% here, not enough to beat `lzo-rle`. The Phase 2 gate proxy:
   `zstd -1` needs 16% less memory than `lzo-rle`, the better of `lz4` + dict and `lzo-rle`, above the
   12% bar. Still not the gate: wrong page population, one run, unfixed frequency.

   Two side findings. `backend_zstd.c` creates a cdict and a ddict also without a dictionary, which
   costs 73 to 89 KiB per zram device for nothing. And the `zstd --train ... --split=4096` in the
   f0f6f7871430 commit message is not an option zstd 1.5.7 accepts; `-B4096` cuts the samples into
   pages.

   The same codecs on the pages zram really holds, the first zram dump (Phase 1): 455 239 pages
   measured, 5684 same-filled skipped. Same machine and setup, median of 3 runs per page. The
   dictionary is the one from above, trained on resident pages. Memory per CPU and per device are the
   same as in the table above:

   | codec | Σ zsmalloc cost | stored uncompressed | compress p99 | decompress cold p50 / p99 |
   | --- | --- | --- | --- | --- |
   | `lz4` | 34.5% | 10 523 | 3540 ns | 1660 / 2930 ns |
   | `lz4` + dict | 33.7% | 11 783 | 3910 ns | 1670 / 3060 ns |
   | `lzo-rle` | 32.4% | 11 852 | 3770 ns | 1660 / 3040 ns |
   | `lzo` | 31.9% | 11 831 | 3770 ns | 2020 / 6270 ns |
   | `zstd -1` | 26.9% | 9638 | 8680 ns | 3290 / 4730 ns |
   | `zstd -1` + dict | 26.8% | 9846 | 9930 ns | 3020 / 5560 ns |
   | `zstd 3` (zram default) | 23.6% | 7491 | 15 700 ns | 3990 / 7080 ns |
   | `zstd 3` + dict | 24.2% | 7499 | 18 530 ns | 3860 / 7210 ns |

   The gate proxy holds up on swapped pages: `zstd -1` needs 16.9% less Σ zsmalloc cost than
   `lzo-rle`, which is again better than `lz4` + dict. A dictionary trained on resident pages saves
   `lz4` only 2.3% on swapped pages, and makes `zstd 3` worse. Still not the gate: one machine,
   one dump, no confidence intervals, and the dictionary was trained on a different page population.

   `lz4` + dict stores 1292 pages uncompressed that `lz4` alone does not, and 75 the other way. The
   harness is right about that, upstream LZ4 1.10.0 gives the same sizes. The dictionary gains 2.72% Σ
   zsmalloc cost on the pages it helps and loses 0.35% on the others, mostly on pages that already
   compress to 2.5 to 3.5 KiB, 0.107% alone for the pages pushed over the cliff. The extreme case is
   weird: a page of `ff`×16 `00`×16 repeated compresses to 49 bytes without a dictionary and to 1543
   bytes with one, and a dictionary of the 8 bytes `00 00 00 00 00 00 00 04` is enough. A match into
   the dictionary at the start shifts the greedy parse, and from then on the "test next position"
   shortcut of `LZ4_compress_generic` only finds matches of 5 to 11 bytes with offsets 2 and 21 to 27,
   512 of them, and never gets back to the search that would find the 4019-byte match at offset 32.
   Without a dictionary the same chain happens too, but it breaks after 72 bytes. Rotating the page
   shows that it is the dictionary: without one, all 32 rotations compress to 36 to 49 bytes, with the
   8-byte dictionary 8 of 32 rotations go to about 1540 bytes. On the zram dump only 107 pages got
   more than twice as large, 0.01%, so it does not change the table. Reported upstream as
   [lz4/lz4#1805](https://github.com/lz4/lz4/issues/1805).

Step 6 is the cheapest check that could disprove the project's central assumption. Reach it before
writing a single line of codec.
