# seqlz: how it works, and why

*Or how to get most of `zstd`'s memory at most of `lz4`'s speed, for zram's 4 KiB pages.*

On the pages my desktop swapped into zram, `seqlz-fast-lit` needs 21% to 24% less memory than
`lzo-rle`, zram's default, and 25% to 28% less than `lz4`. `zstd` at level 3, zram's other choice,
needs 3% to 10% less than `seqlz`, but takes twice the time to write a page and 1.7 to 1.8 times the
time to read one. This document is about the format that makes that possible, the encoder and the
decoder, and the choices behind them, each with the measurement that decided it. The history of
every idea, the ones that failed included, is in [explored-designs.md](explored-designs.md); the
code is in [`explore/seqlz.c`](../explore/seqlz.c), [`explore/seqlz.h`](../explore/seqlz.h) and
[`explore/page_lz.h`](../explore/page_lz.h).

## Contents

* [The result](#the-result-lz4s-time-zstds-memory-almost)
* [The format](#the-format-lz4s-sequences-coded-like-zstds)
* [Why it needs less memory](#why-it-needs-less-memory-offsets-and-literals)
* [The encoder](#the-encoder-one-pass-over-the-page)
* [The decoder](#the-decoder-one-table-lookup-per-sequence)
* [Why it is nearly as fast as lz4](#why-it-is-nearly-as-fast-as-lz4)
* [Why zstd still needs less memory](#why-zstd-still-needs-less-memory)
* [The choices, with their numbers](#the-choices-with-their-numbers)
* [What is not known yet](#what-is-not-known-yet)

## The result: lz4's time, zstd's memory, almost

zram compresses every page on its own when it is swapped out, and decompresses it in the page fault
when it is needed again. Memory is what zsmalloc uses for the compressed pages, in size classes of
16 bytes and more; time is the time per page written, the write plus 0.34 reads, because my desktop
read back one page for every three it swapped out.

![seqlz-fast-lit against zram's codecs](plots/seqlz-speed.svg)

Kernel VM of [`tools/zram-vm/run.sh`](../tools/zram-vm/run.sh), 20 000 pages of each of two zram
dumps, all four codecs in one boot per dump, one CPU of a Ryzen 9 7950X fixed at 4.5 GHz, means over
the pages, first dump / second dump:

| codec | zsmalloc bytes per page | write, us | cold read, us | us per page written |
| --- | --- | --- | --- | --- |
| `lz4` | 1450 / 1755 | 5.27 / 5.74 | 2.53 / 2.54 | 6.13 / 6.61 |
| `lzo-rle` (zram's default) | 1361 / 1679 | 5.09 / 5.66 | 2.76 / 2.81 | 6.03 / 6.61 |
| `seqlz-fast-lit` | 1039 / 1322 | 6.56 / 7.29 | 2.99 / 3.23 | 7.58 / 8.39 |
| `zstd` 3 | 1012 / 1197 | 13.38 / 14.30 | 5.32 / 5.52 | 15.19 / 16.17 |

At p99 `seqlz-fast-lit` reads cold pages in 4.94 and 5.19 us, faster than `lzo-rle` (5.42 and 5.38)
and a bit slower than `lz4` (4.74 and 4.75); it writes in 11.5 and 11.7 us, where `lz4` needs 9.2
and 9.5 and `zstd` 23.2 and 23.7. Per CPU it needs 12 336 bytes, `lz4` 16 440.

So there is no fastest codec here, there is a pareto front: memory against time, `seqlz-fast-lit` is
the best choice for anyone who values a byte of memory saved per page at 24 or more bytes per us of
time per page written, and less than about 150, see [the designs by the
score](explored-designs.md#the-designs-by-the-score).

![Memory against latency for all codecs](plots/codecs-branch.svg)

## The format: lz4's sequences, coded like zstd's

![The seqlz page format](plots/seqlz-format.svg)

Like `lz4`, `seqlz` describes a page as sequences: some literal bytes, copied as they are, then a
match, bytes copied from earlier in the page. The last sequence has only literals. Unlike `lz4`, it
does not write each sequence as bytes, but Huffman codes it with tables that are fixed and compiled
in.

**The token.** The literal length `ll`, the match length `ml` and the class of the offset go into
one symbol: `min(ll, 15) + 16 * min(ml - 4, 31) + 512 * class`, 3072 symbols. Codes have at most 11
bits. The rare tokens have no code and are sent as an escape code plus 12 raw bits, so that the
frequent ones get shorter codes; on my dumps that is 14 to 38 sequences of about 200 per page.

**The offset class.** 6 classes, each with its own number of raw bits after the token:

| class | offset | raw bits |
| --- | --- | --- |
| 0 | the offset of the match before again | 0 |
| 1 | below 16 | 4 |
| 2 | below 256 | 8 |
| 3 | below 4096 | 12 |
| 4 | a multiple of 8 from 16, below 256 | 5, the offset / 8 |
| 5 | a multiple of 8 from 256 | 9, the offset / 8 |

**Length values.** A literal length from 15 and a match length from 35 do not fit into the token;
the rest follows as a length value: below 16 a symbol of its own, above that a symbol for its bit
width and the low bits raw, with a Huffman table each for `ll` and `ml` of at most 8 bits.

**One bitstream** holds everything of the sequences, read least significant bit first: per sequence
the token, the offset bits, then the length values. There is no count of the sequences: the last one
is the one whose literals fill the page.

**Literals** stay raw, or are Huffman coded with one of 8 fixed tables, chosen per page, if that
saves at least 1/16 of them. Coded, they are in 8 streams: literal `k` in stream `k % 8`, most
significant bit first, codes of at most 10 bits. The page then starts with a 19-byte header: the
literal count with the top bit set, the table, and the sizes of the 8 streams.

## Why it needs less memory: offsets and literals

The same sequences, from `seqlz`'s own matcher, cost this many bytes per page in `lz4`'s format and
in `seqlz`'s:

![Where the bytes of a page go](plots/seqlz-bytes.svg)

| bytes per page, first dump | `lz4`'s format | `seqlz` | saved |
| --- | --- | --- | --- |
| tokens | 205 | 198 | 7 |
| offsets | 408 | 136 | 272 |
| lengths beyond the token | 41 | 18 | 23 |
| literals | 700 | 603 | 97 |
| header | 0 | 11 | -11 |
| total | 1355 | 966 | 389 |

**The offsets are two thirds of the gain.** `lz4` spends 2 bytes on every offset. Memory pages are
full of arrays of structs and pointers, and that shows in the offsets:

![What the format is built for](plots/seqlz-distributions.svg)

* 18% to 25% of the matches repeat the offset of the match before: 0 bits.
* 20% to 41% are multiples of 8, 8-byte aligned data, sent divided by 8: 5 or 9 bits instead of 8 or
  12.
* Of the rest, many are short: below 16 in 4 bits, below 256 in 8.

On the first dump that is 136 bytes of offsets per page instead of 408.

**The literals are the second part.** After the matcher, the literals have no structure of more than
one byte left, but the single bytes are far from uniform: zero bytes, ASCII, small integers. Each
page takes the one of 8 static tables that codes its literals in the fewest bits, and keeps the
coded literals only if they save 1/16. 50% to 69% of the pages do, and over all pages the literals
get 14% to 17% smaller.

**The tokens cost about the same as `lz4`'s token byte**, 198 against 205 bytes per page, but they
also carry the offset class, and the lengths beyond the token are rare: 53% to 68% of the matches
are 4 to 8 bytes, and 56% to 63% of the literal runs are 0 or 1 byte long.

**The tables are static.** A page of 4 KiB has little room for its own tables: the 256 code lengths
of a literal table take about 64 bytes coded. A literal table of its own, where that saves 16 bytes,
made writes 13% and 15% slower for 0.3% and 4.6% less memory; with one of 4 token tables as well,
43% slower for 2.6% and 6.6%. The fixed tables are trained on other pages than the ones they are
measured on: on resident pages of the running programs, and measured on the zram dumps.

**It knows zsmalloc.** zsmalloc stores in size classes at least 16 bytes apart at 4 KiB pages, so
saving a few bytes mostly saves nothing. That is why the literals are coded only when they save
1/16: coding them whenever they save anything gave less than 0.1 points of memory more, for decoding
time on every such page.

## The encoder: one pass over the page

The matcher, [`match_page()`](../explore/page_lz.h), is greedy, like `lz4`'s fast mode:

* At every position it checks two candidates: the last offset, and the position of the last time the
  same 5 bytes were seen, from a hash table of 4096 16-bit positions, 8 KiB, cleared for each page.
  Both are read before either is compared, so that one branch decides, not three.
* On a match it extends it backwards into the literals and forwards, the first 16 bytes without a
  branch, then hands the sequence to the encoder right away. Matcher and encoder are one loop, one
  pass over the page.
* The encoder writes the literals to the front of the output buffer and the sequences' bitstream
  behind the room of a page, with a 64-bit accumulator flushed once per sequence.
* At the end, the literal coder counts the bits of the literals in all 8 tables at once: per byte
  its 8 code lengths are the 8 lanes of a `u64`, one add per literal. If the best table saves 1/16,
  literals and bitstream move to the end of the buffer, and the 8 streams are written from the
  front, four side by side, so that each has its accumulator in a register.

zram hands the backend a buffer of two pages, and that is always enough: the most bits per page byte
are sequences of 4-byte matches without literals, 23 bits each, 2948 bytes for a page.

## The decoder: one table lookup per sequence

Per sequence the decoder:

1. refills the bit reader if fewer than 23 bits are left, the most a token and an offset need;
2. looks up the next 11 bits in the token table: the entry has the code length, `ll`, `ml - 4` and
   the offset class;
3. takes the offset's raw bits right after the token, or the last offset for class 0, without a
   branch;
4. copies 16 literal bytes and 16 to 40 match bytes without a loop, if the lengths fit into the
   token and there are 64 bytes of room; otherwise reads the length values and copies with loops.

Coded literals are decoded first, into a scratch of a page, by 8 independent chains: per literal a
table lookup of 10 bits, a shift and a store. Then the sequences take them from there.

Matches with an offset below 8 overlap themselves; for those the decoder builds the first 8 bytes in
a register and stores them in steps of the largest multiple of the offset up to 8, so that no load
waits for the store before it.

## Why it is nearly as fast as lz4

`seqlz` decodes with 2.3 times the instructions of `lz4`, 28 768 against 12 527 per page, but at 3.4
instructions per cycle: 8507 cycles against 5238 with the page in the cache. In the kernel the reads
are closer, 2.99 against 2.53 us, because much of a page fault is the wait for memory that neither
codec controls. What keeps the instruction count and the chains short:

* **One table lookup per sequence.** The offset class is in the token, not a second symbol, and its
  raw bits follow right after it. The chain from one sequence to the next is lookup, shift, lookup.
* **Static tables, small enough for L1**: the token table has 2048 entries of 2 bytes, the page's
  literal table 1024 of 2 bytes, the two length tables 256 of 4 bytes each. Nothing is built from
  the page before decoding starts, where `zstd` builds its literal table for each page with coded
  literals.
* **Only complete prefix codes.** Every bit pattern starts a code, so the decoder never checks for
  one that does not.
* **The common sequence has no loop and no length value**: literal runs up to 14 bytes and matches
  up to 34 bytes are the token alone, and the copies of 16 and 32 bytes cover them.
* **The literals decode in 8 streams**, 8 chains side by side instead of one.
* **The zram backend prefetches the compressed data** before decoding. In the same kernel VM, with
  cold compressed data, prefetching made `lz4`'s p99 read latency 30% lower.

Writing takes 1.25 times `lz4`'s time: the matcher alone costs about as much as all of `lz4`, and
coding the sequences and literals is on top of it. `zstd` 3 needs 2.4 times `seqlz`'s cycles to
compress (54 680 against 22 912 per page) and 2 times to decode (16 973 against 8507).

## Why zstd still needs less memory

On the second dump `zstd` 3 needs 10% less memory than `seqlz-fast-lit`, on the first 3%. Three
things make the difference, each measured:

* **Better matches.** `zstd` 3 searches more: `lz4hc`'s matches coded by `seqlz` need 3% to 7% fewer
  bytes than `seqlz`'s own greedy matcher, but a matcher that searches that much costs more time
  than zram's writes can give it.
* **Its own literal table per page.** Coding the literals is worth 2.3 and 4.7 points of memory to
  `zstd` 3 on the two dumps; a page's own table instead of the best of 8 fixed ones would save
  `seqlz` 0.3% to 4.6% more, for 13% to 15% slower writes.
* **Adaptive sequence coding.** `zstd` codes literal length, match length and offset with separate
  FSE tables. `seqlz`'s one static token table is what makes its decoder fast.

Where the ratio of `zstd` comes from in the first place is in
[explored-designs.md](explored-designs.md#where-the-ratio-of-zstd-comes-from): on the whole first
dump `lz4hc`'s best matches in `lz4`'s format need 30.7%, the same matches with entropy coded
sequences 25.8%, and with coded literals 23.6%. The coding of the sequences is the largest step, and
that is what `seqlz` is built on.

## The choices, with their numbers

Each of these was measured against the alternative; the numbers are from
[explored-designs.md](explored-designs.md), 4 KiB pages.

| choice | against | why |
| --- | --- | --- |
| `ll`, `ml` and the offset class in one token | separate symbols | two table lookups per sequence instead of three: 22% fewer decode cycles, and less memory |
| the offset class in the token, raw bits after it | a Huffman coded offset bucket | one lookup per sequence: 8758 to 8168 decode cycles, for 0.4 points of memory |
| token codes of at most 11 bits, 4 KiB table | 12 bits, 8 KiB | cold p99 5070 to 4350 ns: the decoder's tables have to stay in L1 |
| rare tokens escaped | a code for every token | 11 bits have room for 2048 codes, there are 3072 tokens |
| match length up to 34 in the token | up to 18 | with 15 values, every fifth match needed a length value, and that branch mispredicted (commit `f0381e7`) |
| one repeat offset | three, as `zstd` | moving three to front cost 17% of the decode cycles; the other two were 11% of the matches, 0.2 points |
| multiples of 8 in classes of their own | plain offsets | 24 and 2 bytes per page less in the kernel, for 0.2 us per page written |
| only complete prefix codes | checks for invalid codes | the checks were 6% of the decoder's instructions |
| static tables | a table per page | 13% and 15% slower writes for 0.3% and 4.6% less memory |
| one of 8 literal tables per page | one | 89% and 76% of the way from `lz4`'s to `zstd`'s memory in the kernel |
| literals in 8 streams | 4 | 2.9 cycles per literal with 4; 9070 to 8676 decode cycles per page |
| literals coded if they save 1/16 | whenever they save anything | less than 0.1 points of memory more, for decode time on every such page |
| a hash of 5 bytes | 4 bytes | 5% fewer compress cycles for 0.6 points, 10% less write time at p99 in the kernel |
| the last offset checked at every position | only the table | 0.5 points of memory |
| the hash table cleared for each page | checking each entry's age | 7% faster |
| every position tried | `lz4`'s growing step | writes 1% to 3% faster in the kernel |

Tried and not kept, among others: a token table chosen by the offset class before it (3 to 12 bytes
per page for three times the tables), the literals coded inside the matcher's loop (more cycles than
the pass after it), decoding the literals into the output page to save the scratch (slower cold
reads), `lz4`'s format from a better compressor, the word model of WKdm, BΔI, a byte shuffle, FSST
and Tunstall codes for the literals, recompression of idle pages. Why each of them failed is in
[explored-designs.md](explored-designs.md).

## What is not known yet

All of this is measured on one desktop: two zram dumps of the same machine, and tables trained on
its resident pages. Every latency is from x86-64, and phones are the users of zram; an in-order
little core may well order these designs differently. With 16 KiB pages, which Android is moving to,
`seqlz-fast-lit` needs 32 816 bytes per CPU, twice `lz4`'s, which breaks one of the project's own
limits; a fix is built and measured, but not kept. Writes at p99 take 1.2 to 1.25 times `lz4`'s
time. And the decoder runs 3.4 instructions per cycle on a Ryzen; on a small core that runs one or
two, its 2.3 times `lz4`'s instructions probably count for more. Nevertheless, on the pages this
machine actually swapped, it sits between `lzo-rle` and `zstd` on both axes.
