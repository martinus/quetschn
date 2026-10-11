# Why seqlz is shaped this way {#ch-decisions}

Every chapter of Part II ends a listing with a "why", and most of those answers are a measurement in
[`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md). Read one at a time, they look like a list of small tricks. They are
not. Almost all of them follow from four facts about zram from [chapter 1](#ch-zram): each page is
compressed alone, reading a page back is on the critical path of a program, the stored size is counted
in zsmalloc's size classes, and the code runs in the kernel, once per CPU. This chapter collects the
decisions by theme, says what each was measured against, what it gained, what it cost, and where in the
code it lives.

Two rules hold for all numbers below. First, each one is from the day it was measured, on the code and
the tables of that day; later changes make some of them stale, and the entries in `explored-designs.md`
do not repeat that every time. Second, the times come from different setups: cycles in a loop in
userspace, the kernel in a virtual machine on a Ryzen 9 7950X fixed at 4.5 GHz, or the Xiaomi Mi 9T
phone with its Cortex-A76 big core and Cortex-A55 little core. Each number says which. Two numbers
separated by "and" or a slash are the two desktop zram dumps, the first and the second, unless the text
says otherwise.

## The score decides, not bars {#ch-decisions--score}

The project's goal is in [`docs/plan.md`](https://github.com/martinus/quetschn/blob/main/docs/plan.md#1-goal-and-success-criteria): as close as possible to
`lz4`'s speed and to `zstd`'s stored size, without more work memory than `lz4`. Both ends cannot be
reached at once, so a design is judged by how much closer it gets. At first that was a set of bars, e.g.
"p99 compression latency within 1.2 times `lz4`'s". Bars turned out to be cliffs: a design that saves 4%
of memory failed because its p99 was 1 µs above `lz4`'s, without asking what 1 µs is worth. Since then
designs are chosen by a score ([plan.md §1.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#11-the-score-memory-against-time-not-bars)):

- **bytes**: zsmalloc memory per stored page, measured in the kernel VM;
- **time per page written**: the mean time to write a page, plus 0.34 times the mean time to read one;
- **score**: bytes plus lambda times time, where lambda, the exchange rate, is how many bytes one µs per
  page is worth to you.

The 0.34 is measured on one desktop with zram as its only swap: 3 146 179 pages swapped in against
9 210 320 swapped out in 27.5 days. The mean, not p99, because the mean is the total time spent, and a
burst of swap-ins after switching back to an app waits for all of them. p99 stays a guard against
pathological pages, not a target. lambda is not fixed: `quetschn-score` prints the codecs that have the
lowest score for some lambda, the lower left convex hull of (time, bytes), and the exchange rate between
neighbours on it. A design is worth keeping if it is on the hull at a lambda that matters.

Two limits stay hard and are not traded against anything. **C5**: the work memory per CPU is at most
`lz4`'s 16 416 bytes, because it is taken from every CPU whether it swaps or not. **C6**: the decoder is
safe and bounded in time for any input, because that is what stopped the last new codec for zram,
zBeWalgo, in 2018.

The result, from the table of [`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-result-lz4s-time-zstds-size-almost): the
kernel VM, 20 000 pages of each dump, writes and cold reads through zram's read benchmark, means over
the pages.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 315" role="img" aria-label="Stored bytes per page against time per page written for lz4, lzo-rle, seqlz-fast-lit and zstd 3 on two desktop dumps; seqlz-fast-lit sits near zstd's height at lz4's time.">
<text x="190" y="22" text-anchor="middle" font-size="13">first dump</text>
<line x1="60" y1="270.0" x2="320" y2="270.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="274.0" text-anchor="end" font-size="11">0</text>
<line x1="60" y1="212.5" x2="320" y2="212.5" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="216.5" text-anchor="end" font-size="11">500</text>
<line x1="60" y1="155.0" x2="320" y2="155.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="159.0" text-anchor="end" font-size="11">1000</text>
<line x1="60" y1="97.5" x2="320" y2="97.5" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="101.5" text-anchor="end" font-size="11">1500</text>
<line x1="60" y1="40.0" x2="320" y2="40.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="54" y="44.0" text-anchor="end" font-size="11">2000</text>
<line x1="60.0" y1="270" x2="60.0" y2="274" stroke="currentColor" fill="none"/>
<text x="60.0" y="286" text-anchor="middle" font-size="11">0</text>
<line x1="103.3" y1="270" x2="103.3" y2="274" stroke="currentColor" fill="none"/>
<text x="103.3" y="286" text-anchor="middle" font-size="11">3</text>
<line x1="146.7" y1="270" x2="146.7" y2="274" stroke="currentColor" fill="none"/>
<text x="146.7" y="286" text-anchor="middle" font-size="11">6</text>
<line x1="190.0" y1="270" x2="190.0" y2="274" stroke="currentColor" fill="none"/>
<text x="190.0" y="286" text-anchor="middle" font-size="11">9</text>
<line x1="233.3" y1="270" x2="233.3" y2="274" stroke="currentColor" fill="none"/>
<text x="233.3" y="286" text-anchor="middle" font-size="11">12</text>
<line x1="276.7" y1="270" x2="276.7" y2="274" stroke="currentColor" fill="none"/>
<text x="276.7" y="286" text-anchor="middle" font-size="11">15</text>
<line x1="320.0" y1="270" x2="320.0" y2="274" stroke="currentColor" fill="none"/>
<text x="320.0" y="286" text-anchor="middle" font-size="11">18</text>
<line x1="60" y1="270" x2="320" y2="270" stroke="currentColor" fill="none"/>
<line x1="60" y1="40" x2="60" y2="270" stroke="currentColor" fill="none"/>
<text x="190" y="304" text-anchor="middle" font-size="11">time per page written, µs</text>
<text x="16" y="155" text-anchor="middle" font-size="11" transform="rotate(-90 16 155)">stored bytes per page</text>
<polyline points="147.2,113.5 166.3,151.0 280.9,153.6" stroke="currentColor" stroke-dasharray="4 3" stroke-opacity="0.6" fill="none"/>
<circle cx="148.7" cy="103.2" r="4" class="fill-paper" stroke="currentColor"/>
<text x="156.7" y="97.2" text-anchor="start" font-size="12">lz4</text>
<circle cx="147.2" cy="113.5" r="4" class="fill-paper" stroke="currentColor"/>
<text x="139.2" y="127.5" text-anchor="end" font-size="12">lzo-rle</text>
<g class="acc"><circle cx="166.3" cy="151.0" r="5" class="fill-acc" stroke="currentColor"/><text x="172.3" y="169.0" text-anchor="start" font-size="12">seqlz-fast-lit</text></g>
<circle cx="280.9" cy="153.6" r="4" class="fill-paper" stroke="currentColor"/>
<text x="280.9" y="143.6" text-anchor="middle" font-size="12">zstd 3</text>
<text x="550" y="22" text-anchor="middle" font-size="13">second dump</text>
<line x1="420" y1="270.0" x2="680" y2="270.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="414" y="274.0" text-anchor="end" font-size="11">0</text>
<line x1="420" y1="212.5" x2="680" y2="212.5" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="414" y="216.5" text-anchor="end" font-size="11">500</text>
<line x1="420" y1="155.0" x2="680" y2="155.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="414" y="159.0" text-anchor="end" font-size="11">1000</text>
<line x1="420" y1="97.5" x2="680" y2="97.5" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="414" y="101.5" text-anchor="end" font-size="11">1500</text>
<line x1="420" y1="40.0" x2="680" y2="40.0" stroke="currentColor" stroke-opacity="0.15" fill="none"/>
<text x="414" y="44.0" text-anchor="end" font-size="11">2000</text>
<line x1="420.0" y1="270" x2="420.0" y2="274" stroke="currentColor" fill="none"/>
<text x="420.0" y="286" text-anchor="middle" font-size="11">0</text>
<line x1="463.3" y1="270" x2="463.3" y2="274" stroke="currentColor" fill="none"/>
<text x="463.3" y="286" text-anchor="middle" font-size="11">3</text>
<line x1="506.7" y1="270" x2="506.7" y2="274" stroke="currentColor" fill="none"/>
<text x="506.7" y="286" text-anchor="middle" font-size="11">6</text>
<line x1="550.0" y1="270" x2="550.0" y2="274" stroke="currentColor" fill="none"/>
<text x="550.0" y="286" text-anchor="middle" font-size="11">9</text>
<line x1="593.3" y1="270" x2="593.3" y2="274" stroke="currentColor" fill="none"/>
<text x="593.3" y="286" text-anchor="middle" font-size="11">12</text>
<line x1="636.7" y1="270" x2="636.7" y2="274" stroke="currentColor" fill="none"/>
<text x="636.7" y="286" text-anchor="middle" font-size="11">15</text>
<line x1="680.0" y1="270" x2="680.0" y2="274" stroke="currentColor" fill="none"/>
<text x="680.0" y="286" text-anchor="middle" font-size="11">18</text>
<line x1="420" y1="270" x2="680" y2="270" stroke="currentColor" fill="none"/>
<line x1="420" y1="40" x2="420" y2="270" stroke="currentColor" fill="none"/>
<text x="550" y="304" text-anchor="middle" font-size="11">time per page written, µs</text>
<text x="376" y="155" text-anchor="middle" font-size="11" transform="rotate(-90 376 155)">stored bytes per page</text>
<polyline points="515.5,68.2 516.1,76.9 537.1,116.9 655.2,132.3" stroke="currentColor" stroke-dasharray="4 3" stroke-opacity="0.6" fill="none"/>
<circle cx="515.5" cy="68.2" r="4" class="fill-paper" stroke="currentColor"/>
<text x="523.5" y="62.2" text-anchor="start" font-size="12">lz4</text>
<circle cx="516.1" cy="76.9" r="4" class="fill-paper" stroke="currentColor"/>
<text x="508.1" y="90.9" text-anchor="end" font-size="12">lzo-rle</text>
<g class="acc"><circle cx="537.1" cy="116.9" r="5" class="fill-acc" stroke="currentColor"/><text x="541.1" y="140.9" text-anchor="start" font-size="12">seqlz-fast-lit</text></g>
<circle cx="655.2" cy="132.3" r="4" class="fill-paper" stroke="currentColor"/>
<text x="655.2" y="122.3" text-anchor="middle" font-size="12">zstd 3</text>
</svg>
</div>
<figcaption>Stored bytes per page against time per page written (write + 0.34 × cold read), kernel VM, both desktop dumps; the dashed line joins the codecs on the hull. Lower left is better.</figcaption>
</figure>

