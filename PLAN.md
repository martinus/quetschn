# quetschn — project plan

A compression codec for 4 KiB memory pages, aimed at the Linux kernel's zram module.

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
| C1 | Strictly better stored size than `lzo-rle` **and** `lz4` on real page data, measured in zsmalloc buckets (§3.1) | `lzo-rle` is the zram default; `lz4` is the common Android choice |
| C2 | Strictly better p99 decompression latency than `lz4` on the same data | decompression runs in the page-fault path (§3.2) |
| C3 | Compression latency within 1.2× of `lz4` | zram compresses more often than it decompresses |
| C4 | Beats `lz4` **with a trained dictionary**, not just bare `lz4` | zram supports dictionaries; Honor made dict-lz4 >50% faster in March 2026 (`f0f6f7871430`) |
| C5 | Per-CPU workspace ≤ 4 KiB | `lz4` needs 16416 B/CPU, `lzo` 16384 B/CPU, `842` 61440 B/CPU (§3.3) |
| C6 | Decompressor is fuzz-safe and bounded-time for arbitrary input | this is what killed the last new-codec attempt (§2.2) |

C1 and C2 together are the merge argument: *strictly dominates the current default and the current
fast option, on both memory and tail latency, with less per-CPU memory.* If C1 and C2 cannot both be
met after Phase 2, the project pivots (§8).

**Explicit non-goal:** beating `zstd` level 3 on ratio. If quetschn lands as "lz4-class latency at
zstd-1-class ratio", that is a win.

---

## 2. Precedent: what actually gets a compressor into zram

Two data points, both verified.

### 2.1 The success: lzo-rle (Dave Rodgman, ARM, merged March 2019, `5ee4014af99f`)

What the merged commit message contains, verbatim in substance:

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

These four facts shape the codec design and the benchmark. Three of them are routinely missed by
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

Computed cost for a page compressed to `n` bytes (64-bit, 4 KiB pages, `CONFIG_ZSMALLOC_CHAIN_SIZE=8`,
the default — `huge_class_size` is config-dependent and the harness must read it from
`/sys/kernel/debug/zsmalloc/` or recompute it, never hardcode it):

| comp_len | stored bytes |
| --- | --- |
| 100 | 112 |
| 111 | 128 |
| 1024 | 1040 |
| 2048 | 2064 |
| 3255 | 3264 |
| **3624** | **3632** |
| **3625** | **4096** |
| 4000 | 4096 |

Consequences:

- **Saving fewer than 16 bytes on a page usually saves nothing.** A format header of up to ~8 bytes is
  free. This buys real design freedom: richer per-page mode dispatch, alignment padding for a faster
  decoder, and explicit length fields all cost nothing in practice.
- **There is a cliff at `huge_class_size` = 3625 bytes.** Above it, `zram_write_page()` calls
  `write_incompressible_page()` and stores the full 4096 bytes. Moving one page from 3625 to 3600
  saves 480 bytes — 30× more than the 25 bytes of compression it took. *Pages near the cliff are worth
  disproportionate effort.* No general-purpose codec knows this.
- zsmalloc **merges** size classes with identical `(pages_per_zspage, objs_per_zspage)`, so the real
  granularity is sometimes coarser than 16 bytes. The harness must model the merged class table, not a
  naive 16-byte grid.
- Second-order: two codecs with the same bucket sum can still use different amounts of memory, because
  per-class zspage fill depends on the *distribution* of sizes. Only a real zram run measures this
  (Phase 5).

The primary ratio metric for this project is therefore **Σ zsmalloc bucket bytes**, plus **count of
pages over the cliff**. Not mean compression ratio.

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
  even where it ties on p50. This is the most likely source of a genuine C2 win.
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
| **quetschn target** | **≤ 4096 B, ideally 0** |

On a 16-core phone this is 256 KiB saved versus lz4, permanently resident. Small, but it is a free
line in the cover letter and it is the kind of thing Android vendors notice.

### 3.4 The integration surface is small

Adding a backend is a contained diff:

- `lib/quetschn/` + `include/linux/quetschn.h` — the codec.
- `drivers/block/zram/backend_quetschn.{c,h}` — ~150 lines, modelled on `backend_lz4.c`.
- one entry in the `backends[]` array in `drivers/block/zram/zcomp.c`.
- `drivers/block/zram/Kconfig` + `Makefile`, `MAINTAINERS`, `Documentation/admin-guide/blockdev/zram.rst`.

