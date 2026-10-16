# Why seqlz is shaped this way {#ch-decisions}

Every chapter of Part II ends a listing with a "why", and most answers are a measurement in
[`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md).
Almost all follow from four facts about zram from [chapter 1](#ch-zram): each page is compressed alone,
reading a page back is on the critical path of a program, the stored size is counted in zsmalloc's size
classes, and the code runs in the kernel, once per CPU. This chapter collects the decisions by theme,
with what each was measured against, what it gained, what it cost, and the entry with the numbers.

Each number is from the day it was measured, on the code and tables of that day, and later changes make
some of them stale. The times come from different setups: cycles in a loop in userspace, the kernel in a
virtual machine on a Ryzen 9 7950X fixed at 4.5 GHz, or the Xiaomi Mi 9T with its Cortex-A76 big core
and Cortex-A55 little core. Each number says which.
<!-- undated: the setups the numbers below come from, each dated where it is quoted -->

## The score decides, not bars {#ch-decisions--score}

The goal ([plan.md](https://github.com/martinus/quetschn/blob/main/docs/plan.md#1-goal-and-success-criteria))
is to get as close as possible to `lz4`'s speed and to `zstd`'s stored size. At first that was a set of
bars, e.g. "p99 compression latency within 1.2 times `lz4`'s", and bars turned out to be cliffs: a design
that saved 4% of memory failed because its p99 was 1 µs above `lz4`'s, without asking what 1 µs is worth.
Since then a score decides
([plan.md §1.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#11-the-score-memory-against-time-not-bars)):

- **bytes**: zsmalloc memory per stored page, measured in the kernel VM, plus the work memory per CPU
  times the CPUs, divided by the pages in zram of a named machine;
- **time per page written**: the mean time to write a page plus 0.73 times the mean time to read one;
- **score**: bytes plus lambda times time, where lambda, the exchange rate, is what one µs per page is
  worth to you in bytes.

The 0.73 is a phone in daily use, the project's Pixel 9a: 78 335 824 pages swapped in against 106 938 781
out in 7.1 days, 16th October 2026. A desktop with zram as its only swap had 0.34 in 27.5 days, and the
results of this chapter from before 16th October use that, as they say. The mean and not p99, because the mean is the total time, and a burst of swap-ins after
switching back to an app waits for all of them; p99 stays a guard against pathological pages.
`quetschn-score` prints the lower left convex hull of (time, bytes) and the exchange rate between
neighbours on it, and a design is worth keeping if it is on the hull at a lambda that matters.

One limit stays hard: **C6**, a decoder safe and bounded in time for any input, because that is what
stopped the last new codec for zram, zBeWalgo, in 2018. The work memory per CPU, **C5**, was a hard limit
too, at most `lz4`'s 16 416 bytes, until October 2026. It is in the bytes now, since it is taken from every
CPU whether it swaps or not.

The result: the kernel VM with zram as swap, a swap-out and a cold swap-in of every page, means over
20 000 pages of each of four zram dumps of a phone and a desktop, 3 boots each, 16th October 2026.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 345" role="img" aria-label="Stored bytes per page against time per page written for lz4, lzo-rle, the three seqlz levels and zstd 3, means over four zram dumps of a phone and a desktop; the levels lie between lz4 and zstd 3, seqlz-3 at zstd 3's height in less time.">
<line x1="60" y1="300.0" x2="680" y2="300.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="304.0" text-anchor="end" font-size="11">0</text>
<line x1="60" y1="235.0" x2="680" y2="235.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="239.0" text-anchor="end" font-size="11">500</text>
<line x1="60" y1="170.0" x2="680" y2="170.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="174.0" text-anchor="end" font-size="11">1000</text>
<line x1="60" y1="105.0" x2="680" y2="105.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="109.0" text-anchor="end" font-size="11">1500</text>
<line x1="60" y1="40.0" x2="680" y2="40.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="44.0" text-anchor="end" font-size="11">2000</text>
<line x1="60.0" y1="300" x2="60.0" y2="304" stroke="currentColor" fill="none"/>
<text x="60.0" y="316" text-anchor="middle" font-size="11">0</text>
<line x1="148.6" y1="300" x2="148.6" y2="304" stroke="currentColor" fill="none"/>
<text x="148.6" y="316" text-anchor="middle" font-size="11">3</text>
<line x1="237.1" y1="300" x2="237.1" y2="304" stroke="currentColor" fill="none"/>
<text x="237.1" y="316" text-anchor="middle" font-size="11">6</text>
<line x1="325.7" y1="300" x2="325.7" y2="304" stroke="currentColor" fill="none"/>
<text x="325.7" y="316" text-anchor="middle" font-size="11">9</text>
<line x1="414.3" y1="300" x2="414.3" y2="304" stroke="currentColor" fill="none"/>
<text x="414.3" y="316" text-anchor="middle" font-size="11">12</text>
<line x1="502.9" y1="300" x2="502.9" y2="304" stroke="currentColor" fill="none"/>
<text x="502.9" y="316" text-anchor="middle" font-size="11">15</text>
<line x1="591.4" y1="300" x2="591.4" y2="304" stroke="currentColor" fill="none"/>
<text x="591.4" y="316" text-anchor="middle" font-size="11">18</text>
<line x1="680.0" y1="300" x2="680.0" y2="304" stroke="currentColor" fill="none"/>
<text x="680.0" y="316" text-anchor="middle" font-size="11">21</text>
<line x1="60" y1="300" x2="680" y2="300" stroke="currentColor" fill="none"/>
<line x1="60" y1="40" x2="60" y2="300" stroke="currentColor" fill="none"/>
<text x="370" y="334" text-anchor="middle" font-size="11">time per page written, µs</text>
<text x="16" y="170" text-anchor="middle" font-size="11" transform="rotate(-90 16 170)">stored bytes per page</text>
<polyline points="318.9,105.7 334.0,144.8 356.1,159.0 419.0,164.9 632.2,165.1" stroke="currentColor" stroke-dasharray="4 3" stroke-opacity="0.6" fill="none"/>
<circle cx="318.9" cy="105.7" r="4" class="fill-paper" stroke="currentColor"/>
<text x="326.9" y="99.7" text-anchor="start" font-size="12">lz4</text>
<circle cx="323.1" cy="114.4" r="4" class="fill-paper" stroke="currentColor"/>
<text x="315.1" y="124.4" text-anchor="end" font-size="12">lzo-rle</text>
<g class="acc"><circle cx="334.0" cy="144.8" r="5" class="fill-acc" stroke="currentColor"/><text x="342.0" y="140.8" text-anchor="start" font-size="12">seqlz-1</text></g>
<g class="acc"><circle cx="356.1" cy="159.0" r="5" class="fill-acc" stroke="currentColor"/><text x="348.1" y="175.0" text-anchor="end" font-size="12">seqlz-2</text></g>
<g class="acc"><circle cx="419.0" cy="164.9" r="5" class="fill-acc" stroke="currentColor"/><text x="419.0" y="184.9" text-anchor="middle" font-size="12">seqlz-3</text></g>
<circle cx="632.2" cy="165.1" r="4" class="fill-paper" stroke="currentColor"/>
<text x="632.2" y="155.1" text-anchor="middle" font-size="12">zstd 3</text>
</svg>
</div>
<figcaption>Stored bytes per page against time per page written (swap-out + 0.73 × swap-in), kernel VM, zram as swap, means over four zram dumps of a phone and a desktop, 16th October 2026; the dashed line joins the codecs on the hull. Lower left is better.</figcaption>
</figure>

From the fastest to the smallest the hull is `lz4`, `seqlz-1`, `seqlz-2`, `seqlz-3` and `zstd` 3:
`seqlz-1` saves 597 bytes per µs against `lz4`, `seqlz-2` 145 against `seqlz-1`, `seqlz-3` 21 against
`seqlz-2`, and `zstd` 3 only 0.2 against `seqlz-3`, 1.5 bytes per page for 7.2 µs more. `lzo-rle` stores
4.5% less than `lz4`, but its swap-ins are 0.46 µs slower, and with 0.73 reads per write that leaves it
off the hull
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-numbers-again-on-16th-october-all-three-levels-with-lzo-rle-the-readmes-chart)).
zram's read benchmark, which decodes every page several times, trains the branch predictor and flatters
seqlz; a swap-in decodes a page once, and these are a swap-in's times. About 1.9 µs of a fault on the PC
are the kernel's for every codec, so a decoder 40% slower than `lz4`'s makes a swap-in about 20% slower
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-whole-page-fault-the-kernels-part-is-the-same-for-every-codec-the-gap-to-lz4-about-halves)).

## The overall bet: code the sequences, with tables the page does not carry {#ch-decisions--bet}

### The gap to zstd is how the sequences are coded

The most useful measurement came before seqlz existed: the matches `lz4hc` level 9 finds on a desktop's
zram pages, written in different ways, as Σ zsmalloc cost, the memory zsmalloc needs for all pages as a
share of their size
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#where-the-ratio-of-zstd-comes-from)):

| the same `lz4hc` level 9 matches, written as | Σ zsmalloc cost |
| --- | --- |
| `lz4`'s format, bytes | 30.7% |
| `ll`, `ml` and offset entropy coded, literals raw | 25.8% |
| ... and repeat offsets like `zstd`'s | 25.1% |
| ... and the literals entropy coded too | 23.6% |

The coded rows use ideal code lengths from the same pages, so they are optimistic. `lz4` itself is at
34.5%, `zstd -1`, which codes no literals, at 26.9%. The largest step towards `zstd` is writing the three
numbers of a sequence in bits instead of bytes, not a better search: with the matches of `lz4hc` level 3
the rows are 31.3%, 26.1%, 25.4% and 23.7%. So seqlz has a cheap greedy matcher
([chapter 9](#ch-matcher)) and spends its effort on the format ([chapter 4](#ch-format)).

### Fixed tables, because a page is too small to carry its own

A page that compresses to 1000 bytes has no room for a description of its tables, and building tables
from one costs the decoder time on every read. So seqlz's {{ch17.tables}} tables are fixed, part of the
format, and built once, {{size.tables|grp}} bytes shared by all CPUs ([chapter 8](#ch-codes)). Tables
trained on the very dump they are measured on, which no real table can be, are only 2.0% smaller on the
desktop's pages and 0.7% on the phone's; more starts of k-means did not help either, since the start
with the fewest bits on the training pages is not the one with the fewest on others
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tables-trained-again-4-less-on-one-desktop-dump-the-phone-the-same)).
A literal table built for each page saved 2.4% for writes 14% slower the first time it was built
([below](#ch-decisions--not-kept)); level 3 has one since 15th October 2026, only where it promises
to save enough ([below](#ch-decisions--level3)). The tokens and the lengths stay fixed at every level.

The 4 KiB tables of 13th October 2026 are trained on {{ch17.train_pages|grp}} pages: three sessions of
the Mi 9T under memory pressure, a desktop's resident pages, and 200 000 pages of its zram dump of 28th
September. On two test sessions of the phone, with web sites none of the training sessions loaded,
`seqlz-2` stores up to 1.7% less than with the tables before, and the phone dump of 3rd October,
which the tables before were trained on, 1.8% more
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tables-trained-on-phone-sessions-up-to-17-smaller-on-them-an-old-phone-dump-18-larger)).

Tables per device, sent as zram's dictionary, were measured too: tables trained on one zram dump of the
Mi 9T saved 0.6% on another dump of the phone, 1.4% on their own pages, and cost 1.0% on the desktop's
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-format-written-down-one-set-of-tables-and-stream-sizes-that-hold)).
So seqlz takes no dictionary, and the kernel's backend refuses one. It also needs none to beat `lz4` with
one: 20.4% less on a desktop's zram pages, in the quick benchmark
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#lz4-with-a-dictionary)).

### Huffman codes, not ANS

`zstd` codes its sequences with FSE, a form of ANS that can spend a fraction of a bit on a symbol.
Huffman codes fitted to the pages lose 2.3 bytes per page, 0.3%, against the entropy of the same counts
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).
An ANS decoder steps a state machine on every symbol, a chain the decoder waits for, so seqlz stays with
prefix codes and one table lookup.

### One token per sequence, and a format built for the decoder

zram reads a page while a program waits and writes most pages in the background, so where the format had
a choice it took the shorter chain from one sequence to the next in the decoder. The first prototype had
three symbols per sequence. Folding both lengths into one token cut the decode loop from 14 010 to 10 930
cycles per page, 22%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-lz4s-matches-huffman-coded-sequences-with-static-tables));
the offset class in the token as well, from 8758 to 8168, 7%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
Since then a sequence costs the decoder one lookup, a shift and the next lookup ([chapter 14](#ch-decode)).

## The format {#ch-decisions--format}

Each row is a decision of [chapter 4](#ch-format), against the other way in the second column.

| decision | instead of | gain | price | entry |
| --- | --- | --- | --- | --- |
| `ll`, `ml − 4` and the offset class in one token | a symbol each | 22% fewer decode cycles for the lengths, 7% fewer for the offset class | | [seqlz](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-lz4s-matches-huffman-coded-sequences-with-static-tables) |
| 4 bits of `ll`, 5 of `ml` in the token | 4 and 4 | a length value after 8.5% of the matches instead of 18.7%, 9180 instead of 9400 decode cycles | a larger token alphabet | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes) |
| one repeat offset | three, as `zstd` | 17% fewer decode cycles, 89 instead of 124 mispredictions per page | 26.6% instead of 26.4% | [third decoder round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow) |
| two classes for multiples of 8 | plain offsets only | 13 bytes per page in the kernel | 0.21 µs per page written | [by the score](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8) |
| offset 2 in 1 bit, class 5 below 2048 in 8 bits | offsets below 16 in 4 bits, class 5 in 9 | 0.6% smaller on 4 KiB pages, 1.4% on 16 KiB | none measured | [offset 2 in 1 bit](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-2-in-1-bit-multiples-of-8-below-2048-in-8-03-to-08-smaller-at-the-same-speed-kept) |
| token codes of at most 11 bits | 12 bits | cold p99 4350 instead of 5070 ns, measured with entries of 2 bytes | {{ch17.tok_own}} of {{const.TOKEN_SYMBOLS}} tokens have a code, the rest is escaped | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes) |
| the bitstream read most significant bit first | least significant first | a page touches 42.5 instead of 55 of the token table's {{ch17.tok_lines}} cache lines | none, the same bytes | [bit order](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept) |
| only complete prefix codes | a check for invalid codes | 6% of the decoder's instructions | | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes) |
| trained codes for length values | the symbol in 5 plain bits | 2.2 bytes per page | two more tables in the format | [5 plain bits again](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-again-02-to-03-more-memory-no-read-faster-not-kept) |
| a page ends exactly where its bits end | trailing bytes ignored | a damaged page is rejected | 0.07 to 0.10 µs per warm read on the A55 | [strict end](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-page-ends-where-its-bits-end-no-damaged-page-with-bytes-or-bits-too-many-decodes-01-µs-on-the-a55-kept) |
| no version in the page | a version byte | none needed | a format change needs a new kernel | [format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#no-version-in-the-page) |

### The token: 4 bits, 5 bits and 6 classes

A token is `min(ll, 15) + 16 × min(ml − 4, 31) + 512 × class`, $16 \times 32 \times 6 = 3072$ values
([chapter 6](#ch-header--token)). Before the escape, with 3 classes, 3 and 5 bits gave 26.9% at 8599
decode cycles and 4 and 4 bits 27.0% at 8482; the escape let 4 and 5 fit, 26.8% at 8480
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
A fifth bit for `ll` was not tried: it would double the alphabet, and codes of at most 11 bits have room
for 2048
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-fast-paths-condition-its-22-misses-per-page-are-the-length-values-four-ways-around-them-slower-not-kept)).
@assert const.TOKEN_SYMBOLS == 16 * 32 * 6

### Offset classes: offset 2 in 1 bit, and two for multiples of 8

Class 0 is the repeat offset, class 1 offset 2 in 1 bit, classes 2 and 3 offsets below 256 and below the
page size in 8 and 12 bits, classes 4 and 5 multiples of 8 below 256 and below 2048, divided by 8, in 5
and 8 bits. Classes 4 and 5 are for arrays of 8-byte values: of the offsets from 16 to 255 that are not
the repeat offset, 64% are multiples of 8, from 256 on 52%, and the low 3 bits of an offset carry 2.1
bits, not 3. The 13 bytes per page they saved in the kernel are 62 bytes per µs, against 9 for the step
to `zstd` 3 in the same boots
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)),
for 3072 tokens instead of 2048 and an escape of 12 bits.

On 14th October 2026 class 1, the offsets below 16 in 4 bits, became offset 2 alone, about 8% of the
offsets that are not the repeat offset on phone and desktop pages, from arrays of 16-bit values; class 5
went from every multiple of 8 from 256 on, in 9 bits, to those below 2048, in 8. A search over layouts of
6 to 8 classes, priced on seven dumps, found these. It also found 8 classes with fixed offsets 2 and 8,
3.3 bytes per page smaller, but the decoder's one more select made the A55's writes 1.0% and warm reads
1.7% slower, the A76's warm reads 2.2%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-2-in-1-bit-multiples-of-8-below-2048-in-8-03-to-08-smaller-at-the-same-speed-kept)).
Earlier, 29 539 layouts had found at most 0.15 bits per sequence
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-classes-from-a-histogram-todays-are-close-two-more-give-01-points-at-most)),
offsets 2 and 8 as classes of their own made pages 0.8% smaller and warm reads 1.4% slower on both phone
cores
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)),
and a class 1 for offsets 1 to 7 was larger, 27.1% instead of 27.0%, at 7625 instead of 6883 decode
cycles, cold p50 1410 against 1470 ns and p99 3280 against 3040
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).

### One repeat offset, not three

`zstd` keeps three recent offsets and moves the one used to the front. In seqlz's first prototype the
second and third were 11% of the matches, and the move to front 17% of the decode cycles
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
The repeat offset starts at 1 on every page, so a run of equal bytes is a match from the start.

### 11-bit token codes, and the escape

With 12-bit codes the token table had 4096 entries and no longer stayed in L1 with the other tables.
Package-merge, the optimal length-limited code, gave the same token bits as halving the counts: the limit
decides
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
The {{ch17.tok_entries}} entries of {{ch17.tok_entry_bytes}} bytes, 8 KiB, give {{ch17.tok_own}} tokens a
code; the rest follow the escape in {{ch17.escape_bits}} bits, {{ch17.rare_token_bits}} for a rare token.
10 bits were measured again on the phone, cold reads 2.0 to 2.4 µs faster on both cores for 4.8 bytes per
page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#1-the-token-table-in-10-bits-cold-reads-2-to-3-µs-faster-on-both-cores-for-3-to-6-bytes)),
and with the bit order below nothing on the A76 and 2 to 2.6 µs on the A55
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept)).
11 bits stayed.
@assert ch17.tok_entries * ch17.tok_entry_bytes == 8192

### The bit order: most significant bit first

Read lowest bit first, an $l$-bit code fills every $2^l$-th entry of the token table; highest bit first,
one range ([chapter 3](#ch-cpu--msb-first)). It cost nothing on the PC, 7370 against 7410 decode cycles,
and on the A76, which did not prefetch the token table then, it made a faster cold read hold from one
build of the module to the next
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept)).

### Complete codes only

Every 11 bits start a code, so the decoder has no check for an invalid one. The trainer,
`seqlz_tables_init()` and, since commit `4c42cc5`, the generator of the kernel copy's tables check it
([chapter 8](#ch-codes)).

### Length values: trained codes, measured twice

A literal run of {{ch17.ll_value_from}} bytes or more, or a match of {{ch17.ml_value_from}} or more, has a
length value from one of two tables of {{const.LEN_SYMBOLS}} symbols. The symbol in 5 plain bits would
take them out of the format, the trainer and the prefetch. It cost 3.0 bytes per page the first time
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-instead-of-their-tables-3-bytes-per-page-more-not-kept)),
and on 10th October 2026 (commit `e49b9dd`) 2.2 bytes, with no read faster than a second copy of the same
source in the VM and gcc's writes in the kernel 1.8% to 3.0% slower
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-again-02-to-03-more-memory-no-read-faster-not-kept)).

### The end of a page, and no version

The page has no count of sequences, which saved 0.1 points, 26.7% to 26.6%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
Since 8th October 2026 a page and each literal stream must end in their last byte with zero bits after,
as `lz4` rejects input it did not use; it cost nothing in the VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-page-ends-where-its-bits-end-no-damaged-page-with-bytes-or-bits-too-many-decodes-01-µs-on-the-a55-kept)).
Since 11th October the last sequence has no token: the decoder knows it from the literals left, 1.2 bytes
per page, 0.12% of zsmalloc memory, at no time the VM or the phone could measure
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-last-sequence-without-a-token-12-bytes-per-page-less-kept)).
zram's pages never outlive the kernel that wrote them, so a new format needs a new kernel, not a version
byte, and the values of byte 2 with its top bits set stay free for a format that has to tell itself
apart ([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#no-version-in-the-page)).


## The literals {#ch-decisions--literals}

Literals are 68% of `seqlz-1`'s output, and coding them is worth 3.5 points to `zstd` 3. So
`seqlz-2` Huffman codes them, `seqlz-1` stores them raw, in one format with one decoder
([chapter 11](#ch-literals-enc), [chapter 13](#ch-literals-dec)).

### 8 fixed literal tables, the page picks one

k-means over the pages' literal histograms gives K tables, and each page takes the one that codes it in
the fewest bits
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)):

| tables, longest code | `seqlz-1`'s output saved |
| --- | --- |
| 1, 11 bits | 6.8% |
| 4, 11 bits | 9.2% |
| 8, 10 bits | 11.4% |
| 16, 11 bits | 11.9% |
| 64, 11 bits | 12.9% |

Each of the 8 has a decode table of {{ch17.lit_entries}} entries of {{ch17.lit_entry_bytes}} bytes, 2 KiB,
and a page reads only its own. 16 tables made cold reads 0.27 µs slower in the kernel VM, on the hull only
for lambda 10 to 13
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)),
and on the Mi 9T saved 1.9 bytes per page for 2.0 µs more per write on the A55
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).
Codes of 9 bits made no read faster and pages 20 bytes larger
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#2-the-literal-tables-in-9-bits-no-faster-up-to-64-bytes-larger-not-kept)).
Two details of the trainer were each found with a table no page chose: a page whose literals no table
codes $\frac{1}{16}$ smaller counts for no table, and a table without pages starts again from the page
whose literals cost the most per byte.
@assert ch17.lit_entries * ch17.lit_entry_bytes == 2048 and ch18.bits.lit == 10

### 8 streams, which have nothing to do with the 8 tables

Literal `k` goes to stream `k mod 8`, so the decoder runs 8 independent chains
([chapter 13](#ch-literals-dec)). Against 4 streams, the decode loop went from 9070 to 8676 cycles per
page, and cold reads in the kernel from 2940 / 4810 to 2860 / 4531 ns at p50 / p99
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
4 streams store 2.0 bytes per page less and were measured twice more: no faster on the phone
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#3-4-literal-streams-instead-of-8-27-bytes-no-faster-not-kept)),
1.8% slower with gcc and 0.7% with clang in the VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).

### Coded only where it saves 1/16 and {{ch15.coded_min}} bytes

The coded streams plus {{ch15.coded_min}} bytes must be less than $n - n/16$ for $n$ literals
([chapter 11](#ch-literals-enc--rule)). Coding wherever it saves anything gave less than 0.1 points more
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)),
and a coded page costs the decoder a fixed time, large on the in-order A55: its literal table from memory,
and the literals through a buffer. Raising the minimum from 19 to {{ch15.coded_min}} bytes cost 7.9 bytes
per page on the phone and saved 2.7 µs per page written on the A55 and 1.1 µs on the A76, with the 0.34
reads per write of then; on the PC, 11 bytes for 0.1 µs
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-only-where-they-save-51-bytes-27-µs-per-page-written-less-on-the-a55-kept)).
It is the best of 19, 51 and 115, not a measured optimum, and with the prefetches added later the gain
shrank to 0.4 µs on the A76 and 0.8 µs on the A55
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#9-the-51-bytes-of-coded-literals-only-where-they-save-51-bytes-again-with-item-4s-and-7s-prefetches)).
@assert ch15.coded_min == 51

### Stream sizes in `w` bits

The 8 stream sizes were 2 bytes each, a header of 19 bytes. Now they take `w` bits each, what the largest
needs, {{ch17.width_min}} to {{ch17.width_max}}, with $w - 5$ in byte 2: a header of $3 + w$ bytes, mostly
9 to 11, and pages 5.3 bytes smaller
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#stream-sizes-in-as-many-bits-as-the-largest-needs-kept)).
Letting the smaller header code more pages would have saved 2.2 bytes more on the desktop, but on the
phone those pages were worth 2 bytes for 251 ns more per page written on the A55, with the 0.34 reads per
write of then, so the rule kept the old
header's 19 bytes, which later became the minimum of {{ch15.coded_min}}.

### Pricing all 8 tables at once

Each byte's code lengths in the 8 tables are one 64-bit word, so one add per literal prices all 8, and
every {{ch17.lit_cost_run}} literals the sums move into wider lanes ([chapter 11](#ch-literals-enc--pricing)).
In the kernel that costs 30 to 40 ns per write; writing the streams is what costs, and pricing a sample
could save 400 cycles at most, since the exact stream sizes are needed anyway
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
One stream after the other waited on its accumulator, about 3.5 cycles per literal, so the encoder writes
several side by side
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).

### The buffer: literals in front, bitstream from the back, the matcher's table as scratch

The encoder writes literals from the front and the bitstream from the back of its output
([chapter 10](#ch-encoder--layout)). Before, the bitstream went behind room for a page of literals, which
needed two pages of output; zram gives two, zswap one. The change wrote the same bytes and made writes in
the VM 0.19 to 0.21 µs faster
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-into-a-buffer-of-any-size-the-bitstream-from-the-back-the-same-bytes-in-zram-writes-2-faster-in-the-vm-kept)).
Coding the literals over the raw ones needs a copy of the raw ones, which goes into the matcher's 8 KiB
table, free once the matcher is done. In a buffer of one page that costs 0.23% more memory instead of
2.0%; in zram's two pages it wrote 0.12 µs faster in the VM, partly from the order of the codecs in the
run, as `lz4` was 0.03 µs faster too, and the same on the Mi 9T
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-from-the-matchers-table-a-buffer-of-one-page-costs-01-to-05-instead-of-16-to-29-kept)).

### Raw or coded: a choice for the device

On the phone's little core coded literals are dear. In the harness on the phone's own pages,
`seqlz-2` stores 55 bytes per page less than `seqlz-1`, for 4.2 µs more per page written with the 0.34
reads per write of then, and 1.6 times the cold p99 on the A55
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-second-phone-dump-after-12-hours-of-use-seqlz-fast-reads-faster-than-lzo-rle-at-p99)).
With a desktop's zram pages in the phone's module and the tables before 7th October, `seqlz-2` has
the lowest score on the A55 only up to 12 bytes per µs, `seqlz-1` up to 24
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-numbers-again-with-the-bit-order-and-the-token-tables-prefetch-cold-reads-on-the-a76-3-µs-faster)).
So both stay, as zram's levels 1 and 2, coded the default, and level 3 adds a third choice
([below](#ch-decisions--level3)).

## The matcher {#ch-decisions--matcher}

The matcher ([chapter 9](#ch-matcher)) is where the write time goes: in the third round of compressor
work it took 17 300 cycles per page, `lz4` 17 100 for everything
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-third-round-ideas-from-the-literature)).
Every decision here trades bytes against work per position.

| decision | instead of | gain | price | entry |
| --- | --- | --- | --- | --- |
| greedy, the encoder in the same loop | matcher, then encoder | 22 848 instead of 24 257 cycles per page | | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions) |
| both candidates read, one branch decides | a branch per candidate | 329 instead of 539 mispredictions per page | 7% with the page in the cache | [seqlz-1](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-seqlz-fast) |
| a hash of 5 bytes | 4 bytes | write p99 in the kernel 10 480 instead of 11 650 ns | 0.6 points, with the tables of then | [prefetch, 5 bytes](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression) |
| 4096 slots of 2 bytes, 8 KiB | 2048 or 8192 | | | [item 5](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#5-the-matchers-hash-5-bytes-into-4096-slots-stays) |
| the repeat offset at every position | the table only | 0.5 points | | [seqlz-1](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-seqlz-fast) |
| the table cleared for each page | checking each entry's age | 7% faster | an 8 KiB `memset` per page | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions) |
| every position tried | `lz4`'s growing step | writes 0.23 µs faster in the VM, 0.05 µs in boots with all four codecs | pages stored raw 2.3 µs slower | [no step](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept) |
| the next position loaded ahead | one position at a time | 3.4% fewer cycles on the A55 | | [in-order core](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-on-an-in-order-core-34-fewer-compress-cycles-on-the-a55-same-output-kept) |

### Greedy, in one pass

Two candidates per position, the repeat offset and the hash table's one position, the first match taken,
counted forwards and handed to the encoder at once. A matcher that filled an array for an encoder after
it was slower because the encoder's branches lost the history of the matcher's, which had predicted them
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).
Searching harder always cost more than it saved: one lazy step, 24 808 instead of 20 400 compress
cycles for no smaller pages then
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression));
a hash chain with one lazy step, 27.5% instead of 28.2% for 39 500 cycles and 2.3 times the
mispredictions
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page));
the 2 newest positions per hash in the same 8 KiB, 11 bytes per page for about 1 µs, at the edge of the
hull
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8));
an earlier level 3 and 4 [below](#ch-decisions--not-kept). Level 3 of today searches harder only a
little, with one lazy step for matches below 20 bytes ([below](#ch-decisions--level3)). With one branch
per position instead of three, which had
mispredicted almost twice as often as `lz4`'s one, the cold loop's p99 went from 15.4 to 12.6 µs
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-seqlz-fast)).

### The hash keeps 5 bytes, into 4096 slots

First measured as 2.6% faster for 0.5 points more memory, a hash of 5 bytes was dropped; measured again,
20 425 instead of 21 440 compress cycles, and a write p99 10% lower in the kernel, twice what the cycles
say, because the slowest pages to write have many short matches
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
With coded literals the 4-byte hash is also larger, by 24 bytes per page with the tables of then: a
4-byte match at a new offset costs more bits than 4 coded literals, and the greedy matcher then misses the
longer match behind it. 2048 slots: 939.6 instead of 936.9 bytes per page on the phone, no faster; 8192
slots: 937.1 bytes, 1.6 µs slower per write on the A55, and 16 KiB per CPU
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#5-the-matchers-hash-5-bytes-into-4096-slots-stays)).
A hash of 6 bytes made pages 27.9% instead of 27.0%.
@assert ch17.hash_slots * 2 == 8192

### The repeat offset at every position

It needs no lookup, and it is about 22% of the matches on desktop pages and 32% on phone pages, with the
classes and tables of 14th October 2026 ([chapter 9](#ch-matcher--plan)). Offline on phone pages,
checking it only up to 16 positions after a match cost 0.3%, never checking it 3.7%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#ratio-from-the-parse-and-the-tables-on-phone-pages-measured-offline-not-built)).

### No step, and a cleared table

The step `1 + (pos − anchor) >> 6` needed `anchor` in the loop over positions without a match, which has
no register to spare. Without it, pages that compress are 5% faster to write, and the write p99 went from
1.31 to 1.21 times `lz4`'s
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept)).
Clearing the table for every page also means the work memory keeps nothing from one page to the next,
which [the memory section](#ch-decisions--memory) needs. Not clearing at all, with a compare per position
that the entry is before it, took twice the time on the A55, 44.4 instead of 21.5 µs per page, for a
reason not found
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matchers-table-with-the-bytes-its-loop-in-assembly-on-arm64-9-fewer-compress-cycles-on-the-a55-not-kept)).

### One position ahead, for the in-order core

On the A55 a position without a match is one chain: 8 bytes, the hash's multiply, the table entry, the
bytes at the candidate, the compare. Loading the next position's chain while comparing the current one
gives the same output in 42 349 instead of 43 841 cycles per page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-on-an-in-order-core-34-fewer-compress-cycles-on-the-a55-same-output-kept)).


## The decoder {#ch-decisions--decoder}

The decoder ([chapters 12 to 14](#ch-dec-basics)) runs while a program waits. It runs 2.1 times `lz4`'s
instructions, 25 284 against 11 899 per page, at 3.3 instructions per cycle against 2.4: 7609 against 4992
cycles in a hot loop on the PC
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-numbers-again-with-the-format-as-it-is-now-reads-03-µs-faster-than-on-29th-september)).
Its decisions are about fewer instructions, fewer mispredicted branches, or fewer cache misses on a cold
read.

| decision | instead of | gain | price | entry |
| --- | --- | --- | --- | --- |
| token entries of 4 bytes, ready to use | 2-byte entries unpacked per sequence | A55: 15 129 instead of 16 547 instructions per page; with three smaller changes 17 842 to 15 129, and cold p99 0.75 µs less | 8 KiB instead of 4 KiB, once | [in-order decoder](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept) |
| the next token looked up before the copies | after them | cold p50 against `lz4`: A55 2007 instead of 2169 ns more, A76 330 instead of 456 | two more registers | [next token](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-next-token-before-the-copies-reads-faster-on-both-phone-cores-kept) |
| fixed 16-byte copies on the fast path | 32 or 40 bytes always | 30 ns per decode, 250 ns at swap-in p99 | a branch for longer matches | [branch or more work](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained) |
| the refill every second fast sequence without a check | a check every time | 63 ns per swap-in on x86-64 | | [refill](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-refill-without-its-branch-every-second-fast-sequence-63-ns-less-per-swap-in-on-x86-64-and-the-a76-kept) |
| one decode loop for every core | a loop per kind of core | no CPU model in `lib/` | A55 up to 0.3 µs per warm read | [kernel copy's choices](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16) |
| the compressed data prefetched by the backend | none | `lz4`'s cold p99 5600 to 3891 ns; A55 warm reads 2.3 µs faster for seqlz | | [prefetch](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression) |
| all tables prefetched before a page | none | A55: 14.5 µs per cold read, 30 µs at p99 | A76 cold reads 1.2 µs slower then, not seen again on 10th October | [item 4](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#4-prefetching-the-tables-the-a55-needs-it-the-a76-is-faster-without) |

### One lookup per sequence, with what the decoder needs in the entry

The 2-byte entries of before had the decoder compute the class, its bits, a mask and the shift: 18 of
about 60 instructions per sequence on the A55, where fewer instructions were faster almost one for one.
The 4-byte entries hold everything ready ([chapter 7](#ch-internal--token-entry)); the flag for a length
value saved instructions but no cycles. The other three changes of that step were the fast path's room
checks, the prefetch of 8 lines per iteration and the match copied from `d - off`
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).
The next token's lookup before the copies lets the copies fill the time of its load on an in-order core
([chapter 14](#ch-decode--next-token)).

### The fast path: fixed copies, and the branches that stay

79% of the matches are at most 16 bytes, 32 bytes cover 91%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).
32 bytes always made the mean faster and the p99 slower, 3570 to 4150 ns with the decoder of then,
because the slowest pages have many short matches
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
One copy for every offset, without the branch on `off < 8`, saved 12 mispredictions per page for 10 to
19 more instructions per fast sequence, which cost more
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-offset-below-8-one-copy-for-every-offset-saves-12-misses-per-page-and-costs-more-not-kept)).
The lesson came early: "at 3.4 to 3.9 instructions per cycle, instructions are as expensive as
mispredictions here". Masks instead of branches halved the mispredictions, 155 to 68 per page, and took
9400 to 12 000 cycles
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).

### The refill every second time, and the branches of a fault

In a real swap-in, on a page the predictor has not seen, `seqlz-2` mispredicts about 110 branches
per page more than right after decoding the same page, and `zcomp_decompress()` takes 2.0 µs against
1.4 µs for `lz4`
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
The largest source was the check for fewer than {{ch17.refill_below}} bits left
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#which-branches-three-in-the-sequence-loop-are-58-of-the-130-misses-of-a-page-seen-once)).
Two fast sequences take at most $2 \times (11 + 14) = 50$ of the 56 bits of a refill, so a fast sequence
refills every second time without asking ([chapter 14](#ch-decode--next-token)): `zcomp_decompress()` went
from 1995 to 2008 ns to 1936 to 1942 ns in 3 boots with gcc, and on the A76 a page seen once decoded about
70 ns faster. A refill on every fast sequence took out as many misses, but its loads cost 500 cycles more
on a page the predictor knows
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-refill-without-its-branch-every-second-fast-sequence-63-ns-less-per-swap-in-on-x86-64-and-the-a76-kept)).
The fast path's condition misses once per length value, which is data, and four ways around it were all
slower
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-fast-paths-condition-its-22-misses-per-page-are-the-length-values-four-ways-around-them-slower-not-kept)).

### One loop for all cores

The refill without its check made the in-order A55 slower, so for a while an arm64 kernel read the core's
MIDR register and ran the old loop on Cortex-A53, A55, A510, A520 and Qualcomm's small Kryo cores, which
alone prefetched the token table on arm64. The review of the kernel copy (#152) removed it: nothing else in
`lib/` depends on the CPU model, and the list needs an entry for every new little core. One out-of-order
loop costs the A55 at most about 0.3 µs per warm read, 3%, nothing that shows on the A76, and the token
prefetch that had cost the A76 1.3 to 2.6 µs of cold reads did not show again on 10th October
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16)).
`port.py` now fails on `read_cpuid` or `MIDR_` in `lib/`. The same entry kept `-O3`: with `-O2` writes were
0.16 µs slower in the VM with gcc, 0.11 µs with clang, and 0.93 µs on the A55.

### Prefetch: the compressed data and every table

The backend prefetches the compressed data, the decoder its tables, {{ch17.tok_lines}} lines of token
table, {{ch17.len_lines}} for each length table and {{ch17.lit_lines}} for the page's literal table,
{{ch17.prefetch_lines}} per loop iteration ([chapter 12](#ch-dec-basics--prefetch)). Even the 32 lines of
the length tables were worth 2.8 µs to the A55
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#4-prefetching-the-tables-the-a55-needs-it-the-a76-is-faster-without)),
and one prefetch per iteration cost the A76 0.8 µs on cold reads of coded pages
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
The destination page's prefetch did not help in the kernel. Clang drops the kernel's x86-64
`prefetch()`, `__builtin_prefetch()`, under `-mno-sse`: 0 `prefetcht0` in seqlz's object against 36 with
gcc, and a cold p99 24% worse
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-kernel-built-with-clang-dropped-the-decoders-prefetches-fixed-p99-back-at-gccs)).
The fix is a patch to the kernel, not inline assembly in the codec; without it a clang kernel swaps in 120
to 180 ns slower, 740 to 810 ns at p99
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernels-prefetch-on-x86-64-fixed-in-the-kernel-a-patch-gives-clang-its-prefetches-back)).
@assert ch17.len_lines * 2 == 32

### `decode_literals()` is not inlined

Inside the loop over the sequences, its 8 stream accumulators would take registers that loop needs, also
on pages without coded literals ([chapter 13](#ch-literals-dec--main)).

### Bounded work for any input

Every offset and length is checked against the room, and a page has at most `PAGE / 4 + 1` sequences
([chapter 14](#ch-decode--safety)). A fuzzer that rewards cost found pages that take 1.14 times the
instructions of the real p99 to compress and 1.59 times to decode, the smallest ratio of the four codecs;
`lz4`'s decode worst case is 2.58 times its p99. Timed on the A55, 1.26 and 1.33 times the p99. The slowest
decode found, every token escaped with 4-byte matches at offsets below 8, takes about 107 000 arm64
instructions, 40 µs on the A55, still below the slowest compression
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-worst-case-the-slowest-pages-found-cost-13-times-the-p99-of-real-ones-as-for-lz4)).


## Memory {#ch-decisions--memory}

### C5, counted as the allocator counts

The work memory per CPU was a limit: 4 KiB at first, `lz4`'s 16 416 bytes from 23rd September 2026,
because 8 points less memory on the compressed pages are worth far more than a few KiB per CPU. Neither
number was priced. Since October 2026 it is a cost in the score, bytes per CPU times the CPUs divided by
the pages in zram. On the Mi 9T's 8 cores every 4 KiB per CPU is 32 KiB, resident for good, 0.1 bytes per
page with 1 GiB in zram. It moves the hull only where zram holds little: `zstd` 3 holds 176 160 bytes per
CPU more than `seqlz-2` and drops off the hull below about 27 MiB in zram per CPU on one desktop
dump
([plan.md §1.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#11-the-score-memory-against-time-not-bars)).

What counts is what `kmalloc()` and `vmalloc()` hand out ([chapter 1](#ch-zram--work-memory)): `lz4` asks
`vmalloc()` for 16 416 bytes and gets 5 pages, 20 480 bytes. seqlz needs {{size.state}} bytes of hash
table to compress and {{const.SCRATCH}} bytes of scratch to decompress, never at the same time and keeping
nothing from one page to the next, so the kernel's backend gives each CPU one union of
{{ch15.mem.union}} bytes ([chapter 15](#ch-zram-glue--backend)). As one struct of
{{ch15.state_plus_scratch|grp}} bytes it would have come as 16 KiB. With zram's contexts split, as a series
of 5th October 2026 proposes, it is two allocations of 8 KiB each, a split that changed no codec's time in
the VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zrams-contexts-split-into-compression-and-decompression-the-same-time-in-the-vm-the-same-memory-for-seqlz)).
Level 1 has no decompression context. The tables, {{size.tables|grp}} bytes, are built once and shared
by every CPU.
@assert ch15.mem.union_gets == ch15.mem.cctx_gets == ch15.mem.dctx_gets == 8192 and ch15.mem.struct_gets == ch15.mem.split_gets == 16384

### 16 KiB pages: why the kernel copy is 4 KiB only

seqlz is written for both page sizes, and 16 KiB pages have their own tables, trained on the Android 17
emulator's zram and 2.6% smaller on another of its dumps than tables from desktop pages
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#android-17-in-the-emulator-the-4-kib-tables-fit-the-16-kib-ones-trained-again-26-smaller)).
But the work memory doubles with the page: the union asks for {{k16.ch15.mem.union|grp}} bytes and gets
{{k16.ch15.mem.union_gets|grp}}, the split contexts ask for {{k16.ch15.state_plus_scratch|grp}} and get
{{k16.ch15.mem.split_gets|grp}}, against `lz4`'s 32 768. Each split context alone stays at `lz4`'s 32 KiB,
their sum does not, and the score counts the sum
([plan.md §3.3](https://github.com/martinus/quetschn/blob/main/docs/plan.md#33-per-cpu-workspace-is-real-memory-on-a-phone)).
Decoding the coded literals into the end of the output page removes the scratch; it was built, and made
cold reads 0.08 to 0.22 µs slower on 4 KiB pages in the kernel VM, for memory those pages do not need
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#16-kib-pages-tuned-c5-does-not-hold-the-literals-in-the-page-would-fix-it-not-kept)).
zram's own buffer of two pages per stream, unused while reading and half unused while writing, is not
tried yet
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#not-evaluated-yet)).
Until that is decided, and until there is a phone with 16 KiB pages, their format is not fixed and the
kernel copy is for 4 KiB pages only
([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#status)).
@assert k16.ch15.mem.cctx_gets <= 32768 and k16.ch15.mem.dctx_gets <= 32768 and k16.ch15.mem.split_gets == 48 * 1024

## Level 3: a lazy step and a literal table of the page's own {#ch-decisions--level3}

Level 3 is for a device that values memory more than level 2 does. On 15th October 2026, in the Mi
9T's own kernel, with 19 775 pages of a phone session in zram (`tools/zram-phone/run.sh`), means of 3
rounds in µs:

| codec, Mi 9T, 15th October 2026 | zsmalloc bytes per page | A76 write | A76 warm / cold read | A55 write | A55 warm / cold read |
| --- | ---: | ---: | ---: | ---: | ---: |
| `lz4` | 1364.0 | 10.16 | 3.65 / 6.06 | 25.48 | 8.85 / 23.68 |
| `seqlz-2` | 997.0 | 11.94 | 4.50 / 7.77 | 32.78 | 12.91 / 31.68 |
| `seqlz-3` | 952.0 | 16.18 | 4.88 / 8.30 | 42.38 | 14.00 / 34.59 |
| `zstd` 3 | 951.4 | 36.41 | 16.10 / 21.39 | 114.23 | 39.38 / 69.03 |

Level 3 stores pages as small as `zstd` 3, 0.6 bytes more, in less than half its time to write one and
about 40% of its time to read one cold, for 4.2 µs more per write than level 2 on the Cortex-A76. From
level 2 to level 3 a µs of the score buys 10.2 bytes per page on the A76 and 4.2 on the A55, from level
3 to `zstd` 3 0.02 and 0.01 ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-a-lazy-matcher-and-a-literal-table-of-the-pages-own-zstd-3s-memory-in-less-than-half-its-time-on-the-phone-kept)). On ten samples of 20 000 pages, in the
bench's model of zsmalloc, level 3 stored 0.30% more than `zstd` 3 on average and 4.5% less than level
2. One phone, and one session for the times.

| decision | instead of | why |
| --- | --- | --- |
| one lazy step, only for matches below 20 bytes | a lazy step at every match, or a hash chain | a longer match one byte later is rare after a long one, and looking takes time ([chapter 9](#ch-matcher--lazy)) |
| matches extended backwards, at level 3 only | the step back at every level | at levels 1 and 2 it cost writes 1% to 7% ([chapter 9](#ch-matcher--hit)) |
| the same format and decoder at every level | a format per level | one decoder and one specification for all levels |
| a table of the page's own, only from 512 literals on and where it saves 16 bytes | a table on every page | the description costs bytes, and building a decode table for every read costs time |
| the entropy first, then the code | building the code on every page | many pages leave after one pass over 256 counts ([chapter 11](#ch-literals-enc--own)) |
| code lengths from $f \cdot 2^l \ge n$, repaired in 16 buckets | a Huffman code | no sort and no tree; a few bits more on some pages |
| the description in byte order, in a fixed code `OWN` | 256 lengths, or the bytes and their lengths | a byte value costs no bits, only its length ([chapter 4](#ch-format--own)) |
| the decode table built in the output page | in work memory | no byte more per CPU ([chapter 13](#ch-literals-dec--own)) |

The description was measured six ways, bytes per table of the page's own, counted offline on the tables
level 3 made on three samples of 20 000 pages, each way with a code built for it, the mean over the
samples
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-pages-own-literal-table-in-the-order-of-the-bytes-decoded-two-lengths-at-a-time-05-smaller-than-by-code-length-reads-29-slower-on-the-a76-kept)):

| how the lengths are sent | bytes per table |
| --- | ---: |
| by length, each byte as its distance to the one before, Exp-Golomb order 0 | 68.8 |
| the same, Exp-Golomb order 1 | 61.3 |
| the same, Exp-Golomb order 2 | 62.7 |
| by length, the distance's width in a prefix code, then its bits | 59.3 |
| in byte order, the lengths and runs in `OWN` (level 3) | 53.1 |
| in byte order, the changes of the length and runs | 53.0 |

An Exp-Golomb code needs no table. Order 0 is the code of a run in [chapter 4](#ch-format--own):
the number's width as zeros, then its bits, only shifted by one so that 0 has a code too. Order `j`
sends the `j` lowest bits of the number as they are, and what is left above them in order 0. A higher
order makes the small numbers longer and the large ones shorter:

| number | order 0 | order 1 | order 2 |
| ---: | --- | --- | --- |
| 0 | `1` | `10` | `100` |
| 1 | `010` | `11` | `101` |
| 2 | `011` | `0100` | `110` |
| 3 | `00100` | `0101` | `111` |
| 5 | `00110` | `0111` | `01001` |
| 8 | `0001001` | `001010` | `01100` |
| 20 | `000010101` | `00010110` | `0011000` |

E.g. 5 in order 1: its lowest bit is `1`, what is left above it is `10` in binary, 2, and 2 in order 0
is `011`; together `0111`. Order 1 fit the distances between the byte values best of the three, 61.3
bytes per table against 68.8 and 62.7. On 9th October the description was the fourth way, the distance's width in a fixed code
of 10 symbols. Exp-Golomb order 1 replaced it first: tables built 12% faster on the PC, for 2.0 bytes
more per table. Byte order then saved 6.2 bytes against the fourth way: the byte values cost no bits,
only the lengths and the runs between them, and a run is coded like the fourth way's distances, its
width in `OWN` and then its bits ([chapter 4](#ch-format--own)). The last two ways are 0.1 bytes apart.

The literal table of the page's own was tried three times before and not kept, for the reasons
[below](#ch-decisions--not-kept): it cost too much time for what it saved, and it was a second way to
code the literals, with its own rules for an invalid page, in the decoder and in both reference
decoders. This time it came together with the lazy matcher as one level, and the result above is what
changed the decision: `zstd` 3's memory, one end of the project's goal
([plan.md §1.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#11-the-score-memory-against-time-not-bars)).
The cost is in the code: `own_code()`, `own_lengths()` and their helpers in the encoder, `build_own()`,
and `own_table()` in the decoder, which parses one more structure of untrusted input; the fuzzers and
the reference decoders cover it like the rest.

## Tried and not kept {#ch-decisions--not-kept}

Each of these is an obvious idea with a number now. The common reasons: the gain is below what the time
is worth on the hull, or the format would get a second way of doing something.

### A hash chain at levels 3 and 4: a deeper search for the same format

An earlier level 3 and 4, before the one [above](#ch-decisions--level3): a hash chain priced by the tables' code lengths, with one or two lazy steps, for the same format and
decoder
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#levels-3-and-4-a-hash-chain-priced-by-the-tables-as-small-as-zstd-3-on-two-of-four-dumps-and-faster-to-write-and-read-not-kept)).
Level 3 stored 3.3% less than level 2, 22.7% of the pages' size against 22.2% for `zstd` 3, and wrote a
page in 1.8 times level 2's time, still 27% faster than `zstd` 3. In the VM it is just off the hull:
`zstd` 3 follows level 2 at 8.4 bytes per µs, level 3 would at 8.3, with the 0.34 reads per write of then.
It pays only where a µs of writing is worth less than about 8 bytes, which is `zstd`'s place, and it cost 850 lines in the codec, the tests and
the kernel port, and 12 KiB more work memory per CPU.

### A literal table of the page's own, the first three times

Built three times before level 3 kept it ([above](#ch-decisions--level3)). With `seqlz-opt`, cold p99 went from 4809 to 5849 ns and the scratch grew by 3.3 KB
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-opt-with-a-literal-table-per-page-less-memory-than-zstd-slower-reads-not-kept)).
In `seqlz-2` (#43, #44) writes were 14% slower for 2.4% less memory, with 4 token tables per page
as well 44% slower for 4.6%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept)).
At level 3 it gave 3.1% in the VM, but "a second way to code the literals: a table description with its
own rules for an invalid one, a parser of untrusted input in the decoder and in both reference decoders,
about 900 lines with the tests, for one level"
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-literal-table-of-the-pages-own-at-levels-3-and-4-25-to-34-less-a-second-literal-path-in-the-format-not-kept)),
and more for C6 to cover.

### Token tables per page or per context

4 token tables per page need all tokens known before the first is written, two passes for 5700 to 6400
cycles per page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept));
8 would save about 0.3 to 0.4 points, and matching first and encoding after cost 2300 cycles per page in
that estimate
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
A token table chosen by the offset class of the sequence before, 3 tables, saved 7.9 bytes per page in
the kernel, for 0.18 µs more per page written with the 0.34 reads per write of then, cold reads 0.14 to
0.18 µs slower in every boot, and 20 KiB
more tables
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tokens-table-by-the-offset-before-it-3-and-12-bytes-per-page-not-kept)).

### Recompression

zram can compress idle pages again with a second algorithm. `seqlz-opt`, an optimal parser for the same
format, took 237 µs per page for 83 bytes, 0.35 bytes per µs, where the step from `seqlz-2` to
`zstd` 3 for every page is 9. `zstd` 3 as the second algorithm recompresses 16 times as fast, but each
read of a recompressed page is then about 2.3 µs slower, and those reads come in bursts when an idle app
is used again
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#recompression-measured-not-pursued)).
`seqlz-opt` is removed, and no design depends on recompression.

### The matcher in assembly

On the A55, the loop over positions without a match in arm64 assembly, with table entries of 8 bytes that
hold the bytes and a page generation, compressed with 9% fewer cycles, and in the phone's kernel its
slowest writes were as fast as `lz4`'s. The same design in C gained 2.5% in the harness, nothing in the
phone's kernel, and needed 16 392 bytes of work memory instead of 8192
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matchers-table-with-the-bytes-its-loop-in-assembly-on-arm64-9-fewer-compress-cycles-on-the-a55-not-kept)).
The assembly is commit `0288b1e`, and the pull request went back to the C matcher (`f1db05d`): the codec
stays C, also for one architecture.

### Other ways to compress a page

| design | best result | why not | entry |
| --- | --- | --- | --- |
| word model, WKdm style: a 2-bit tag per 64-bit word | cold p99 350 ns below `lz4` | 55.7% of the page; even with every repeat of 8 bytes and more for 3 bytes, more than `lz4` | [word model](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#word-model-wkdm-style-64-bit-words), [with runs](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-word-model-with-a-path-for-runs-not-even-lz4s-memory-not-built) |
| BΔI, a base and deltas per 64-byte block | the fastest decoder measured, 960 ns below `lz4` at cold p99 | 70.5% | [BDI](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#base--delta-per-64-byte-block-bdi) |
| byte shuffle, then `lz4` | cheaper than `lz4` on a quarter of the pages | 40.9%; no cheap rule finds the pages where it pays | [shuffle](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#byte-shuffle--lz4), [#29 and #31](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-ideas-of-29-and-31-measured) |
| memlz, words found through a 16-bit hash | | 63.6%, 1.5 MB per CPU | [memlz](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#memlz) |
| `bytelz`, seqlz's matcher and a byte format | cold read p99 4560 instead of 5720 ns, compress 21 000 instead of 21 600 cycles | 29.4% against 26.4% then, on no hull | [bytelz](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#bytelz-seqlz-fasts-matcher-a-byte-oriented-format) |
| `lz4`'s format from a compressor for pages | reads 5% to 10% faster than `lz4`'s pages | at best `lzo-rle`'s memory for twice the write time | [lz4page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#lz4s-format-from-a-compressor-for-pages-measured-not-kept) |

The word model was the hope of the plan's Phase 2b: control flow that barely depends on the data might win
the p99 of a read. It did, by 350 ns, with 55.7% of the page against `lz4`'s 34.5%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#word-model-wkdm-style-64-bit-words)). Tricks on 8 or 64
bytes do not get the ratio; it comes from repeats across the whole page.

### Other ways to code the literals and the offsets

| idea | result | why not | entry |
| --- | --- | --- | --- |
| ANS instead of Huffman | 0.3% at most | a state machine on every symbol | [how tight](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2) |
| FSST, symbols of 1 to 8 bytes as one byte | about 40% of what the Huffman tables get | not faster either, 2.9 to 3.1 ticks per literal to decode against about 1.1 cycles | [one of 8 tables](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page) |
| Tunstall codes | 7.1 to 9.3 bits per byte, the entropy is 6.1 to 6.7 | dropped without building | [one of 8 tables](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page) |
| a literal XOR the byte at the repeat offset, as LZMA | 0.17 points | read p99 840 ns slower, write p99 1.4 µs | [one of 8 tables](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page) |
| a literal's table by its context, as Brotli | 5 to 15 bytes per page | 64 KiB of tables | [by the score](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8) |
| two literals per lookup | 12% faster with the data in the cache | slower cold | [prefetch](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression) |
| the literal coder in AVX2 | 2.53 instead of 3.09 ticks per literal | the kernel would need `kernel_fpu_begin()`, and arm64 a NEON version | [#29 and #31](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-ideas-of-29-and-31-measured) |
| offsets as whole bytes in a stream of their own | | 28.1% instead of 27.0%, no faster in the kernel | [prefetch](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression) |
| two sequence streams for the in-order A55 | | slower everywhere, 2.4 bytes per page more | [seqlz-1 on the phone](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-on-the-phone-time-goes-per-sequence-and-code-layout-moves-reads-by-130-ns) |

FSST has the clearest lesson: "what is left of a page after the matcher has no multi-byte structure worth
a symbol". The gain is all in how uneven the single bytes are, which a Huffman code uses.

### Outside the codec

Compressing neighbouring swap slots together in blocks of 16 KiB gives `zstd` 3 18% fewer bytes per page,
before zsmalloc, than `seqlz-2` on single pages, and a delta against a similar page 3.1 points
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-ideas-of-29-and-31-measured)).
Both are zram's work: an index, or more than one page decoded per fault. Neither is built.

## Where zstd still wins, and why {#ch-decisions--zstd}

`zstd` at level 3, zram's default level for it, stores pages 6% smaller than `seqlz-2` on a
desktop's zram pages, 1105 against 1183 bytes per page in `docs/seqlz.md`'s result table. As Σ zsmalloc
cost on all pages of the desktop's and the phone's zram
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#levels-3-and-4-a-hash-chain-priced-by-the-tables-as-small-as-zstd-3-on-two-of-four-dumps-and-faster-to-write-and-read-not-kept)):

| codec | Σ zsmalloc cost |
| --- | --- |
| `lz4` | 32.6% |
| `seqlz-2` | 23.5% |
| `zstd` 3 | 22.2% |

1. **Better matches.** `lz4hc`'s matches coded by seqlz take 3% to 7% fewer bytes than the greedy
   matcher's
   ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#not-evaluated-yet)),
   and a search that finds them takes more time than zram's writes can spend, as levels 3 and 4 showed.
2. **A literal table of the page's own.** Coding the literals is worth 3.5 points to `zstd` 3; a table per
   page instead of the best of 8 saved seqlz another 2.4%, 32 bytes per page, for 14% slower writes the
   first time it was measured, mostly on pages whose literals fit the 8 tables badly
   ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept)).
   Level 3 has one, and with it and a lazy step it stores pages as small as `zstd` 3 on the Mi 9T, in
   less than half its time to write ([above](#ch-decisions--level3)).
3. **Adaptive sequence coding.** `zstd` codes `ll`, `ml` and the offset with three tables it can build for
   the data, three lookups per sequence; seqlz's one fixed token table keeps its decoder fast. The offsets
   are the clearest loss: as plain bits they cost 18 bytes per page, 2%, above their entropy
   ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).

`zstd` pays about twice the time to write a page, 1.7 times the time to read one in a swap-in, and 186 112
bytes of work memory per CPU plus 91 496 per zram device, a dictionary it creates even without one, which
a series of October 2026 removes; seqlz needs 8 KiB, 16 KiB with zram's contexts split. No level changes
that: in a swap-in every `zstd` level that stores less than `seqlz-2` takes at least 7.9 µs more
per page written; `zstd` 1, the closest, stores 19 bytes per page less, 2.0%, and the fast negative levels
store more and are still slower
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zstd-at-every-level-in-a-swap-in-no-level-stores-less-in-less-time-than-seqlz-fast-lit)).

## What is not known yet {#ch-decisions--unknown}

([seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#what-is-not-known-yet) has the
same list.)

- **One desktop, one phone.** The pages are zram dumps of one desktop and one phone, a Mi 9T with Android
  11 and Linux 4.14, plus the Android 17 emulator, and the tables are trained on pages of the same desktop
  and phone. On another machine the tables may fit less well.
- **The exchange rate and `r`.** The 0.73 reads per write is one phone in one week of one person's use; a
  desktop had 0.34. The size of the bursts of swap-ins is not measured.
- **arm64.** Most times are x86-64. In the phone's own kernel, on its own pages, with the tables of 7th
  October 2026, `seqlz-2` stores 28% less than `lz4` and swaps in 13% slower warm and 25% slower
  cold on the A76, 11% and 17% on the A55
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tables-trained-again-4-less-on-one-desktop-dump-the-phone-the-same)).
  On the little core it has the lowest score only up to 12 bytes per µs. A current phone has not been
  measured.
- **What a user sees.** On the Mi 9T, with the same RAM given to zram for every codec and 25 apps switched
  in turn, no app had to start cold in 3 runs each of `seqlz-1` and `seqlz-2`, and 54 launches
  did in 3 runs of `lz4`; the mean launch was 154 ms lower with `seqlz-1`
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#apps-on-the-phone-with-the-same-ram-no-cold-launch-in-6-runs-of-seqlz-54-in-3-runs-of-lz4)).
  One phone, one set of apps, 3 runs per setting; 5 runs each could not tell `seqlz-1` and
  `seqlz-2` apart.
- **Writes at p99** take 1.20 times `lz4`'s time, a guard of the score.
- **The decoder in a fault.** About 110 mispredicted branches per page are left that a predictor gets
  right only on a page it has seen. The refill without its branch took out about 29 and was kept; every
  other attempt so far cost more than the misses.
- **16 KiB pages**: no phone with them yet, the format not fixed, and the split contexts' 48 KiB above
  `lz4`'s 32 KiB.
- **Whether zram wants a new algorithm at all.** The question went to linux-mm, linux-block and
  linux-kernel on 9th October 2026, with the phone's numbers, and is not answered yet
  ([plan.md §9](https://github.com/martinus/quetschn/blob/main/docs/plan.md#9-where-the-project-stands-and-the-next-actions)).
  The answer also decides the form, a zram backend or an algorithm of the crypto API; `lib/seqlz/` stays
  the same in either. A hardware `lz4` on new Qualcomm phones would not change the argument: even at a time
  of 0, `seqlz-2` stays on the hull from about 6 to 64 bytes per µs
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#lz4-in-hardware-a-what-if-seqlz-fast-lit-stays-on-the-hull-up-to-73-and-72-bytes-per-µs-instead-of-155-and-211)).

The merge argument is not the one the project started with. The plan hoped for a decoder faster than
`lz4`'s at p99; in a real swap-in it is 17% to 25% slower cold on the Mi 9T, and about 20% on the PC. What
it has is memory: about 28% less than `lz4` for a small cost in time, measured in the kernel
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tables-trained-again-4-less-on-one-desktop-dump-the-phone-the-same),
[explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-whole-page-fault-the-kernels-part-is-the-same-for-every-codec-the-gap-to-lz4-about-halves)),
with a decoder that is fuzzed, bounded and specified in a document two independent decoders were written
from.