On the first dump `seqlz-fast-lit` saves 247 bytes per µs more than `lzo-rle`, and `zstd` 3 saves only 3
bytes per µs more than `seqlz-fast-lit`; on the second dump 238 and 16. The chart leaves out
`seqlz-fast`, which was measured in the same boot: with it on the hull, `seqlz-fast-lit` has the lowest
score from 3 to 153 bytes per µs on the first dump and from 16 to 190 on the second. Below that range
`zstd` is the better choice, above it `seqlz-fast`
([The numbers again](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-numbers-again-with-the-format-as-it-is-now-reads-03-µs-faster-than-on-29th-september)).
These numbers have the tables seqlz had before 7th October 2026, when they were trained again.

The read benchmark of that chart decodes every page several times, which trains the branch predictor on
the page and flatters seqlz. With the times of a real swap-out and swap-in, each page compressed and
decompressed once, the upper end of the range falls. With the tables of 7th October 2026,
`seqlz-fast-lit` has the lowest score from 3.1 to 155 bytes per µs on the first dump and from 10.3 to 211
on the second, against 3.2 to 200 and 10.5 to 279 in the read benchmark of the same boots, with
`seqlz-fast` between it and `lzo-rle`
([The score with the swap times](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-score-with-the-swap-times-seqlz-fast-lits-range-ends-at-155-and-211-bytes-per-µs-instead-of-200-and-279)).
In such a swap-in the codec is not all of the time: on the PC about 1.9 µs of a fault are the kernel's
for every codec, so a decoder 40% slower than `lz4`'s makes a swap-in about 20% slower
([The whole page fault](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-whole-page-fault-the-kernels-part-is-the-same-for-every-codec-the-gap-to-lz4-about-halves)).

## The overall bet: code the sequences, with tables the page does not carry {#ch-decisions--bet}

### The gap to zstd is how the sequences are coded

