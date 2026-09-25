# Explored designs

Every design idea that was measured, with the result, and why it was kept or dropped. The goal is
`PLAN.md` §1: `lz4`-class decompression latency at `zstd -1`-class memory, measured as Σ zsmalloc
cost (§3.1) and cold-cache p99 per page (§5.2), and since the score of `PLAN.md` §1.1 as memory against
the mean time per page, see [The designs by the score](#the-designs-by-the-score). Add an entry for
everything that gets measured, also and especially for what did not work.

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

## The designs by the score

`PLAN.md` §1.1 replaces the bars C1 to C3 as the target with a score: zsmalloc bytes per page against
the time per page written, `write + r * read + b * recompression`, means over the pages, with `r =
0.34` reads per write from the development machine and `b` the weight of recompression. The designs on
the lower left convex hull of (time, bytes) have the lowest `bytes + lambda * time` for some lambda;
between neighbours the exchange rate in bytes per us is the lambda at which the faster one starts to
win. `quetschn-score` computes it from the logs.

**In the kernel `seqlz-fast-lit` has the lowest score for any lambda from 24 to 150 bytes per us.**
VM of `tools/zram-vm/run.sh`, 20 000 pages per dump, one boot per dump for the 7 devices and one more
per dump for #37, one CPU at a fixed 4.5 GHz; means over the pages of the median of 3 runs, cold reads
with another page before and the compressed data flushed, first dump / second dump, times in us,
`b = 1`:

| codec | bytes per page | write | cold read | recompression | us per page written |
| --- | --- | --- | --- | --- | --- |
| `lz4` | 1 450.4 / 1 754.5 | 5.28 / 5.76 | 2.55 / 2.58 | 0.0 / 0.0 | 6.15 / 6.64 |
| `lzo-rle` | 1 361.1 / 1 678.5 | 5.14 / 5.72 | 2.80 / 2.92 | 0.0 / 0.0 | 6.09 / 6.71 |
| `zstd` | 1 012.3 / 1 197.5 | 13.58 / 14.53 | 5.35 / 5.60 | 0.0 / 0.0 | 15.40 / 16.44 |
| `bytelz` | 1 250.7 / 1 600.5 | 6.09 / 6.65 | 2.71 / 2.81 | 0.0 / 0.0 | 7.01 / 7.61 |
| `seqlz` | 1 147.7 / 1 487.9 | 6.02 / 6.59 | 2.70 / 2.79 | 0.0 / 0.0 | 6.94 / 7.54 |
| `seqlz-lit` | 1 079.9 / 1 398.6 | 6.48 / 7.06 | 2.65 / 2.95 | 0.0 / 0.0 | 7.38 / 8.06 |
| `seqlz-lit+seqlz-opt` | 987.1 / 1 225.9 | 6.47 / 7.07 | 2.83 / 3.02 | 235.6 / 233.1 | 243.04 / 241.18 |
| `seqlz-lit+seqlz-opt`, own tables (#37) | 973.6 / 1 175.6 | 6.42 / 7.07 | 3.00 / 3.31 | 245.2 / 243.7 | 252.65 / 251.89 |

| `b = 1`, from the fastest to the smallest | first dump | second dump |
| --- | --- | --- |
| `lzo-rle` to `seqlz` | 213 bytes for 0.85 us, 252 per us | 191 bytes for 0.83 us, 229 per us |
| `seqlz` to `seqlz-lit` | 68 bytes for 0.45 us, 152 per us | 89 bytes for 0.52 us, 172 per us |
| `seqlz-lit` to `zstd` 3 | 68 bytes for 8.02 us, 8.4 per us | 201 bytes for 8.38 us, 24 per us |
| `zstd` 3 to the own tables of #37, recompressed | 39 bytes for 237 us, 0.2 per us | 22 bytes for 235 us, 0.1 per us |

`lz4` is on the hull of the second dump only, 76 bytes above `lzo-rle` for 0.07 us: the two are the
same speed within the noise of one boot. `bytelz` is behind `seqlz` in both, and `seqlz-opt` with the
fixed tables is not on the hull at any `b` below.

**The weight of recompression decides whether recompression pays.** It takes 233 to 245 us per page,
about 17 times a write with `zstd` 3. At `b = 0.1` the step from `zstd` 3 to the own tables is 2.3 and 1.4
bytes per us, at `b = 0.03` the own tables follow `seqlz-lit` directly with 14 and 30 bytes per us,
and `zstd` 3 drops off the hull; at `b = 0` they follow `lzo-rle` with 287 and 339 bytes per us, and
recompression with `seqlz-opt` is the best choice for every lambda below that. Where recompression is on the hull,
it is with the own tables of #37 and not the fixed ones: 13.5 and 50 bytes smaller, for 0.01 and 0.1
us more of writes and reads per page and 10 us more of recompression. Whether that is worth it depends on how many
pages get recompressed and what the CPU time of an idle CPU costs, energy on a phone and nothing much
on a desktop. #37 stays closed until there is a number for that.

**In userspace the hull is the same.** `quetschn-bench-interleaved`, 20 000 pages per dump, CPU 2 at
4.5 GHz, the mean of each page's median, same-filled pages excluded, zsmalloc cost of the model, times
in us:

| codec | bytes per page | compress | cold decompress | us per page written |
| --- | --- | --- | --- | --- |
| `lz4:1` | 1 411.8 / 1 737.3 | 3.51 / 3.95 | 1.27 / 1.29 | 3.95 / 4.39 |
| `lz4hc:9` | 1 257.1 / 1 560.0 | 36.89 / 31.00 | 1.04 / 1.07 | 37.25 / 31.37 |
| `lzo` | 1 306.6 / 1 631.7 | 3.85 / 4.41 | 1.89 / 1.83 | 4.50 / 5.03 |
| `lzo-rle` | 1 326.1 / 1 646.4 | 2.90 / 3.10 | 1.33 / 1.30 | 3.35 / 3.54 |
| `zstd:-1` | 1 101.3 / 1 449.6 | 7.85 / 8.57 | 3.01 / 3.05 | 8.87 / 9.61 |
| `zstd:3` | 964.6 / 1 163.0 | 12.44 / 13.84 | 3.90 / 4.14 | 13.77 / 15.25 |
| `bdelta:1` | 2 888.6 / 3 404.7 | 3.40 / 3.02 | 0.51 / 0.46 | 3.57 / 3.17 |
| `bytelz` | 1 212.8 / 1 574.7 | 4.72 / 5.30 | 1.57 / 1.65 | 5.26 / 5.86 |
| `seqlz:1` | 1 125.1 / 1 449.9 | 6.86 / 7.88 | 1.91 / 2.08 | 7.51 / 8.59 |
| `seqlz-fast` | 1 106.5 / 1 462.3 | 4.78 / 5.37 | 1.64 / 1.77 | 5.34 / 5.97 |
| `seqlz-fast-lit` | 1 037.6 / 1 368.6 | 3.96 / 4.43 | 1.35 / 1.43 | 4.42 / 4.91 |
| `seqlz-hc:3` | 1 049.2 / 1 350.5 | 17.51 / 19.83 | 1.61 / 1.76 | 18.05 / 20.43 |
| `seqlz-hc-lit:3` | 978.8 / 1 231.6 | 13.22 / 14.68 | 1.34 / 1.49 | 13.68 / 15.19 |
| `seqlz-opt` | 940.7 / 1 198.3 | 253.20 / 251.45 | 1.75 / 1.98 | 253.80 / 252.12 |
| `shuffle-lz4:1` | 1 678.7 / 2 109.1 | 4.45 / 5.13 | 1.73 / 1.67 | 5.04 / 5.69 |
| `spike-branchless` | 2 286.0 / 2 928.6 | 2.37 / 2.26 | 1.93 / 1.53 | 3.02 / 2.77 |
| `spike-slots` | 2 286.0 / 2 928.6 | 1.47 / 1.45 | 1.19 / 0.99 | 1.87 / 1.79 |
| `spike-switch` | 2 286.0 / 2 928.6 | 1.27 / 1.27 | 1.41 / 1.18 | 1.75 / 1.68 |
| `spike-zeroskip` | 2 286.0 / 2 928.6 | 1.24 / 1.26 | 1.40 / 1.21 | 1.72 / 1.67 |
| `zstd-nolit:3` | 1 058.4 / 1 357.5 | 8.49 / 9.69 | 2.42 / 2.58 | 9.32 / 10.57 |

The hull, the same on both dumps up to `zstd` 3: `spike-zeroskip`, `lzo-rle` (590 and 687 bytes per us
from the spike), `seqlz-fast-lit` (268 and 202), `zstd` 3 (7.8 and 19.9). Without zram and zsmalloc the
times are 1.6 to 3 us shorter per page, so the rates are higher than in the VM, the order is the
same. The spike decoders win only for lambda above 590 bytes per us. `zstd -1` is behind
`seqlz-fast-lit` on both axes, `seqlz-hc-lit` just behind the line from `seqlz-fast-lit` to `zstd` 3
(978.8 bytes for 13.68 us against 964.6 for 13.77), `lz4hc` 9 far behind. `seqlz-fast-lit`
compresses faster than `seqlz-fast` here, 3.96 against 4.78 us on the first dump, where in the VM it
is 6.48 against 6.02: the harness times compression by the codec's position in the list, see
[seqlz-fast-lit by the score](#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8).

The other designs in this file have no codec in the harness any more, only the size, or ticks from
their own loops: the word model, BΔI, the shuffle, XOR, FSST, BPC, the pair matcher. None of them was
dropped for a p99 alone; they were larger than a codec that was also faster, which the score does not
change.

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

### seqlz, third decoder round: one repeat offset, offset class in the token, and why cold is slow

*Kept on the branch, not merged yet.* Decode cycles per page with `perf stat` (`--decode-loop`), cold
with `tools/quick-bench.sh`.

| step | Σ zsmalloc cost | decode cycles | cold p50 / p99 | warm p99 |
| --- | --- | --- | --- | --- |
| `seqlz-fast` before | 26.4% | 10 479 | 2840 / 5720 ns | 3310 ns |
| one repeat offset instead of three | 26.6% | 8758 | | |
| the offset's class in the token, raw offset bits | 27.0% | 8168 | 2830 / 5560 ns | 3050 ns |
| ... and the page and the output prefetched | 27.0% | 6306 | 1640 / 3270 ns | 3030 ns |
| `lz4` | 34.5% | 5100 | 1640 / 3540 ns | 2370 ns |
| `lz4` with the same prefetches | 34.5% | | 1030 / 2710 ns | 2380 ns |

* **One repeat offset:** the move to front of three cost the decoder 17% of its cycles, and the
  other two were only 11% of `seqlz-fast`'s matches. Mispredictions 124 to 89 per page, `lz4` has 94.
* **The offset's class in the token:** class 0 the last offset, 1 below 256 in 8 raw bits, 2 below
  4096 in 12, right after the token. So one table lookup per sequence and not two on the chain from
  token to token. The token has 1536 symbols then, and with 11 bits each of them needs at least
  1/2048 of the code space, 75% of it together: 27.5% instead of the estimated 26.6%. Package-merge,
  the optimal length limit, gave the same token bits as halving the counts (106.6 against 106.7
  million, 90.6 without a limit), so it is the limit, not the method. A 12-bit token table (8 KiB)
  gives 27.0%. Fewer symbols with 11 bits: 3 / 5 bits for ll / ml 26.9% at 8599 cycles, 4 / 4 27.0%
  at 8482.
* **The bitstream before the literals**, so that the first token is in the header's cache line: no
  change of the cold latency, reverted.
* **Why cold is slow:** with only the compressed page and the output flushed, as the harness does, a
  page costs `lz4` 10 913 cycles and `seqlz-fast` 15 173, with the same cache misses (97 and 101 per
  page) and mispredictions. Prefetching the compressed page first did nothing. Prefetching the 64 lines
  of the output for writing first: 11 567. The writes into cold lines were the wait, and with 2.4
  times the instructions of `lz4` fewer of them overlapped.
* **But `lz4` gets the same from the prefetches:** 1030 / 2710 ns. Against that, `seqlz-fast` is 560
  ns slower at cold p99, in all 5 runs. The prefetch belongs into zram, before any decompression, not
  into the format. Whether the destination page is cold in a real page fault is not measured: the page
  allocator may hand out a page that was just freed and is still in the cache.

**More decoder experiments**, against `lz4-prefetch`, the kernel's `lz4` with the same prefetches
before it (a codec of the benchmark for this comparison). Cycles on 2000 pages whose compressed data
fits into L3 (`zram0-sample2000`): with 20 000 pages the loop reads from memory, and the result moved by
up to 20% between processes. With 200 pages the branch predictor learns the pages: `lz4` then
mispredicts 1.6 times per page instead of 98.

* **Where the time goes**, by leaving parts out (wrong output, but the loop does not check it): the
  match copies are 7100 instructions, about 2700 cycles and 63 of the 91 mispredictions per page, the
  literal copies 4600 instructions and 500 cycles. The rest, bit reader, tokens and checks, is 113
  instructions per sequence; `lz4` needs 55 for everything. Sampling instructions had put half of them
  into the match copy, wrongly: the samples pile up behind its loads.
* **Tables cold**, 2 MiB of other data read before each page (`--decode-loop --cold`), 3 runs each:
  the 12-bit token table 2670 to 2830 / 3920 to 4070 ns, the 11-bit one 2460 to 2530 / 3690 to 3780,
  `lz4` 1520 / 2720. Prefetching the tables at the start: about 100 ns less, within the scatter.
* **Tried and slower:** the match copy with a bound computed once, 16 bytes per step for offsets of 16
  and more, the pattern stored without loads for offsets 1, 2 and 4: 9193 cycles instead of 8410,
  mispredictions 91 to 115. 32 bytes without a loop for offsets of 32 and more: 9223, 108. Decoding in
  batches, 32 sequences into an array, then their copies: 10 667, 136 mispredictions, cold p99 1820 ns
  behind `lz4-prefetch`. Every branch split mispredicts, and the copies' branches lose the history of
  the decoding branches that predicted them.

**Rare tokens escaped, and smaller tables.** With an escape, only the frequent tokens get a code, a
rare one is the escape's code and 11 raw bits; the escape has at most 8 bits, so the encoder's bound of
31 bits per 4 bytes holds. The trainer tries how many tokens get a code and keeps the fewest bits.
Decode cycles as the median of 5 processes on `zram0-sample2000`, cold as `--decode-loop 5 --cold`:

| change | Σ zsmalloc cost | decode cycles | cold p50 / p99 |
| --- | --- | --- | --- |
| 1536 tokens, 12-bit table | 27.0% | 8168 | 2670 to 2830 / 3920 to 4070 ns |
| 1536 tokens, 11-bit table and the escape | 26.8% | 8480 | 2490 to 2500 / 3690 ns |
| four offset classes, 4 × class raw bits: below 16, 256, 4096 | 26.7% | 8237 | |
| the encoder's token table with 16-bit entries (4 KiB) | 26.7% | | |
| length values with codes of at most 8 bits (1 KiB tables) | 26.7% | | 2400 to 2420 / 3600 to 3620 ns |
| no count of the sequences, the last one fills the page | 26.6% | 8107 | 2380 to 2410 / 3560 to 3600 ns |

A 10-bit token table: 0.16% more bytes, cold 40 ns faster, within the scatter, 6 more mispredictions:
dropped. The full benchmark on the whole corpus after these changes:

| codec | Σ zsmalloc cost | compress p50 / p99 | decompress cold p50 / p99 | warm p99 |
| --- | --- | --- | --- | --- |
| `lz4-prefetch` | 34.5% | 2260 / 4190 ns | 1040 / 2650 ns | 2360 ns |
| `lz4` | 34.5% | 2260 / 4190 ns | 1260 / 2900 ns | 2360 ns |
| `seqlz-fast` | 26.6% | 3110 / 6490 ns | 1540 / 3130 ns | 2880 ns |
| `seqlz-hc` | 25.6% | 10 950 / 15 920 ns | 1390 / 3270 ns | 3030 ns |
| `zstd -1` | 26.9% | 5180 / 10 270 ns | 2580 / 4640 ns | 3890 ns |

`seqlz-fast` has less memory than `zstd -1` and decodes 33% faster than it at cold p99, 8% slower than
the kernel's `lz4` and 18% slower than `lz4` with the prefetches.

**With clang**, which builds Android's kernels, and the same kernel flags: decoding `lz4` 5107
cycles per page (gcc 5268), `seqlz-fast` 8449 (8192), `bytelz` 7808 (7276); `seqlz-fast` needs 34 108
instructions instead of 31 176, `bytelz` 25 137 instead of 23 467, with the same mispredictions.
Compressing, `lz4` 16 860 (17 105), `seqlz-fast` 20 687 (21 025). So with clang the decoders are 4 to
8% further behind `lz4` than with gcc; the kernel numbers above are gcc's.

## bytelz: `seqlz-fast`'s matcher, a byte oriented format

*Kept for arm64, out of the default comparison.* In the kernel VM of 25 September no exchange rate makes
`bytelz` the best choice on either dump (see [The designs by the score](#the-designs-by-the-score)); the
same holds for `seqlz-fast` on the second dump, and on the first only from 172 to 211 bytes per us. Both
stay for the little cores of arm64, where a byte format or raw literals may beat the Huffman coded ones
of `seqlz-fast-lit`; if they are not the best choice there either, `bytelz` goes. `tools/plot-codecs.py`
and its run in the README leave both out.

*Earlier: close to `lz4` warm, but 1.33 times as slow at cold p99 in both directions.* Code:
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

**Token layouts, costed** (`quetschn-lz-analysis --codec seqlz`, token classes, on the 20 000 page
sample): the 256 token values split into classes, each with fields for literal length and match length
- 4, with or without an extension, and one kind of offset: the last one, the one before, 1 byte, near
(k bits in the token and 1 byte, as `lzo`'s M2) or 2 bytes. A search from `bytelz`'s layout changes one
field at a time and keeps what gets smaller, with each extension counted as w bytes more, because each
is a branch in the decoder:

| w | Σ zsmalloc cost | ll extensions | ml extensions | layout (ll / ml bits, e: with extension) |
| --- | --- | --- | --- | --- |
| `bytelz` | 29.33% | 8.0% | 25.0% | last, before, byte, word, each 3e / 3e |
| 0 | 28.93% | 15.1% | 18.9% | last 2e/3e, before 2e/4e, byte 2e/4e, word 3e/3e, near3 1e/1e |
| 0.5 | 28.96% | 8.4% | 19.4% | last 2/3e, before 2e/4e, byte 2/4e, word 3e/3e, near2 1e/2e |
| 1 | 29.02% | 8.5% | 17.3% | last 3e/2, before 2e/4e, byte 2/4e, word 3e/3e, near2 1e/2e |
| 2 | 29.07% | 8.0% | 16.2% | last 3e/2, before 1/5e, byte 2/4e, word 3e/3e, near2 1e/2e |

So a better layout is worth at most 0.4 points of memory, and cuts the sequences with an extension from
33% to about 24%; with `lz4`'s 4 bits 13% of the matches would still need one. `lzo`'s near matches
help little here, because 49% of the offsets already fit into 1 byte.

**One class per offset kind**, every field with an extension, so that the class follows from the
offset and the encoder stays free of branches: the best of four is 28.76% (last 3e/3e, before 2e/3e,
byte 2e/4e, near2 1e/3e, word 2e/3e), but 21% of the sequences need an extension of ll and 20% one of
ml, 41% together against `bytelz`'s 33%. A token byte cannot make both rarer: 82% of the literal runs
are shorter than 3, but 59% of the matches 4 to 7 bytes and 13% 19 or longer. `bytelz`'s layout stays.

**Next, from `lzo`:** `lzo` (31.9%) beats `lz4` with its format, not its matcher: a match with an offset
up to 2048, 3 to 8 bytes and up to 3 literals after it costs 2 bytes. 26% of `seqlz-fast`'s offsets are
between 257 and 2048 and cost `bytelz` 2 bytes of offset. A token layout with fewer extensions and such
near matches, decoded through a 256-entry table from token to lengths and offset bytes, should cut
both the mispredictions and the memory; `quetschn-lz-analysis` can cost the layouts exactly before
any of it is written.

## zram: prefetch the compressed data before decompression

*A zram change for every codec, not a format. In the kernel, with cold compressed data, `lz4`'s p99 read
latency is 30% lower.* Code: `tools/zram-vm/`, `run.sh <linux tree> <corpus>`.

In the userspace harness, prefetching the output page for writing made both `seqlz-fast` and `lz4`
much faster when the compressed page and the output were flushed (seqlz's third decoder round). Does
that hold in the kernel? `tools/zram-vm/run.sh` builds a kernel with `zram-prefetch.patch`: a module
parameter prefetches, in `read_compressed_page()` before `zcomp_decompress()`, the 64 lines of the
destination for writing (1), all lines of the compressed object (2), or both (3), and another one
flushes the compressed object from the cache first, to stand for data that was not read for a long
time. In a VM on CPU 2 at a fixed 4.5 GHz, `/init` writes the 20 000 sample pages to `/dev/zram0`
with `lz4`, and reads each back with `O_DIRECT`, so zram decompresses straight into the program's
page; the prefetch alternates from page to page, the median of 3 runs per page. p50 / p90 / p99 in ns:

| condition | none | destination | compressed data | both |
| --- | --- | --- | --- | --- |
| warm | 1880 / 2460 / 3531 | 1910 / 2490 / 3560 | 1890 / 2470 / 3600 | 1911 / 2491 / 3600 |
| destination flushed | 2061 / 2631 / 3680 | 2080 / 2661 / 3680 | 2060 / 2650 / 3660 | 2080 / 2670 / 3689 |
| compressed data flushed | 2320 / 3849 / 5600 | 2340 / 3770 / 5461 | 2190 / 2811 / 3891 | 2210 / 2840 / 3939 |
| both flushed | 2440 / 3840 / 5470 | 2520 / 3810 / 5480 | 2360 / 2960 / 4040 | 2450 / 3040 / 4070 |

* **In the kernel the compressed data matters, not the destination:** prefetching its lines first
  took p99 from 5600 to 3891 ns when it was cold, and cost nothing when it was warm. `lz4` reads its
  input front to back, and on the pages with the most compressed bytes the hardware prefetcher comes
  too late; all lines at once, their misses overlap.
* **The destination:** flushing it cost only 140 to 180 ns, and prefetching it did not help. In the
  userspace harness it was the other way round, which is not explained; the kernel's numbers are the
  ones that count.
* **Not known yet:** how often the compressed data is really cold at a page fault, and how an arm64
  little core behaves. The flush happens inside the timed part, for all four variants alike. zram
  gives the codec a copy in `local_copy` when an object spans two pages; that copy is warm anyway.

**`seqlz-fast` in the kernel.** `ALGOS=lz4,seqlz tools/zram-vm/run.sh` builds seqlz as a zram backend
(`backend_seqlz.c`, with `lz4`'s `-O3`), one device per algorithm, and reads the pages from both in turn
(the destination prefetch left out, it did not help). zram's own `mm_stat` for the 20 000 pages: `lz4`
27 239 770 bytes compressed, 29 007 872 used by zsmalloc; `seqlz` 20 975 271 and 22 679 552, 22% less
memory. Read latency p50 / p90 / p99 in ns:

| condition | `lz4` | `lz4`, prefetch | `seqlz` | `seqlz`, prefetch |
| --- | --- | --- | --- | --- |
| warm | 1950 / 2510 / 3560 | 1890 / 2469 / 3549 | 2550 / 3291 / 4150 | 2491 / 3211 / 4120 |
| compressed data flushed | 2370 / 3269 / 4490 | 2200 / 2820 / 3900 | 3150 / 4240 / 5450 | 2790 / 3580 / 4410 |
| both flushed | 2480 / 3290 / 4490 | 2379 / 2970 / 3990 | 3269 / 4340 / 5490 | 2950 / 3730 / 4580 |

In the real read path `seqlz` is 550 to 600 ns slower per page than `lz4`, warm or cold, with the
prefetch in both: the difference of the decoders' work, about 0.6 µs, and not of the caches.

**All four in the kernel** (`ALGOS=lz4,lzo-rle,zstd,seqlz`, `zstd` at zram's default level), the
20 000 sample pages, compressed data flushed, p50 / p99 in ns:

| algorithm | used by zsmalloc | read, prefetch of the compressed data | read, no prefetch |
| --- | --- | --- | --- |
| `lz4` | 29 007 872 | 2220 / 3960 | 2480 / 4490 |
| `lzo-rle` | 27 222 016 | 2270 / 3860 | 2651 / 4880 |
| `zstd` | 20 246 528 | 5431 / 9970 | 6141 / 11 309 |
| `seqlz` | 22 679 552 | 2820 / 4470 | 3271 / 5530 |

`seqlz` needs 17% less memory than `lzo-rle`, zram's default, which is C1, and reads about 600 ns
slower than `lz4` and `lzo-rle`, half as long as `zstd`, which needs 11% less memory. The prefetch
helps all four.

Writing each page again, timed, compression and zsmalloc together, p50 / p90 / p99 in ns: `lz4` 5311 /
7360 / 9209, `lzo-rle` 5040 / 7230 / 9520, `zstd` 13 760 / 18 880 / 23 580, `seqlz` 6280 / 9010 /
11 620. So `seqlz` writes 1.26 times as long as `lz4` at p99, C3 allows 1.2; in the harness, which times
the codec alone, it was 1.55: zram's own work on a write makes the difference smaller.

**`bytelz` in the kernel** (`ALGOS=lz4,lzo-rle,bytelz,seqlz`), compressed data flushed, with the
prefetch, p50 / p99 in ns:

| algorithm | used by zsmalloc | read | write |
| --- | --- | --- | --- |
| `lz4` | 29 007 872 | 2210 / 3951 | 5260 / 9090 |
| `lzo-rle` | 27 222 016 | 2260 / 3860 | 4991 / 9560 |
| `bytelz` | 24 813 568 | 2390 / 3910 | 6280 / 11 560 |
| `seqlz` | 22 679 552 | 2810 / 4460 | 6179 / 11 590 |

`bytelz` reads as fast as `lz4` and `lzo-rle` at p99, 180 ns slower at p50, with 14.5% less memory
than `lz4` and 8.8% less than `lzo-rle`: C1 just, C2 not quite, it asks for faster. It writes as slowly
as `seqlz`: the matcher they share is most of it.

**`seqlz-hc` for zram's recompression** (`ALGOS=lz4,zstd,seqlz,seqlz-hc`; `backend_seqlz_hc.c` takes
the kernel's `lz4hc` at level 3 and codes its matches with seqlz). Compressed data flushed, with the
prefetch, p50 / p99 in ns:

| algorithm | used by zsmalloc | read | write |
| --- | --- | --- | --- |
| `lz4` | 29 007 872 | 2210 / 3990 | 5350 / 9260 |
| `zstd` | 20 246 528 | 4689 / 7650 | 13 979 / 23 870 |
| `seqlz` | 22 679 552 | 2790 / 4400 | 6291 / 11 620 |
| `seqlz-hc` | 21 893 120 | 2531 / 4370 | 19 410 / 32 300 |

For pages recompressed in the background, where the write does not wait, `seqlz-hc` needs 8% more
memory than `zstd` and reads 43% faster at p99, 380 ns behind `lz4`.

**`seqlz-hc-lit`: the literals Huffman coded too**, the gap to `zstd`'s ratio (`seqlz_encode_coded()`,
`seqlz_decode_scratch()`, experimental). A page with coded literals has its own header, 256 static code
lengths for bytes trained with the others, and the literals are decoded into a per-CPU scratch buffer
first, then the sequences run as before. `zstd` here at zram's default level. Quick benchmark:

| variant | Σ zsmalloc cost | decode cycles | cold p50 / p99 |
| --- | --- | --- | --- |
| `seqlz-hc` | 25.6% | 7515 | 1320 / 3120 ns |
| literals in one stream, coded if smaller | 24.1% | 11 901 | 2150 / 6210 ns |
| one stream, coded if 1/8 smaller | 24.6% | 9522 | 1530 / 5270 ns |
| four streams, literal k in stream k % 4, 1/8 | 24.9% | 8146 | 1370 / 3840 ns |
| four streams, coded if smaller | 24.3% | | 1760 / 4280 ns |
| four streams, 1/16 | 24.4% | | 1590 / 4010 ns |
| `zstd` | 23.6% | 16 928 | 3350 / 6320 ns |

One stream is one chain of table lookups per literal, and the pages with the most literals made the
p99 as slow as `zstd`'s; four streams are four chains side by side, as `zstd` does it. With 1/16,
`seqlz-hc-lit` needs 3.4% more memory than `zstd` and decodes in 37% less time at p99, 53% at p50.

In the kernel (`ALGOS=lz4,zstd,seqlz-hc,seqlz-hc-lit`), compressed data flushed, with the prefetch,
p50 / p99 in ns:

| algorithm | used by zsmalloc | read | write |
| --- | --- | --- | --- |
| `lz4` | 29 007 872 | 2200 / 3890 | 5460 / 9410 |
| `zstd` | 20 246 528 | 4689 / 7611 | 14 130 / 24 071 |
| `seqlz-hc` | 21 893 120 | 2501 / 4270 | 19 540 / 32 389 |
| `seqlz-hc-lit` | 20 873 216 | 2771 / 5400 | 16 130 / 30 240 |

So for recompression `seqlz-hc-lit` needs 3.1% more memory than `zstd` and reads 41% faster at p50,
29% at p99.

**`seqlz-fast-lit`**: `seqlz-fast` with the literals coded the same way (`seqlz_compress_coded()`: the
raw page first, then the literals moved to the end of dst and coded from there). Quick benchmark, and
compress cycles per page with `perf stat`:

| codec | Σ zsmalloc cost | compress cycles | decompress cold p50 / p99 |
| --- | --- | --- | --- |
| `lz4` | 34.5% | 17 067 | 1030 / 2700 ns (with the prefetch) |
| `seqlz-fast` | 26.6% | 21 059 | 1540 / 3150 ns |
| `seqlz-fast-lit` | 25.4% | 23 300 | 1780 / 4130 ns |
| `zstd` | 23.6% | | 3680 / 6580 ns |

1.2 points less memory for 11% more compress time and 1 µs more at cold p99: another point between
`seqlz-fast` and `zstd`.

**All candidates in the kernel**, one boot, 7 zram devices read in turn (so each read finds less in
the caches than in the runs above), compressed data flushed, with the prefetch, p50 / p99 in ns:

| algorithm | used by zsmalloc | read | write |
| --- | --- | --- | --- |
| `lz4` | 29 007 872 | 2520 / 4310 | 5739 / 9670 |
| `lzo-rle` | 27 222 016 | 2580 / 4180 | 5470 / 9991 |
| `bytelz` | 24 813 568 | 2720 / 4251 | 6770 / 11 939 |
| `seqlz-fast` | 22 679 552 | 3070 / 4640 | 6560 / 11 840 |
| `seqlz-fast-lit` | 21 614 592 | 3280 / 5849 | 5690 / 12 480 |
| `seqlz-hc-lit` | 20 873 216 | 3080 / 5700 | 20 519 / 34 360 |
| `zstd` | 20 246 528 | 5120 / 8130 | 14 509 / 24 350 |

Without the insert of the position 2 bytes before the end of each match into the hash table:
27.1% instead of 26.6% for 1.8% fewer compress cycles, dropped like the other matcher trades. In this
run `seqlz-fast` writes 1.22 times as long as `lz4` at p99, in the run before 1.26: C3's 1.2 is within
the scatter between runs in the kernel, while the harness, which times the codec alone, says 1.55.

Three more runs with only `lz4`, `lzo-rle`, `seqlz-fast` and `bytelz`, write p50 / p99 in ns: `lz4`
5571 to 5610 / 9420 to 9450, `seqlz-fast` 6470 to 6520 / 11 660 to 11 740, `bytelz` 6580 to 6610 /
11 750 to 11 819. So both write 1.24 times as long as `lz4` at p99, 1.16 at p50: C3 missed by about
3% of the write time, 300 ns on the slowest pages.

**Tables trained on the device**, passed as zram's dictionary (2355 bytes of code lengths): trained on
the 20 000 sampled swap pages and measured on the whole dump, `seqlz-fast` needs 495 461 348 bytes
instead of 496 301 529 with the tables from the resident pages, 0.17% less. The static tables carry
over; retraining them per device is not worth it, and for C4 `seqlz` needs no dictionary to beat
`lz4` with one (33.7%).

The Pareto front on this machine, from `lz4`'s speed to `zstd`'s memory: `bytelz`, `seqlz-fast`,
`seqlz-fast-lit`, and for recompression `seqlz-hc-lit`. `seqlz-fast-lit` writes faster than
`seqlz-fast` at p50, not explained; zram's own work per write depends on the size of the object.

**The same prefetch before compression does nothing.** A switch `zram_prefetch_write` asked for all
lines of the page in `zram_write_page()` before `zcomp_compress()`, with the page flushed from the
cache before each timed write. The first run said 1.0 to 1.2 µs less at p50 for every algorithm, and a
prefetch in seqlz's own backend instead said nothing at all, which made no sense: both run a few
instructions apart. A third value, prefetch in the backend, gave it away: `lz4`'s backend ignores it,
and `lz4` was still the fastest with it. The variants of one algorithm followed each other in the
loop, so the write before was the same page to the same device, and zram's slot and the freed
zsmalloc object were still in the cache. With the devices in turn between two writes, p50 / p99 in ns:

| algorithm | no prefetch | prefetch in zram | prefetch in the backend |
| --- | --- | --- | --- |
| `lz4` | 4440 / 9020 | 4381 / 8840 | 4390 / 8970 (ignored) |
| `seqlz-fast` | 5260 / 11 160 | 5449 / 11 219 | 5490 / 11 110 |
| `bytelz` | 5300 / 11 160 | 5460 / 11 230 | 5499 / 11 160 |

All within the scatter. The compressors read the page front to back, and the hardware prefetcher keeps
up with that. The write numbers of the tables above rotate through the devices and are not affected.

**The prefetch before decompression survives the same check, and the codec can do it itself.** The
read loop had the same order, so it got the same treatment: the devices in turn, and a mode 8 that
prefetches the compressed data in seqlz's and bytelz's backend instead of in zram, which `lz4`'s and
`lzo-rle`'s backends ignore. Compressed data flushed, p50 / p99 in ns:

| algorithm | no prefetch | prefetch in zram | prefetch in the backend |
| --- | --- | --- | --- |
| `lz4` | 2350 / 4510 | 2280 / 3939 | 2379 / 4680 (ignored) |
| `lzo-rle` | 2470 / 4660 | 2350 / 3980 | 2480 / 4800 (ignored) |
| `seqlz-fast` | 3020 / 5400 | 2830 / 4419 | 2840 / 4440 |
| `bytelz` | 2541 / 4710 | 2461 / 3960 | 2470 / 3950 |

So the saving is real, and it does not need a change to zram: the backend is the first to read the
compressed data, and it can ask for all of it at once. Against a stock kernel, where `lz4` does not
prefetch, `seqlz-fast` reads cold pages 1.6% faster at p99 and 21% slower at p50, `bytelz` 12% faster
at p99 and 5% slower at p50. That is C2 at p99 for both, but only as long as `lz4` does not do the
same, which is a 4 line change to its backend.

**A fast path in seqlz's decoder, as `lz4` and `bytelz` have one.** `seqlz-fast` decoded at IPC 3.74
with 30 100 instructions per page, `lz4` 12 400: about 133 instructions per sequence against 55, so it
waits for instructions, not for memory. A good part of them checked the room behind each literal and
match copy. Now a sequence without length values, at least 64 bytes before the end of the page and 16
before the end of the literals, copies 16 literal bytes, and 16 or 32 bytes of match for offsets of 8
and more, without those checks; only the literal count and the offset are checked. The format stays
the same.

| | instructions per page | decode cycles, 2000 pages | cold p50 / p99 | kernel read p50 / p99, cold |
| --- | --- | --- | --- | --- |
| before | 30 100 | 8131, 8179 | 1550 / 3150, 1570 / 3160 ns | 2840 / 4440 ns |
| fast path | 26 200 | 7725, 7750 | 1410 / 3140, 1400 / 3120 ns | 2740 / 4461 ns |

13% fewer instructions and 5% fewer cycles: now something else limits the loop. Cold p50 is 10%
lower, p99 does not move, and in the kernel it is 100 ns at p50. The literal count in the fast path
needs its own check, which no test covered: without it, `lit` runs past the literals into the
bitstream, and the careful path, where `lit_end - lit` is then negative and unsigned, reads behind the
input. A test builds such a page now.

Tried and dropped on top of it: no branch on the offset in the fast path, the first 8 bytes as
`(load64(d - off) & mask) * repeat[off]` for every offset and then copies step bytes apart as in
`copy_match`. Three copies and a loop for longer matches: 8264 cycles instead of 7740, the loop exit
mispredicts instead. Five copies: mispredictions 99 to 82 per page, but 8987 cycles. Each copy loads
what a store a few cycles before wrote, and with `u64` copies such a load often spans two stores and
cannot take its bytes from them: `ls_bad_status2.stli_other` counts 359 per page for `seqlz-fast`,
363 for `lz4`, 92 for `bytelz`. More copies, more of those waits.

Kept: for offsets below 8, `copy_match` built the first 8 bytes in a register and then loaded, step
bytes on, what the store before had written. Step is a multiple of the offset, so that is the same 8
bytes again: now the register is stored at each step, without loads. The fast path stores it 5 times
unconditionally (at least 28 bytes) and loops only for longer matches. Decode cycles on 2000 pages
7750 to 7276, mispredictions 99 to 92 per page, cold p50 / p99 1405 / 3130 to 1380 / 3090 ns. In the
kernel it is within the scatter (2970 / 4621 ns, other page first). `bytelz` does not change, its
fast path rarely gets there.

`bytelz`'s fast path now copies matches the same way (and needs 64 bytes of room instead of 32): 16
bytes, 16 more for longer matches, `copy_match` only past 32 bytes, and short offsets with stores
only. Its mispredictions, from the branch stack (`perf record -b`), were 31% the extension of ml,
22% the choice between the 16-byte copy and `copy_match`, 17% the loop in `copy_match`, 13% the
entry to the fast path. 7310 to 7019 cycles per page, mispredictions 149 to 147: the branch on
`len > 16` mispredicts where the loop did.

Tried and dropped: **the extensions of ml in their own stream**, downwards from the end of the page,
so that the fast path reads them without a branch and the position of the next token does not wait
for them. Same size. Mispredictions 147 to 107 per page, but 7862 cycles instead of 7019 and 30 500
instructions instead of 22 200; with a branch on the extension again 7418 cycles. The second pointer
costs a register the loop does not have, and the branchless read of up to 3 bytes costs more than the
misprediction it saves.

Tried and dropped: **no offsets below 8 from the matcher**, a run with a short period taken from a
multiple of its period that is at least 8, when the bytes before repeat too. With such matches only,
the fast path needs no branch on the offset: 6864 cycles instead of 7750, mispredictions 99 to 75,
but 27.9% instead of 26.6%. The short runs, e.g. 4 to 11 zero bytes, have no 8 equal bytes before them
and become literals. Taking the multiple only where it works and the short offset otherwise keeps
26.6% and gains 1.5%: most short offsets are at the start of a run. It would also be a format change
for `seqlz-hc`, whose `lz4hc` matches have short offsets.

**The warm-up read flattered the kernel numbers.** Before each timed read, `/init` read the same page
from the same device, to warm the path. That also lets the branch predictor learn the branches of just
this page, which a page fault does not get. With another page read first (i + n / 2), compressed data
flushed, p50 / p99 in ns, two boots:

| algorithm | same page first | other page first | other page first, again |
| --- | --- | --- | --- |
| `lz4` | 2399 / 4451 | 2530 / 4661 | 2510 / 4740 |
| `lzo-rle` | 2469 / 4890 | 2690 / 5490 | 2680 / 5230 |
| `seqlz-fast`, prefetch in the backend | 2760 / 4490 | 2969 / 4600 | 2950 / 4600 |
| `bytelz`, prefetch in the backend | 2480 / 3980 | 2710 / 4339 | 2750 / 4351 |

`lz4` gets 130 ns slower at p50, the others 180 to 230: the more mispredictions a decoder has, the
more it gained from the page it had just seen. Against stock `lz4` `seqlz-fast` is now 18% slower at
p50 and 3% faster at p99, `bytelz` 9% slower and 8% faster. The other page first is the condition to
compare with from here on. Writes with another page before them: 130 to 180 ns more for all four,
`seqlz-fast` 1.26 times `lz4` at p99 as before (11 640 and 9230 ns).

**All candidates again, with the other page first**, one boot, 7 devices, the decoder changes above,
`seqlz`'s and `bytelz`'s backends with their own prefetch, the others as the kernel has them. p50 /
p99 in ns:

| algorithm | used by zsmalloc | vs `lzo-rle` | read, cold | read, warm | write |
| --- | --- | --- | --- | --- | --- |
| `lz4` | 29 007 872 | +6.6% | 2549 / 4820 | 1990 / 3560 | 5471 / 9299 |
| `lzo-rle` | 27 222 016 | | 2730 / 5609 | 2080 / 3650 | 5260 / 9690 |
| `bytelz` | 24 813 568 | -8.8% | 2780 / 4410 | 2179 / 3701 | 6571 / 11 740 |
| `seqlz-fast` | 22 679 552 | -16.7% | 2941 / 4660 | 2440 / 4190 | 6409 / 11 650 |
| `seqlz-fast-lit` | 21 614 592 | -20.6% | 3019 / 5510 | 2610 / 4819 | 6820 / 13 469 |
| `seqlz-hc-lit` | 20 873 216 | -23.3% | 3000 / 5540 | 2459 / 4760 | 20 190 / 34 349 |
| `zstd` | 20 246 528 | -25.6% | 5560 / 9960 | 4700 / 7900 | 14 171 / 24 040 |

With cold compressed data, `bytelz` and `seqlz-fast` read faster than `lz4` and `lzo-rle` at
p99, and 9% and 15% slower than `lz4` at p50; with warm data they are slower throughout. At p99 they
write 1.26 and 1.25 times as long as `lz4`, C3 wants 1.2. `seqlz-fast-lit` now writes slower than
`seqlz-fast` (6820 against 6409 ns at p50): in the run before, where it came right after
`seqlz-fast` on the same page and was faster, it got the branch history of the same code on the same
bytes.

**A hash of 5 bytes: C3 in the kernel.** The second round of the compressor dropped it, 2.6% faster
for 0.5 points. Measured in the kernel it is worth more than that. The matcher of `seqlz-fast` and
`bytelz`, compress cycles per page with `perf stat`, Σ zsmalloc cost with `tools/quick-bench.sh`:

| matcher | compress cycles | Σ zsmalloc cost |
| --- | --- | --- |
| hash of 4 bytes | 21 440 | 26.6% |
| hash of 5 bytes | 20 425 | 27.2% |
| step growing twice as fast (`>> 5`) | 20 994 | 26.7% |
| both | 20 037 to 20 436 | 27.4% |
| hash of 5 bytes, tables trained on its matches | | 27.0% |

In the kernel, with the other page before each write, the 5-byte hash takes `seqlz-fast`'s write p99
from 11 650 to 10 480 ns, 10%, twice what the cycles say: the slowest pages are the ones with many
short matches, and the hash of 5 bytes finds fewer of them. Both changes together gave 10 380 ns, the
step adds nearly nothing and is left out. Kept: the hash of 5 bytes and `seqlz-fast`'s tables trained
again (only `seqlz_default_own` changes, the tables for `lz4`'s and `lz4hc`'s matches stay). Matches
of 4 bytes still come from the last offset. With 16 KiB pages (`resident-16k`) the hash of 5 bytes
takes `bytelz` from 29.8% to 29.6% (7.8% below `lzo-rle`, still short of C1) and `seqlz-fast` from
26.1% to 26.3%, and 26.1% again with its 16 KiB table trained again. One boot, other page first, p50 /
p99 in ns:

| algorithm | used by zsmalloc | vs `lzo-rle` | read, cold | read, warm | write |
| --- | --- | --- | --- | --- | --- |
| `lz4` | 29 007 872 | +6.6% | 2490 / 4739 | 1950 / 3570 | 5369 / 9099 |
| `lzo-rle` | 27 222 016 | | 2690 / 5399 | 2040 / 3571 | 5100 / 9500 |
| `bytelz` | 25 014 272 | -8.1% | 2600 / 4160 | 2060 / 3609 | 6240 / 10 320 |
| `seqlz-fast` | 22 953 984 | -15.7% | 2860 / 4480 | 2329 / 4071 | 6250 / 10 470 |

`seqlz-fast` now needs 1.2% more memory than with the 4-byte hash and writes 1.15 times as long as
`lz4` at p99, `bytelz` 1.13 times: C3 is met for both. With cold compressed data both read faster than
stock `lz4` at p99, 5% and 12%, and are 15% and 4% slower at p50; with warm data slower throughout.
`bytelz` is 8.1% below `lzo-rle`, C1 wants 8%, so it only just passes.

Fewer sequences also decode faster: 7276 to 6883 cycles per page for `seqlz-fast`, 7019 to 6696 for
`bytelz`. A hash of 6 bytes goes on the same way, 19 487 compress cycles and 6668 decode cycles, but
27.9% (with the tables of the 5-byte hash) and `bytelz` 30.4%, which fails C1. Dropped.

Also dropped: **one step of lazy matching**, after a match the candidate of the next position, taken
if it is longer by 2 bytes or more. 24 808 compress cycles instead of 20 400, 21% more, `seqlz-fast`
stays at 27.0%, `bytelz` 29.7% to 29.4%, decoding within 1%.

**The full benchmark after these changes** (decoder fast path, short offsets with stores, hash of 5
bytes, tables trained again), all 455 239 pages, CPU 2 at 4.5 GHz:

| codec | Σ zsmalloc cost | compress p50 / p99 | decompress cold p50 / p99 | warm p99 |
| --- | --- | --- | --- | --- |
| `lz4-prefetch` | 34.5% | 2260 / 4200 ns | 1040 / 2650 ns | 2360 ns |
| `lz4` | 34.5% | 2260 / 4200 ns | 1060 / 2600 ns | 2350 ns |
| `bytelz` | 29.7% | 3030 / 6270 ns | 1140 / 2690 ns | 2340 ns |
| `seqlz-fast` | 27.0% | 3110 / 6590 ns | 1240 / 2990 ns | 2770 ns |
| `seqlz-hc` | 25.6% | 10 960 / 15 950 ns | 1160 / 3080 ns | 2830 ns |
| `zstd -1` | 26.9% | 5210 / 10 350 ns | 2410 / 4460 ns | 3910 ns |

Against the full run before: `seqlz-fast` decodes cold in 1240 / 2990 instead of 1540 / 3130 ns, for
0.4 points more memory, now 0.1 points more than `zstd -1`, which takes twice as long at p50. `lz4`
without the prefetch also came out faster than in that run (1060 / 2600 instead of 1260 / 2900 ns),
with the same code, so differences of that size between two full runs are not all ours. In the
harness, which compresses with the page in cache, `seqlz-fast`'s compress p99 does not move with the
5-byte hash (6590 ns, 1.57 times `lz4`); in the kernel it went down 10%. The harness is not the place
to judge C3.

**The harness lets the branch predictor learn the page, too.** For each page it first compresses and
decodes once to check the roundtrip, then times 5 runs of each codec, one page after the other: every
timed run comes right after the same code on the same bytes, as the warm-up read in `/init` did.
`--decode-loop --flush` goes through all 20 000 pages of the sample before it sees a page again, with
the page and the output flushed as the harness's cold decode does. `bytelz`'s glue now prefetches like
`seqlz`'s (`explore/zram_bytelz.c`), as its kernel backend does. p50 / p99 in ns:

| codec | harness, cold (full run) | `--decode-loop 11 --flush` |
| --- | --- | --- |
| `lz4-prefetch` | 1040 / 2650 | 1400 / 2740 |
| `lz4` | 1060 / 2600 | 1940 / 3380 |
| `bytelz` | 1140 / 2690 (without prefetch) | 1770 / 2830 |
| `seqlz-fast` | 1240 / 2990 | 1840 / 3180 |

Without the learned page `seqlz-fast` is 31% slower than `lz4-prefetch` at p50 instead of 19%,
`bytelz` 26%. The loop also has TLB misses the harness does not have, each page's data is somewhere
else, while the kernel's direct map uses large pages, so neither is the kernel. The numbers to decide
with are the VM's, with the other page first; the harness is for quick comparisons of the same codec.

**The harness now times each page after other pages** (`bench/harness.cpp`). It checks 16 pages at a
time, then goes through all 16 once per repetition, first the compressions, then the warm and then the
cold decompressions, so no timed run comes right after a run on the same page. The warm decompression
finds its data in the cache by reading it and writing the output page, not by decoding it once before.
The 16 pages keep their compressed data in 1 MiB per codec. Quick benchmark, p50 / p99 in ns, two runs
each within 10 ns:

| codec | Σ zsmalloc cost | before, cold | now, cold | before, warm p99 | now, warm p99 |
| --- | --- | --- | --- | --- | --- |
| `lz4-prefetch` | 34.5% | 1020 / 2690 | 1080 / 2620 | 2380 | 2410 |
| `lz4` | 34.5% | | 1010 / 2560 | | 2380 |
| `lzo-rle` | 32.4% | | 1110 / 2500 | | 2470 |
| `bytelz` | 29.7% | 1190 / 2770 | 1230 / 2640 | 2390 | 2480 |
| `seqlz-fast` | 27.0% | 1270 / 3060 | 1470 / 3040 | 2810 | 2890 |

`seqlz-fast`'s p50 moves the most, 200 ns: 460 ns behind `lz4` instead of 210, close to the 370 ns of
the kernel. p99 hardly moves, the slowest pages were not the ones the predictor could learn. So the
quick benchmark's cold p50 can be trusted more than before, and it was too kind to `seqlz-fast` by
about half its gap to `lz4`.

Tried and dropped, measured with it: **the branch on short offsets decided early.** `off >= 8` depends
on the raw bits, so a misprediction there is found late. With class 1 for offsets 1 to 7 (3 raw bits)
instead of 1 to 15, the class and the last offset say whether an offset is short before the raw bits
are read. Tables trained again: 27.1% instead of 27.0%, cold p50 / p99 1410 / 3280 against 1470 /
3040 ns, 7625 decode cycles against 6883, 2800 more instructions per page and no fewer
mispredictions (95).

**A refill only when the bits might run out.** The decoder refilled its 64-bit buffer once per
sequence, and the next token's lookup waited for the refill's load, which with cold data is a miss.
A sequence on the fast path needs at most 11 + 12 bits, a refill leaves 56: now it refills only below
23 bits, and the escape and the literal length value refill before they read. Same format. The four
measurements do not agree:

| | before | refill below 23 bits |
| --- | --- | --- |
| loop over 2000 pages, cycles / mispredictions | 6883 / 91 | 7115 / 130 |
| quick benchmark, cold p50 / p99 | 1470 / 3040 ns | 1370 / 2970 ns |
| `--decode-loop 11 --flush`, p50 / p99 | 1840 / 3160 ns | 1895 / 3140 ns |
| kernel, cold, other page first | 2811 / 4501 ns | 2610 / 4300 ns |
| kernel, warm | 2290 / 4049 ns | 2129 / 3790 ns |

The branch on the bit count mispredicts, 39 more per page, but the load no longer holds up every
lookup. The kernel decides: 7% less at p50, cold and warm, and with cold data `seqlz-fast` is now 4%
behind `lz4` at p50 (2510 ns in the same boot) and 15% ahead at p99 (5089 ns). Kept.

**All candidates with the refill change**, one boot, 7 devices, other page first, `seqlz`'s and
`bytelz`'s backends with their own prefetch, p50 / p99 in ns:

| algorithm | used by zsmalloc | vs `lzo-rle` | read, cold | read, warm | write |
| --- | --- | --- | --- | --- | --- |
| `lz4` | 29 007 872 | +6.6% | 2510 / 4680 | 1979 / 3530 | 5470 / 9240 |
| `lzo-rle` | 27 222 016 | | 2690 / 5479 | 2060 / 3620 | 5280 / 9750 |
| `bytelz` | 25 014 272 | -8.1% | 2670 / 4190 | 2080 / 3610 | 6440 / 10 551 |
| `seqlz-fast` | 22 953 984 | -15.7% | 2620 / 4340 | 2150 / 3840 | 6360 / 10 690 |
| `seqlz-fast-lit` | 21 848 064 | -19.7% | 2760 / 5301 | 2370 / 4519 | 6850 / 12 720 |
| `seqlz-hc-lit` | 20 873 216 | -23.3% | 2840 / 5511 | 2310 / 4590 | 20 070 / 34 160 |
| `zstd` | 20 246 528 | -25.6% | 5500 / 9700 | 4650 / 7810 | 14 110 / 23 981 |

`seqlz-fast` now reads cold data as fast as `bytelz` at p50 and 150 ns slower at p99, for 8% less
memory: `bytelz` is no longer on the Pareto front. Against `lz4` it is 4% slower at p50 and 7% faster
at p99 with cold data, 9% slower with warm data, and writes 1.16 times as long at p99.
`seqlz-fast-lit` writes 1.38 times as long, which fails C3; it is a candidate for recompression, next
to `seqlz-hc-lit`.

**6 literals per refill instead of 5** in the coded literals: at most 9 bits each, 54 of the 56 bits a
refill leaves. Decode cycles 9258 to 8929 per page, mispredictions 142 to 130, quick benchmark cold
p50 / p99 1490 / 3880 to 1480 / 3740 ns, warm p99 3660 to 3530 ns, both runs alike. The check that no
stream was read too far past its end allows 54 bits now instead of 44; past the end the decoder reads
zeros, which decode as the shortest code, so for tables the trainer writes either bound holds.

Tried and dropped: **the bits to drop in the token's table entry.** Only code length plus raw offset
bits is on the chain from one token's lookup to the next; the raw bits themselves are needed for the
offset only. With `u32` entries (8 KiB) that hold the sum in bits 16 to 20: 7296 cycles instead of
7088, 1760 more instructions per page, quick benchmark cold 1290 / 3090 against 1190 / 2940 ns.

Tried and dropped: **the offsets as bytes in a stream of their own**, now built, with the refill
change in mind: without raw bits a token needs at most 11 bits, so the bit buffer lasts 4 or 5
sequences. Layout: `u16` literal count, `u16` offset bytes, literals, the offsets read downwards,
the bitstream; while encoding, the offsets grow down towards the literals, which they never reach,
because each offset of at most 2 bytes has a match of at least 4 bytes. 28.1% instead of 27.0%.
Mispredictions 130 to 114 per page, but 1500 more instructions, and quick benchmark cold 1220 / 3010
against 1190 / 2940 ns. In the kernel, other page first, cold 2659 / 4370 against 2610 / 4300 ns,
warm 2150 / 3890 against 2129 / 3790 ns, 3.5% more memory. The refill change took most of what the
raw bits cost.

**Where `seqlz-fast`'s decoder still spends its instructions**: 24 800 per page against `lz4`'s 13 750
in the loop above, at about the same mispredictions (90 against 97). Of the about 95 instructions of a
sequence on the fast path, the raw offset bits take about 15 (class to bit count, mask, two shifts, the
choice of the last offset), the checks of the fast path about 20, the refill about 12, six values live
on the stack. Priced, not built: the offsets as whole bytes in a stream of their own, read with a load
instead of taken from the bit buffer. With the raw bits widened to 0 / 8 / 8 / 16 as an estimate:
28.1% instead of 27.0%. That is between `seqlz-fast` and `bytelz` (29.7%), and would pay if it decoded
at `bytelz`'s speed; but it needs one more pointer in a loop that already has none to spare, which is
what sank `bytelz`'s stream of extensions.

Tried and dropped: **two literals per lookup** for `seqlz-fast-lit`, as `zstd`'s Huffman decoder does
(HUF_decompress4X2): a table of 2048 `u32` entries with one or two literals and their bits, the
streams each a quarter of the literals in a row instead of literal k in stream k % 4, and the last
literals of each stream one at a time. Same size, 25.5%. In the loop over 2000 pages with the data in
the cache 12% faster (8967 to 7865 cycles), but with the data cold slower: quick benchmark cold p50 /
p99 1600 / 4210 against 1560 / 3950 ns, `--decode-loop 11 --flush` on 20 000 pages 2120 / 4790
against 2080 / 4540 ns. With a table of 1024 entries (4 KiB) 8620 cycles and no better cold. The
loop exits now depend on the literals, and the short pages decode their literals mostly one stream
after the other.

## seqlz-fast-lit per page: its own literal table, where it gets the page into a smaller class

*Since #44 only the own literal table: the token tables are gone and a page tries its own table only
where a sample says it can pay.* Both together wrote 43% slower; that is too much for 2.6% and 6.6% of
memory. Without the token tables and with the check below, in the kernel VM, one boot per dump, against
the same code without own tables:

| | bytes per page | write | cold read | us per page written |
| --- | --- | --- | --- | --- |
| `lzo-rle` | 1361.1 / 1678.5 | 5.13 / 5.69 | 2.74 / 2.86 | 6.06 / 6.66 |
| `seqlz-fast-lit`, fixed tables only | 1035.3 / 1331.8 | 6.82 / 7.54 | 2.88 / 3.06 | 7.80 / 8.58 |
| `seqlz-fast-lit`, own tables, kept | 1032.6 / 1271.2 | 7.70 / 8.65 | 2.84 / 3.13 | 8.66 / 9.72 |
| `zstd` 3 | 1012.3 / 1197.5 | 13.45 / 14.27 | 5.27 / 5.58 | 15.24 / 16.17 |

The writes are 13% and 15% slower, the pages 2.7 and 60.6 bytes smaller, 0.3% and 4.6%: 53 bytes per us
on the second dump, 3 on the first. The own table is for pages like the second dump's.

**The check.** Every page with 256 literals and more built the whole table before, and most did not use
it. Now bank 0 of the histogram, every 4th literal, comes first, and Shannon's lengths of that sample
with 64 bytes for the lengths, about what they take, give an estimate of the page with its own table.
Only if that is in a smaller class of 16 bytes, zsmalloc's step at these sizes, than the page with the
fixed table, the rest of the histogram and the table follow; the final choice is the same rule with the
exact sizes. Compress cycles per page in the loop over 2000 pages, second dump / first dump, and
what the model loses against no check:

| | cycles more than without own tables | bytes per page more |
| --- | --- | --- |
| no check | 7500 / 6700 | |
| saves 16 bytes with 40 bytes for the lengths | 6500 / 5300 | 2.2 / 2.0 |
| ... with 64 bytes | 4800 / 3800 | 3.7 / 3.8 |
| ... with 96 bytes | 4200 / 3000 | 5.4 / 6.5 |
| smaller class with 64 bytes, kept | 5500 / 3800 | 3.2 / 3.2 |
| ... with 80 bytes | 4400 / 3600 | 4.1 / 4.7 |

The token tables would need a stream of their own to be chosen without a second pass, which changes the
format and gives the decoder a second bit reader; not tried.

**Before #44**, in #43, `seqlz-fast-lit` gave a page its own literal table where that saves 16 bytes, and coded its tokens with
the one of 4 token tables that codes them in the fewest bits. Both were rejected before for the old
bars: the own table for its cold read p99 (#37), the token tables because the encoder has to know all
tokens first, which cost the p99 of the writes. By the score of `PLAN.md` §1.1 both pay. In the kernel
the page gets 2.6% and 6.6% smaller, 26.7 and 87.4 bytes, for 3.0 and 3.4 us more per page written;
on the first dump that is less memory than `zstd` 3.

Kernel VM, 20 000 pages per dump, one boot per dump, means over the pages, first dump / second dump:

| | bytes per page | write | cold read | us per page written |
| --- | --- | --- | --- | --- |
| `lzo-rle` | 1361.1 / 1678.5 | 5.17 / 5.79 | 2.80 / 2.84 | 6.12 / 6.75 |
| `seqlz-fast-lit` before | 1035.3 / 1331.8 | 6.89 / 7.65 | 2.87 / 3.03 | 7.86 / 8.68 |
| own literal tables | 1031.8 / 1274.5 | 8.08 / 8.99 | 2.89 / 3.14 | 9.06 / 10.06 |
| 4 token tables | 1027.5 / 1299.3 | 8.61 / 9.62 | 2.77 / 2.93 | 9.55 / 10.62 |
| both, #43, not kept since #44 | 1008.6 / 1244.4 | 9.87 / 11.02 | 2.85 / 3.09 | 10.84 / 12.08 |
| `zstd` 3 | 1012.3 / 1197.5 | 13.64 / 14.57 | 5.34 / 5.65 | 15.45 / 16.49 |

Over both dumps: the own tables 30.4 bytes for 1.29 us, 24 bytes per us; the token tables on top of them
26.7 bytes for 1.9 us, 14; from there to `zstd` 3 21.6 bytes for 4.5 us, 4.8. On the first dump the own
tables alone save 3.5 bytes in the kernel against 14.3 in the model: most savings stay within their
zsmalloc class, both together push more pages into a smaller one, 26.7 against 3.5 + 7.8. The cold
reads stay as they were. Per-CPU memory does not grow: the encoder's work for the own table, and before
that the sequences for the choice of the token table, live in the decoder's scratch, which zram's stream
uses only for decompression; `seqlz-fast-lit`'s context stays 15 664 bytes, below C5's 16 416.

**The own table** is the format of #37: the lengths of all 256 bytes behind the header, coded by the
length the page's fixed table gives the byte, in 4 streams. The encoder with #37's heap Huffman cost
37 000 cycles more per page. Now: a histogram in 4 banks; Shannon's lengths, ceil(log2(n / count)) from
`clz` and one compare; then as many codes of each length made longer or shorter as the code needs to be
complete, decided per length and applied in one pass (10 000 cycles when made byte by byte); the
lengths per stream summed in 8 registers; the encoder's half of the table only. 6200 to 6600 cycles
more per page, and 3 and 5.5 bytes per page more than with Huffman's lengths. Pages below 256
literals do not try.

**The token tables**: bits 13 and 14 of the page's first u16 name the table, on 4 KiB pages where the
literal count needs 13 bits (16 KiB pages keep one table). Table 0 is the table of all pages, which
`seqlz-fast` and `seqlz-hc` use; tables 1 to 3 are `explore/seqlz_token_sets.c`, from `quetschn-seqlz-train
--codec seqlz --token-sets`: k-means over the pages' tokens with table 0 fixed, 8 rounds, 6 seconds. The
compressor first collects the matcher's sequences into the scratch, prices them with all 4 tables and
then encodes with the cheapest: 7900 to 9100 cycles more per page, 5700 to 6400 of them for the two
passes, because in one pass the encoding runs while the matcher waits for its loads. Estimated with
ideal code lengths: 13 and 24 bytes per page with 4 tables, 18 and 29 with 8; measured in the model 14.7
and 25.7. 8 tables would be 32 KiB of decode tables, not tried.

**Found on the way**: the compressor codes the literals only if the page with raw literals fits into a
page. A page that the matcher cannot shrink but whose literals have few different bytes stays raw; in
the test of the own tables that was 28 of 100 pages of 24 different bytes with few copies.

Tests: pages of 24 bytes as likely as each other get their own table and are under 5 bits per byte,
pages drawn as a fixed table codes them do not; pages from random sequences are never larger than with
token table 0 only and not all take table 0; the fuzz test decodes damaged pages of both kinds.
Mutations, each caught: never an own table, the most expensive token table, the decoder always with
token table 0.

## seqlz-fast-lit by the score: no budget, offsets in steps of 8

By the score of `PLAN.md` §1.1, `seqlz-fast-lit` now codes the literals of every page where that pays,
and has two more offset classes for offsets that are multiples of 8. In the kernel that is 4.1% and
4.8% less memory than before, 44.6 and 66.8 bytes per page, for 0.19 and 0.41 us more per page
written, 235 and 163 bytes per us. `zstd` 3 is still 3.1 and 17.2 bytes per us further. Four ideas
were measured, two are kept. Where the dumps disagree, the decision is over both together: the mean
bytes and time of the two.

Kernel VM, 20 000 pages per dump, one CPU at a fixed 4.5 GHz, means over the pages, first dump /
second dump, times in us; the first three rows are from one boot per dump, the others from two more,
where `lzo-rle` came out within 0.03 us of the first:

| | bytes per page | write | cold read | us per page written |
| --- | --- | --- | --- | --- |
| `lzo-rle` | 1361.1 / 1678.5 | 5.10 / 5.69 | 2.76 / 2.88 | 6.04 / 6.66 |
| `zstd` 3 | 1012.3 / 1197.5 | 13.39 / 14.39 | 5.31 / 5.56 | 15.20 / 16.28 |
| `seqlz-fast-lit` before | 1079.9 / 1398.6 | 6.55 / 7.08 | 2.84 / 2.90 | 7.52 / 8.07 |
| without the budget | 1059.2 / 1333.7 | 6.60 / 7.30 | 2.72 / 2.82 | 7.53 / 8.26 |
| and offsets in steps of 8, kept | 1035.3 / 1331.8 | 6.78 / 7.50 | 2.72 / 2.87 | 7.71 / 8.48 |
| and 16 literal tables | 1033.2 / 1323.8 | 7.06 / 7.81 | 2.98 / 3.15 | 8.07 / 8.88 |

**Without the budget: 21 and 65 bytes per page, kept.** The budget of #33 kept the literals of pages
with many sequences raw, for C3's p99 of the writes. By the score it cost memory for nothing: on the
first dump the time is the same within the noise, on the second 0.19 us more for 65 bytes, 340 bytes
per us. The reads get faster, 2.72 against 2.84 and 2.82 against 2.90 us, the pages are smaller.
`SEQLZ_LIT_BUDGET` is gone.

**Offsets in steps of 8: 24 and 2 bytes per page, kept.** Memory pages are full of 8-byte aligned
data: of the offsets from 16 to 255 that are not the last one, 72% and 56% are multiples of 8, of those
from 256 on 62% and 41%. The low 3 bits of an offset carry 1.8 and 2.4 of their 3 bits. The two new
classes send such offsets divided by 8, in 5 and 9 bits instead of 8 and 12, which is what LZX's
aligned offset blocks and LZMA's align bits do with a model; here the class is in the token, so the
decoder still has one table lookup per sequence and one more shift. The token has 3072 symbols instead
of 2048, the escape needs 12 bits, and its code got shorter, 4 bits instead of 5: 7 more mispredictions
per page from escaped tokens. Estimated from the offsets alone 17 and 8 bytes per page, measured with
the zsmalloc model 19.0 and 7.4 (the tables retrained, which alone changes nothing: the trainer gives
exactly today's tables for today's format), in the kernel 24 and 1.9. In loops over 2000 pages the
compressor needs 450 to 650 cycles more per page, the decoder 330 to 440: 57 and 110 bytes per us in
userspace. On the second dump in the kernel it is little, 1.9 bytes for 0.22 us, 8.6 bytes per us,
below the step to `zstd` 3 there (17). Over both dumps: 13 bytes for 0.20 us, 65 per us, against 10
for the step from it to `zstd` 3.

**Contexts for the literals, as Brotli's context modeling: 5 to 18 bytes per page, dropped.** Priced
with ideal code lengths, tables per fixed table and context trained on the resident pages, measured on
the dumps, on top of the 8 tables per page. 4 classes of context (0, below 32, below 128, the rest):
the byte before in the page 17.6 and 11.9 bytes per page, the literal before 12.6 and 9.4, the literal
8 before 8.7 and 9.5. Only the last fits the decoder, which decodes the literals in 8 streams before
the sequences and so knows neither the byte before in the page nor the literal before in another
stream. It needs 4 times the decode tables, 64 KiB, and a table choice in each stream's chain. With 2
classes, zero or not: 10.7 and 9.8, 5.0 and 5.5, 4.8 and 6.1. 16 tables get as much without changing
the decoder, see below.

**16 literal tables instead of 8: 2 and 8 bytes per page, dropped, at the edge.** In the model 7.3 and
8.5 bytes, in loops 1300 to 1640 compress cycles more; in the kernel twice the decode tables make the
cold reads 0.26 and 0.28 us slower: 5.6 and 20 bytes per us, over both dumps 13 against 10 for the step
from it to `zstd` 3. On the hull only for lambda from 10 to 13, within the noise of one boot, for twice
the decode tables in the cache of every CPU that reads.

**The 2 newest positions per hash, the longer match wins: 11 bytes per page, dropped.** The same 8 KiB
table as 2 x 2048 entries, so C5 does not change. In the model 10.9 and 11.1 bytes per page, 4400 to
4700 compress cycles more, 1 us, the decoder 80 to 140 cycles faster: about 11 bytes per us, better than
the step to `zstd` 3 on the first dump (3.1), worse on the second (17.2), at the edge over both (10).
Not measured in the kernel.

**The interleaved harness times compression by position.** `quetschn-bench-interleaved` measured
`seqlz-fast-lit` without the budget 1.1 us faster than with it, which is impossible, it does more.
The codec first in `--codecs` compresses up to 1.5 us slower than it does later in the list, also with
the input flushed before each timed compression, so it is not the input in the cache but where each
codec's buffers are. The userspace table in "The designs by the score" has this error in its
compression times, and it explains `seqlz-fast-lit` compressing faster than `seqlz-fast` there. The
decisions here use `perf stat` over loops of one codec per process, and the kernel VM.

Tests: a page of 512 records of 4 literals and 4 equal bytes, one sequence each, which the budget kept
raw, now has its literals coded and is byte for byte what `seqlz_encode_coded()` writes for the same
sequences; the classes of offsets 8, 16, 248, 256, 4088; the reference encoder of the format test
writes the new classes. Mutations, each caught: raw literals for pages of more than 1500 literals (the
new test), the encoder without the new classes (the format test and the parser's bound on words), the
decoder without the shift (every roundtrip).

## seqlz-fast-lit within C3: a budget for coding the literals

*Gone since the score: see [seqlz-fast-lit by the score](#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8).*

With a budget for the work per page, `seqlz-fast-lit` writes 1.16 and 1.17 times as long as `lz4` at
p99, within C3, and needs 16.7% and 20.7% less memory than `lzo-rle` on the two dumps. Without the
budget it needed 20.5% and 22.2% less, but wrote 1.29 and 1.31 times as long. `seqlz-fast` needs
11.4% and 15.7% less.

**The idea.** The p99 of the writes is a few pages, those with many sequences or many literals: they
take long to match, and then long to code. The encoder knows both counts when the matcher is done. If
14 * sequences + literals is more than `SEQLZ_LIT_BUDGET`, 5300, the literals stay raw. The weights
come from a fit of the compress time per page in the quick benchmark: 14.8 ns per sequence, 1.05 ns
per literal. `seqlz_encode_coded()`, which `seqlz-hc-lit` uses for recompression, has no budget.

**Simulated first**, with the per-page times of the quick benchmark on the second dump, write time
plus 2800 ns of zram as an estimate of the kernel's, p99 against `lz4`'s: `seqlz-fast` 35.70% and
1.182, `seqlz-fast-lit` 31.89% and 1.216. With the measured compress time of the coded page as the
rule, the best any predictor can do: at 8000 ns 32.16% and 1.187. With the matcher's own time plus
0.43 ns per literal: at 7000 ns 33.09% and 1.190. With the counts, fitted on half the pages and judged
on the other half: 33.57% and 1.190. With the literals and the bytes of the sequences' bitstream
instead of the sequences, which `code_literals()` knows without a counter: 34.36% and 1.190, worse.
The counts need no clock and give the same output on every machine.

**The budget in the kernel**, 20 000 pages per dump, p99 of the writes against `lz4`'s in the same boot:

| budget | first dump: used by zsmalloc, write p99 | second dump |
| --- | --- | --- |
| none | 21 184 512, 1.31 | 26 673 152, 1.29 |
| 6500 | 21 274 624, 1.28 | 26 755 072, 1.26 |
| 5800 | 21 487 616, 1.22 | 27 316 224, 1.23 |
| 5300 | 21 598 208, 1.17 | 27 971 584, 1.15 |
| `seqlz-fast` | 22 953 984, 1.12 | 29 757 440, 1.11 |

All candidates with the budget of 5300, one boot per dump, other page first, p50 / p99 in ns:

| second dump | used by zsmalloc | vs `lzo-rle` | read, cold | read, warm | write |
| --- | --- | --- | --- | --- | --- |
| `lz4` | 35 090 432 | +4.5% | 2510 / 4800 | 1920 / 3569 | 5991 / 9510 |
| `lzo-rle` | 33 570 816 | | 2680 / 5580 | 2049 / 3591 | 5859 / 9790 |
| `seqlz-fast` | 29 757 440 | -11.4% | 2659 / 4371 | 2230 / 3791 | 6899 / 10 501 |
| `seqlz-fast-lit` | 27 971 584 | -16.7% | 2800 / 4330 | 2400 / 3729 | 7580 / 11 000 |
| `zstd` | 23 949 312 | -28.7% | 5741 / 9591 | 4930 / 7491 | 14 730 / 23 931 |

| first dump | used by zsmalloc | vs `lzo-rle` | read, cold | read, warm | write |
| --- | --- | --- | --- | --- | --- |
| `lz4` | 29 007 872 | +6.6% | 2480 / 4649 | 1951 / 3560 | 5450 / 9240 |
| `lzo-rle` | 27 222 016 | | 2660 / 5261 | 2050 / 3580 | 5180 / 9560 |
| `seqlz-fast` | 22 953 984 | -15.7% | 2580 / 4331 | 2171 / 3891 | 6229 / 10 280 |
| `seqlz-fast-lit` | 21 598 208 | -20.7% | 2659 / 4310 | 2310 / 3870 | 6820 / 10 819 |
| `zstd` | 20 246 528 | -25.6% | 5471 / 9570 | 4630 / 7689 | 13 771 / 23 240 |

The reads get a bit faster at p99 too, the pages with the most literals to decode are the ones that
stay raw: cold p99 4330 and 4310 ns against 4551 and 4390 without the budget, and 7% to 10% below
`lz4`'s. At p50 `seqlz-fast-lit` reads 7% and 12% slower than `lz4`. The budget keeps 58% and 77% of
the memory that coding all pages saves. It was chosen with some room to C3's 1.2, because the writes
of the same code differ by about 100 ns at p99 from boot to boot.

Tests: a page with the same skewed literals once in a long run with a few matches and once as 512
records of 4 literals and 4 bytes that repeat, one sequence each, 14 * 512 + 2048 over the budget:
the first is coded, the second not, and `seqlz_encode_coded()` without a budget codes the second.
Mutation, caught: a weight of 1 instead of 14 per sequence.
## Recompression, measured, not pursued

*`seqlz-opt` is removed. The project focuses on `seqlz-fast-lit` (#42).* With zram's recompression the
pages are written with `seqlz-fast-lit`, and idle pages are compressed again with a second algorithm
later. By the score of `PLAN.md` §1.1 recompression with `seqlz-opt` takes 237 us per page for 59 and
106 bytes: 0.3 and 0.4 bytes per us, where the step from `seqlz-fast-lit` to `zstd` 3 for every write
is 3 and 17. At 237 us it would have to save 700 to 4000 bytes per page to compete, more than the
page. A better ratio does not fix that, only a much cheaper recompression.

Kernel VM, 20 000 pages per dump, one boot per dump, written with `seqlz-fast-lit`, all pages
recompressed as idle, compacted, then read, means over the pages, first dump / second dump:

| | bytes per page | recompression | cold read |
| --- | --- | --- | --- |
| `seqlz-fast-lit` | 1035.3 / 1331.8 | | 2.90 / 3.13 us |
| recompressed with `seqlz-opt` | 975.9 / 1225.7 | 237 / 237 us | 2.89 / 3.12 us |
| recompressed with `seqlz-hc-lit` | 1001.1 / 1254.2 | 20 / 23 us | 2.86 / 3.09 us |
| recompressed with `zstd` 3 | 983.9 / 1184.2 | 14 / 16 us | 3.94 / 4.68 us |
| `zstd` 3 for every write | 1012.3 / 1197.5 | | 5.18 / 5.36 us |

`zstd` recompresses 16 times as fast as `seqlz-opt`, and gets less memory than `zstd` for every write:
zram keeps the recompressed page only if it is smaller, so each page gets the smaller of the two. With
the time of recompression at full weight it is the next step after `seqlz-fast-lit` on the first dump,
3.5 bytes per us, on the second writing with `zstd` is better (17); at a tenth of the weight it is 30
and 72 bytes per us. But each read of a recompressed page is about 2.3 us slower, twice as long, and
recompressed pages are the idle ones: they come back when a program that was not used for a while is
used again, all at once, while someone waits. The score counts every read the same and cannot see
that. `seqlz-hc-lit` keeps the reads at `seqlz`'s speed, but saves less than `zstd` on both dumps for
more time. In loops over 2000 pages the recompressors need, compared with `seqlz-fast-lit`:

| | saved per page | compress cycles | bytes per us |
| --- | --- | --- | --- |
| `seqlz-hc-lit` 3 | 37 / 73 | 71 000 / 89 000 | 2.4 / 3.7 |
| `seqlz-hc-lit` 9 | 41 / 77 | 179 000 / 157 000 | 1.0 / 2.2 |
| `seqlz-opt` | 67 / 102 | 1 074 000 / 1 058 000 | 0.3 / 0.4 |
| `zstd` 3 | 33 / 136 | 54 000 / 58 000 | 2.7 / 10.5 |
| `zstd` 9 | 66 / 179 | 302 000 / 323 000 | 1.0 / 2.5 |
| `zstd` 19 | 107 / 230 | 3 260 000 / 3 158 000 | 0.15 / 0.3 |

A much better ratio for idle pages would come from outside the codec: neighbouring pages together, 8%
to 9% with `zstd` in blocks of 16 KiB, or deltas against a similar page, 2.3 to 3.9 points (see the
ideas of #29 and #31). Both are zram's work.

## seqlz-opt: a parser that knows seqlz's costs, zstd's memory at lz4's read speed

*Removed in #42, see [Recompression, measured, not pursued](#recompression-measured-not-pursued).*

`seqlz-opt` writes the same format as `seqlz-fast-lit`, with the same decoder, but finds the sequences
with a parser that prices every choice by the code lengths of the tables. In the kernel it needs 2.6%
less memory than `zstd` 3 on the first dump and 2.5% more on the second, and reads cold data twice as
fast as `zstd`, at `lz4`'s speed: p50 / p99 2720 / 4670 and 2910 / 4900 ns, `lz4` 2491 / 4730 and 2510 /
4781, `zstd` 5440 / 9560 and 5730 / 9480. It needs about 0.25 ms per page to compress, 0.47 and 0.54 ms
at p99: not for zram's writes, for recompression of idle pages.

**Why.** Codecs we had not looked at, 2000 pages per dump, one binary, `-O2` for x86-64 without
SIMD, ticks per page in a loop with the data in the cache, the old dump / the new one:

| codec | memory | compress p50 | decompress p50 / p99 |
| --- | --- | --- | --- |
| `lz4` | 35.02% / 41.61% | 11 025 | 2970 / 10 215 |
| `seqlz-fast-lit` | 25.87% / 32.78% | 17 820 | 5130 / 10 305 |
| `zstd` 3 | 23.81% / 27.75% | 39 735 | 13 860 / 22 635 |
| LZSA2, matches from 2 bytes | 24.34% / 29.52% | 158 million | 6255 / 24 795 |
| LZSA1 | 27.45% / 33.21% | 4 million | 3915 / 24 705 |
| lzfse | 26.48% / 30.38% | 171 540 | 24 570 / 36 585 |
| lzav, lzav hi | 35.00% / 41.37%, 32.58% / 39.42% | 12 735, 50 175 | 3060, 2745 |
| Lizard 10 to 47 | 32.0% to 44.8% / 34.4% to 47.8% | up to 10 million | 2000 to 3500 |

LZSA2 has no entropy coding, only nibbles, a repeat offset and matches from 2 bytes, and is still
smaller than `seqlz-fast-lit`: its compressor parses optimally, with a suffix array. LZSA1, a byte
format like `lz4`'s, gets `seqlz-fast-lit`'s size the same way. The parse is worth more here than the
coding. Nothing in the table decodes as fast as `lz4` for less memory than `seqlz-fast-lit`.
[lzav](https://github.com/avaneev/lzav), [Lizard](https://github.com/inikep/lizard),
[LZSA](https://github.com/emmanuel-marty/lzsa), [lzfse](https://github.com/lzfse/lzfse).

**The parser** (`seqlz_parse_opt()`): forward over the page, at each position the literal, priced by
the literal table that codes the whole page in the fewest bits, and the matches from a hash chain of
4 bytes, 16 steps, and the last offset. Each length from 4 to the token's cap of 35, above that only
the longest, priced with the token for the literals since the last match, the raw offset bits and
the length values. One state per position, the cheapest, with its literal count and last offset. The
token tables are trained on its own parses of the resident pages (`--codec opt`,
`seqlz_default_opt`); the sequences go through `seqlz_encode_coded()`. A match of 256 bytes and more is
taken as it is, the positions inside it only go into the chains: without that a run cost every
position a count of the run per candidate, and the p99 of the writes was 7.7 and 9 ms in the kernel.

2000 pages per dump, userspace, ticks per page:

| variant | first dump | second dump | ticks p50 / p99 |
| --- | --- | --- | --- |
| `seqlz-fast-lit` | 25.13% | 31.20% | |
| `lz4hc` level 3's matches (`seqlz-hc-lit`) | 23.9% | 30.2% | |
| parser, `seqlz-fast`'s tables, chain 64 | 23.39% | 28.79% | 14.6 / 9.3 million mean |
| `lz4hc`'s tables | 23.36% | 28.52% | |
| its own tables | 23.22% | 28.50% | |
| its own tables, chain 16 | 23.28% | 28.57% | 4.8 / 3.4 million mean |
| chain 4 | 23.43% | 28.78% | 2.3 / 1.7 million mean |
| chain 16, matches of 256 taken as they are | 23.28% | 28.57% | 1.1 to 1.3 million / 2.9 to 3.1 million |
| the same, 64 | 23.34% | 28.61% | 0.8 / 1.8 million |
| 256, prices from a table per parse | 23.28% | 28.57% | 0.94 to 1.05 million / 2.0 to 2.1 million |
| `zstd` 3 | 23.81% | 27.75% | |

A second pass with the literal table of the first gains 0.01 points. On the second halves of the dumps,
with literal tables trained on the first halves (the device's own tables, see the device tables below):
`zstd` 3 31.74% and 24.18%, `seqlz-opt` with the resident tables 33.17% and 23.66%, with the device's
own 32.06% and 23.42%.

Kernel VM, 20 000 pages per dump, one boot per dump, p50 / p99 in ns:

| | used by zsmalloc | read, cold | read, warm | write |
| --- | --- | --- | --- | --- |
| second dump: `lz4` | 35 090 432 | 2510 / 4781 | 1920 / 3550 | 6221 / 9901 |
| `seqlz-fast-lit` | 26 673 152 | 3020 / 4890 | 2490 / 4080 | 8000 / 12 700 |
| `seqlz-opt` | 24 539 136 | 2910 / 4900 | 2410 / 3960 | 238 259 / 536 538 |
| `zstd` 3 | 23 949 312 | 5730 / 9480 | 4910 / 7470 | 16 170 / 26 180 |
| first dump: `lz4` | 29 007 872 | 2491 / 4730 | 1940 / 3530 | 5710 / 9580 |
| `seqlz-fast-lit` | 21 184 512 | 2840 / 4690 | 2310 / 3989 | 7200 / 12 540 |
| `seqlz-opt` | 19 714 048 | 2720 / 4670 | 2240 / 4050 | 264 419 / 468 008 |
| `zstd` 3 | 20 246 528 | 5440 / 9560 | 4600 / 7661 | 15 520 / 26 510 |

The parser's pages read a bit faster than `seqlz-fast-lit`'s, fewer and longer sequences. In the
kernel the cut at 256 costs 0.13 points on the first dump (19 607 552 without it), in userspace
nothing. The work memory is 110 KB per stream, the price table in it and not on the kernel's stack.

**With zram's recompression**, the way it would be used: the pages written with `seqlz-fast-lit`,
then all marked idle and recompressed with `seqlz-opt` as the secondary algorithm (`recomp_algorithm`,
`idle`, `recompress`, then `compact`, `run.sh` enables `ZRAM_MULTI_COMP`), then the reads. The writes
are `seqlz-fast-lit`'s, the recompression takes 233 and 235 us per page, about 17 MB/s on one CPU:

| | before | after recompression | `zstd` 3 | read after, cold | `lz4` |
| --- | --- | --- | --- | --- | --- |
| second dump | 26 673 152 | 24 498 176 | 23 949 312 | 2920 / 4980 | 2491 / 4710 |
| first dump | 21 184 512 | 19 742 720 | 20 246 528 | 2720 / 4730 | 2509 / 4770 |

Without `compact` zsmalloc keeps the holes of the old objects: in a first try with 200 pages the memory
went up after recompression.

Tests: the parser's pages from four kinds of page come back; on pages of words from a vocabulary of
60, where the first match found is often not the cheapest, they are 8.5% smaller than the greedy
matcher's with the same tables, the bound is 7%. Mutations, each caught: only the longest length of
each match (5.4%), a chain of 1 (5.6%).

## seqlz-opt with a literal table per page: less memory than `zstd`, slower reads, not kept

Built on the branch `feat/page-tables` (#37), not merged: `seqlz-opt` codes the literals of a page with
a table of its own when that saves at least 16 bytes, otherwise with one of the 8 fixed tables as
before. After zram's recompression that is 1.4% and 4.1% less memory than before, and 3.8% and 1.8%
less than `zstd` 3. The price is the reads: 17% and 34% of the pages get their own table, the mean
decode goes up by 10% and 13%, most of it for building the table. In the kernel p99 of a cold read goes
from 4809 to 5849 ns and from 5180 to 6020 ns, `lz4` has 4870 and 5080, `zstd` 9360 and 9630. Only
`seqlz-opt` writes such pages: `seqlz-fast-lit` decodes in 8193 and 7752 cycles per page, 8203 and 7740
before.

Not kept because the gain is small for what it costs: 1.4% and 4.1%, only on the pages recompression
reaches, for cold reads above `lz4`'s at p99, about 300 more lines in `seqlz.c`, a Huffman header the
decoder parses from untrusted input, and 3.3 KB more scratch per CPU on every seqlz device. The
recompressed pages are idle and rarely read, which speaks for it. The device's own literal tables
(see the ideas of #29 and #31) gained 1.7 points on the second dump without a slower decoder.

Kernel VM, 20 000 pages per dump, one boot per dump, written with `seqlz-fast-lit`, all pages
recompressed as idle with `seqlz-opt`, compacted, then read; p50 / p99 in ns, cold is the compressed
data flushed with another page read before:

| | used by zsmalloc | read, cold | read, warm | recompress per page |
| --- | --- | --- | --- | --- |
| first dump: `lz4` | 29 007 872 | 2510 / 4870 | 1951 / 3539 | |
| recompressed, fixed tables (before) | 19 742 720 | 2770 / 4809 | 2290 / 4100 | about 234 us |
| recompressed, own tables | 19 472 384 | 2870 / 5849 | 2339 / 4790 | 246 us |
| `zstd` 3 | 20 246 528 | 5440 / 9360 | 4600 / 7530 | |
| second dump: `lz4` | 35 090 432 | 2560 / 5080 | 1930 / 3540 | |
| recompressed, fixed tables (before) | 24 518 656 | 3020 / 5180 | 2480 / 4030 | about 234 us |
| recompressed, own tables | 23 511 040 | 3259 / 6020 | 2609 / 4710 | 243 us |
| `zstd` 3 | 23 949 312 | 5749 / 9630 | 4930 / 7430 | |

The rows "before" are from the chart run of the budget and the parser. In the model, 20 000 pages per
dump, zsmalloc cost: `seqlz-opt` 18 568 409 to 18 291 496 bytes (23.0% to 22.6%) and 23 467 675 to
22 446 498 (29.3% to 28.0%), `zstd` 3 19 038 669 (23.5%) and 22 775 569 (28.4%). With a histogram and
Huffman lengths per page, no bytes for the lengths, a first estimate had 22.73% to 22.29% and 28.75%
to 27.33%: the lengths cost about a third of the gain.

**The format.** Bit 0x40 of the table number (`SEQLZ_LIT_OWN`) says that the page has its own table.
Behind the header come the code lengths of all 256 bytes, 0 for a byte without a code, then the
streams as before. The length of byte `b` is coded with a small Huffman code chosen by the length the
page's fixed table gives `b`: a byte that is short in the fixed table is mostly short in the page's own
too. The 11 codes for the 11 possible lengths (`seqlz_lit_hdr`) are trained on the resident pages. The
lengths take 64 bytes per page on average. They are in 4 streams, byte `b`'s in stream `b % 4`, so that
the decoder has 4 chains of lookups instead of one; the sizes of the first 3 take 3 bytes. The encoder
builds the code with a heap, limits it to 10 bits and prices the page with it. The decoder builds a
canonical table from the lengths in its scratch, which grows by 3.3 KB to 7472 bytes; that makes
`seqlz-fast-lit`'s per-CPU context 15 664 bytes instead of 12 336, still below C5's 16 416.

**The decoder.** Cycles per page decoding, 2000 pages per dump in a loop, the compressed data in the
L3, one CPU at a fixed 4.5 GHz, median of 5 runs, second dump / first dump:

| variant | cycles per page | memory in the model |
| --- | --- | --- |
| before, fixed tables only | 8468 / 7702 | 29.3% / 23.0% |
| own tables, the lengths in one stream, the table filled symbol by symbol | 11 628 / 9905 | 27.9% / 22.6% |
| the table filled in canonical order, 64-bit stores for the short codes | 10 805 / 9316 | |
| the lengths in 4 streams | 10 163 / 8788 | 28.0% / 22.6% |
| the table counted and sorted in 4 banks of 64 bytes | 9810 / 8625 | 22 436 300 / 18 284 375 bytes |
| only if it saves 16 bytes | 9585 / 8505 | 22 446 498 / 18 291 496 |
| 48 bytes | 9382 / 8183 | 22 494 867 / 18 340 748 |
| 96 bytes | 9164 / 7973 | 22 575 216 / 18 415 240 |

With one stream the lengths were a chain of 256 lookups, each waiting for the length of the one
before. The banks split the other chain, in the counting sort: neighbours with the same length
incremented the same counter. zsmalloc's size classes are 16 bytes apart, so a smaller saving mostly
buys nothing; above 16 bytes every byte saved costs time. Tried and not kept: 8 banks of 32 (9501 /
8527, within the noise on the first dump and 1.1 KB on the stack of the read), filling the table length
by length so that the branches predict (9649 / 8444), refilling the short streams of the lengths from
the rest of the page and checking at the end (9502 / 8401). Measured alone on the pages with their own
table, the build takes 2917 of 14 836 ticks per page.

Tests on the branch: pages of 24 bytes as likely as each other all get their own table and come back, under 5 bits
per byte; pages drawn as a fixed table codes them never get one. Mutations, each caught: never an own
table (0 of 100 pages, 374 180 bytes against the bound of 256 000), the lengths of stream 2 read for
stream 1 (the decoder rejects the page). The fuzz test decodes the parser's pages with bytes replaced
and random headers with the bit set; with short headers it found that the decoder read the 3 sizes
before checking that the lengths have 3 bytes.

## seqlz-fast-lit: one of 8 literal tables per page

`seqlz-fast-lit` now picks one of 8 static literal tables per page, and decodes the literals in 8
streams instead of 4. In the kernel it needs 22% less memory than `lzo-rle` on the first dump and 20%
less on the second, where `seqlz-fast` needs 16% and 11% less. That is 89% and 76% of the way from
`lz4` to `zstd`. It reads cold data 6% to 8% faster than `lz4` at p99 and 6% to 13% slower at p50. It
writes 1.27 to 1.31 times as long as `lz4` at p99, and C3 allows 1.2.

**Where the gap to `zstd` is.** The second dump (24th September 2026, after a forced reclaim, 1.3 GB)
compresses worse than the first, and `seqlz-fast` got only 48% of the way from `lz4` to `zstd` on it.
Sizes on 20 000 sampled pages of each dump:

| | first dump | second dump |
| --- | --- | --- |
| `seqlz-fast` | 27.0% | 35.7% |
| `zstd -1`, literals not coded | 26.9% | 35.4% |
| `zstd-nolit` 3 | 25.8% | 33.1% |
| `zstd` 3 | 23.5% | 28.4% |
| `seqlz-hc`, `lz4hc` 3's matches | 25.6% | 33.0% |

Coding the literals is worth 2.3 points to `zstd` 3 on the first dump and 4.7 on the second, better
matches 1.1 and 2.6. The literals are 66% and 69% of `seqlz-fast`'s output. With its matcher, the
order-0 entropy of each page's literals, plus a header estimate for a table per page, would save 10.7%
and 17.6% of the output. The one static table of `seqlz-fast-lit` saved 6.1% and 5.2%.

**K tables, the cheapest per page choice.** k-means over the literal histograms: each page goes to the
table that codes its literals in the fewest bits, each table is the Huffman code of its pages'
literals. Trained on the resident pages, priced on the dumps, share of `seqlz-fast`'s output saved:

| tables, code bits | first dump | second dump |
| --- | --- | --- |
| 1, 11 | 7.3% | 6.3% |
| 4, 11 | 9.2% | 9.1% |
| 8, 10 | 10.2% | 12.5% |
| 8, 11 | 10.3% | 12.5% |
| 16, 11 | 10.6% | 13.1% |
| 64, 11 | 11.4% | 14.4% |

The tables are not tied to the pages they are trained on. Trained on either dump and priced on the
other, they save within 1 point of tables trained on the pages they are priced on: 8 tables of 10
bits trained on the first dump save 12.9% on the second, trained on the second 13.3%, trained on the
resident pages 12.5%.

**Built:** 8 tables of at most 10 bits (`explore/seqlz_lit_sets.c`, from `quetschn-seqlz-train
--corpus resident --codec seqlz --lit-sets`), the table number in one byte of the coded page's
header. The trainer runs k-means from 4 starts and keeps the one that saves the most on the training
pages. Two things in it matter, each found with a table that no page chose:

* A page whose literals no table codes in 1/16 fewer bytes stays raw and counts for no table.
  Otherwise the pages with the flattest literals get a table of their own, and it never pays.
* A table without pages starts again from the page whose literals cost most per byte.

Share of `seqlz-fast`'s output saved, on the resident pages, the first and the second dump: plain
k-means 6.91%, 8.68%, 10.10%, one table unused; with raw pages left out 6.87%, 8.41%, 9.98%, still
one unused; with both 6.97%, 8.48%, 11.29%. The last one saves the most on the training pages and is
in `explore/seqlz_lit_sets.c`. The start matters too: the prototype of the trainer found a set with
6.914 bits per literal on the resident pages against 6.889 for plain k-means, and 6.566 against 6.676
on the second dump, 610 KB of the 81.9 MB in the kernel. Training on the pages that are measured flatters: tables
trained on the resident pages save 8.49% and 11.03% of `seqlz-fast`'s output on the two dumps, with
the second dump added to the training 12.62% on it. Trained on the resident pages and the first dump,
they save 11.33% on the second, trained on the resident pages and the second 8.31% on the first. So
the tables stay trained on the resident pages only. Measured sizes: 24.6% and 31.8% with 4 streams. 16 tables 24.5% and 31.5%,
8 of 11 bits 24.6% and 31.7%, 8 of 9 bits 24.9% and 32.4%, 4 of 10 bits 24.9% and 33.1%. Coding
the literals of a page when it saves anything instead of 1/16 saves less than 0.1 points.

**The encoder prices all 8 tables at once.** Per byte, its code lengths in all 8 tables are the 8
lanes of a `u64` (2 KiB); adding one per literal gives the bits in all tables, widened to 16-bit
lanes every 25 literals of a stream, before a lane could overflow. One lane per stream, so the exact
size of each stream is known before it is written. The first version, a histogram and 8 * 256
products, cost 1400 ns on pages with few literals.

**Writing the streams is what costs.** In the kernel, pricing alone costs 30 to 40 ns per write: a
build that prices and then stores the page with raw literals wrote in 6920 / 10 529 ns, `seqlz-fast`
in 6890 / 10 490. A build that encoded the streams into a scratch buffer and threw them away wrote as
slow as the real thing. Steps, second dump, kernel, p50 / p99 in ns, `lz4` at about 5870 / 9370:

| literal encoder | write |
| --- | --- |
| histogram, 8 * 256 products, one stream after the other | 7820 / 12 880 |
| packed prices, accumulator shifted left, marker bit | 7680 / 12 619 |
| 4 streams in one loop, sizes from the prices | 7530 / 12 030 |
| bit count from the sum of the table entries, no marker | 7560 / 12 101 |
| code and length in two tables | 7690 / 12 410 |
| 8 streams, 4 in one loop, twice | 7590 / 12 030 |

One stream after the other waited on its accumulator, a shift and an or per literal, about 3.5
cycles per literal. The kernel still pays about 0.6 ns per coded literal, and the slowest pages to
write have about 2600 literals. Coding only pages with fewer literals does not help: with at most 2048
literals the saving drops from 8.8% to 7.3% of the output on the first dump and from 11.3% to 8.3% on
the second, and even a threshold of 1/4 leaves the 99th percentile of coded literals per page at about
2200.

**8 streams decode faster.** With 4 streams the literals took about 2.9 cycles each: four chains of
a shift, a table load and a shift. With 8 the containers stay in registers and the stream pointers go
to the stack, they are only needed for a refill. Decode loop, 2000 pages in the cache: 9070 to
8676 cycles (median of 5, `seqlz-fast` 7316). Kernel, cold, p50 / p99: 2940 / 4810 to 2860 / 4531 ns
(two boots, same numbers). The header grows by 8 bytes, 0.1 to 0.2 points.

**The literal decoder's place in memory matters by 250 ns.** Two kernels with byte for byte the same
pages and the same decoder source, only the encoder built differently, read in 2860 / 2870 and in
3100 / 3120 ns at p50 (two boots each). The kernel builds with `-falign-loops=1`. With
`decode_literals()` aligned to 64 bytes: 2920 ns. In userspace the difference does not show. Kept the
alignment, so that a change elsewhere in the file does not move this number.

All candidates, kernel, one boot per dump, other page first, backend prefetch, p50 / p99 in ns:

| second dump | used by zsmalloc | vs `lzo-rle` | read, cold | read, warm | write |
| --- | --- | --- | --- | --- | --- |
| `lz4` | 35 090 432 | +4.5% | 2519 / 4840 | 1920 / 3529 | 6010 / 9600 |
| `lzo-rle` | 33 570 816 | | 2700 / 5580 | 2049 / 3600 | 5849 / 9800 |
| `seqlz-fast` | 29 757 440 | -11.4% | 2640 / 4369 | 2200 / 3789 | 6940 / 10 570 |
| `seqlz-fast-lit` | 26 673 152 | -20.5% | 2840 / 4551 | 2450 / 3960 | 7670 / 12 170 |
| `zstd` | 23 949 312 | -28.7% | 5770 / 9730 | 4929 / 7560 | 14 779 / 23 940 |

| first dump | used by zsmalloc | vs `lzo-rle` | read, cold | read, warm | write |
| --- | --- | --- | --- | --- | --- |
| `lz4` | 29 007 872 | +6.6% | 2500 / 4770 | 1950 / 3510 | 5450 / 9240 |
| `lzo-rle` | 27 222 016 | | 2680 / 5520 | 2040 / 3541 | 5190 / 9609 |
| `seqlz-fast` | 22 953 984 | -15.7% | 2550 / 4300 | 2140 / 3830 | 6250 / 10 379 |
| `seqlz-fast-lit` | 21 184 512 | -22.2% | 2640 / 4390 | 2280 / 3950 | 6810 / 12 100 |
| `zstd` | 20 246 528 | -25.6% | 5460 / 9870 | 4630 / 7720 | 13 840 / 23 660 |

Against `lz4`, `seqlz-fast-lit` reads cold data 13% and 6% slower at p50, 6% and 8% faster at p99,
and writes 1.27 and 1.31 times as long at p99. Userspace sizes on the 20 000 pages: 24.8% and 31.9%,
`seqlz-fast` 27.0% and 35.7%. Quick benchmark with the tables of the prototype: cold p50 / p99 1400 /
3100 and 1640 / 3040 ns, `lz4` 1210 / 2880 and 1210 / 2780 ns.

Tests: the encoder's choice against the bits of every table computed in the test, on pages whose
literals are drawn as one table expects them and in every other page the first stream's as another
does; a decoder in the test, written from `seqlz.h`, for the header, the 8 streams, the canonical
codes and the sequences' bitstream behind them; 20 000 random or damaged coded pages for the decoder.
Mutations, each caught: the choice from the first stream's bits only, the canonical codes of one
length in the other order (encoder and decoder still agree, only the format test sees it), no check
of the table number (with the sanitizers).

Tried and dropped, all on 20 000 pages of each dump with `seqlz-fast-lit`, compress cycles per page
in the loop over 2000 pages of the second dump (`seqlz-fast` about 22 500):

* **A 4-byte hash** again: 25.0% and 32.6% instead of 24.6% and 31.8%. With coded literals a 4-byte
  match costs about as much as its 4 literals. **A 6-byte hash**: 25.1% and 32.3%.
* **A hash table of 13 bits**: 24.4% and 31.6%, collisions are not what the matcher misses.
* **A second table on 8 bytes** (`zstd`'s double fast), checked after the last offset: 24.5% and
  31.6%. Checked before the last offset: 24.8% and 31.9%.
* **A hash chain**, up to 3 older positions with the same hash, the longest match wins: 24.2% and
  31.4% for 28 400 cycles. With 1 step 24.4% and 31.5% for 26 400.
* **The chain plus one step of lazy matching**: 24.0% and 31.0%, 39 500 cycles and 2.3 times the
  mispredictions. Lazy only after matches shorter than 12 bytes: 24.6% and 31.6% for 30 600.
  `lz4hc`'s matches have 834 literal bytes per page against 976 with the same number of sequences,
  but this matcher cannot afford to find them.
* **Slower acceleration** (`>> 8`): 24.5% and 31.6%. **Faster** (`>> 5`): 24.8% and 32.1% for 3%
  fewer cycles.
* **The sequences into an array first, then encoded**, so that each loop has its registers:
  25 100 cycles instead of 22 800, more mispredictions.
* **Two literals per lookup**, priced from the chosen table and each page's histogram: 10 bits give
  a pair in 20% and 29% of the lookups, 12 bits 39% and 43%, 14% to 27% fewer lookups. Not built
  again, see the last try above.
* **Coded literals only on pages with at most 1536 literals**, in the kernel on the second dump:
  28 127 232 bytes instead of 26 791 936 (`seqlz-fast` 29 757 440), write p99 11 510 ns, 1.22 times
  `lz4`. Most of the gain is on the pages with many literals, and C3 still fails.
* **16 tables instead of 8**, priced in two words: 24.7% and 31.7% instead of 24.8% and 32.0%, 4% more
  compress cycles, and twice the decode tables in use.
* **One branch for both candidates of the matcher**, as the product of the two differences, because
  gcc turns `!(rep_hit | cand_hit)` into two branches: 23 900 cycles instead of 22 500.
* **`lz4hc`'s matches at higher levels** for `seqlz-hc-lit`: levels 3, 6, 9 and 12 all give 23.8% to
  23.9% and 30.1% to 30.2%, `zstd` 1 23.9% and 29.1%. `lz4hc`'s parse optimizes `lz4`'s costs, not
  these.
* **8 token tables per page, as for the literals**, priced from each page's token histogram: the
  tokens are 16.8% and 15.5% of the output, 8 tables would make them 15.6% and 14.1%, about 0.3 to 0.4
  points of memory. The encoder would have to know all tokens before it writes the first, and
  matching first and encoding after cost 2300 cycles per page.

**Three more ideas for the literals, priced, not built.** Per page of the resident pages and the two
dumps: `seqlz-fast`'s and `seqlz-fast-lit`'s lengths, the literal histogram, the histogram of each
literal XOR the byte at the last offset, and the literal histograms by page offset mod 8. For each
variant 8 tables are trained on the resident pages the way the trainer does it (raw pages left out,
tables without pages started again), then priced on the dumps with the zsmalloc model, the streams
estimated at 2 bytes above the whole bits. The model's baseline is 24.52% and 31.15%, the real
`seqlz-fast-lit` 24.8% and 31.9%, so only the differences count:

| variant | first dump | second dump |
| --- | --- | --- |
| today, a table per page | 24.52% | 31.15% |
| a flag per page: literals raw or XOR the byte at the last offset | 24.37% | 30.78% |
| XOR for every page | 25.03% | 31.64% |
| a table per byte lane (page offset mod 8), 3 more header bytes | 24.12% | 31.29% |
| per page: a table, or a table per lane | 24.08% | 31.23% |
| per page: a table, the XOR, or a table per lane | 23.99% | 30.81% |

* **XOR with the byte at the last offset**, as `lzma` codes the literal after a match: 0.15 and 0.37
  points, on both dumps. Built, and dropped, see below.
* **A table per byte lane**: 0.4 points on the first dump, 0.1 points worse on the second, where the
  pages have less structure in 8-byte words. The decoder would have to gather each literal from the
  lane of its page offset instead of copying 16 bytes. Dropped for now.
* **Coding the literals only when the page moves to a smaller zsmalloc class**: of the coded pages
  only 0.5% and 0.2% stay at the same cost, with 0.4% and 0.1% of the coded literals. The classes are
  too fine for this to save work. Dropped.

**The XOR with the byte at the last offset, built** (branch `feat/lit-xor`, not merged). In every run of
literals but the page's first, the first min(ll, last, 16) literals are coded XOR the byte `last`
bytes before them, `last` the offset of the match before the run; bit 7 of the table byte says so.
The encoder writes the XOR literals next to the raw ones while it matches, prices both with all 8
tables and takes the cheaper. The decoder XORs after the literal copy, with 16 bytes and two masks,
without a loop; the bytes it XORs with are before the run, so nothing waits for the run's own bytes.
Without the cap of 16 the size is the same, 0.03% of the bytes.

Kernel VM, 20 000 pages per dump, p50 / p99 in ns:

| | used by zsmalloc | read, cold | read, warm | write |
| --- | --- | --- | --- | --- |
| first dump, without the XOR | 21 184 512 | 2640 / 4390 | 2280 / 3950 | 6810 / 12 100 |
| first dump, with | 21 090 304 | 2671 / 5381 | 2310 / 4720 | 7330 / 13 631 |
| second dump, without | 26 673 152 | 2840 / 4551 | 2450 / 3960 | 7670 / 12 170 |
| second dump, with | 26 484 736 | 2981 / 5240 | 2600 / 4600 | 8250 / 13 460 |

0.1 and 0.25 points less memory, for 700 to 1000 ns more at read p99, which is then slower than
`lz4`'s (4910 and 5110 ns in the same boots), and 1.3 µs more at write p99. The same build with the
XOR never chosen reads as before (2830 / 4620 ns on the second dump), so it is the pages with the XOR:
the XOR loads the bytes just before `d`, which the match before has often just stored, and with
`last` = 8, as in arrays of words, an 8-byte load spans stores that cannot be forwarded to it. In
the loop over 2000 pages with the data in the cache it is only 3% slower (8964 against 8678 cycles).
The writes pay for the XOR literals and their prices on every page, whether the XOR wins or not: 12%
more compress cycles. Dropped.

**FSST for the literals, priced and measured, dropped.** FSST (Fast Static Symbol Table, Boncz et al.,
VLDB 2020, [cwida/fsst](https://github.com/cwida/fsst)) codes up to 255 symbols of 1 to 8 bytes as
one byte each: the encoder writes one byte per symbol without packing bits, the decoder copies 8 bytes
per code without a bit reader. That could have helped the writes and the literal decoding at the same
time. With the reference library, compiled without AVX-512, on the literals of `seqlz-fast`'s matcher,
tables trained on the resident pages, priced with the zsmalloc model:

| | first dump | second dump |
| --- | --- | --- |
| literals raw (`seqlz-fast`) | 26.74% | 35.08% |
| 8 Huffman tables (`seqlz-fast-lit`) | 24.55% | 31.33% |
| 1 FSST table, library's sample of 32 KB | 26.50% | 34.62% |
| 8 FSST tables, one per group of pages with the same Huffman table | 25.64% | 32.99% |
| the same, trained on samples of 4 MB | 25.46% | 32.97% |

FSST gets about 40% of what the Huffman tables get. With ideal static code lengths, Huffman on the
bytes needs 6.708 and 6.466 bits per literal, FSST alone 7.421 and 7.247, and Huffman on FSST's codes
6.729 and 6.506: no better than on the bytes. What is left of a page after the matcher has no
multi-byte structure worth a symbol, the gain is all in how uneven the single bytes are, and a code
of 8 bits cannot use that. It is not faster either: 7.7 to 8.0 TSC ticks per literal to encode and
2.9 to 3.1 to decode with the library, where the 8 Huffman streams decode in about 1.1 cycles per
literal. The symbols are short, about 1.1 bytes, and many bytes need the escape.

**Tunstall codes** (fixed length codes for strings of bytes, the other byte aligned choice), bounded
from the byte distribution of each group of resident pages: with codes of 12 bits 7.6 to 9.3 bits per
byte, with 16 bits (a table of 64K entries) 7.1 to 7.9, where the entropy is 6.1 to 6.7. Dropped
without building.

## 16 KiB pages

*`seqlz-fast` keeps its lead over `lzo-rle` with 16 KiB pages, `bytelz` falls below C1's 8%.* The page
size of `explore/` is now `QUETSCHN_PAGE_BITS` (12 or 14, CMake), with its own tables for 16 KiB
(`explore/seqlz_default_tables_16k.inc`): class 3 of seqlz's offsets has as many raw bits as the page,
the length values one bucket more, the escape at most 6 bits so that 31 bits per 4 bytes still fit into
two pages, the hash table 16 KiB like `lz4`'s. There is no zram dump of 16 KiB pages: the corpus is
2088 groups of four adjacent resident pages of the same mapping, 16 KiB aligned
(`resident-16k`), against the same 8352 pages as 4 KiB pages. Our tables are trained on the resident
pages, so they have seen these pages: optimistic for `seqlz`, by about a point on the zram dump.

| codec | 4 KiB | 16 KiB |
| --- | --- | --- |
| `lz4` | 39.2% | 34.6% |
| `lzo-rle` | 36.7% | 32.1% |
| `zstd` (level 3) | 27.8% | 23.5% |
| `bytelz` | 33.2% | 29.8% |
| `seqlz-fast` | 29.4% | 26.1% |
| `seqlz-hc` | 28.0% | 24.2% |
| `seqlz-hc-lit` | 26.6% | 23.2% |

Against `lzo-rle`, `seqlz-fast` needs 20% less at 4 KiB and 19% less at 16 KiB; `bytelz` 9.5% and
7.2%. Every codec gains about 4 points from the larger pages.

Timing with the harness on the 2088 pages (so p99 is about 20 pages), p50 / p99 in ns:

| codec | compress | decompress cold |
| --- | --- | --- |
| `lz4` | 10 210 / 14 540 | 5100 / 12 790 |
| `lzo-rle` | 12 090 / 20 500 | 4570 / 10 120 |
| `zstd` | 35 690 / 55 610 | 11 390 / 16 830 |
| `bytelz` | 17 890 / 29 280 | 5880 / 10 940 |
| `seqlz-fast` | 18 670 / 31 140 | 7600 / 13 130 |
| `seqlz-hc-lit` | 61 470 / 98 430 | 7550 / 13 140 |

Decoding keeps the proportions of 4 KiB pages. Compressing takes 1.8 to 2.1 times `lz4`'s time in the
harness, but in the loop (`perf stat`) 1.32 times its cycles (95 419 against 72 400 per page), `bytelz`
1.24; there `seqlz-fast` mispredicted 1608 times per page against `lz4`'s 1192, a gap it does not
have on 4 KiB pages. The branch stack showed why: for 16 KiB, gcc turned the encoder's offset class
into branches, because of the extra bits of class 3. With the raw bits from a packed constant: 1176
mispredictions, 92 362 cycles, 1.28 times `lz4`. Not looked into yet: the page, the output and the
16 KiB hash table share a 32 KiB L1 in the harness, and the matcher's step was tuned on 4 KiB pages.

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

## The ideas of #29 and #31, measured

What was still open in the lists of ideas of issues #29 and #31, each with its first check. The budget
for the literals and the parser for recompression have their own sections. Unless noted, 20 000 pages
per dump, zsmalloc cost in the model, first dump / second dump.

**The device's own literal tables: 0.3 and 1.7 points.** The dumps store the pages in the order of
their swap slots, which the kernel hands out about in the order of time. Literal tables trained on the
first half of each dump, measured on a sample of the second half, `seqlz-fast-lit`:

| tables | first dump, second half | second dump, second half |
| --- | --- | --- |
| trained on the resident pages | 25.8% | 36.1% |
| trained on the first dump's first half | 25.5% | 36.5% |
| trained on the second dump's first half | 25.7% | 34.4% |

The other dump's history helps little or not at all, the device's own a lot on the second dump, so it
is the device, not only more training data. Decoding stays as fast. It needs tables in generations,
one byte per page for the generation, and the trainer running on the device (38 s on one CPU for
56 559 pages now, a sample would do). Combined with the parser for recompression: 32.06% and 23.42%
against `zstd` 3's 31.74% and 24.18% on the second halves. Not built.

**A choice per page, after Apple's memory compressor: the word model no, the shuffle yes, but at a
price.** The smaller of `seqlz-fast-lit` and another codec per page, from the per-page lengths:

| other codec | first dump | second dump | pages where it is smaller |
| --- | --- | --- | --- |
| none | 24.81% | 31.89% | |
| word model (`spike-branchless`) | 24.81% | 31.89% | 16 / 14 |
| `lz4` | 24.80% | 31.87% | 21 / 38 |
| BΔI (`bdelta`) | 24.77% | 31.71% | 29 / 97 |
| byte shuffle, then `lz4` | 24.46% | 31.39% | 817 / 566 |
| byte shuffle, then `seqlz-fast-lit` | 23.95% | 30.61% | 1899 / 1464 |

The last row comes from another tool, where `seqlz-fast-lit` alone is 24.55% and 31.33%. The shuffle gives 0.6 and 0.7 points if the compressor knows which pages to shuffle, and nothing
cheap tells it: the best rule of one or two features (words whose high halves repeat, bytes equal to
the one 8 back against the one before, ...) found on the resident pages makes the first dump worse,
24.60%, and the second hardly better, 31.29%. Compressing both ways where a filter says maybe, on 35%
to 39% of the pages, gets 24.10% and 30.71%: a second compression for a third of the pages, which C3
does not allow, recompression would. The decoder pays for undoing the shuffle, 3300 ticks per shuffled
page with the kernel's flags, 0.75 us. Not built.

**Two, four neighbouring swap slots together: 5% to 8%.** 80 000 pages in slot order from the second
halves, compressed alone and in blocks, bytes per page before zsmalloc:

| block | `seqlz-fast-lit` | `zstd` -1 | `zstd` 3 |
| --- | --- | --- | --- |
| 4 KiB | 1060 / 1686 | 1163 / 1884 | 1003 / 1437 |
| 8 KiB | | 1097 / 1759 | 953 / 1371 |
| 16 KiB | 1010 / 1559 | 1048 / 1645 | 920 / 1314 |

`seqlz`'s tables do not fit an 8 KiB build. A fault on one page decodes the whole block. That fits
the swap-out of large folios in recent kernels, where zram gets 16 KiB and more at once, better than
collecting single pages; `seqlz` has a 16 KiB build already.

**A delta against a similar page: 2.3 to 3.9 points, the largest outside the codec.** Per page the page
with the most of 16 min-hashes of its 8-byte windows in common, as a `zstd` 3 dictionary. Against any
other page of the 20 000: 23.31% to 17.69% and 27.90% to 24.98%. Causal and one level deep, as zram
could do it, against an earlier page that is stored whole, where it saves 10% at least, on the first
20 000 slots of the second halves: 25.03% to 21.12% (8465 pages as deltas) and 46.31% to 43.97% (3801).
The full dumps, 16 and 23 times the sample, would have more candidates. It needs an index over all
pages, reference counts on the base pages, and a fault on a delta page decodes its base page too.
Not built, it is zram's work more than the codec's.

**BPC** (bit-plane compression, Kim et al., ISCA 2016): per 128-byte block of 32-bit words the
differences of neighbours as 33 bit planes, each plane XOR the next, then `zstd` 3, per page the
smaller: 23.31% to 23.15% (156 pages) and 27.90% to 27.63% (239). Made for cache lines in hardware.
Dropped.

**The literal coder in AVX2**: 8 streams in 64-bit lanes, the codes gathered, the whole bytes out every
4 rounds, the same bits as the scalar coder. 3.16 against 3.34 ticks per literal for 512 literals, 2.53
against 3.09 for 4096: the gather and the flush per lane eat most of it, and the kernel would need
`kernel_fpu_begin()` and arm64 a NEON version. Dropped; the budget of #33 gets the writes within C3.

**Xpress**, the format of Windows' memory compression, with the open
[ms-compress](https://github.com/coderforlife/ms-compress), 2000 pages per dump, ticks per page:
Xpress Huffman 29.63% / 34.00%, compress 139 095 / 145 305, decompress 18 000 / 20 565; Xpress 29.47%
/ 35.07%, compress 51 795 / 56 880, decompress 4680 / 5040. `seqlz-fast-lit` 25.87% / 32.78%, 4770 /
5130. Windows' own compressor may parse better.

**Two positions per step in the matcher**, as `zstd`'s fast mode: one 8-byte load gives the 5-byte
windows of `pos` and `pos + 1`, both table entries and both last-offset checks are read before any
store, one branch for all four. Compress cycles per page in the loop over 2000 pages, second dump /
first dump, `seqlz-fast` 22 713 / 20 861 before:

| variant | cycles | mispredictions per page |
| --- | --- | --- |
| pairs while fewer than 64 literals, the old loop after | 24 390 / 22 157 | 346 / 307 (296 / 275) |
| always pairs, acceleration in steps of 2 | 22 569 / 21 752 | 338 / 301 |

The sizes stay within 0.1 points. Whether one of two positions hits is less predictable than whether
one does, and the loop waits for its branches and its chain of loads, not for instructions. Dropped;
SWAR is already where it pays (the prices of all literal tables in one word, the bits of a stream from
the low byte of the sum of its entries, 16 bytes compared per step, the pattern of short offsets from
one multiply).

**Answered by other results, not built:**

* **Coding the literals later, without matching again**: the coding is about 4000 cycles per page of the
  compressor's time, 1 us, where recompression with the parser takes 234 us. It needs its own zram
  hook, recompression decompresses and compresses again, and it would get `seqlz-fast-lit`'s memory,
  where the parser through zram's recompression gets `zstd`'s.
* **A table of short matches per page**, the most frequent pairs of bytes in its header: Huffman on
  FSST's codes is no better than on the bytes, the literals have no structure of more than one byte
  left.
* **A delta against the page's own last swap-out**: the dumps have each page once, it needs a trace
  of pages that go out, come in and go out again.
* **Oodle's Selkie and Mermaid**: closed, not measured.

## Not evaluated yet

* **A faster decoder for seqlz**, see its section. The format has the memory, the decoder has to get
  to `lz4`'s speed.
* **A better matcher for seqlz-fast**: `lz4hc` level 3's matches give 25.2% against 26.4%, but it
  must not get slower.
* **Word model + a path for runs and long repeats.** Where the word model loses to `lz4` is exactly
  where `lz4` copies long matches. `PLAN.md` Phase 3, candidate 3.
* **`lz4` tuned for 4 KiB pages:** offsets limited to the page, word-aligned matches, a parser that
  does not get stuck. The `lzo-rle` route, the easiest merge.
* **The device's own literal tables** and **deltas against similar pages**, see the ideas of #29 and
  #31: both measured, neither built.
* **arm64.** Every latency above is x86-64 only. The phone's little core may order these designs
  differently; `bytelz` and `seqlz-fast` stay for it.