`struct zcomp_params` carries `dict` / `dict_sz` / `level`, so **dictionary support is available and
should be designed in from the start** (see C4).

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
  a hard prerequisite for submission — lzo-rle was merged on arm64-first evidence, and phones are the
  users. Phase 6 exists solely to close this and **gates Phase 7**.

---

## 5. Phases

Each phase ends in an artifact and a gate. Hours are calendar estimates at 3–8 h/week.

### Phase 0 — Repository foundation (2 weeks)

- `LICENSE-MIT`, `LICENSE-GPL-2.0`, SPDX headers, `README.md`, `CLAUDE.md` (from `martinus/ai` template).
- `.gitignore` with `corpus/` and `*.pages` excluded from the first commit. **Page dumps are never
  committed or published** — they contain keys, passwords and personal data.
- CMake + C++20 for tools and harness; the codec core stays C11.
- CI from day one, including a **kernel-compat job**: compile the core with kernel-like flags
  (`-std=gnu11 -ffreestanding -nostdinc -Wframe-larger-than=256 -fno-builtin`), plus `checkpatch.pl`,
  ASan/UBSan, and a big-endian cross-compile (`s390x` or `mips` via qemu-user).
- `CONTRIBUTING.md` requiring `Signed-off-by:` (DCO), matching kernel practice.

*Gate: CI green on an empty stub codec.*

### Phase 1 — Corpus tooling (4 weeks)

The corpus decides everything downstream. Two collectors, because they answer different questions:

1. **Resident-anonymous sampler.** Walks `/proc/<pid>/maps`, reads anonymous private mappings via
   `process_vm_readv()`, writes 4 KiB pages. Cheap, broad, biased.
2. **Swap-path capture (the better one).** Rodgman sampled *resident* memory; zram actually stores
   *cold, reclaimed* pages, which are a different distribution. Capture the real thing: a small
   BPF/kprobe tool on `zram_write_page()`, or a locally patched zram with a debugfs page dumper, run
   under deliberate memory pressure in a VM. **This is a methodological improvement over the published
   precedent and is worth the extra effort.**

Both record, per page: all-zero, same-filled (zram handles these before any codec —
`page_same_filled()`), zero density, byte entropy, source workload.

Workload set, scripted and reproducible in a VM: Firefox with a fixed URL list, a JVM service, a
Python/numpy job, a `make -j` kernel build, a GNOME session, PostgreSQL, Electron.

**Reproducibility strategy.** Two corpora:
- *private*: real dumps, never leave the machine, used for the headline numbers.
- *public*: generated by the scripted VM workloads above, redistributable, so third parties can
  reproduce the comparison without trusting anyone's private data.

*Gate: ≥ 500 000 pages across ≥ 6 workload types; same-filled fraction measured and reported separately.*

### Phase 2 — Benchmark harness and published baseline (8 weeks) — **first public artifact**

The most valuable thing this project can produce, independent of whether a codec ever ships.

#### 5.1 Codecs under test

Built **from the kernel tree's own sources** (`lib/lzo`, `lib/lz4`, `lib/zstd`, `lib/842` from the
local clone), not from upstream releases — the kernel's lz4 in particular lags upstream. This makes the
numbers directly predictive of kernel behaviour, which is exactly what maintainers need and what no
existing benchmark provides.

`lzo`, `lzo-rle`, `lz4` (acceleration 1/2/4), `lz4` + trained dict, `lz4hc`, `zstd` −1/1/2/3 with and
without trained dict, `deflate`, `842`. Plus, as design references: WKdm, WK4x4, memlz, LZAV.

#### 5.2 Metrics

1. **Σ zsmalloc bucket bytes** using the *real merged class table* (§3.1) — the primary ratio metric.
2. **Pages at or over `huge_class_size`** (3625 B) — the cliff count.
3. **Per-page decompression latency**: p50 / p90 / p99 / p99.9 / max, in **both** warm-cache and
   **cold-cache** modes (flush the source and destination between pages).
4. **Per-page compression latency**, same statistics.
5. **Weighted roundtrip**, with the compression:decompression ratio measured from the actual workload
   rather than assumed.
6. **Per-CPU workspace bytes.**
7. **`perf` counters per page**: instructions, cycles, branch-misses, LLC-misses — these explain *why*
   a codec wins and make the eventual design argument credible.

Timing via nanobench. Pinned cores, fixed frequency, `perf` counters where available.

#### 5.3 Statistical presentation

Copy the shape of the lzo-rle commit message, because it worked:

- per-page **paired** differences against each baseline, not just aggregates;
- count of pages regressing by > 1σ, and the largest regression;
- the full distribution (violin/CDF per codec), since the data is bimodal;
- results split by workload.

