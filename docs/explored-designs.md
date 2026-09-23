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
symbol, capped at 15 and 31, Huffman coded with at most 11 bits; longer lengths add a value from a
second table, offsets are symbols with extra bits and three repeat offsets like `zstd`'s. Everything
goes into one bitstream, read least significant bit first. Literals stay raw. The tables are compiled
in, 577 bytes of code lengths, trained on the resident pages and not on the zram dump.

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

### Second round: branches, tails and table sizes

`seqlz-hc` after the second round, against the state above. First the decode loop
(`--decode-loop 11`, 20 000 sampled pages, median per page over the loops), warm, and cold with 2 MiB of
other data read and the page and output flushed before each decode:

| `seqlz-hc` | warm p50 | warm p99 | cold p50 | cold p99 |
| --- | --- | --- | --- | --- |
| first round | 2230 ns | 4150 ns | 3080 ns | 4960 to 5070 ns |
| second round | 1880 ns | 3350 ns | 2670 to 2740 ns | 4160 to 4290 ns |
| `lz4` | 1130 ns | 2420 ns | 1500 ns | 2690 ns |
| `zstd -1` | 2730 ns | 4600 ns | 3380 ns | 5340 ns |

The full run of the harness sees less: cold p50 1830 against 1970 ns, cold p99 4090 against 4110 ns,
warm p99 3240 against 3020 ns, Δ cold p99 against `lz4` +1380 ns [+1360, +1400] as before. The two
measurements differ in what runs between two decodes: in the harness the other codecs, in the loop
the same decoder. This is not resolved. Σ zsmalloc cost is 25.2% and 27.0% as before.

What was tried, in the order it was measured, with the branch stack of Zen 4 (`perf record -j any,u`)
for the mispredicted branches per source line, and the decode loop for p99:

* **The compiler turns `?:` into branches.** The match length value "without a branch" and the
  selection of the repeat offset were both compiled to branches, 30% and 28% of all mispredictions.
* **Masks instead cost more than they saved.** They halved the mispredictions, 155 to 68 per page, and
  added 15 000 instructions per page: 9400 to 12 000 cycles. At 3.4 to 3.9 instructions per cycle,
  instructions are as expensive as mispredictions here.
* **Rarer escapes: a token of 512 symbols**, 4 bits literal length and 5 bits match length, so a
  match length value follows 8.5% of the matches instead of 18.7%. Same memory, 9400 to 9180 cycles.
* **The repeat offsets in an array of four**, the offset and the move to front as lookups: no
  branch, few instructions. 89 mispredictions per page, fewer than `lz4`'s 94, and p99 4230 to 3570 ns.
* **Only complete prefix codes** (Kraft sum exactly 1): every bit pattern starts a code, and the
  decoder does not need to check for one that does not. The checks were 6% of the instructions.
* **The mean is not the tail.** Copying 32 bytes of every match without a loop made the mean faster
  and p99 slower, 3570 to 4150 ns: the slowest pages have many short matches. Back to 16 bytes. That
  is why the decode loop reports percentiles per page, not only cycles per page from `perf stat`.
* **Tables have to stay in L1.** The 512-symbol token with codes up to 12 bits needs a decode table
  of 8 KiB; with the three value tables that was 20 KiB, and the cold p99 was 650 ns worse than with
  256 symbols. With codes up to 11 bits, 4 KiB, cold p99 went from 5070 to 4350 ns, without costing
  memory. Codes of lengths and offsets are limited to 9 bits now, 2 KiB per table; that did not change
  the time, but it is 6 KiB less per device.
* **Short offsets** with lz4's trick, four bytes one by one and four from a position in a table: fewer
  mispredictions, but more cycles, the load after four single-byte stores waits for them. Building
  the first 8 bytes in a register and storing them once is a little faster, and kept.

Next for the decoder: find out why the harness and the decode loop disagree on cold p99.

### The compressor: seqlz-fast

*Kept, but still 1.3 to 1.5 times as slow as `lz4`; `PLAN.md` C3 allows 1.2.* Code: `seqlz_find` and
`seqlz_compress` in `explore/seqlz.c`, codec `seqlz-fast`.

