# Explored designs

Every design idea that was measured, with the result, and why it was kept or dropped. The goal is
`PLAN.md` §1: `lz4`-class decompression latency at `zstd -1`-class memory, measured as Σ zsmalloc
cost (§3.1) and cold-cache p99 per page (§5.2). Add an entry for everything that gets measured, also
and especially for what did not work.

Short version so far: the gap between `lz4` and `zstd -1` is mostly how the sequences are coded, see
[Where the ratio of `zstd` comes from](#where-the-ratio-of-zstd-comes-from). A format built on that,
[seqlz](#seqlz-lz4s-matches-huffman-coded-sequences-with-static-tables), needs less memory than
`zstd -1`, 25.2% against 26.9%, and decodes faster than it, but is still 1.4 µs slower than `lz4` at
cold p99. Two decoders beat `lz4` on cold p99, but only with formats that need 55.7% and 70.5% of the
uncompressed size, against 34.5% for `lz4`. The ratio has to come from repeats across the whole page;
local tricks on 8 or 64 bytes do not get there.

## How the numbers are measured

All numbers here are from the first zram dump of the development machine (`PLAN.md` Phase 1): 460 923
pages swapped out by a Fedora desktop, 5684 of them same-filled and skipped, 455 239 measured. Ryzen 9
7950X, one core pinned, `powersave` governor, so the frequency is not fixed. Every codec is built with
the kernel's compiler flags (`cmake/kernel_codecs.cmake`), the candidates with `lz4`'s `-O3`.

Two benchmarks, and a few rules that came from getting it wrong first:

* **Fast:** `tools/quick-bench.sh build <corpus> <out> lz4,<candidates>`, 91s for five codecs. The
  zsmalloc cost comes from the whole corpus without timing (`--no-timing`), so it is exact. Latency
  comes from a fixed random sample of 20 000 pages (`quetschn-sample-corpus`), interleaved, in 5
  separate processes.
* **Full:** `quetschn-bench-interleaved` on the whole corpus, 89s for six codecs. Only to confirm a
  result that goes into this file or `PLAN.md`. The fast one agreed with it within 2% to 4% for five
  of six codecs; for `spike-slots` the fast one said 1800 ns cold p99, the full one 2000 ns. So a
  difference below about 300 ns needs the full run.
* **Interleaved, always.** Separate runs drifted by 6%, as much as the effects measured.
  `run_interleaved` runs every codec once per repetition on the same page, in rotating order.
* **Reverse the codec order** when a difference is below about 200 ns. All codecs are in one binary
  and the code layout of one shifts the others: `shuffle-lz4` against `lz4` was +380 ns in one order
  and +210 ns in the other.
* **Buffers aligned like the kernel.** The output page is page aligned, allocations of a page or more
  too, and the compressed data starts at a different offset for every page, like zsmalloc objects.
  With 64-byte aligned buffers the difference of `shuffle-lz4` to `lz4` moved from +390 to -110 ns
  between two builds that only differed in how the corpus was allocated.
* **Nothing else runs on the machine while a benchmark times.** A build or a test run during a
  benchmark gave numbers that the next clean run did not reproduce.
* **Fixed clock, compressions apart from decompressions, and several processes.** Cold latency was
  the hard part; warm latency and compression time were always stable to 1% or 2%. Three causes,
  found one after the other:
  * The clock. With boost on, warm decoding ran at 5.3 GHz every time, but the cold numbers of two
    identical runs differed by up to 520 ns. With CPU 2 fixed at 4.5 GHz and boost off, identical
    runs agreed within 10 to 80 ns. Counting cycles (APERF via `rdpru`) instead of time did not help:
    the time spent waiting for DRAM counts more cycles at a higher clock.
  * The other codecs in the run. With `lz4hc` in a run, `lzo-rle` against `lz4` moved from -100 to
    +200 ns. `lz4hc` touches a 256 KiB workspace when it compresses, and that ran right before the
    next codec's decompression. Now every repetition first times all compressions, then all
    decompressions; `lzo-rle` against `lz4` was then -50 to -110 ns with and without `lz4hc`.
  * The process. Five identical runs gave `zstd -1` against `lz4` +2390, +4620, +2300, +1800 and
    +2490 ns, while each run's own confidence interval was about ±80 ns. It only covers which pages
    were sampled, not e.g. which physical pages the buffers got. So `tools/quick-bench.sh` runs the
    latency in 5 processes and shows the median and the smallest and largest difference.

  Every benchmark prints the frequency range and boost state, a table needs min equal to max and
  boost off. The commands for that are in `README.md`.

The latencies in the sections on the word model, byte shuffle and base + delta were measured before
these three fixes, with boost on and one process. Their differences to `lz4` can be off by a few
hundred ns; the Σ zsmalloc cost is exact in every section.

## Baselines

Full run, all 455 239 pages. Cold p99 over all pages is what zram sees: for pages stored uncompressed
it is a `memcpy`. The last column is over the 290 693 pages that neither `lz4` nor any candidate
below stores uncompressed, which compares the decoders on the same work.

| codec | Σ zsmalloc cost | stored raw | per CPU | compress p99 | cold p99, all pages | cold p99, compressed pages |
| --- | --- | --- | --- | --- | --- | --- |
| `lz4` | 34.5% | 10 523 | 16 440 B | 3500 ns | 2350 ns | 2450 ns |
| `lzo-rle` | 32.4% | 11 852 | 16 384 B | 3740 ns | 2330 ns | 2360 ns |
| `zstd -1` | 26.9% | 9638 | 169 728 B | 8650 ns | 3850 ns | 3880 ns |

The target is the gap between the first two rows and the third: `zstd -1` needs 17% less memory than
`lzo-rle`, and 60% more time at cold p99 than `lz4`.

## Where the ratio of `zstd` comes from

*The most useful result so far: the gap to `zstd -1` is how the sequences are coded, not the literals
and not better matching.* Code: `bench/lz_analysis_main.cpp`, `explore/zstd_nolit.c`,
`bench/kernel_codecs/zram_lz4hc.c`.

Σ zsmalloc cost on all 455 239 pages, without timing, so exact:

| codec | Σ zsmalloc cost | what changes |
| --- | --- | --- |
| `lz4` | 34.5% | |
| `lz4hc` 1 / 3 / 9 / 16 | 32.4% / 31.3% / 30.7% / 30.6% | better matches, same `lz4` format and decoder |
| `lzo-rle` | 32.4% | |
| `zstd -5` | 37.1% | |
| `zstd -1` | 26.9% | |
| `zstd` 1 / 3 / 9 / 19 | 23.9% / 23.6% / 22.7% / 21.7% | |
| `zstd-nolit` 1 / 3 / 9 / 19 | 26.9% / 25.9% / 24.9% / 22.7% | the same without Huffman coded literals |

* **`zstd` never Huffman codes literals at negative levels.** `zstd-nolit 1` writes exactly the bytes
  of `zstd -1`, so the difference between `zstd -1` and `zstd 1`, 26.9% against 23.9%, is the
  Huffman coding of the literals and nothing else.
* **Better matching within the `lz4` format ends at about 30.7%**, reached at `lz4hc` level 9. That is
  11% less than `lz4` and 5% less than `lzo-rle`, with `lz4`'s decoder.
* So `zstd -1` gets from 30.7% to 26.9% without coding literals and with a fast matcher that is no
  better than `lz4hc`'s: through how it codes the sequences.

`quetschn-lz-analysis` puts a number on that. It takes the matches `lz4hc` level 9 finds, checks that
every page comes back from them, and costs the same matches with zstd-like symbols and a static
entropy model trained on the whole corpus (tables fixed in the decoder, because a 4 KiB page has no
room for its own):

| the same `lz4hc` level 9 matches, coded as | Σ zsmalloc cost |
| --- | --- |
| `lz4` format, what `lz4hc` writes | 30.7% |
| entropy coded literal lengths, match lengths and offsets, raw literals | 25.8% |
| ... with repeat offsets like `zstd`'s | 25.1% |
| ... and entropy coded literals | 23.6% |

This is an estimate: a real entropy coder needs a bit more than -log2 of the frequency, and the model
is trained on the pages it is measured on, so it is on the optimistic side. With `lz4hc` level 3 it
is 31.3%, 26.1%, 25.4% and 23.7%, the matching quality barely matters once the sequences are
entropy coded. A page has 636 literal bytes and 182 sequences on average: `lz4` spends a byte on
every token and two on every offset, where 4 KiB of page need 12 bits of offset at most, and
usually fewer.

Latency with the clock fixed at 4.5 GHz, the median of 5 runs of `tools/quick-bench.sh`, and in
brackets the smallest and largest difference to `lz4` at cold p99:

| codec | cold p50 / p99 | Δ cold p99 | warm p99 | compress p50 / p99 |
| --- | --- | --- | --- | --- |
| `lz4` | 1130 / 2770 ns | | 2370 ns | 2.3 / 4.2 µs |
| `lzo-rle` | 1010 / 2570 ns | -200 [-320, -10] | 2290 ns | 2.0 / 4.7 µs |
| `lz4hc` level 9 | 840 / 2370 ns | -400 [-440, -200] | 2180 ns | 23 / 97 µs |
| `zstd -1` | 2530 / 4560 ns | +1830 [+1730, +1880] | 3970 ns | 5.2 / 10.4 µs |
| `zstd 1` | 3640 / 6600 ns | +3850 [+3770, +4010] | 5480 ns | 8.3 / 14.9 µs |

* `lz4hc` decodes faster than `lz4`: the same decoder, and fewer, longer sequences. But it
  compresses 10 to 23 times slower, `PLAN.md` C3 allows 1.2 times `lz4`.
* Huffman coded literals, `zstd 1` over `zstd -1`, cost another 2000 ns at cold p99 for 3 points of
  Σ zsmalloc cost.

For the design this means: an LZ whose sequences are entropy coded with static tables, and literals
raw, is worth about 25% Σ zsmalloc cost by this estimate, better than `zstd -1`, and the matcher can
be cheap. Whether its decoder is fast is the open question. `zstd -1` also decodes entropy coded
sequences and is slow; what in its decoder costs the time is not measured yet.

## seqlz: `lz4`'s matches, Huffman coded sequences with static tables

*Kept, the most promising format so far: less memory than `zstd -1` and faster to decode than it. Still
1.4 µs slower than `lz4` at cold p99.* Code: `explore/seqlz.{h,c}`, `explore/zram_seqlz.c`,
`bench/seqlz_train_main.cpp`.

The estimate above, built. The matches come from the kernel's `lz4` (`seqlz`) or `lz4hc` level 3
(`seqlz-hc`); the prototype takes their output apart and codes it again, so its compression time
says nothing about a real encoder yet. Like `lz4`'s token, literal length and match length share one
symbol, each capped at 15, Huffman coded with at most 11 bits; longer lengths add a value from a
second table, offsets are symbols with extra bits and three repeat offsets like `zstd`'s. Everything
goes into one bitstream, read least significant bit first. Literals stay raw. The tables are compiled
in, 321 bytes of code lengths, trained on the resident pages and not on the zram dump.

Full run on all pages, CPU 2 at 4.5 GHz, one process:

| codec | Σ zsmalloc cost | cold p50 / p99 | Δ cold p99 | warm p99 | compress p50 / p99 |
| --- | --- | --- | --- | --- | --- |
| `lz4` | 34.5% | 1160 / 2740 ns | | 2350 ns | 2.3 / 4.2 µs |
| `lzo-rle` | 32.4% | 1160 / 2790 ns | +50 [+30, +60] | 2290 ns | 2.0 / 4.6 µs |
| `zstd -1` | 26.9% | 2680 / 4790 ns | +2050 [+2030, +2070] | 3910 ns | 5.2 / 10.4 µs |
| `seqlz` | 27.0% | 2230 / 4290 ns | +1550 [+1540, +1570] | 3090 ns | 4.5 / 8.1 µs |
| `seqlz-hc` | 25.2% | 1970 / 4110 ns | +1370 [+1360, +1390] | 3020 ns | 11.0 / 16.0 µs |

* **Memory better than estimated.** The estimate with separate symbols said 27.3% and 25.4%; with the
  token, which also sees how the two lengths go together, it is 27.0% and 25.2%.
* **The tables carry over.** Tables trained on the zram dump itself gave the same sizes as the ones
  trained on resident pages (measured with the earlier three-symbol format).
* **Decoding.** Per page, decoding only (`--decode-loop`, `perf stat`): `lz4` 4760 cycles and 11 700
  instructions, `zstd -1` 11 500 and 39 600, `seqlz-hc` 9800 and 32 200. `seqlz` decodes at 3.3 to 3.7
  instructions per cycle, `lz4` at 2.5; it is bound by the chain of dependent steps per sequence
  (lookup, shift, next lookup) and by 156 mispredicted branches per page, `lz4` has 94.

How the decoder got there. The first rows are warm p99 of the quick benchmark, the later ones cycles per
page of the decode loop, measured with `perf stat` and AMD's IBS sampling per source line:

| change | warm p99 | cycles per page (`seqlz`) |
| --- | --- | --- |
| first version: a symbol, then its extra bits; byte loops for matches, `memcpy` for literals | 5590 ns | |
| 8 and 16 byte copies, with a pattern step for offsets below 8 | 5550 ns | |
| one table entry gives code length, extra bits and base; repeat offsets with conditional moves | 5570 ns | |
| fast copies up to 8 or 16 bytes before the page end, not only where the whole run fits | 4780 ns | |
| the first 16 bytes of literals and matches copied without a loop, like `lz4` | 4560 ns | 14 160 |
| decoding 32 sequences, then copying them (dropped) | 6110 ns | |
| one bitstream instead of three | | 15 610 |
| a signed bit count, no clamping and no flag per value | | 14 010 |
| one token for both lengths, like `lz4`'s; 321 instead of 65 bytes of tables | 3420 ns | 10 930 |
| one refill per sequence, invalid codes checked after the loop | 3110 ns | 10 880 |

* A run to the end of the page went byte by byte, and with offset 1 every byte waited for the store
  before it. The slowest 1% of pages took 5700 ns there, where `lz4` took 500.
* Decoding in batches made it worse; the stack arrays and the second loop cost more than the overlap
  gains.
* The kernel flags have `-fsanitize=shift` and `-fsanitize=bounds-strict`; the decoder had 65 of these
  checks, `lz4`'s 18. Without them the instruction count was the same, they are not in the hot path.
* One stream instead of three was meant to take load off the registers: three bit readers are 15
  values, and the profile showed them loaded and stored on the stack. It did not change the
  instruction count and cost 10% of cycles, so the three streams apparently overlapped. It stays for
  the token, which needs the lengths and the offset in one order anyway.
* The token was the step that mattered: two table lookups per sequence instead of three, 22% fewer
  cycles, and it saved memory.
* The kernel flags compile for the x86-64 baseline without BMI2, so every variable shift has to go
  through register `cl`.

Next for the decoder: fewer mispredicted branches, e.g. for literal runs of more than 16 bytes and for
the escapes, and a matcher of its own, cheap like `lz4`'s and as good as `lz4hc` level 3's.

## Word model: WKdm-style 64-bit words

*Kept as a direction for the decoder, not as a format.* Code: `spike/`, `PLAN.md` Phase 2b.

Every 64-bit word gets a 2-bit tag: zero, exact match or high-32-bits match against a 16-entry table
of recent words, or literal. Four decoders of the same format:

| decoder | cold p99, compressed pages | what it does |
| --- | --- | --- |
| `spike-switch` | about `lz4` (separate runs) | `switch` on the tag |
| `spike-branchless` | 3690 ns, `lz4` 2790 ns (separate runs) | masks instead of branches |
| `spike-zeroskip` | about `lz4` (separate runs) | `switch`, and 4 zero words at once |
| `spike-slots` | 1940 ns, `lz4` 2450 ns | table slot from the stream |

Σ zsmalloc cost 55.7%, 110 284 pages stored raw.

What was learned:

* Branchless is slower everywhere. Data-independent control flow is not the win `PLAN.md` §3.2
  hoped for.
* The win is a short dependency chain per word. The first decoders stored every word at
  `slot_of(w)`, a hash of the decoded word, so the store address waited for the table load.
  `spike-slots` takes the slot from the stream, and an exact match needs no store at all.
* `spike-slots` against `lz4` at cold p99: -480 and -820 ns in the two runs of PR #14, with 64-byte
  aligned buffers; -350 ns [-360, -330] in the full run with kernel-like alignment. The last one is
  the number to use.
* `lz4` stays faster on pages it compresses below 512 bytes, 73% zero words: it copies long matches,
  the word model visits every tag.

## Byte shuffle + `lz4`

*Dropped as a codec. The per-page result is a hint for later.* Code: `explore/shuffle.c`,
`explore/zram_explore.c`.

Byte `j` of every 8-byte word goes to plane `j` (the blosc trick for arrays), then the kernel's `lz4`
compresses the shuffled page. The idea: the high bytes of pointers into the same region turn into
long runs. Unshuffling works on 8x8-byte blocks with 64-bit shifts and masks, no SIMD.

| | Σ zsmalloc cost | stored raw | cold p99, all pages | cold p99, compressed pages |
| --- | --- | --- | --- | --- |
| `shuffle-lz4` | 40.9% | 26 078 | 2770 ns | 2720 ns |

* Worse than `lz4` overall: 40.9% against 34.5%. Shuffling destroys byte-level repeats that are not
  word aligned, e.g. text: a text page needs 3543 bytes shuffled.
* But it is cheaper than `lz4` on 111 871 pages, a quarter of them. Taking the cheaper of `lz4` and
  `shuffle-lz4` per page gives 32.1%, as good as `lzo-rle`. Word structure matters for a quarter of
  the pages, and a per-page mode could pick it up.
* Decoding is 270 to 420 ns slower than `lz4` at cold p99: `lz4` decodes into a per-CPU buffer, and
  the unshuffle writes the page again.

## Base + delta per 64-byte block (BDI)

*Dropped.* Code: `explore/bdelta.c`. After Pekhimenko et al., "Base-Delta-Immediate Compression",
PACT 2012, a design for CPU cache lines.

Each block of 8 words gets a 4-bit mode: zero, one repeated word, or a base with 1, 2 or 4 byte
deltas (64-bit or 32-bit values), where each value is either base + delta or a small immediate, or
raw.

| | Σ zsmalloc cost | stored raw | cold p99, all pages | cold p99, compressed pages |
| --- | --- | --- | --- | --- |
| `bdelta` | 70.5% | 162 920 | 1430 ns | 1490 ns |

* The fastest decoder measured so far, 960 ns faster than `lz4` at cold p99 on compressed pages. It
  decides once per 64 bytes, not once per word or per sequence.
* Hopeless on memory. A block only compresses when all 8 words are close to one base or small, and
  real pages mix. It is cheaper than `lz4` on only 9163 pages. Taking the cheaper of `lz4`,
  `shuffle-lz4` and `bdelta` per page gives 32.1%, the same as without `bdelta`.

## memlz

*Dropped.* Code: `explore/zram_memlz.c`, built only with `-DQUETSCHN_MEMLZ_DIR=<checkout>`, measured at
[rrrlasse/memlz](https://github.com/rrrlasse/memlz) commit 3b28cc5.

memlz is an 8-byte word version of the Chameleon algorithm: each word is either found through a
16-bit hash in a table of recent words, then 2 bytes are written, or stored whole; runs of equal bytes
are coded separately. It is built for streams of megabytes, and for those it is very fast. Its state
is two tables of 65 536 entries, 768 KiB, and the decoder rebuilds them from the data like the
encoder, so for independent 4 KiB pages both sides reset them for every page.

| | Σ zsmalloc cost | stored raw | per CPU | cold p50 / p99 | compress p50 / p99 |
| --- | --- | --- | --- | --- | --- |
| `memlz` | 63.6% | 60 449 | 1.5 MB | 9710 / 10 190 ns | 12.8 / 13.4 µs |

* Worse on memory than the word model spike (55.7%): a word only compresses when it came before
  exactly, there are no partial matches.
* The latency is almost the same at p50 and p99 because nearly all of it is the 768 KiB reset before
  every page, in both directions. For pages its tables would have to be a hundred times smaller, and
  then it is the spike's design again.
* In the same run the 768 KiB resets made the cold decodes of the other codecs slower too (`lz4` cold
  p99 3480 instead of about 2700 ns), so a codec with a large working area needs its own run.

## `lz4` with a dictionary

*A baseline, not a candidate. Kept in the comparison because zram supports it.* Details in
`PLAN.md` §9, next action 6.

* Trained on resident pages, measured on swapped pages: 2.4% less Σ zsmalloc cost than `lz4`, still
  worse than `lzo-rle`. For `zstd 3` the same dictionary costs 2.7% more.
* The dictionary gains 2.72% on the pages it helps and loses 0.35% on the others. In the extreme case
  an 8-byte dictionary makes a periodic page 1543 bytes instead of 49: the greedy parse gets stuck in
  a chain of short matches. Reported as [lz4/lz4#1805](https://github.com/lz4/lz4/issues/1805).
* Not measured yet: a dictionary trained on swapped pages, which needs a second zram dump
  (`tools/bench-dict.sh`).

## Not evaluated yet

* **A faster decoder for seqlz**, see its section. The format has the memory, the decoder has to get
  to `lz4`'s speed.
* **A matcher for seqlz** that is cheap like `lz4`'s and finds matches like `lz4hc` level 3's: that is
  2 points of Σ zsmalloc cost, 27.0% against 25.2%.
* **Word model + a path for runs and long repeats.** Where the word model loses to `lz4` is exactly
  where `lz4` copies long matches. `PLAN.md` Phase 3, candidate 3.
* **`lz4` tuned for 4 KiB pages:** offsets limited to the page, word-aligned matches, a parser that
  does not get stuck. The `lzo-rle` route, the easiest merge.
* **Per-page mode selection**, e.g. between a byte-oriented and a word-oriented coder. The shuffle
  result says a quarter of the pages would pick the word side.
* **Entropy coded literals** on top, 1.5 points by the estimate, but `zstd 1` shows it costs a lot of
  decode time. Maybe only for pages just above a size class boundary (`PLAN.md` Phase 3, candidate 4).
* **arm64.** Every latency above is x86-64 only. The phone's little core may order these designs
  differently.