*Gate (go/no-go): with the baseline table in hand, is there ≥ 8% bucket-bytes headroom over the best of
`lz4`+dict and `lzo-rle` at equal-or-better p99 decompression latency? If no plausible headroom
exists, go to §8.*

Publish this on martin.ankerl.com and in the repo regardless of the outcome. A negative result is
still the first honest public zram codec comparison.

### Phase 3 — Page analysis and design exploration (12 weeks)

Answer, with numbers from the corpus:

- What fraction of pages is genuinely incompressible (already-compressed data, encrypted buffers)?
  These should be detected in a few hundred cycles and bailed out on, not compressed slowly.
- What is the 8-byte-word structure? Distribution of distinct high-32-bit prefixes per page (pointer
  density), stride-8 and stride-16 autocorrelation (array-of-struct layout), small-integer density.
- Where does `lz4` lose relative to `zstd`? Entropy coding, or match finding?
- How many pages sit within 256 bytes above the cliff, and what recovers them?

Candidate designs to prototype in userspace, ranked by expected value:

1. **Word-granular model with mode dispatch.** WKdm's idea, modernised: 64-bit words, a larger
   recent-word dictionary, tags for zero / exact-match / high-bits-match+low-bits-literal / literal.
   Near-data-independent control flow → the p99 win of §3.2. The 16-byte quantum pays for a per-page
   or per-512-byte mode header for free.
2. **Zero/RLE fast path.** Rodgman's corpus was bimodal: 44% of pages ≤ 5% zeros, 44% ≥ 90% zeros.
   Two well-separated modes beat one compromise model. Verify the bimodality holds on our corpus first.
3. **Word model + short LZ pass** over the residual.
4. **Cliff-recovery mode**: when the fast path lands just above `huge_class_size`, spend extra
   compression time (a static Huffman or a small rANS stage) on that page only. Compression time is
   less precious than decompression time, and the payoff is 400+ bytes per rescued page.
5. **Dictionary support** from the start, via `zcomp_params->dict` (C4).

A slower, higher-ratio sibling for `CONFIG_ZRAM_MULTI_COMP` recompression of idle pages is a legitimate
second target (working name `wuzl`) — but only after the primary codec lands. Do not split effort.

*Gate: one design beats `lz4`+dict on bucket bytes at equal p99 latency, in a userspace prototype.*

### Phase 4 — Reference implementation, format spec, fuzzing (20 weeks)

This phase exists because of §2.2. Everything Biggers asked for, before the first patch.

- `quetschn.c` / `quetschn.h`: C11, freestanding, no libc, no allocation, `≤ 4 KiB` scratch passed in
  by the caller.
- **`FORMAT.md`**: byte-exact format specification, plus an independent, deliberately slow reference
  decoder written from the spec alone. Differential-test the fast decoder against it.
- **Fuzzing** (AFL++ is already checked out at `~/gra/AFLplusplus`):
  - decompressor vs arbitrary input — must never read or write out of bounds, must always terminate,
    for truncated, corrupted and adversarial input;
  - roundtrip property fuzzing with structure-aware generators;
  - differential fuzzing fast decoder vs reference decoder;
  - continuous fuzzing in CI, plus an OSS-Fuzz submission. *"We run under OSS-Fuzz"* is a strong,
    checkable answer to objection 3.
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
  Measure **actual** `mm_stat` memory used — this is where the class-fragmentation effect of §3.1
  shows up and where the userspace bucket model gets validated or falsified.
- Run `tools/testing/selftests/zram/`; add cases as needed.
- Correctness under `CONFIG_DEBUG_ATOMIC_SLEEP`, `PROVE_LOCKING`, KASAN, and with preemption disabled.
  Minchan's zBeWalgo panic was exactly this class of bug.

*Gate: a kernel build with quetschn survives sustained swap thrash under KASAN, and in-VM `mm_stat`
confirms the userspace prediction within 2%.*

### Phase 6 — arm64 validation (**hard gate before Phase 7**)

Unavoidable. No arm64 numbers, no patch. Options in descending order of value:

1. A rooted Android phone (real silicon, real ART heap data, little-core pinning under `cpuset`).
2. An arm64 SBC or cloud instance (Graviton, Ampere, Pi 5) — real timings, desktop-class data.
3. Ask Dave Rodgman (ARM, author of lzo-rle) or the linux-mm list for help measuring. A good Phase 2
   publication makes this ask reasonable rather than presumptuous.