Until here the prototype took `lz4`'s or `lz4hc`'s output apart and coded it again. `seqlz-fast` has
its own matcher, greedy like `lz4`'s fast mode: at every position the last offset and one candidate
from a hash of 4 bytes, the step growing with the distance to the last match. The hash table has
16-bit positions and is cleared for each page. Each sequence is coded as soon as it is found; literals
go to the front of the buffer, the bitstream behind the room of a page of literals and is moved in at
the end. zram's buffer has two pages, and that is always enough: at most 31 bits per 4 bytes of page,
so at most 3972 bytes of bitstream, and 4076 fit behind the literals, for any tables. Its own tables
are trained on its own matches. Per CPU: an 8 KiB hash table, `lz4` has 16 KiB.

| codec | Σ zsmalloc cost | compress p50 / p99, page in cache | compress p50 / p99, page cold |
| --- | --- | --- | --- |
| `lz4` | 34.5% | 2260 / 4190 ns | 3930 / 7540 ns |
| `seqlz-fast` | 26.4% | 3070 / 6390 ns | 4950 / 10 030 ns |
| `zstd -1` | 26.9% | 5300 / 10 380 ns | 7590 / 12 370 ns |
| `seqlz` (`lz4`'s matches, coded again) | 27.0% | 4850 / 9150 ns | 7550 / 13 810 ns |
| `seqlz-hc` (`lz4hc` 3's matches) | 25.2% | 11 290 / 16 770 ns | 17 490 / 29 950 ns |

"Page in cache" is the full benchmark on the whole corpus, which compresses right after reading the
page; "page cold" is the compress loop (`--decode-loop 11 --compress`), 20 000 pages one after the
other, so each page comes from memory. For zram, reclaim swaps out pages nobody used for a while, so
the cold column is probably closer, but that is not measured. The first round got `seqlz-fast` to
3790 / 8090 and 6010 / 11 590 ns, the second round (next section) to the numbers in the table.

* **Memory:** 26.4%, less than `zstd -1`. Checking the last offset first is worth 0.5 points, tables
  trained on its own matches 0.1.
* **The matcher alone cost more than all of `lz4`:** 22 000 cycles per page without the encoding,
  before the change below, `lz4` 17 200 with everything. After it, the encoding is about 40% of the
  time.
* **Three branches per position mispredicted almost twice as often as `lz4`'s one.** Reading both
  candidates and deciding with one branch (the last offset and a table entry always point into the
  page, so both may be read): mispredictions 539 to 329 per page, p99 of the cold loop 15.4 to 12.6 µs.
  With the page in cache the extra reads cost 7%.
* **One pass instead of three** (matcher, literals, bits): 27 700 to 26 300 cycles per page.
* **Tried and dropped:** looking only at every 2nd, 4th or 8th position, aligned or not: 28.2% to
  41.1% memory, the unaligned matches matter. A faster growing step: 4% faster at 0.7 points more
  memory. Without the last-offset check or without extending matches backwards: no faster, more
  memory.
* The slowest pages are not the incompressible ones, those go by at 670 ns. They are pages of 1.5 to 3
  KiB output with many sequences: `seqlz-fast` needed about 118 cycles per sequence, `lz4` 73.

### The compressor, second round: fewer instructions

Measured with `perf stat` on CPU 2 at a fixed 4.5 GHz, as the difference of 22 and 11 loops of
`--decode-loop --compress` over the 20 000 pages, so per page and without the setup. At the start
`seqlz-fast` needed 25 594 cycles and 70 135 instructions per page, `lz4` 17 203 and 30 266. The IPC
was 2.74 against 1.76, so it is the number of instructions, not stalls. Now it is 21 300 to 21 450
cycles and 61 500 instructions, 1.24 times `lz4`. Each step, in the order it was done:

* **Repeat offsets in three variables instead of an array:** 25 594 to 25 225. With computed indices
  the array has to live in memory.
* **Encoder arrays of a power of 2 entries, indices and shifts masked:** 25 225 to 25 064, 4.7% fewer
  instructions. The kernel builds with `-fsanitize=bounds-strict` and `-fsanitize=shift`, and each
  check the compiler cannot prove away is a compare and a branch.
* **The hash table cleared for each page:** 25 064 to 23 327, 7%. An 8 KiB `memset` is cheaper than
  checking at every position whether the entry is before it; `lz4` in the kernel clears its 16 KiB for
  each page too. The output got a bit smaller, 491 809 373 bytes instead of 491 913 153.
* **Code and length in one `u32` per symbol, one flush per sequence:** 23 327 to 22 848. After a flush
  the accumulator holds at most 7 bits, token, match length value and offset add at most 51.
* **The first 16 bytes of a match compared without a branch:** 22 848 to 21 916, mispredictions 293
  to 280 per page. Of the mispredictions, 66% were the decision whether a position matches, which is
  the nature of the method, and 29% the loop in `count`: 76% of the matches are shorter than 12 bytes,
  13% are 20 bytes or longer. Now only those take a branch.
* **Positions instead of pointers in the matcher:** 21 916 to 21 786. The end is a constant.
* **Token and offset in one put, unless there are length values between them:** 21 786 to 21 233.

The matcher alone, with the encoder replaced by a sum, needs 17 300 to 17 900 cycles, as much as all
of `lz4`. It finds 226 matches per page in 763 positions, `lz4` writes 232 sequences. The encoder adds
about 4000 cycles, even though matcher and encoder in one loop leave the encoder no registers: all its
state lives on the stack.

Tried and dropped:

* **Two passes**, the matcher into an array and then the encoder: 24 257 instead of 22 848 cycles.
  Mispredictions went from 293 to 319 per page: the branches of the encoder lose the history of the
  matcher's branches, which predicted them.
* **Second and third repeat offset packed into one register, the first from the matcher:** 21 757
  instead of 21 300, more instructions than the spills it saves.
* **A hash of 5 bytes:** 20 746 cycles, 2.6% faster, but 26.9% instead of 26.4%.
* **Hash table of 11 bits:** the same speed and 0.2% more bytes. 13 bits: 1.2% slower, 0.1% fewer
  bytes.
* **8 bytes of literals copied without a check** (in the page they never reach the end): fewer
  instructions, but 21 644 cycles, the branch for longer literals mispredicts.
* **Telling the compiler that a match has at least 4 bytes**, so the checks for the last sequence drop
  out: 2% fewer instructions, 3.7% more cycles. `__builtin_expect` on the length values: no change.
  Small changes like these move the register allocation, and with it the result by 2 to 3%, which
  makes small steps hard to measure.
* **Not tried:** dropping the second and third repeat offset. They are 11% of the matches, which would
  cost about 30 bytes per page, 0.7 points. Checking the last offset only right after a match: 41.6 of
  its 50.2 hits per page are 1 or 2 bytes behind a match, but it would save 1.5% at most.

Next for the compressor: with the page in cache p99 is still 1.53 times `lz4`, cold 1.33. The matcher
alone is as expensive as all of `lz4`, so that is where the rest has to come from.

### The compressor, third round: ideas from the literature

*Nothing kept yet. The encoder is no longer the problem, the matcher is: alone it costs as much as all
of `lz4`.* Three searches through papers, blogs and codecs (fast LZ match finding, memory page
compression, cheap entropy encoding) gave a short list, measured here the same way as the second round.
Cycles per page, `seqlz-fast` at 21 450 to 21 600 before, `lz4` 17 100.

**Which pages make the p99.** `--decode-loop --compress --out` now writes the time and length per page,
and the full benchmark writes them anyway. With the page in cache, 70% of `seqlz-fast`'s p99 pages
have 2 to 3 KiB of output, where it is 1.49 to 1.50 times `lz4`; pages of 1 to 2 KiB are 1.42 to 1.46.
Pages that zram stores raw (3625 bytes and more) take 1952 ns and are 0.2% of the p99 pages, so
stopping early on them, as Google's far memory does, would not change the p99. The cost is per
sequence.

| idea | source | cycles per page | Σ zsmalloc cost |
| --- | --- | --- | --- |
| before | | 21 599 | 26.4% |
| hash of the next position before this one is decided | [zstd #2749](https://github.com/facebook/zstd/pull/2749), +16% at level -1 there | 21 481 | the same bytes |
| two positions per round, one branch for both | the same | 21 653 | 26.4% |
| byte oriented format, lz4-like encoder | Bloom's LZNib, Oodle Selkie | 20 289 | 29.4% |
| only the last offset as a repeat, tables retrained | zstd's fast mode | 20 557 | 26.6% |
| ... plus a 20-bit tag in each table entry, 4096 × `u32` | Bloom's cache tables, lzav | 21 468 | 26.6% |
| ... tag, 2048 × `u32` | | 21 347 | 26.7% |
| ... plus the offset code as `(offset << length) + base[symbol]` | libdeflate | 20 492 | the same bytes |

* **Pipelining the hash** does nothing on Zen 4: the core already computes ahead, the loads of the
  next position do not wait for the branch of this one. Two positions per round save branches but
  need more instructions. On an in-order Cortex-A55 both could look different; not measured.
* **Byte oriented formats** are costed exactly by `quetschn-lz-analysis --codec seqlz` on
  `seqlz-fast`'s matches: a token byte with 3 bits of literal length, 3 of match length and 2 for the
  offset (the last, the one before, 1 byte, 2 bytes) is the best at 29.4%, tied with 2 / 4 / 2, with a Huffman coded token
  byte 28.0%, `lz4`'s own layout 34.2%. The encoder of the 29.4% format has 43 300 instructions per page
  instead of 55 000, but 356 mispredictions instead of 279, from the offset mode and the varints. It is
  1% faster for 3 points more memory.
* **Only the last offset as a repeat** needs no format change, the decoder still has three. The
  encoder keeps one offset instead of three and does not compare or move them: 4.8% fewer cycles,
  5600 fewer instructions. The second and third repeat offsets were 11% of the matches; with tables
  trained for it the output grows by 0.9%, 496 209 760 instead of 491 809 373 bytes. With the additive
  offset code, p99 of the cold loop is 9590 ns instead of 9990, `lz4` 7550: 1.27 times.
* **A tag in the table**, so that a miss needs no load from the page: more mispredictions (312 and 317
  instead of 277), a second branch for tags that match but bytes that do not, and 16 KiB of table miss
  L1 more often.
* **The additive offset code** writes the same bytes with 1100 fewer instructions, but the cycles are
  within the noise.
* **Not tried:** zram's recompression (`lz4` at swap-out, `seqlz` later for idle pages from user
  space) changes what the project is, not the codec. A dictionary table shared read-only by all CPUs
  is for C4. Offsets relative to a running base, so that the table is not cleared: the second round
  measured the check for stale entries as 7% slower than the `memset`.

The matcher alone needs 17 300 cycles and 32 400 instructions per page at IPC 1.81, `lz4` all
together 17 100 and 30 300 at 1.77. `lz4`'s encoding hides in the cycles its matcher waits, ours does
not quite, but even a free encoder would leave `seqlz-fast` at `lz4`'s speed and not below.

## bytelz: `seqlz-fast`'s matcher, a byte oriented format

*Open. Close to `lz4` warm, but 1.33 times as slow at cold p99 in both directions.* Code:
`explore/bytelz.c`, format in `explore/bytelz.h`, codec `bytelz`. The matcher and the literal and match
copies are shared with `seqlz` in `explore/page_lz.h`.

The question: `PLAN.md`'s goal is `lz4`'s speed in both directions at `zstd`'s ratio. `seqlz` has the
ratio, but its Huffman codes cost in both directions. The third compressor round costed byte oriented
formats on `seqlz-fast`'s matches, and the best one gets 29.4%: a token byte with 3 bits of literal
length, 3 bits of match length and 2 bits for the offset (the last one, the one before, 1 byte, 2
bytes), extensions as 7-bit varints, literals inline, no header. Does it decode like `lz4`? C1 asks for
8% less than the better of `lz4` and `lzo-rle` (32.4%), so at most 29.8%: `bytelz` just makes it.

| codec | Σ zsmalloc cost | decompress cold p50 / p99 | warm p99 | compress p99, cold loop |
| --- | --- | --- | --- | --- |
| `lz4` | 34.5% | 1770 / 3430 ns | 2370 ns | 7540 ns |
| `seqlz-fast` | 26.4% | 2840 / 5720 ns | 3310 ns | 10 010 ns |
| `bytelz`, first version | 29.4% | 2330 / 4870 ns | 2490 ns | 10 630 ns |
| `bytelz` | 29.4% | 2160 / 4560 ns | 2430 ns | 9560 ns |

Decompression with `tools/quick-bench.sh`, compression with `--decode-loop 11 --compress`. Per page
with `perf stat`, decoding: `lz4` 5135 cycles, 12 400 instructions, 94 mispredictions; `bytelz` first
7671, 28 400, 141; now 7155, 23 100, 145. Compressing: `lz4` 17 200 cycles and 30 300 instructions,
`bytelz` first 22 200 and 61 400, now 21 000 and 51 200 at 307 mispredictions, `seqlz-fast` 21 600.

**Why decoding is slower than `lz4`:**

* **Mispredictions, 145 against 94 per page.** From the branch stack: 29% whether the match length
  has an extension, 26% the branch into the general match copy, 16% the end of its loop, 10% whether
  the literal length has one. With 3 bits for ml - 4, 25% of the matches have an extension, with
  `lz4`'s 4 bits 13% would: of `seqlz-fast`'s matches 59% are 4 to 7 bytes, 16% 8 to 10, 12% 11 to 18
  and 13% longer. Of the literal runs 82% are shorter than 3, 11% 3 to 6.
* **Instructions, 23 100 against 12 400**: two repeat offsets, offsets of 0, 1 or 2 bytes, extensions
  of 1 to 3 bytes, and the checks for the end of input and page.
* **Not the code size:** in the loop with 2 MiB of other data between the pages, both miss the
  instruction cache 5 times per page. The harness's cold decode only flushes the compressed page and the
  output, with a warm decoder, and there the gap is 380 ns larger than warm: not explained yet.

**What helped the decoder:** far from the end of input and page, the same steps without bounds checks,
16 bytes of literals copied without a loop, and matches of up to 16 bytes with an offset of at least 8
in two 8-byte copies (the second after the first is stored, it may read what the first wrote): 7671 to
7155 cycles. **What did not:**

* an `lz4`-like short path only for sequences without extensions: 8935 cycles, its entry branch
  mispredicts for a quarter of the sequences;
* extensions read without a branch: 17 409 cycles with both, 8844 with only the one of ml, although
  mispredictions fell to 97 and 118. The position of the next token then waits for the loads of the
  extension, where a predicted branch does not wait;
* the offset selected with masks instead of a small array: 8192.

**Why compressing is slower than `lz4`:** the matcher alone costs as much as all of `lz4`, and the
encoder adds 84 instructions per sequence where `lz4` needs about 25. Written without branches, the
extensions cost 20 instructions each, with branches 22 200 became 21 000 cycles. The rest is the
compiler: the encoder's state lives on the stack, the last sequence's path is merged into the loop, and
the rarely used byte loop for literals became a vectorized one with alignment checks.

**Next, from `lzo`:** `lzo` (31.9%) beats `lz4` with its format, not its matcher: a match with an offset
up to 2048, 3 to 8 bytes and up to 3 literals after it costs 2 bytes. 26% of `seqlz-fast`'s offsets are
between 257 and 2048 and cost `bytelz` 2 bytes of offset. A token layout with fewer extensions and such
near matches, decoded through a 256-entry table from token to lengths and offset bytes, should cut
both the mispredictions and the memory; `quetschn-lz-analysis` can cost the layouts exactly before
any of it is written.

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
* **A better matcher for seqlz-fast**: `lz4hc` level 3's matches give 25.2% against 26.4%, but it
  must not get slower.
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