The most useful single measurement of the project came before seqlz existed
([Where the ratio of `zstd` comes from](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#where-the-ratio-of-zstd-comes-from)). It took the
matches that `lz4hc` level 9, `lz4`'s slow and thorough compressor, finds on the first desktop dump, and
priced the same matches written in different ways, as Σ zsmalloc cost, the memory zsmalloc needs for all
pages as a share of their size:

| the same `lz4hc` level 9 matches, written as | Σ zsmalloc cost |
| --- | --- |
| `lz4`'s format, bytes | 30.7% |
| `ll`, `ml` and offset entropy coded, literals raw | 25.8% |
| ... and repeat offsets like `zstd`'s | 25.1% |
| ... and the literals entropy coded too | 23.6% |

The coded rows are an estimate with ideal code lengths and a model trained on the same pages, so they
are on the optimistic side.

Better matches within `lz4`'s format end at 30.7%, and `lz4` itself is at 34.5%. `zstd -1`, which codes
no literals, is at 26.9%. So the largest step from `lz4` towards `zstd` is not a better search and not
the literals: it is writing the three numbers of each sequence in bits instead of bytes. `lz4` spends a
byte on every token and two on every offset, where an offset in a 4 KiB page needs 12 bits at most. With
the matches of `lz4hc` level 3 instead of 9 the same rows are 31.3%, 26.1%, 25.4% and 23.7%: once the
sequences are coded well, the quality of the matcher barely matters. That is why seqlz has a cheap
greedy matcher ([chapter 9](#ch-matcher)) and spends its effort on the format ([chapter 4](#ch-format)).

### Fixed tables, because a page is too small to carry its own

`zstd` writes a description of its Huffman tables into the compressed data, or picks predefined ones. A
page of 4 KiB that compresses to 1000 bytes has no room for that, and building a table from a
description costs the decoder time on every read. So seqlz's 11 tables are fixed, trained once on many
pages, and part of the format: the token table, two tables for length values, and 8 literal tables
([chapter 8](#ch-codes)). The encoder's codes and the decoder's lookup tables are built from them once,
43 280 bytes shared by all CPUs.

What fixed tables cost was measured from both sides. Tables trained on the very dump they are measured
on, which no real table can be, are only 1.5% and 2.5% smaller on the desktop dumps and 0.7% on the
phone ([The tables trained again](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tables-trained-again-4-less-on-one-desktop-dump-the-phone-the-same)).
A literal table built for each page saves 0.3% and 4.6% for writes 13% and 15% slower, see
[below](#ch-decisions--not-kept).

The training: k-means over the literal histograms of pages, 32 starts and up to 60 rounds, on a corpus of
524 912 pages, the desktop's resident pages, 60 132 swapped pages of the desktop and the first phone dump
5 times. Each copy of the phone dump gives some of the desktop's gain back for the phone; with 5 the
phone is within 0.07% of the tables before and the second desktop dump 4% smaller. Training on more
starts did not help: the start with the fewest bits on the training pages is not the one with the
fewest on other pages.

Tables per device were measured too, sent as zram's dictionary
([The format written down](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-format-written-down-one-set-of-tables-and-stream-sizes-that-hold)).
Tables trained on one zram dump of the Mi 9T saved 0.3% and 1.0% on the phone's other dump, cost 0.9% and
1.0% on the desktop, and saved only 1.1% to 1.7% on their own pages. So seqlz takes no dictionary: the
kernel's backend refuses one with `-EOPNOTSUPP`. That also answers zram's dictionary
criterion: seqlz needs no dictionary to beat `lz4` with one, 20.4% less than `lz4` with a dictionary on
the desktop's zram dump of 28th September 2026, in the quick benchmark ([`lz4` with a dictionary](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#lz4-with-a-dictionary)).

### Huffman codes, not ANS

`zstd` codes its sequences with FSE, a form of ANS that can spend a fraction of a bit on a symbol. A
static Huffman code cannot. How much that loses was bounded with Huffman codes fitted to each dump
against the entropy of the same counts: 1.4 to 3.5 bytes per page, 0.2% to 0.3%
([How tight the bits are](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).
An ANS decoder is a state machine stepped on every symbol, which is a chain the decoder waits for. For
0.3% at most, seqlz stays with prefix codes, decoded with one table lookup.

### One token per sequence, and a format built for the decoder

zram reads a page while a program waits, and writes most pages in the background. So when the format
had a choice, it took the one that makes the decoder's chain from one sequence to the next shorter. The
first prototype had three symbols per sequence, `ll`, `ml` and the offset's size, each with its own table
lookup. Folding both lengths into one token, as `lz4`'s token byte does, cut the decode loop from 14 010
to 10 930 cycles per page, 22%
([seqlz](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-lz4s-matches-huffman-coded-sequences-with-static-tables));
putting the offset class into the token as well cut it from 8758 to 8168 cycles, 7%
([seqlz, third decoder round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
Since then a sequence costs the decoder one table lookup, a shift, and the next lookup.
[Chapter 14](#ch-decode) is that loop.

## The format {#ch-decisions--format}

Each row is one decision of [chapter 4](#ch-format), measured against the other way in the second
column, with the entry that has the numbers.

| decision | instead of | gain | price | entry |
| --- | --- | --- | --- | --- |
| `ll`, `ml − 4` and the offset class in one token | a symbol each | 22% fewer decode cycles for the lengths, 7% fewer for the offset class | | [seqlz](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-lz4s-matches-huffman-coded-sequences-with-static-tables) |
| 4 bits of `ll`, 5 of `ml` in the token | 4 and 4 | a length value after 8.5% of the matches instead of 18.7%, 9180 instead of 9400 decode cycles | a larger token alphabet | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes) |
| one repeat offset | three, as `zstd` | 17% fewer decode cycles, 89 instead of 124 mispredictions per page | 26.6% instead of 26.4% | [third decoder round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow) |
| two classes for multiples of 8 | plain offsets only | 24 and 1.9 bytes per page in the kernel | 0.20 µs per page written, over both dumps | [by the score](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8) |
| token codes of at most 11 bits | 12 bits | cold p99 4350 instead of 5070 ns, measured with entries of 2 bytes | 576 of 3072 tokens have a code, the rest is escaped | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes) |
| the bitstream read most significant bit first | least significant first | a page touches 42.5 instead of 55 of the token table's 128 cache lines | none, the same bytes | [bit order](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept) |
| only complete prefix codes | a check for invalid codes | 6% of the decoder's instructions | | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes) |
| trained codes for length values | the symbol in 5 plain bits | 1.6 to 2.8 bytes per page | two more tables in the format | [5 plain bits again](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-again-02-to-03-more-memory-no-read-faster-not-kept) |
| a page ends exactly where its bits end | trailing bytes ignored | a damaged page is rejected | 0.07 to 0.10 µs per warm read on the A55 | [strict end](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-page-ends-where-its-bits-end-no-damaged-page-with-bytes-or-bits-too-many-decodes-01-µs-on-the-a55-kept) |
| no version in the page | a version byte | none needed | a format change needs a new kernel | [format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#no-version-in-the-page) |

The rows, one by one, with what the table cannot say.

### The token: 4 bits, 5 bits and 6 classes

A token is `min(ll, 15) + 16 × min(ml − 4, 31) + 512 × class`, $16 \times 32 \times 6 = 3072$ values
([chapter 6](#ch-header)). The split of bits between the lengths was measured when the token had 3
classes and codes of at most 11 bits, before the escape existed: 3 and 5 bits gave 26.9% at 8599 decode
cycles, 4 and 4 gave 27.0% at 8482. The escape then let 4 and 5 bits fit, at 26.8% and 8480 cycles
([seqlz, third decoder round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
With 5 bits for the match length, a match of up to 34 bytes is the token alone. Earlier, with a token
of 512 symbols, 5 bits for the match length instead of 4 had cut the matches with a length value from
18.7% to 8.5%, at the same memory, and decoding went from 9400 to 9180 cycles per page
([Second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).

A fifth bit for the literal length was not tried: it would double the alphabet again, and codes of at
most 11 bits have room for 2048 codes
([The fast path's condition](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-fast-paths-condition-its-22-misses-per-page-are-the-length-values-four-ways-around-them-slower-not-kept)).

### Offset classes, and why multiples of 8 have two of their own

The offset class says how the offset is stored: class 0 is the repeat offset in 0 bits, classes 1 to 3
are offsets below 16, below 256 and below the page size in 4, 8 and 12 plain bits, and classes 4 and 5
are multiples of 8 sent divided by 8, in 5 and 9 bits. The last two are the newest, and the reason is
what memory pages hold: arrays of 8-byte values. Of the offsets from 16 to 255 that are not the repeat
offset, 72% and 56% are multiples of 8; from 256 on, 62% and 41%. The low 3 bits of an offset carry 1.8
and 2.4 bits of information, not 3. In the kernel the two classes saved 24 and 1.9 bytes per page, for 0.20 µs more per page
written over both dumps: 65 bytes per µs, against 10 for the step from `seqlz-fast-lit` to `zstd` 3 in
the same boots, with the tables of then
([seqlz-fast-lit by the score](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)). The
decoder still does one table lookup per sequence, and one more shift. The price is in the format: the
token alphabet grew from 2048 to 3072, so the escape needs 12 bits.

Other layouts were searched and not taken. A search over 29 539 layouts of classes found at most 0.15
bits per sequence, about 0.1 points
([Offset classes from a histogram](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-classes-from-a-histogram-todays-are-close-two-more-give-01-points-at-most)).
Offsets 2 and 8 as classes of their own, since they are 70% to 85% of class 1, made pages 0.6% to 0.9%
smaller and the warm read 1.4% slower on both phone cores, and were not built for a format change of
that size
([How tight the bits are](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).
Class 1 for offsets 1 to 7 only, so that a short offset is known before its bits are read, was larger,
27.1% instead of 27.0%, and took 7625 instead of 6883 decode cycles per page; the cold p50 was faster,
1410 against 1470 ns, the cold p99 slower, 3280 against 3040 ns
([zram: prefetch the compressed data](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).

### One repeat offset, not three

`zstd` keeps three recent offsets and moves the one used to the front. seqlz's first prototype did the
same. The move to front cost the decoder 17% of its cycles, and the second and third offsets were only
11% of the matches. With one repeat offset the pages grew from 26.4% to 26.6%, and the mispredictions per
page fell from 124 to 89, fewer than `lz4`'s 94
([seqlz, third decoder round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
The repeat offset starts at 1 on every page, so a run of equal bytes is a match at offset 1 from the
start.

### 11-bit token codes, and the escape

The decoder finds a token's code by looking up the next 11 bits in a table of 2048 entries of 4 bytes,
8 KiB ([chapter 12](#ch-dec-basics)). The limit comes from the cache: a table of 12-bit codes had 4096
entries, and with the other tables it no longer stayed in L1. Measured with the token entries of 2 bytes
of that time, the 11-bit table took the cold p99 from 5070 to 4350 ns
([Second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)). The limit, not the method of
building the code, is what decides the bits: package-merge, the optimal length-limited code, gave the
same token bits as halving the counts
([seqlz, third decoder round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).

2048 codes are fewer than 3072 tokens. Only 576 tokens have a code of their own; the others share one
code, the escape, followed by the token in 12 plain bits, 16 bits for a rare token. On the phone 10 bits
were measured again: cold reads 2.0 and 2.3 µs faster on the A76 and 2.4 and 2.3 on the A55, for 4.8
bytes per page on the phone's pages
([Six choices made on the PC, item 1](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#1-the-token-table-in-10-bits-cold-reads-2-to-3-µs-faster-on-both-cores-for-3-to-6-bytes)).
With the bit order below, and the token table then prefetched on in-order cores only, 10 bits gave the
A76 nothing and the A55 2 to 2.6 µs of cold reads for the same 4.8 bytes
([The sequences' bitstream most significant bit first](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept)).
11 bits stayed.

### The bit order: most significant bit first

The bitstream was read lowest bit first at first. Then the entries of a code of $l$ bits are spread over
the whole token table, every $2^l$ entries. Read highest bit first, the next token is the top 11 bits of
the register, and a code's entries are one range of the table: a page touches 42.5 instead of 55 of
the table's 128 cache lines, counted on 20 000 pages
([The sequences' bitstream most significant bit first](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept)).
The bytes of every page are the same. On the PC it cost nothing, 7370 against 7410 decode cycles per
page. On the A76, with the token table not prefetched there as it was then, it made a faster cold read
hold from one build of the module to the next, where the spread layout depended on where the module's
tables landed. Since #152 every core prefetches the token table.

### Complete codes only

Every code in seqlz's tables is complete: the codes use up every bit pattern, so every 11 bits the
decoder looks up start some code, and the decode table has no empty entry. The decoder needs no check
for an invalid code, which was 6% of its instructions
([Second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)). The trainer,
`seqlz_tables_init()` and, since commit `4c42cc5`, the generator of the kernel copy's tables check
completeness, see [chapter 8](#ch-codes).

### Length values: trained codes, measured twice

A literal run of 15 or more bytes, or a match of 35 or more, needs a length value after the token: a
symbol from a small table and some plain bits. Two tables of 24 symbols each, 26 with 16 KiB pages, are
part of the format for that. Replacing their codes with the symbol in 5 plain bits would take two tables out of the format, the
trainer and the decoder's prefetch. It was measured twice. The first time the pages grew by 3.2 and 2.8
bytes ([Length values in 5 plain bits](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-instead-of-their-tables-3-bytes-per-page-more-not-kept)).
The second time, on 10th October 2026 (commit `e49b9dd`), with the decoder of that day and timed: 1.6 to
2.8 bytes more per page, 0.2% to 0.3%, no read faster than a second copy of the same source in the VM,
and gcc's writes in the kernel 1.8% to 3.0% slower
([Length values in 5 plain bits again](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-again-02-to-03-more-memory-no-read-faster-not-kept)).
The tables cost the format two tables once; the 5 bits would cost every page 2 to 3 bytes. The tables
stay.

### The end of a page, and no version

There is no count of sequences in the page: the last sequence has literals only, and the bitstream ends
where the compressed page ends. That saved 0.1 points, 26.7% to 26.6%, when it was introduced
([seqlz, third decoder round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)). Since
8th October 2026 a page is valid only if it ends exactly in its last byte with the trailing bits zero,
and so does each literal stream
([A page ends where its bits end](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-page-ends-where-its-bits-end-no-damaged-page-with-bytes-or-bits-too-many-decodes-01-µs-on-the-a55-kept)).
zram keeps no checksum, so this strict end is what rejects a damaged page with bytes too many, as `lz4`
rejects input it did not use. It cost nothing in the VM and 0.07 to 0.10 µs per warm read on the A55.
Since 11th October 2026 the last sequence has no token either: the decoder knows it from the literals
left, which saves 1.2 bytes per page, 0.12% of zsmalloc memory, at no time the VM or the phone could
measure
([The last sequence without a token](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-last-sequence-without-a-token-12-bytes-per-page-less-kept)).

The page has no version byte. zram's pages never outlive the kernel that wrote them, so a new format
needs a new kernel, not a new algorithm name
([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#no-version-in-the-page)). The values of byte 2 with its top bits set are invalid
today and stay free for a format that has to tell itself apart.

## The literals {#ch-decisions--literals}

Literals are 66% and 69% of `seqlz-fast`'s output on the two dumps. Coding them is worth 2.3 and 4.7
points to `zstd` 3. So the second level of seqlz, `seqlz-fast-lit`, Huffman codes them; the first level,
`seqlz-fast`, stores them raw. Both are one format and one decoder ([chapter 11](#ch-literals-enc),
[chapter 13](#ch-literals-dec)).

### 8 fixed literal tables, the page picks one

One table for all pages saved 7.3% and 6.3% of `seqlz-fast`'s output. k-means over the literal
histograms gives K tables, each the Huffman code of the pages that chose it, and the page takes the one
that codes it in the fewest bits
([seqlz-fast-lit: one of 8 literal tables per page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)):

| tables, longest code | first dump | second dump |
| --- | --- | --- |
| 1, 11 bits | 7.3% | 6.3% |
| 4, 11 bits | 9.2% | 9.1% |
| 8, 10 bits | 10.2% | 12.5% |
| 16, 11 bits | 10.6% | 13.1% |
| 64, 11 bits | 11.4% | 14.4% |

8 tables of 10 bits get most of it. Each has a decode table of 1024 entries of 2 bytes, 2 KiB, and a
page reads only the one it uses. 16 tables were measured three times, in userspace, in the kernel VM and on
the phone. In the kernel VM the twice larger decode
tables made cold reads 0.26 and 0.28 µs slower, on the hull only for lambda 10 to 13
([by the score](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)). On the Mi 9T they
saved 1.9 bytes per page for 2.0 µs more per write on the A55, about 1 byte per µs
([How tight the bits are](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).
Codes of 9 bits instead of 10 made no read faster and the second desktop dump 64 bytes per page larger
([Six choices, item 2](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#2-the-literal-tables-in-9-bits-no-faster-up-to-64-bytes-larger-not-kept)).

Two details of the trainer matter, each found with a table that no page chose: a page whose literals no
table codes $\frac{1}{16}$ smaller counts for no table, and a table without pages starts again from the page whose
literals cost the most per byte.

### 8 streams, which have nothing to do with the 8 tables

Coded literals are split into 8 streams, literal `k` into stream `k mod 8`, so that the decoder runs 8
independent chains of "shift, look up, shift" side by side ([chapter 3](#ch-cpu)). With 4 streams a
literal took about 2.9 cycles. With 8, the decode loop went from 9070 to 8676 cycles per page, and in the
kernel the cold read from 2940 / 4810 to 2860 / 4531 ns at p50 / p99; the header grew by 8 bytes then
([seqlz-fast-lit: one of 8 literal tables per page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
That 8 is also the number of literal tables is a coincidence.

4 streams were measured twice more, since they store 1.1 to 2.8 bytes per page less: on the phone no
faster ([Six choices, item 3](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#3-4-literal-streams-instead-of-8-27-bytes-no-faster-not-kept)), in
the VM 1.8% slower with gcc and 0.7% with clang
([Five simplifications](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).

### Coded only where it saves 1/16 and 51 bytes

The encoder codes a page's literals only if the coded streams plus 51 bytes are less than $n - n/16$,
for $n$ literals (`SEQLZ_LIT_CODED_MIN`, [chapter 11](#ch-literals-enc)). Both parts have a reason.
Coding wherever it saves anything gave less than 0.1 points more, for decoding literals on every such
page
([seqlz-fast-lit: one of 8 literal tables per page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)). And a page with coded literals costs the decoder a fixed time on top of its literals: its literal
table has to come from memory, and the literals are decoded into a buffer first. On the in-order A55
that fixed time is large. Raising the minimum from 19 to 51 bytes cost 7.9 bytes per page on the phone
and saved 2.7 µs per page written on the A55 and 1.1 µs on the A76; on the PC it was 11 bytes for 0.1 µs
([Coded literals only where they save 51 bytes](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-only-where-they-save-51-bytes-27-µs-per-page-written-less-on-the-a55-kept)).
51 is the best of three values measured, 19, 51 and 115, not a measured optimum. With the prefetches
added later, the gain shrank to 0.4 µs on the A76 and 0.8 µs on the A55, and 51 stayed "with less
reason than it had"
([Six choices, item 9](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#9-the-51-bytes-of-coded-literals-only-where-they-save-51-bytes-again-with-item-4s-and-7s-prefetches)).

### Stream sizes in `w` bits

The 8 stream sizes were 2 bytes each, a header of 19 bytes. Most streams are below 128 bytes, so the
sizes are now written in `w` bits each, the width the largest needs, 5 to 12, and byte 2 of the header
holds $w - 5$. The header is $3 + w$ bytes, mostly 9 to 11, and pages are 5.2 and 5.5 bytes smaller
([Stream sizes in as many bits as the largest needs](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#stream-sizes-in-as-many-bits-as-the-largest-needs-kept)).
Letting the smaller header code more pages would have saved 2.7 and 1.7 bytes more on the desktop. On
the phone's first dump those extra coded pages were worth 2 bytes per page for 251 ns more per page
written on the A55, 8 bytes per µs, so the rule kept counting 19 bytes, the old header, instead of the
real one. That 19 later became the minimum of 51 above.

### Pricing all 8 tables at once

To pick the table, the encoder needs the bits of all literals in all 8 tables. Each byte's code lengths
in all 8 tables are packed into the 8 bytes of one 64-bit word, so one add per literal prices all 8 tables
at once, and every 25 literals of a stream the sums move into wider lanes before a byte can overflow
(SWAR, [chapter 11](#ch-literals-enc)). In the kernel that pricing costs 30 to 40 ns per write. Writing the
streams is what costs. Pricing only a sample of the literals could save 400 cycles at most, because the
exact stream sizes are needed anyway
([seqlz-fast-lit: one of 8 literal tables per page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
One stream after the other waited on its accumulator, about 3.5 cycles per literal, so the encoder
writes several streams side by side
([Numbers that were only in the codec's comments](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).

### The buffer: literals in front, bitstream from the back, the matcher's table as scratch

The encoder writes the literals to the front of its output and the bitstream backwards from the end, and
copies the bitstream behind the literals at the end ([chapter 10](#ch-encoder)). Before, the bitstream
went behind room for a whole page of literals, so the compressor needed a buffer of two pages. zram gives
two pages, zswap one. The change wrote the same bytes and made writes in the VM 0.16 to 0.24 µs faster
([The compressor into a buffer of any size](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-into-a-buffer-of-any-size-the-bitstream-from-the-back-the-same-bytes-in-zram-writes-2-faster-in-the-vm-kept)).
Coding the literals needs the raw ones while the coded ones are written. They are copied into the
matcher's 8 KiB hash table, which is free once the matcher is done, so the coded ones can go where the
raw ones were. In a buffer of exactly one page that costs 0.08% to 0.50% more memory instead of 1.6% to
2.9%. In zram's two pages it wrote 0.08 to 0.18 µs faster in the VM, partly from the order of the
codecs in the run, since `lz4` was 0.02 to 0.06 µs faster too; on the Mi 9T the time was the same
([Coded literals from the matcher's table](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-from-the-matchers-table-a-buffer-of-one-page-costs-01-to-05-instead-of-16-to-29-kept)).

### Raw or coded: a choice for the device

On the phone's little core, coded literals are dear. In the harness on the phone, on its second dump, `seqlz-fast-lit`
stores 55 bytes per page less than `seqlz-fast`, for 4.2 µs more per page written and 1.6 times the cold
p99 on the A55 ([A second phone dump](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-second-phone-dump-after-12-hours-of-use-seqlz-fast-reads-faster-than-lzo-rle-at-p99)).
On the A55, with the first desktop dump's pages in the phone's module and the tables before 7th October,
`seqlz-fast-lit` has the lowest score only up to 16 bytes per µs, `seqlz-fast` up to 36
([The numbers again, with the bit order](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-numbers-again-with-the-bit-order-and-the-token-tables-prefetch-cold-reads-on-the-a76-3-µs-faster)). That
is why both levels stay, as zram's level parameter: level 1 for raw literals, level 2, the default, for
coded ones.

## The matcher {#ch-decisions--matcher}

The matcher ([chapter 9](#ch-matcher)) is where the write time goes. "The matcher alone costs as much as
all of `lz4`": in the third round of compressor work it was 17 300 cycles per page, `lz4` 17 100 for
everything
([The compressor, third round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-third-round-ideas-from-the-literature)). Even a free
encoder would leave the compressor at `lz4`'s speed. So every decision here trades bytes against the
work per position.

| decision | instead of | gain | price | entry |
| --- | --- | --- | --- | --- |
| greedy, the encoder in the same loop | matcher, then encoder | 22 848 instead of 24 257 cycles per page | | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions) |
| both candidates read, one branch decides | a branch per candidate | 329 instead of 539 mispredictions per page | 7% with the page in the cache | [seqlz-fast](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-seqlz-fast) |
| a hash of 5 bytes | 4 bytes | write p99 in the kernel 10 480 instead of 11 650 ns | 0.6 points, with the tables of then | [prefetch, 5 bytes](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression) |
| 4096 slots of 2 bytes, 8 KiB | 2048 or 8192 | | | [item 5](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#5-the-matchers-hash-5-bytes-into-4096-slots-stays) |
| the repeat offset at every position | the table only | 0.5 points | | [seqlz-fast](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-seqlz-fast) |
| the table cleared for each page | checking each entry's age | 7% faster | an 8 KiB `memset` per page | [second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions) |
| every position tried | `lz4`'s growing step | writes 0.21 and 0.25 µs faster in the VM, 0.05 µs in boots with all four codecs | pages stored raw 2.3 µs slower | [no step](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept) |
| the next position loaded ahead | one position at a time | 3.4% fewer cycles on the A55 | | [in-order core](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-on-an-in-order-core-34-fewer-compress-cycles-on-the-a55-same-output-kept) |

### Greedy, in one pass

At every position the matcher checks two candidates, the repeat offset and the one position the hash
table holds for the next 5 bytes, takes the first match it finds, extends it backwards and forwards, and
hands the sequence to the encoder at once. Splitting that into a matcher that fills an array and an
encoder after it cost 24 257 instead of 22 848 cycles per page: the encoder's branches lost the history
of the matcher's branches, which had predicted them
([The compressor, second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).

Searching harder was measured many times and always cost more than it saved
([chapter 9](#ch-matcher) has the code that stayed). One step of lazy matching: 24 808 instead of
20 400 compress cycles, 21% more, for no smaller pages at the time
([zram: prefetch the compressed data](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)). A
hash chain with one lazy step: 24.0% and 31.0% instead of 24.6% and 31.8%, for 39 500 cycles and 2.3
times the mispredictions
([seqlz-fast-lit: one of 8 literal tables per page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
The 2 newest positions per hash, in the same 8 KiB: 11 bytes per page for about 1 µs, about 11 bytes per
µs, at the edge of the hull
([by the score](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)). The full version
of this, levels 3 and 4, is in [Tried and not kept](#ch-decisions--not-kept).

Both candidates are read before either is compared, so that one branch decides whether this position has
a match. Three branches per position had mispredicted almost twice as often as `lz4`'s one. With one,
the mispredictions fell from 539 to 329 per page and the p99 of the cold loop from 15.4 to 12.6 µs
([The compressor: seqlz-fast](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-seqlz-fast)).

### The hash keeps 5 bytes, into 4096 slots

A hash of 5 bytes finds fewer short matches at new offsets than one of 4. At first that looked like a
loss: 2.6% faster for 0.5 points more memory, and it was dropped. Measured again it was worth more:
20 425 instead of 21 440 compress cycles in userspace, for 0.6 points, and in the kernel
`seqlz-fast`'s write p99 went from 11 650 to 10 480 ns, 10%, twice what the cycles say, because the slowest pages to write are the ones with many
short matches ([zram: prefetch the compressed data](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression),
section "A hash of 5 bytes"). With coded literals the 4-byte hash is also larger, by 18 to 35 bytes per
page with the tables of then: a match of 4 bytes at a new offset costs more bits than the 4 coded literals
it replaces, and the greedy matcher then skips the longer match behind it
([Six choices, item 5](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#5-the-matchers-hash-5-bytes-into-4096-slots-stays)). A hash of 6 bytes made
pages 27.9% instead of 27.0% ([zram: prefetch the compressed data](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)). Matches of 4 bytes still come from the repeat offset.

The table has 4096 slots of 2 bytes, 8 KiB. 2048 slots: 939.6 instead of 936.9 bytes per page on the
phone, no faster. 8192 slots: 937.1 bytes, 1.6 µs slower per write on the A55, and 16 KiB per CPU, right
at C5 ([Six choices, item 5](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#5-the-matchers-hash-5-bytes-into-4096-slots-stays)). "A page has 4096 positions, and a table of 4096 entries already keeps nearly every candidate that
helps" ([seqlz-fast on the phone](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-on-the-phone-time-goes-per-sequence-and-code-layout-moves-reads-by-130-ns)).

### The repeat offset at every position

Checking the repeat offset first, at every position, was worth 0.5 points when `seqlz-fast` got its own
matcher. Offline on phone pages, checking it only up to 16 positions after a match cost 0.3%, and not
checking it at all 3.7%
([Ratio from the parse and the tables on phone pages](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#ratio-from-the-parse-and-the-tables-on-phone-pages-measured-offline-not-built)).
It costs no table lookup, and on the two desktop dumps 18% to 25% of the matches have it.

### No step, and a cleared table

`lz4` and `lzo` skip ahead faster and faster where they find no match, which saves time on data that does
not compress. seqlz did the same, with a step of `1 + (pos − anchor) >> 6`, until it was measured
without: the step needs `anchor` in the loop over positions without a match, and that loop has no
register to spare. Without it, pages that compress are 5% faster to write, and `seqlz-fast-lit` wrote
0.21 and 0.25 µs faster per page in the VM, 3%, in boots with `lzo-rle` and `seqlz-fast-lit` alone; in
boots with all four codecs 0.05 µs. Its write p99 went from 1.32 and 1.29 times `lz4`'s to 1.23 and
1.19, against `lz4`'s p99 from older boots. The price is the
pages zram stores raw: they take 2.3 µs longer, since every position is tried
([The matcher without its step](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept)).

The table is cleared with a `memset()` of 8 KiB for every page, so every position in it belongs to the
current page and needs no check of its age. That was 7% faster than checking at every position whether
the entry is before it ([The compressor, second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)),
and it also means the work memory holds nothing from one page to the next, which [the memory
section](#ch-decisions--memory) needs. Not clearing at all, with one compare per position that the
entry is before it, since an old position is still a valid candidate once its bytes are compared, took
twice the time, 44.4 instead of 21.5 µs per page on the A55, for a reason not found
([The matcher's table with the bytes](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matchers-table-with-the-bytes-its-loop-in-assembly-on-arm64-9-fewer-compress-cycles-on-the-a55-not-kept)).

### One position ahead, for the in-order core

On the A55 a position without a match is one chain: 8 bytes, the multiply of the hash, the table entry,
the 4 bytes at the candidate, the compare. An in-order core waits at every link. The matcher loads the
next position's bytes, hash, entry and candidate while it compares the current one. The output is the
same, and the A55 compresses in 42 349 instead of 43 841 cycles per page
([The matcher on an in-order core](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-on-an-in-order-core-34-fewer-compress-cycles-on-the-a55-same-output-kept)).

## The decoder {#ch-decisions--decoder}

The decoder ([chapters 12 to 14](#ch-dec-basics)) runs while a program waits. It decodes with 2.1 times
`lz4`'s instructions, 25 284 against 11 899 per page, at 3.3 instructions per cycle against 2.4: 7609
against 4992 cycles in a hot loop on the PC
([The numbers again](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-numbers-again-with-the-format-as-it-is-now-reads-03-µs-faster-than-on-29th-september)).
Most of its decisions are about one of three things: fewer instructions, fewer mispredicted branches, or
fewer cache misses on a cold read.

| decision | instead of | gain | price | entry |
| --- | --- | --- | --- | --- |
| token entries of 4 bytes, ready to use | 2-byte entries unpacked per sequence | A55: 15 129 instead of 16 547 instructions per page; with three smaller changes 17 842 to 15 129, and cold p99 0.7 to 0.8 µs less | 8 KiB instead of 4 KiB, once | [in-order decoder](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept) |
| the next token looked up before the copies | after them | cold p50 against `lz4`: A55 2007 instead of 2169 ns more, A76 330 instead of 456 | two more registers | [next token](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-next-token-before-the-copies-reads-faster-on-both-phone-cores-kept) |
| fixed 16-byte copies on the fast path | 32 or 40 bytes always | 30 ns per decode, 250 ns at swap-in p99 | a branch for longer matches | [branch or more work](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained) |
| the refill every second fast sequence without a check | a check every time | 63 ns per swap-in on x86-64 | | [refill](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-refill-without-its-branch-every-second-fast-sequence-63-ns-less-per-swap-in-on-x86-64-and-the-a76-kept) |
| one decode loop for every core | a loop per kind of core | no CPU model in `lib/` | A55 up to 0.3 µs per warm read | [kernel copy's choices](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16) |
| the compressed data prefetched by the backend | none | `lz4`'s cold p99 5600 to 3891 ns; A55 warm reads 2.3 µs faster for seqlz | | [prefetch](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression) |
| all tables prefetched before a page | none | A55: 14.5 µs per cold read, 30 µs at p99 | A76 cold reads 1.2 µs slower then, not seen again on 10th October | [item 4](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#4-prefetching-the-tables-the-a55-needs-it-the-a76-is-faster-without) |

### One lookup per sequence, with what the decoder needs in the entry

The token table's entries were 2 bytes at first, and the decoder took them apart: the offset class, its
raw bits from a packed constant, a mask, the shift of classes 4 and 5. On the in-order A55, where fewer instructions were faster almost one for one, that was 18 of about 60
instructions per sequence. Entries of 4 bytes
now hold `ll`, `ml`, the bits to drop, the two shifts that cut out the offset, and a bit for "a length
value follows" ([chapter 7](#ch-internal)). On the A55 the entries and the flag took the decode loop
from 16 547 to 15 129 instructions per page; the flag saved instructions but no cycles. Together with
three smaller changes in the same step, the room checks of the fast path, the prefetch of 8 lines per
iteration and the match copied from `d - off`, it went from 17 842 to 15 129, and its cold p99 against
`lz4` in the harness fell by 0.7 to 0.8 µs. The table doubled to 8 KiB, once for all CPUs
([seqlz's decoder for an in-order core](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).

The next token's entry needs only the bits after this sequence, which are known before its copies. So
the decoder looks it up first and copies after, and the copies fill the time the load takes on an
in-order core. Against `lz4` at cold p50 the gap fell from 2169 to 2007 ns on the A55 and from 456 to
330 ns on the A76 ([The next token before the copies](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-next-token-before-the-copies-reads-faster-on-both-phone-cores-kept)).

### The fast path: fixed copies, and the branches that stay

A sequence without a length value, far enough from the ends of the page, the literals and the input,
takes the fast path: 16 bytes of literals and a 16-byte match copied without a loop, 16 more if the
match is longer than 16 bytes and 8 more if it is longer than 32. 79% of the matches are at most 16
bytes, 32 bytes would cover 91%
([Numbers that were only in the codec's comments](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).
Copying 32 bytes always made the mean faster and the p99 slower, 3570 to 4150 ns, with the decoder of
that time, because the slowest pages have many short matches
([Second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)). In a swap-in, 32 bytes always was 30 ns slower per decode and 250 ns at
p99 ([A branch or more work, again in a swap-in](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained)).
A match with an offset below 8 overlaps the bytes it writes, so it never uses a block copy: its first 8
bytes are built in a register with a multiply and stored again and again. One copy for every offset,
without the branch on `off < 8`, saved 12 mispredictions per page and needed 10 to 19 more instructions
per fast sequence, which cost more
([The offset below 8](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-offset-below-8-one-copy-for-every-offset-saves-12-misses-per-page-and-costs-more-not-kept)).

The general lesson came early: "at 3.4 to 3.9 instructions per cycle, instructions are as expensive as
mispredictions here". Masks instead of branches halved the mispredictions, 155 to 68 per page, and added
15 000 instructions, 9400 to 12 000 cycles
([Second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).

### The refill every second time, and the branches of a fault

The read benchmark and the harness decoded each page right after decoding it before, and that trains
the branch predictor on the page. In a real swap-in `seqlz-fast-lit` mispredicts about 110 branches per
page more than right after decoding the same page, and
`zcomp_decompress()` takes 2.0 µs against 1.4 µs for `lz4`
([The decoder in a fault, found](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
The largest source of them was the refill check: are fewer than 23 bits left in the register? That
depends on the code lengths of the sequences before, which a predictor learns only for a page it has
seen
([Which branches](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#which-branches-three-in-the-sequence-loop-are-58-of-the-130-misses-of-a-page-seen-once)).

A refill leaves at least 56 bits, and two fast sequences take at most $2 \times (11 + 14) = 50$. So a fast
sequence now refills every second time without asking, and a branch that alternates is one any predictor
gets right. In a swap-in on x86-64 that took `zcomp_decompress()` from 1995 to 2008 ns down to 1936 to
1942 ns, in 3 boots of the code as it was kept, with gcc; an earlier series of 3 boots gave 2010 to 2011
against 1935 to 1941 ns; on the A76 a page seen once decoded about 70 ns faster. Refilling on every fast sequence took
out as many misses, but its loads ate the gain, and it cost 500 cycles more on a page the predictor
knows
([The refill without its branch](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-refill-without-its-branch-every-second-fast-sequence-63-ns-less-per-swap-in-on-x86-64-and-the-a76-kept)).
The second largest source, the fast path's condition, misses once per length value, and that is data:
four ways around it were all slower
([The fast path's condition](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-fast-paths-condition-its-22-misses-per-page-are-the-length-values-four-ways-around-them-slower-not-kept)).

### One loop for all cores

The refill without its check made the in-order A55 slower, because it waits for every load and pays less
for a miss. For a while the decoder had two loops: an arm64 kernel read the core's id from the MIDR
register and ran the old loop on Cortex-A53, A55, A510, A520 and Qualcomm's small Kryo cores, and on
arm64 only those prefetched the token table; other architectures always did. The review of the kernel copy (#152) removed it: nothing else in
`lib/` depends on the CPU model, and, as issue #152 says, the list needs an entry for every new little
core. Measured, one
out-of-order loop costs the A55 at most about 0.3 µs per warm read, 3%, and nothing that shows on the
A76; the token prefetch that had cost the A76 1.3 to 2.6 µs of cold reads did not show again on 10th
October ([The kernel copy's choices](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16)).
`port.py` now fails on `read_cpuid` or `MIDR_` in `lib/`. The same entry kept `-O3`: with `-O2` writes
were 0.1 to 0.19 µs slower in the VM and 0.93 µs on the A55.

### Prefetch: the compressed data and every table

A cold read starts with misses: the compressed data, the tables and the destination page are not in the
cache. `lz4` reads its input front to back, and the hardware prefetcher keeps up with that only partly.
Asking for all lines of the compressed data before decoding took `lz4`'s cold p99 in the kernel from 5600
to 3891 ns ([zram: prefetch the compressed data](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
seqlz's backend does that itself, since it is the first to read the data. On the phone the module lacked
it at first; with it the A55's warm reads got 2.3 µs faster, 15%
([Six choices, item 7](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#7-the-phones-module-did-not-prefetch-the-compressed-data-23-µs-faster-warm-reads-on-the-a55)).
Prefetching the destination page did not help in the kernel.

The decoder also prefetches its tables at the start: the token table, 128 lines, the two length tables,
16 lines each, and the page's literal table, 32 lines, 8 lines per loop iteration
(`PREFETCH_STEP`, [chapter 12](#ch-dec-basics)). The A55 needs every one of them: without any, its cold
reads were 14.5 µs slower and 30 µs at p99, and even the 32 lines of the length tables were worth 2.8 µs
([Six choices, item 4](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#4-prefetching-the-tables-the-a55-needs-it-the-a76-is-faster-without)). A
loop with one prefetch per iteration cost the A76 0.8 µs on cold reads of coded pages
([Five simplifications](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).

On x86-64 the kernel's `prefetch()` is `__builtin_prefetch()`, and the kernel builds with `-mno-sse`.
Without SSE, clang drops `__builtin_prefetch()` without a warning: a clang-built kernel had 0
`prefetcht0` in seqlz's object against 36 with gcc, and a cold p99 22% and 26% worse
([A kernel built with clang dropped the decoder's prefetches](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-kernel-built-with-clang-dropped-the-decoders-prefetches-fixed-p99-back-at-gccs)).
The codec does not carry inline assembly for that. The fix is a patch to the kernel,
`tools/kernel-port/x86-prefetcht0.patch`, which gives x86-64 a `prefetch()` of `prefetcht0`. Without it a
clang kernel swaps in 120 to 180 ns slower, 740 to 810 ns at p99
([The kernel's prefetch() on x86-64](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernels-prefetch-on-x86-64-fixed-in-the-kernel-a-patch-gives-clang-its-prefetches-back)).

### `decode_literals()` is not inlined

`decode_literals()` is `noinline`. Its comment gives the reason: inside the loop over the sequences its
registers made pages with raw literals slower too ([chapter 13](#ch-literals-dec)). With 8 stream
accumulators live, the sequence loop would lose registers it needs, also on pages that have no coded
literals. As a function of its own, it uses the registers and gives them back before the sequence loop
starts.

### Bounded work for any input

C6 asks for a decoder that is safe and bounded for any input. Every offset is checked against the bytes
before it in the page. The fast path runs only where its largest copies fit, and the slow path checks
each length against the room in the page and the literals left. A page has at most `PAGE / 4 + 1`
sequences. The worst case was searched with a fuzzer that rewards cost: the slowest pages found took
1.14 times the instructions of the real p99 to compress and 1.59 times to decode, the smallest ratio of
the four codecs; `lz4`'s decode worst case is 2.58 times its p99. In time on the A55 the slowest made-up
pages took 1.26 and 1.33 times the p99
([The worst case](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-worst-case-the-slowest-pages-found-cost-13-times-the-p99-of-real-ones-as-for-lz4)).
The slowest decode found is a page the matcher never writes, made from chosen sequences: every token
escaped, with 4-byte matches at offsets below 8. It takes about 107 000 arm64 instructions, 40 µs on the
A55, still below the slowest compression.

## Memory {#ch-decisions--memory}

### C5, counted as the allocator counts

The limit is `lz4`'s work memory, 16 416 bytes per CPU. It was 4 KiB at first, and moved to `lz4`'s
16 KiB on 23rd September 2026: 8 points less memory on the compressed pages are worth far more than a few
KiB per CPU, and a budget of 4 KiB costs the matcher speed or ratio
([plan.md §3.3](https://github.com/martinus/quetschn/blob/main/docs/plan.md#33-per-cpu-workspace-is-real-memory-on-a-phone)). On the Mi 9T, with its 8
cores, every 4 KiB below `lz4` is 32 KiB, resident for good.

What counts is what `kmalloc()` and `vmalloc()` hand out, not what is asked for. `lz4` asks `vmalloc()`
for 16 416 bytes and gets 5 pages, 20 480 bytes. seqlz needs the 8192 bytes of the hash table to compress
and the 4112 bytes of the literal scratch, a page plus 16, to decompress
([chapter 6](#ch-header)). zram holds the stream's mutex for either, and neither keeps anything from one
page to the next: the matcher clears its table, the decoder writes the scratch before it reads it. So
the kernel's backend gives each CPU one union of both, 8192 bytes from `kmalloc()`
([chapter 15](#ch-zram-glue)). Asked as one struct of 12 304 bytes, it would have come as 16 KiB. With
zram's contexts split into one for compression and one for decompression, as a series of 5th October
2026 proposes, it is two allocations of 8 KiB each, 16 KiB; that split changed no codec's time in the VM
([zram's contexts split](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zrams-contexts-split-into-compression-and-decompression-the-same-time-in-the-vm-the-same-memory-for-seqlz)).
At level 1 there is no decompression context at all, since pages with raw literals need no scratch.

The tables are not work memory: 43 280 bytes, built once and shared by every CPU.

### 16 KiB pages: why the kernel copy is 4 KiB only

seqlz is written for both page sizes: `QUETSCHN_PAGE_BITS` is 12 or 14, the offset class 3 has as many
bits as the page, and 16 KiB pages have their own tables, trained on the Android 17 emulator's zram and
2.6% smaller on a second dump than tables from desktop pages
([Android 17 in the emulator](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#android-17-in-the-emulator-the-4-kib-tables-fit-the-16-kib-ones-trained-again-26-smaller)).
But the work memory doubles with the page: the union asks for 16 400 bytes and gets 32 768, the split
contexts ask for 32 784 and get 49 152. `lz4` gets 32 768 with 16 KiB pages. Each split context alone stays at or below
`lz4`'s 32 KiB, their sum of 48 KiB does not, and whether C5 limits each context or the sum is not
decided
([plan.md §3.3](https://github.com/martinus/quetschn/blob/main/docs/plan.md#33-per-cpu-workspace-is-real-memory-on-a-phone)).

Decoding the coded literals into the end of the output page instead of a scratch removes the scratch.
It was built, measured and reverted: on 4 KiB pages it made cold reads 0.08 to 0.22 µs slower in the
kernel, for memory C5 does not need there
([16 KiB pages, tuned](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#16-kib-pages-tuned-c5-does-not-hold-the-literals-in-the-page-would-fix-it-not-kept)).
Another way is not built: zram's own buffer of two pages per stream is unused while reading and half
unused while writing ([Not evaluated yet](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#not-evaluated-yet)). Until that is decided, and until
there is a phone with 16 KiB pages to measure on, the format for 16 KiB pages is not fixed and the kernel
copy is built for 4 KiB pages only ([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#status)).

## Tried and not kept {#ch-decisions--not-kept}

Most of `explored-designs.md` is designs that lost. They are worth knowing, because each is an obvious
idea, and each has a number now. The common reasons: the gain is below what the time is worth on the
hull, the format would get a second way of doing something, or the work memory would break C5.

### Levels 3 and 4: a deeper search for the same format

A matcher that searches a hash chain and prices each match by the code lengths of the tables, with one or
two lazy steps, writing the same format for the same decoder
([Levels 3 and 4](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#levels-3-and-4-a-hash-chain-priced-by-the-tables-as-small-as-zstd-3-on-two-of-four-dumps-and-faster-to-write-and-read-not-kept)).
Level 3 stored 2.7% to 4.3% less than level 2 on four dumps, as much as `zstd` 3 on two of them, and wrote
a page in 1.8 to 1.9 times level 2's time, still 23% to 31% faster than `zstd` 3. In the VM it was on the
hull at 4.2 and 8.3 bytes per µs on two dumps and off it on the other two. Not kept: it pays only where a
µs of writing is worth less than 4 to 8 bytes, and that is `zstd`'s place, not that of a codec that has to
stay close to `lz4`. It cost 850 lines in the codec, the tests and the kernel port, and 12 KiB more work
memory per CPU.

### A literal table of the page's own

The largest difference to `zstd` is that `zstd` codes a page's literals with a table built for that
page. It was built three times. With `seqlz-opt`, cold p99 went from 4809 to 5849 ns and the scratch grew
by 3.3 KB ([seqlz-opt with a literal table per page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-opt-with-a-literal-table-per-page-less-memory-than-zstd-slower-reads-not-kept)).
In `seqlz-fast-lit` (#43, #44) writes were 13% and 15% slower for 0.3% and 4.6% less memory, and with 4
token tables per page as well 43% slower for 2.6% and 6.6%
([Per page: its own literal table and one of 4 token tables](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept)).
At level 3 it gave 2.5% to 3.4% in the VM, but "a second way to code the literals: a table description
with its own rules for an invalid one, a parser of untrusted input in the decoder and in both reference
decoders, about 900 lines with the tests, for one level" ([A literal table of the page's own](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-literal-table-of-the-pages-own-at-levels-3-and-4-25-to-34-less-a-second-literal-path-in-the-format-not-kept)).
A decoder that parses a table from the page also adds to what C6 has to cover.

### Token tables per page or per context

4 token tables per page need all tokens known before the first is written, two passes that cost 5700
to 6400 cycles per page
([Per page: its own literal table and one of 4 token tables](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept)).
8 would make the tokens about 0.3 to 0.4 points of memory smaller, and matching first and encoding after
cost 2300 cycles per page in that estimate
([seqlz-fast-lit: one of 8 literal tables per page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
A token table chosen by the offset class of the sequence before, 3 tables, saved 3.3 and 12.5 bytes per
page in the kernel, for 0.11 and 0.24 µs more per page written, cold reads 0.13 to 0.21 µs slower in
every boot, and 20 KiB more tables
([The token's table by the offset before it](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tokens-table-by-the-offset-before-it-3-and-12-bytes-per-page-not-kept)).

### Recompression

zram can compress idle pages again later with a second algorithm. `seqlz-opt`, an optimal parser for the
same format, took 237 µs per page for 59 and 106 bytes, 0.3 and 0.4 bytes per µs, where the step from
`seqlz-fast-lit` to writing every page with `zstd` 3 is 3 and 17: at that time it would have to save
700 to 4000 bytes per page to compete. `zstd` 3 as the second algorithm recompresses 16 times as fast
as `seqlz-opt`, but each
read of a recompressed page is then about 2.3 µs slower, and those reads come in bursts when an app that
was idle is used again ([Recompression, measured, not pursued](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#recompression-measured-not-pursued)).
`seqlz-opt` is removed, and no design depends on recompression.

### The matcher in assembly

On the A55 the loop over positions without a match in arm64 assembly, with table entries of 8 bytes
that hold the bytes and a page generation, compressed with 9% fewer cycles, and in the phone's kernel its
slowest writes were as fast as `lz4`'s. The same design in C gained 2.5% in the harness, nothing in the
phone's kernel, and needed 16 392 bytes of work memory instead of 8192
([The matcher's table with the bytes](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matchers-table-with-the-bytes-its-loop-in-assembly-on-arm64-9-fewer-compress-cycles-on-the-a55-not-kept)).
The assembly is in the history as commit `0288b1e`, and the same pull request went back to the C matcher
(`f1db05d`): the codec stays C, without inline assembly, also for one architecture. A gap in the kernel is fixed in the kernel, as with `prefetch()` above.

### Other ways to compress a page

| design | best result | why not | entry |
| --- | --- | --- | --- |
| word model, WKdm style: a 2-bit tag per 64-bit word | cold p99 350 ns below `lz4` | 55.7% of the page; even with every repeat of 8 bytes and more for 3 bytes, more than `lz4` | [word model](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#word-model-wkdm-style-64-bit-words), [with runs](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-word-model-with-a-path-for-runs-not-even-lz4s-memory-not-built) |
| BΔI, a base and deltas per 64-byte block | the fastest decoder measured, 960 ns below `lz4` at cold p99 | 70.5% | [BDI](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#base--delta-per-64-byte-block-bdi) |
| byte shuffle, then `lz4` | cheaper than `lz4` on a quarter of the pages | 40.9%; no cheap rule finds the pages where it pays | [shuffle](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#byte-shuffle--lz4), [#29 and #31](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-ideas-of-29-and-31-measured) |
| memlz, words found through a 16-bit hash | | 63.6%, 1.5 MB per CPU | [memlz](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#memlz) |
| `bytelz`, seqlz's matcher and a byte format | cold read p99 4560 instead of 5720 ns, compress 21 000 instead of 21 600 cycles | 29.4% against 26.4% then, on no hull | [bytelz](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#bytelz-seqlz-fasts-matcher-a-byte-oriented-format) |
| `lz4`'s format from a compressor for pages | reads 5% to 10% faster than `lz4`'s pages | at best `lzo-rle`'s memory for twice the write time | [lz4page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#lz4s-format-from-a-compressor-for-pages-measured-not-kept) |

The word model was the hope of the plan's Phase 2b, the decoder latency spike: control flow that barely depends on the data might
win the p99 of a read. It did win it, by 350 ns, with a format that needs 55.7% of the page against
`lz4`'s 34.5%. Local tricks on 8 or 64 bytes do not get the ratio; it has to come from repeats across the
whole page.

### Other ways to code the literals and the offsets

| idea | result | why not | entry |
| --- | --- | --- | --- |
| ANS instead of Huffman | 0.2% to 0.3% at most | a state machine on every symbol | [how tight](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2) |
| FSST, symbols of 1 to 8 bytes as one byte | about 40% of what the Huffman tables get | not faster either, 2.9 to 3.1 ticks per literal to decode against about 1.1 cycles | [one of 8 tables](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page) |
| Tunstall codes | 7.1 to 9.3 bits per byte, the entropy is 6.1 to 6.7 | dropped without building | [one of 8 tables](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page) |
| a literal XOR the byte at the repeat offset, as LZMA | 0.1 and 0.25 points | read p99 700 to 1000 ns slower, write p99 1.3 µs | [one of 8 tables](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page) |
| a literal's table by its context, as Brotli | 5 to 18 bytes per page | 64 KiB of tables | [by the score](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8) |
| two literals per lookup | 12% faster with the data in the cache | slower cold | [prefetch](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression) |
| the literal coder in AVX2 | 2.53 instead of 3.09 ticks per literal | the kernel would need `kernel_fpu_begin()`, and arm64 a NEON version | [#29 and #31](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-ideas-of-29-and-31-measured) |
| offsets as whole bytes in a stream of their own | | 28.1% instead of 27.0%, no faster in the kernel | [prefetch](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression) |
| two sequence streams for the in-order A55 | | slower everywhere, 2.4 bytes per page more | [seqlz-fast on the phone](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-on-the-phone-time-goes-per-sequence-and-code-layout-moves-reads-by-130-ns) |

FSST is the one with the clearest lesson: "what is left of a page after the matcher has no multi-byte
structure worth a symbol". The gain is all in how uneven the single bytes are, which a Huffman code uses
and a code of whole bytes cannot.

### Outside the codec

The largest gains left are zram's work, not the codec's. Compressing neighbouring swap slots together in
blocks of 16 KiB gives `zstd` 3 13% to 22% fewer bytes per page, before zsmalloc, than `seqlz-fast-lit` on single pages, and a delta
against a similar page 2.3 to 3.9 points
([The ideas of #29 and #31](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-ideas-of-29-and-31-measured)). Both need zram to keep an index or
decode more than one page per fault, and neither is built.

## Where zstd still wins, and why {#ch-decisions--zstd}

`zstd` at level 3, zram's default level for it, stores pages 2% and 10% smaller than `seqlz-fast-lit` on
the two desktop dumps by the stored bytes of `docs/seqlz.md`'s result table, 1012 against 1035 and 1197
against 1331 bytes per page.
As Σ zsmalloc cost on four whole dumps, two of the desktop, named by their dates, and two of the phone
([Levels 3 and 4](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#levels-3-and-4-a-hash-chain-priced-by-the-tables-as-small-as-zstd-3-on-two-of-four-dumps-and-faster-to-write-and-read-not-kept)):

| codec | desktop, 23rd September | desktop, 28th September | phone 1 | phone 2 |
| --- | --- | --- | --- | --- |
| `lz4` | 34.5% | 39.6% | 25.8% | 30.5% |
| `seqlz-fast-lit` | 24.4% | 30.3% | 17.2% | 21.9% |
| `zstd` 3 | 23.6% | 27.3% | 16.8% | 20.9% |

This table has the desktop dump of 28th September 2026. Elsewhere in this chapter the second desktop
dump is the one of 24th September.

Three things make the difference, each measured:

1. **Better matches.** `lz4hc`'s matches coded by seqlz take 3% and 7% fewer bytes than seqlz's greedy
   matcher
   ([Not evaluated yet](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#not-evaluated-yet)).
   A matcher that searches that much takes more time than zram's writes can spend, as levels 3
   and 4 showed.
2. **A literal table of the page's own.** Coding the literals is worth 2.3 and 4.7 points to `zstd` 3
   ([seqlz-fast-lit: one of 8 literal tables per page](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)). A
   table per page instead of the best of 8 fixed ones would save seqlz another 0.3% and 4.6%, 2.7 and
   60.6 bytes per page, for 13% to 15% slower writes
   ([Per page: its own literal table](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept)). The second desktop dump, of 24th September, is
   the one whose literals fit the 8 tables badly.
3. **Adaptive sequence coding.** `zstd` codes `ll`, `ml` and the offset with three tables it can build
   for the data, three lookups per sequence. seqlz's one fixed token table is what keeps its decoder fast.
   The offsets are the clearest loss: as plain bits they cost 15 to 20 bytes per page, 2%, above their
   entropy
   ([How tight the bits are](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).

What `zstd` pays for that: about twice the time to write a page, 1.7 times the time to read one in a
swap-in, and 186 112 bytes of work memory per CPU plus 91 496 bytes per zram device, against seqlz's 8 KiB,
16 KiB with zram's contexts split. The 91 496 bytes are a dictionary `zstd` creates even without one,
which a series of October 2026 removes. No `zstd` level
changes the picture. In a swap-in, every level that stores less than `seqlz-fast-lit` takes at least 6.6 µs
more per page written; `zstd` 1, the closest, stores 15 and 23 bytes per page less, 1.5% and 2.5%, for 7.0
and 6.6 µs more, and the fast negative levels store more and are still slower
([zstd at every level](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zstd-at-every-level-in-a-swap-in-no-level-stores-less-in-less-time-than-seqlz-fast-lit)).

## What is not known yet {#ch-decisions--unknown}

The design rests on measurements of few machines, and seqlz.md says so
([seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#what-is-not-known-yet)).

- **One desktop, one phone.** The pages are three zram dumps of one desktop, two of them in most measurements, and two of one phone, a Mi 9T
  with Android 11 and Linux 4.14, plus the Android 17 emulator. The tables are trained on pages of the
  same desktop and phone. The second desktop dump gained 4% from swapped desktop pages in the training;
  on another machine that gain may be smaller.
- **The exchange rate and `r`.** The 0.34 reads per write is one desktop. A phone swaps differently, and
  `r` on a phone is not measured; nor is the size of the bursts of swap-ins, which needs a machine under
  memory pressure.
- **arm64.** Most times are x86-64. In the phone's own kernel, on its own pages, `seqlz-fast-lit` stores
  28% less than `lz4` and swaps in 13% slower warm and 25% slower cold on the A76, 11% and 17% on the A55
  ([The tables trained again](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tables-trained-again-4-less-on-one-desktop-dump-the-phone-the-same)).
  On the little core it has the lowest score only up to 16 bytes per µs, `seqlz-fast` up to 36. A
  current phone has not been measured.
- **What a user sees.** On the Mi 9T, with the same RAM given to zram for every codec and 25 apps
  switched in turn, no app had to start cold in 3 runs each of `seqlz-fast` and `seqlz-fast-lit`, and 54
  launches did in 3 runs of `lz4`; the mean launch was 154 ms lower with `seqlz-fast`
  ([Apps on the phone](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#apps-on-the-phone-with-the-same-ram-no-cold-launch-in-6-runs-of-seqlz-54-in-3-runs-of-lz4)).
  That is one phone, one set of apps and 3 runs per setting; between `seqlz-fast` and `seqlz-fast-lit`,
  5 runs each could not tell the 6% of RAM apart.
- **Writes at p99** take 1.21 and 1.19 times `lz4`'s time, at the bar the plan set before the score.
- **The decoder in a fault.** About 110 mispredicted branches per page are left that a predictor gets
  right only on a page it has seen. Fewer data-dependent branches in the decoder are the next action of
  the plan. The refill without its branch took out about 28 misses per page and was kept; every other
  attempt so far cost more than the misses.
- **16 KiB pages**: no phone with them yet, the format for them not fixed, and with split contexts their
  sum, 48 KiB, above `lz4`'s 32 KiB.
- **Whether zram wants a new algorithm at all.** The question to the zram maintainers went to linux-mm,
  linux-block and linux-kernel on 9th October 2026, with the phone's numbers, and is not answered yet
  ([plan.md §9](https://github.com/martinus/quetschn/blob/main/docs/plan.md#9-where-the-project-stands-and-the-next-actions)). The answer also decides the form:
  a zram backend, as `tools/kernel-port/` writes now, or an algorithm of the crypto API. `lib/seqlz/`
  stays the same in either; the glue around it changes. A hardware `lz4` on new Qualcomm phones would not
  change the argument: even at a time of 0, `seqlz-fast-lit` stays on the hull from 3.1 to 73 and from
  10.3 to 72 bytes per µs
  ([lz4 in hardware, a what-if](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#lz4-in-hardware-a-what-if-seqlz-fast-lit-stays-on-the-hull-up-to-73-and-72-bytes-per-µs-instead-of-155-and-211)).

The merge argument the project ends up with is not the one it started with. The plan hoped for a decoder
faster than `lz4`'s at p99. In a real swap-in it is 17% to 25% slower cold on the Mi 9T, and about 20% on the PC. What it has is memory: about 28%
less than `lz4` for a small cost in time, measured in the kernel, with a decoder that is fuzzed, bounded
and specified in a document two independent decoders were written from.