Also needed: an arm64 corpus. Android page data may differ substantially from desktop data (ART heap
layout, different allocator). If quetschn was tuned on desktop pages and loses on Android pages, that
is a late and expensive discovery — which is why the Phase 3 design should stay parameterised rather
than hand-tuned to one corpus.

*Gate: C1–C3 hold on arm64, on both a big and a little core.*

### Phase 7 — Upstreaming (6+ months, expect v5+)

- Before anything: post the Phase 2 benchmark results to linux-mm / LKML **without** proposing a codec.
  Build credibility, invite criticism of the methodology, and find out whether maintainers even want a
  new backend. Cheap, and it de-risks the whole project.
- Then an RFC series. Cover letter modelled on `5ee4014af99f`: corpus provenance, distribution, both
  architectures, paired per-page regression analysis, fuzzing status, workspace comparison.
- Series shape: (1) `lib/quetschn` + spec + `MAINTAINERS`, (2) zram backend + Kconfig/Makefile,
  (3) documentation, (4) selftests.
- Audience: Sergey Senozhatsky and Minchan Kim (zram/zsmalloc maintainers, per `MAINTAINERS`),
  linux-mm, Andrew Morton (lib/ goes via the mm tree), Eric Biggers (reviewed zBeWalgo; will review
  this), Dave Rodgman.
- Commit to maintaining it. A `MAINTAINERS` entry is a multi-year promise and maintainers will read it
  as one.

---

## 6. Repository layout

```
include/quetschn.h          C11, freestanding, no libc
src/quetschn.c              the codec core
src/quetschn_ref.c          independent reference decoder (from FORMAT.md only)
FORMAT.md                   byte-exact format specification
tools/collect/              corpus collectors (C++)
tools/analyze/              page statistics (C++)
bench/                      harness: nanobench + kernel-sourced codecs
bench/kernel_codecs/        lib/lzo, lib/lz4, lib/zstd, lib/842 built for userspace
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
| R1 | **No arm64 hardware.** Phones are the users; lzo-rle was merged on arm64-first data. | Confirmed constraint | Phase 6 is a hard gate. Budget for an SBC or cloud instance early; a rooted phone is worth real money here. Do not submit without it. |
| R2 | **Insufficient headroom over lz4+dict.** Nobody has measured this. The whole project rests on an unverified assumption. | Handoff reflection §2 | Phase 2 gate answers it before any codec work. Fallback: publish the benchmark, then pursue a *targeted improvement to lz4 or lzo-rle for 4 KiB inputs* — that is exactly what lzo-rle was, and it is a much easier merge. |
| R3 | **Maintainers do not want another backend.** Each one is permanent maintenance cost. | zBeWalgo reached v7 and died | Ask first, in Phase 7's pre-RFC posting, before investing in the patch series. A "no" discovered early redirects to R2's fallback. |
| R4 | **Desktop-tuned codec loses on Android data.** Different heap layout, different allocator. | Handoff reflection §2 | Keep Phase 3 designs parameterised, not hand-tuned. Obtain Android pages before freezing the format. |
| R5 | **zram backend API churn.** 2024 rewrite, 2025 preemption series, 2026 param and naming changes. | `git log drivers/block/zram/` | Codec core has zero kernel-API dependency; all churn is absorbed by `backend_quetschn.c`. Rebase against mainline in CI. |
| R6 | **Fuzz-safety or a sleeping-in-atomic bug burns maintainer goodwill.** | Biggers's objection; Minchan's panic | Phase 4 and the Phase 5 KASAN/`DEBUG_ATOMIC_SLEEP` gate exist for this. OSS-Fuzz before submission. |
| R7 | **Timeline.** 3–8 h/week against an 18–24 month path. | lzo-rle: 4 months, v5, paid work, existing codec | Each phase publishes independently. Phase 2 alone is a worthwhile public contribution. |
| R8 | **Employer rules on open-source side projects**, particularly kernel contributions with a `MAINTAINERS` entry. | Handoff reflection §3 | Check before Phase 7, ideally before Phase 0. Cheap to check, expensive to discover late. |

---

## 9. Immediate next actions

1. Phase 0 repository foundation: licenses, SPDX, CI with the kernel-flag build job.
2. Write the zsmalloc bucket-cost model (merged class table, `huge_class_size` cliff) as a standalone,
   tested C++ component — it is the core of every measurement that follows.
3. Build the resident-anonymous corpus collector; collect a first small corpus locally.
4. Stand up the harness with `lzo-rle`, `lz4` and `zstd -1` built from the local kernel tree, and
   produce the first bucket-bytes and p99-latency table.

Step 4 is the cheapest check that could disprove the project's central assumption. Reach it before
writing a single line of codec.
