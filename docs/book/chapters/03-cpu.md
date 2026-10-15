# What makes code slow on a CPU {#ch-cpu}

seqlz's decoder runs 2.1 times as many instructions as `lz4`'s, 25 284 against 11 899 per page, and
takes 1.5 times the cycles, 7609 against 4992, as of 5th October 2026. In a real swap-in on the PC it
takes 2.0 µs against 1.4. The gap between instructions and time is what this chapter is about. Most
choices in `src/` that look odd at first, a table of exactly {{ch03.tok_dec.entries}} entries, a
bitstream read from the top bit, 8 streams of literals, a copy of 16 bytes for 3 literals, follow
from three facts: a core waits for memory, for wrong guesses at branches, and for chains of steps
that need each other's results. The small cores of phones make all three worse. Each section names
the function and the chapter where the code is.

## Instructions are not time {#ch-cpu--ipc}

A modern core fetches instructions ahead, finds the ones whose inputs are ready, and runs several of
them in the same cycle on separate units. The instructions it completes per cycle, the **IPC**, say
how much of that parallel work a piece of code allows.

::: measured seqlz against lz4, the decoder alone
A loop over 2000 pages a desktop swapped into zram, the pages in the cache, on the Ryzen 9 7950X of
the development machine, per page, as of 5th October 2026
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-numbers-again-with-the-format-as-it-is-now-reads-03-µs-faster-than-on-29th-september)):

| codec | instructions | cycles | instructions per cycle |
| --- | ---: | ---: | ---: |
| `lz4` | 11 899 | 4992 | 2.4 |
| `seqlz-2` | 25 284 | 7609 | 3.3 |

This loop decodes every page several times, which flatters seqlz, see
[the branches](#ch-cpu--branches). In a swap-in in the kernel, in a VM on the same CPU,
`zcomp_decompress()` takes 2.0 µs for seqlz and 1.4 µs for `lz4`
([The decoder in a fault, found](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
:::

seqlz gets more instructions per cycle because more of its work is independent: 8 streams of
literals decoded side by side, and a token's lookup that does not wait for the copies of the sequence
before.

## Memory comes in cache lines {#ch-cpu--cache}

A load whose data is in no cache takes about 100 ns, 450 cycles at 4.5 GHz. The smallest and fastest
cache, **L1**, holds a few dozen KiB per core and answers in about 1 ns; L2 and L3 are larger and
slower. The unit of a cache is the **cache line**, 64 bytes on every CPU this book is about: a load of
one byte brings the aligned 64 bytes around it into L1.
<!-- undated: typical latencies of any PC, not this project's measurement -->

In a swap-in the compressed bytes are cold ([chapter 1](#ch-zram--path)), and the decoder's tables,
{{size.tables|grp}} bytes shared by all CPUs, may have been pushed out of the caches by whatever ran
since the last swap-in. The decoder needs its tables on every page, so their sizes are chosen for L1.
The tables of 4 KiB pages, from `struct seqlz_tables` ([chapter 7](#ch-internal)):

| table | entries | bytes | cache lines | read |
| --- | --- | ---: | ---: | --- |
| token decode table | {{ch03.tok_dec.entries}} of 4 bytes | {{ch03.tok_dec.bytes}} | {{ch03.tok_dec.lines}} | once per sequence |
| `LL` and `ML` decode tables | {{ch03.len_dec.entries}} of 4 bytes, each | {{ch03.len_dec.bytes}} each | {{ch03.len_dec.lines}} each | for [length values](#ch-format--length-values) |
| one literal decode table, of 8 | {{ch03.lit_dec.entries}} of 2 bytes | {{ch03.lit_dec.bytes}} | {{ch03.lit_dec.lines}} | once per coded literal |
| token codes for the encoder | {{ch03.tok_enc.entries}} of 2 bytes | {{ch03.tok_enc.bytes}} | about {{ch03.tok_enc.lines}} | once per sequence |
| the matcher's hash table, per CPU | {{ch03.hash.entries}} of 2 bytes | {{ch03.hash.bytes}} | {{ch03.hash.lines}} | once per position |

The decoder of a page with coded literals reads from {{ch03.dec_kib}} KiB of tables, the encoder
from the {{ch03.tok_enc.kib}} KiB of token codes next to the matcher's {{ch03.hash.kib}} KiB. Each
size is a format decision: the token table has {{ch03.tok_dec.entries}} entries because token codes
have at most {{ch03.tok_bits}} bits (`SEQLZ_TOKEN_BITS`), the length tables
{{ch03.len_dec.entries}} for codes of at most {{ch03.len_bits}} (`SEQLZ_MAX_BITS`), the literal
tables {{ch03.lit_dec.entries}} for codes of at most {{ch03.lit_bits}} (`SEQLZ_LIT_BITS`). One bit
more would compress a little better and double a table.

::: measured Tables have to stay in L1
In an early version of the format, the token had 512 symbols and codes of up to 12 bits, a decode
table of 8 KiB of 2-byte entries. With it, the slowest 1% of cold reads were 650 ns slower than with
the 256-symbol token before it. Limiting the same 512 symbols to codes of up to 11 bits, a table of 4
KiB, took the p99 of cold reads from 5070 to 4350 ns. Measured in the userspace harness on the Ryzen 9
7950X
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
:::

## Prefetching: many misses at once {#ch-cpu--prefetch}

A core can have several misses in flight at once: ten misses one after the other cost ten times 100
ns, ten that start together little more than one. An out-of-order core finds such misses on its own
when the addresses are known early. When they are known late, e.g. because each table index depends
on the bits the lookup before consumed, the code asks for the lines ahead of time with a
**prefetch**, an instruction that starts the load and does not wait for it: `prefetcht0` on x86-64,
`prfm` on arm64, `prefetch()` in the kernel.
<!-- undated: a typical latency of any PC, and arithmetic on it -->

<!-- undated: the animation is a model with round numbers, not a measurement -->
@anim misses A model, not a measurement: 8 lookups whose addresses do not depend on each other and whose lines are in no cache, about 100 ns per miss as above, and 20 ns of work on each entry. The out-of-order core's window holds the work of 3 lookups. One step is 20 ns.

`seqlz_decode()` asks for the lines of its tables before it reads the first byte of the page,
{{ch03.prefetch_lines}} lines per round of a small loop, `prefetch_lines()`
([chapter 12](#ch-dec-basics--prefetch)), which also says why a table that does not start at a cache
line misses its last line: the literal tables always do.
@assert ch03.lit_at_mod != 0 and ch03.lit_size_mod == 0

::: measured What the prefetches give
On the Mi 9T's little core, a Cortex-A55, in the phone's own kernel: without any table prefetch, cold
reads were 14.5 µs slower on average and 30 µs slower at p99, 77.28 against 62.76 µs and 120.45
against 90.26
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#4-prefetching-the-tables-the-a55-needs-it-the-a76-is-faster-without)).
On the PC the prefetches cover the tables so well that, in a swap-in in the kernel VM, reading every
line of them before the decode made it only 6 to 25 ns faster
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-028-µs-slower-than-in-zrams-read-benchmark-warm-caches-give-back-012)).
The same idea for the compressed data, in zram before the decoder runs, took `lz4`'s p99 of cold
reads from 5600 to 3891 ns in the kernel VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
:::

Two traps around prefetches are handled in `src/seqlz_compat.h` ([chapter 5](#ch-compat--prefetch)):
clang drops `__builtin_prefetch()` without a word when x86-64 is built without SSE, as the kernel is,
and the Mi 9T's 4.14 kernel says that arm64 cache lines are 128 bytes, while its cores have 64.

## Which cache lines a lookup touches {#ch-cpu--msb-first}

A code of $l$ bits fills $2^{11-l}$ entries of the token table, all indices that start with its bits
([chapter 2](#ch-compression--table-decoding)). Which entries those are depends on the order the bits
are read in, and that decides how many cache lines a page touches. Read **most significant bit
first**, as seqlz does ([chapter 4](#ch-format--bitstream)), they are one range. Token 49's code
`{{tok.code.49}}` fills the indices {{ch03.t49.first}} to {{ch03.t49.last}}: at 16 entries of 4 bytes
per line, lines {{ch03.t49.line_first}} to {{ch03.t49.line_last}}. Token 4's code `{{tok.code.4}}`
fills {{ch03.t4.entries}} entries, {{ch03.t4.first}} to {{ch03.t4.last}}, all in line
{{ch03.t4.line_first}}.
@assert tok.len[49] == 4 and ch03.t4.line_first == ch03.t4.line_last

Read **least significant bit first**, as seqlz did until 6th October 2026, a code's entries are every
$2^l$-th entry ([chapter 2](#ch-compression--msb)). Token 49's {{ch03.t49.entries}} entries are then
indices {{ch03.t49.lsb.at.0}}, {{ch03.t49.lsb.at.1}}, {{ch03.t49.lsb.at.2}} and so on, one in each of
the {{ch03.t49.lsb.lines}} lines, and token 4's are in {{ch03.t4.lsb.lines}} lines,
{{ch03.t4.lsb.line.0}}, {{ch03.t4.lsb.line.1}}, {{ch03.t4.lsb.line.2}} and so on,
{{ch03.t4.lsb.step}} lines apart:
@assert (ch03.t49.line_first, ch03.t49.line_last, ch03.t4.line_first, ch03.t49.lsb.lines, ch03.t4.lsb.line[0], ch03.t4.lsb.step, ch03.t4.lsb.lines) == (0, 7, 62, 128, 3, 16, 8)

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 230" role="img" aria-label="Two grids of the 128 cache lines of the token table. Read most significant bit first, the 4-bit code of token 49 fills lines 0 to 7 and token 4 one line, 62; read least significant bit first, token 49 has an entry in every line and token 4 in 8 lines" font-size="12">
  <text x="186" y="28" text-anchor="middle" font-weight="bold">most significant bit first</text>
  <rect x="50" y="40" width="272" height="120" class="fill-paper"/>
  <g class="acc"><rect x="50" y="40" width="136" height="15" class="fill-acc" fill-opacity="0.3"/></g>
  <g class="mark"><rect x="288" y="85" width="17" height="15" class="fill-mark" fill-opacity="0.75"/></g>
  <path d="M67,40 V160 M84,40 V160 M101,40 V160 M118,40 V160 M135,40 V160 M152,40 V160 M169,40 V160 M186,40 V160 M203,40 V160 M220,40 V160 M237,40 V160 M254,40 V160 M271,40 V160 M288,40 V160 M305,40 V160 M50,55 H322 M50,70 H322 M50,85 H322 M50,100 H322 M50,115 H322 M50,130 H322 M50,145 H322" fill="none" stroke="currentColor" stroke-width="0.5"/>
  <rect x="50" y="40" width="272" height="120" fill="none" stroke="currentColor" stroke-width="0.5"/>
  <text x="44" y="51" text-anchor="end" class="mut" font-size="11">0</text>
  <text x="44" y="66" text-anchor="end" class="mut" font-size="11">16</text>
  <text x="44" y="81" text-anchor="end" class="mut" font-size="11">32</text>
  <text x="44" y="96" text-anchor="end" class="mut" font-size="11">48</text>
  <text x="44" y="111" text-anchor="end" class="mut" font-size="11">64</text>
  <text x="44" y="126" text-anchor="end" class="mut" font-size="11">80</text>
  <text x="44" y="141" text-anchor="end" class="mut" font-size="11">96</text>
  <text x="44" y="156" text-anchor="end" class="mut" font-size="11">112</text>
  <text x="546" y="28" text-anchor="middle" font-weight="bold">least significant bit first</text>
  <g class="acc"><rect x="410" y="40" width="272" height="120" class="fill-acc" fill-opacity="0.3"/></g>
  <g class="mark"><rect x="461" y="40" width="17" height="120" class="fill-mark" fill-opacity="0.75"/></g>
  <path d="M427,40 V160 M444,40 V160 M461,40 V160 M478,40 V160 M495,40 V160 M512,40 V160 M529,40 V160 M546,40 V160 M563,40 V160 M580,40 V160 M597,40 V160 M614,40 V160 M631,40 V160 M648,40 V160 M665,40 V160 M410,55 H682 M410,70 H682 M410,85 H682 M410,100 H682 M410,115 H682 M410,130 H682 M410,145 H682" fill="none" stroke="currentColor" stroke-width="0.5"/>
  <rect x="410" y="40" width="272" height="120" fill="none" stroke="currentColor" stroke-width="0.5"/>
  <text x="404" y="51" text-anchor="end" class="mut" font-size="11">0</text>
  <text x="404" y="66" text-anchor="end" class="mut" font-size="11">16</text>
  <text x="404" y="81" text-anchor="end" class="mut" font-size="11">32</text>
  <text x="404" y="96" text-anchor="end" class="mut" font-size="11">48</text>
  <text x="404" y="111" text-anchor="end" class="mut" font-size="11">64</text>
  <text x="404" y="126" text-anchor="end" class="mut" font-size="11">80</text>
  <text x="404" y="141" text-anchor="end" class="mut" font-size="11">96</text>
  <text x="404" y="156" text-anchor="end" class="mut" font-size="11">112</text>
  <g class="acc"><rect x="50" y="186" width="14" height="14" class="fill-acc" fill-opacity="0.3" stroke="currentColor"/></g>
  <text x="70" y="198" text-anchor="start">lines with an entry of token 49, code {{tok.code.49}}</text>
  <g class="mark"><rect x="50" y="206" width="14" height="14" class="fill-mark" fill-opacity="0.75" stroke="currentColor"/></g>
  <text x="70" y="218" text-anchor="start">lines with an entry of token 4, code {{tok.code.4}}</text>
</svg>
</div>
<figcaption>The 128 cache lines of the token table, 16 per row. Most significant bit first, a code&#39;s entries are one range; least significant bit first, they are spread over the table.</figcaption>
</figure>

The lookup reads one entry, and which one depends on the bits after the code too. Most significant
bit first, the frequent short codes keep coming back to the same few lines. Counted on 20 000 pages, a
page touches 42.5 of the token table's 128 lines most significant bit first, and 55.3 least
significant bit first, with the same bytes of the pages
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#six-choices-made-on-the-pc-measured-on-the-phone-the-token-table-and-the-prefetches-matter-the-rest-does-not)).
The same count for the {{slab.tokens}} token lookups of [chapter 16](#ch-trace)'s page, one for each
sequence but the last, which has no token:

@anim lines wide The token lookups of chapter 16's page, found with the real codec, and the line of the token table each one reads: most significant bit first as seqlz reads, and the same bits reversed. One page, so the counts are its own, not the mean of 20 000 pages.

::: measured The bit order on the phone
On the Mi 9T's big core, a Cortex-A76, with the token table not prefetched on that core as it was
then: with the codes in one range, cold reads were 9.57 to 9.73 µs in every run; with the codes
spread, one build of the same decoder read in 9.39 µs and another in 10.50, depending on where the
tables of the kernel module landed in memory. On the PC the change cost nothing: 7370 against 7410
cycles per page in the decode loop
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept)).
:::

The price of most significant bit first is a byte swap on every refill of the bit register, not on
every code ([byte order](#ch-cpu--endian)).

## Branches, and a predictor that learns the page {#ch-cpu--branches}

A core works many instructions ahead, so it has to guess the outcome of a branch long before its
condition is computed. A **branch predictor** remembers how each branch went the last times, and how
the branches before it went. A right guess is nearly free. A wrong one, a **misprediction**, throws
away the work done on the wrong path, about 15 to 20 cycles.
<!-- undated: a typical cost on current cores, not this project's measurement -->

::: measured What a misprediction costs, and that the predictor learns a page
In the kernel VM on the Ryzen 9 7950X at 4.5 GHz, `seqlz-2`'s decoder in a swap-in mispredicted
200.9 branches per page. When the same page was decoded once right before, it mispredicted 89.5, and
took 1867 cycles less, with the same instructions and the same L1 misses: about 17 cycles for each of
the 111 mispredictions
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
:::

The surprise is the second number: a branch predictor learns a page of 4 KiB after one decode. In
userspace it is the same, 130 mispredictions per page for a page decoded once and 11.6 right after
itself. Every benchmark that decodes a page more than once measures a decoder that knows the page:
a loop over pages in the cache, and the project's read benchmark in the kernel VM, which reads each
page 6 times in a row in each run ([chapter 1](#ch-zram--codecs)). A swap-in decodes a page once.
Since then, the decode times the project trusts come from `MODE=swap` of its kernel VM, which swaps
each page in once per pass, with 20 000 other pages in between.

Three branches of the sequence loop made 58% of the mispredictions of a page seen once: whether the
bit register needs a refill, 32 per page; whether the sequence takes the **fast path**, the common
case of a short sequence away from the end of the page ([chapter 14](#ch-decode)), 24; and whether
the match's offset is below 8, 20 to 22
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#which-branches-three-in-the-sequence-loop-are-58-of-the-130-misses-of-a-page-seen-once)).
The first one is gone. Whether {{ch03.refill_at}} bits are left depends on the lengths of the codes
before, which no predictor knows for a new page. But a refill leaves at least 56 bits, and two fast
sequences take at most {{ch03.two_fast}} of them on 4 KiB pages, {{k16.ch03.two_fast}} on 16 KiB, so
`next_token_fast()` refills every second fast sequence without asking, a branch that alternates and
that every predictor gets right ([chapter 14](#ch-decode--next-token)).

### Code without a branch

The other way around a misprediction is arithmetic instead of a branch. `seqlz_off_class()` picks a
match's offset class from comparisons added and masked, without an `if`, because the classes come in
no order a predictor could learn ([chapter 6](#ch-header--offsets)). It is not always faster: it runs
the work of both outcomes, and a branch the predictor gets right costs almost nothing. An early
version of the decoder that replaced its branches with masks mispredicted 68 instead of 155 times per
page, ran 15 000 more instructions, and took 12 000 instead of 9400 cycles. "At 3.4 to 3.9
instructions per cycle, instructions are as expensive as mispredictions here"
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
The fast path's copies, measured again on pages seen once: fixed copies of 32 or 40 bytes instead of
copies behind two branches had 14 fewer mispredictions per page and took more cycles
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained)).
And a `?:` is no promise of code without a branch: the same early measurement found two `?:` of the
decoder compiled to branches that made 30% and 28% of all mispredictions.

### One branch for two questions

At every position of the page the matcher asks whether the next 4 bytes match at the repeat offset,
and whether they match at the candidate from the hash table. Both answers are close to random. It
computes both first and decides with one branch on `rep_hit | cand_hit`
([chapter 9](#ch-matcher--position)). With three branches per position the matcher mispredicted 539
times per page, with one 329, and the p99 of compressing a cold page went from 15.4 to 12.6 µs
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-seqlz-fast)).

## Chains of dependent steps {#ch-cpu--chains}

A **chain** is a sequence of steps where each needs the result of the one before. It bounds the time
from below, whatever the width of the core. Decoding Huffman codes is such a chain: to decode a
literal, the decoder looks up the next 10 bits of its stream in the literal table and shifts the
stream by the code length the entry holds ([chapter 13](#ch-literals-dec--decode)). The next lookup
needs the shifted bits, so in one stream every literal waits for the load and the shift of the one
before. The 8 streams of a page with coded literals ([chapter 4](#ch-format--layout)) are 8 chains
that do not depend on each other: the decoder steps all 8 in turn, and while one stream's load is on
its way, the next one's can start.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 296" role="img" aria-label="A schematic timeline: one stream decodes literal after literal, each lookup waiting for the shift before it; 8 streams run 8 such chains side by side, so 16 literals take the time of 2 steps" font-size="12">
  <text x="20" y="30" text-anchor="start" font-weight="bold">one stream</text>
  <g class="acc"><rect x="120" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="152.0" y="32" text-anchor="middle" font-size="11">load 0</text>
  <rect x="184" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="200" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="232.0" y="32" text-anchor="middle" font-size="11">load 1</text>
  <rect x="264" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="280" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="312.0" y="32" text-anchor="middle" font-size="11">load 2</text>
  <rect x="344" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="360" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="392.0" y="32" text-anchor="middle" font-size="11">load 3</text>
  <rect x="424" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="440" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="472.0" y="32" text-anchor="middle" font-size="11">load 4</text>
  <rect x="504" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="520" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="552.0" y="32" text-anchor="middle" font-size="11">load 5</text>
  <rect x="584" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="608" y="32" text-anchor="start">...</text>
  <text x="20" y="74" text-anchor="start" font-weight="bold">8 streams</text>
  <text x="110" y="74" text-anchor="end" class="mut" font-size="11">0</text>
  <g class="acc"><rect x="120" y="60" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="152.0" y="74" text-anchor="middle" font-size="11">load 0</text>
  <rect x="184" y="60" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="200" y="60" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="232.0" y="74" text-anchor="middle" font-size="11">load 8</text>
  <rect x="264" y="60" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="98" text-anchor="end" class="mut" font-size="11">1</text>
  <g class="acc"><rect x="126" y="84" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="158.0" y="98" text-anchor="middle" font-size="11">load 1</text>
  <rect x="190" y="84" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="206" y="84" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="238.0" y="98" text-anchor="middle" font-size="11">load 9</text>
  <rect x="270" y="84" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="122" text-anchor="end" class="mut" font-size="11">2</text>
  <g class="acc"><rect x="132" y="108" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="164.0" y="122" text-anchor="middle" font-size="11">load 2</text>
  <rect x="196" y="108" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="212" y="108" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="244.0" y="122" text-anchor="middle" font-size="11">load 10</text>
  <rect x="276" y="108" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="146" text-anchor="end" class="mut" font-size="11">3</text>
  <g class="acc"><rect x="138" y="132" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="170.0" y="146" text-anchor="middle" font-size="11">load 3</text>
  <rect x="202" y="132" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="218" y="132" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="250.0" y="146" text-anchor="middle" font-size="11">load 11</text>
  <rect x="282" y="132" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="170" text-anchor="end" class="mut" font-size="11">4</text>
  <g class="acc"><rect x="144" y="156" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="176.0" y="170" text-anchor="middle" font-size="11">load 4</text>
  <rect x="208" y="156" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="224" y="156" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="256.0" y="170" text-anchor="middle" font-size="11">load 12</text>
  <rect x="288" y="156" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="194" text-anchor="end" class="mut" font-size="11">5</text>
  <g class="acc"><rect x="150" y="180" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="182.0" y="194" text-anchor="middle" font-size="11">load 5</text>
  <rect x="214" y="180" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="230" y="180" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="262.0" y="194" text-anchor="middle" font-size="11">load 13</text>
  <rect x="294" y="180" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="218" text-anchor="end" class="mut" font-size="11">6</text>
  <g class="acc"><rect x="156" y="204" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="188.0" y="218" text-anchor="middle" font-size="11">load 6</text>
  <rect x="220" y="204" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="236" y="204" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="268.0" y="218" text-anchor="middle" font-size="11">load 14</text>
  <rect x="300" y="204" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="242" text-anchor="end" class="mut" font-size="11">7</text>
  <g class="acc"><rect x="162" y="228" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="194.0" y="242" text-anchor="middle" font-size="11">load 7</text>
  <rect x="226" y="228" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="242" y="228" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="274.0" y="242" text-anchor="middle" font-size="11">load 15</text>
  <rect x="306" y="228" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <line x1="120" y1="266" x2="690" y2="266" stroke="currentColor" marker-end="url(#ch-cpu-arrow)"/>
  <text x="690" y="284" text-anchor="end" class="mut">time</text>
  <defs><marker id="ch-cpu-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0 0 L10 5 L0 10 z" fill="currentColor"/></marker></defs>
  <g class="acc"><rect x="400" y="62" width="14" height="14" class="fill-acc" fill-opacity="0.3" stroke="currentColor"/></g>
  <text x="420" y="74" text-anchor="start">table lookup of a literal</text>
  <rect x="400" y="86" width="14" height="14" class="fill-sheet" stroke="currentColor"/>
  <text x="420" y="98" text-anchor="start">shift the stream by its code length</text>
</svg>
</div>
<figcaption>Schematic, not to scale: one stream is one chain, lookup then shift then the next lookup. 8 streams are 8 chains that start one or two cycles apart.</figcaption>
</figure>

With 4 streams a literal took about 2.9 cycles to decode, and 8 streams made a page decode faster
for a few bytes more header ([chapter 13](#ch-literals-dec--cost)). Measured again in October 2026,
4 streams made reads 0.7% to 1.8% slower in the kernel VM, for 0.1% to 0.2% less memory, so 8 stayed
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
The encoder has the same chain the other way round, a shift and an or on a stream's accumulator per
code, and `lit_streams()` writes 4 streams at a time, each with its own accumulator
([chapter 11](#ch-literals-enc)); one stream after the other took about 3.5 cycles per literal.

The sequences have a chain too, and it is the reason for the format's main idea. The bits of the next
token start where this sequence's bits end, which is known only from the token's table entry: lookup,
shift, lookup. Every step added to that chain costs on every sequence. Putting the offset class into
the token, instead of a symbol of its own after it, took a second lookup off the chain: 8168 instead
of 8758 cycles per page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
The copies of the literals and the match hang off this chain but are not on it.

## In-order cores wait at every load {#ch-cpu--in-order}

The cores of a PC and the big cores of a phone are **out of order**: they look at a window of the next
few hundred instructions and run whichever have their inputs ready. When a load misses, they keep
working on everything that does not need it. The Mi 9T has big Cortex-A76 cores, out of order, and
little Cortex-A55 cores, which are **in order**: an A55 issues instructions in program order, at most
two per cycle, and when one needs a value that has not arrived, it stops, and everything behind it
stops too. A swap-in can run on either kind of core, so the project measures on both. Three
consequences show in `src/`:

**Every instruction costs.** On the A55 the decoder's loop ran at 0.99 instructions per cycle, so
fewer instructions were faster almost one for one. That is why the token table's entries are 4 bytes
with every field ready to use ([chapter 7](#ch-internal)): taking the token apart at run time took 18
of about 60 instructions per sequence. The entries of 4 bytes, with a flag for "a length value
follows", took the decoder from 16 547 to 15 129 instructions per page on the A55, and with three
smaller changes from 17 842
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).

**A load has to start early.** Loaded right before it is used, a value costs an in-order core the
whole latency of the load; issued a few instructions earlier, the instructions in between fill the
wait. The matcher loads the next position's 8 bytes, hash, table entry and candidate bytes while it
compares the current one, **software pipelining** ([chapter 9](#ch-matcher--position)). On the A55
that, with two smaller changes to the same loop, took compression from 43 841 to 42 349 cycles per
page, 3.4%, with the same output; on the PC from 16 489 to 16 148
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-on-an-in-order-core-34-fewer-compress-cycles-on-the-a55-same-output-kept)).
The decoder's fast path does the same: `next_token_fast()` looks up the next token before the copies
of the current sequence, which fill the time the load takes. On the phone that made cold reads at the
median 162 ns closer to `lz4` on the little core and 126 ns on the big one
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-next-token-before-the-copies-reads-faster-on-both-phone-cores-kept)).

**A miss it did not see coming stops it.** An out-of-order core finds the table lookups of the next
sequences in its window and starts their misses early; an in-order core reaches each one only when it
gets there. So the table prefetches matter far more on the A55, 14.5 µs per cold read, than on the PC,
where prefetching the tables once made about 100 ns of difference, within the scatter of that
measurement
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
The [model of 8 misses](#anim-misses) shows the two kinds of core side by side.

Until 10th October 2026 the arm64 decoder had a second loop, written for in-order cores; now one loop
runs on every core, because nothing else in the kernel's `lib/` asks which CPU it runs on
([chapter 17](#ch-decisions--one-loop-for-all-cores) has the cost on the A55).

## Registers and the 64-bit bit register {#ch-cpu--registers}

x86-64 has 16 general-purpose registers, arm64 31. A value that does not fit is **spilled** to the
stack, and in a hot loop that can put a load onto a chain. The decoder reads its bitstream through
one register: `struct seqlz_bit_reader` has a `u64 bits` with the next bits on top and an `int count`
of how many are real ([chapter 12](#ch-dec-basics--bit-reader)). One refill of 8 bytes serves two or
three sequences: it leaves at least 56 bits, and a sequence on the fast path takes at most
{{ch03.tok_bits}} bits of token and 14 of offset ({{ch03.off_max}} on 4 KiB pages).

The 8 literal streams need 8 such registers at once and a pointer each, and with a count each they
did not fit into x86-64's registers. So a stream has no count: its 64 bits carry a 1 below the last
real bit, and the number of bits used is the position of that 1, one count-trailing-zeros at the
stream's refill ([chapter 13](#ch-literals-dec)). Even so, on the A55 the 8 stream pointers went to
the stack; they are needed only at a refill, once per {{ch03.lit_rounds}} literals of a stream.

Registers also decided a choice in the matcher. Like `lz4`, it once moved on faster and faster when it
found no match for a while. That step needs one more value in the loop, which has no register to
spare. Without it, the pages that compress are written 5% faster, and in the kernel VM
`seqlz-2` writes a page 0.23 µs faster, 3%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept)).

## Byte order, and loads at any address {#ch-cpu--endian}

The code reads 2, 4 and 8 bytes at any position of a byte array, so through `memcpy()` into a local,
which gcc and clang turn into one load, never through a cast pointer
([chapter 5](#ch-compat--endian)). x86-64 and arm64 load from any address at full speed in most cases.
They are little endian, and s390x, which Linux also runs on, is big endian, so each load names its
byte order, `get_unaligned_le64()` or `get_unaligned_be64()`, and a page has the same bytes on every
CPU; CI checks that on s390x. Both orders are used on purpose:

- **Little endian for comparing bytes.** `seqlz_match_len()` loads 8 bytes of two strings and XORs
  them: byte 0 of a string is the lowest byte, so the first byte that differs holds the lowest set
  bit, and counting the trailing zero bits finds it ([chapter 9](#ch-matcher--match-len)).
- **Big endian for the bitstream.** Loaded big endian, the first byte of the stream lands in the top
  8 bits of the register, and its bit 7 in bit 63, where the decoder takes the next code from. On
  x86-64 that is `movq` and `bswap`, on arm64 `ldr` and `rev`: one instruction more per refill.

## Eight sums in one register {#ch-cpu--swar}

A 64-bit register can hold 8 numbers of 8 bits side by side, and one add adds all 8 pairs, as long as
no lane overflows into the next: **SWAR**, SIMD within a register. The kernel may not use the vector
registers in most places, so SWAR is how seqlz does 8 things at once. The encoder uses it to price a
page's literals in all 8 literal tables ([chapter 11](#ch-literals-enc--pricing)): `lit_cost` in
`struct seqlz_tables` holds, for each byte value, one `u64` with the byte's code length in all 8
tables, table `k` in bits `8k` to `8k + 7`, so one add per literal sums all 8 tables. A lane holds at
most 255, so after {{ch03.lit_cost_run}} literals of at most {{ch03.lit_bits}} bits the sums move into
wider lanes. `seqlz_hash5()` ([chapter 9](#ch-matcher--hash)) and `repeat_pattern()`
([chapter 12](#ch-dec-basics--pattern)) also use a register as 8 bytes at once, with one multiply
each.

## Copying more than needed {#ch-cpu--wild-copy}

Most sequences are short: up to {{ch03.token_ll_max}} literals and a match of up to
{{ch03.token_ml_max}} bytes fit into the token alone, and 79% of the matches are at most 16 bytes
long. A copy of exactly `ll` bytes is a loop whose end mispredicts often, because the length changes
from one sequence to the next. So the decoder copies a fixed 16 bytes, two loads and two stores, and
moves on by the real length; the next sequence writes over the rest:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 198" role="img" aria-label="A copy of 16 literal bytes for a sequence with 3 literals: the decoder moves on by 3 and the next sequence overwrites the 13 extra bytes" font-size="12">
  <text x="20" y="56" text-anchor="start" font-weight="bold">before</text>
  <text x="20" y="136" text-anchor="start" font-weight="bold">after</text>
  <rect x="90" y="40" width="72" height="22" class="fill-sheet"/>
  <g class="acc"><rect x="162" y="40" width="288" height="22" class="fill-acc" fill-opacity="0.3"/></g>
  <rect x="450" y="40" width="216" height="22" class="fill-paper"/>
  <path d="M108,40 v22 M126,40 v22 M144,40 v22 M162,40 v22 M180,40 v22 M198,40 v22 M216,40 v22 M234,40 v22 M252,40 v22 M270,40 v22 M288,40 v22 M306,40 v22 M324,40 v22 M342,40 v22 M360,40 v22 M378,40 v22 M396,40 v22 M414,40 v22 M432,40 v22 M450,40 v22 M468,40 v22 M486,40 v22 M504,40 v22 M522,40 v22 M540,40 v22 M558,40 v22 M576,40 v22 M594,40 v22 M612,40 v22 M630,40 v22 M648,40 v22" fill="none" stroke="currentColor" stroke-width="0.5"/>
  <rect x="90" y="40" width="576" height="22" fill="none" stroke="currentColor" stroke-width="0.5"/>
  <rect x="90" y="120" width="126" height="22" class="fill-sheet"/>
  <g class="mark"><rect x="216" y="120" width="234" height="22" class="fill-mark" fill-opacity="0.3"/></g>
  <rect x="450" y="120" width="216" height="22" class="fill-paper"/>
  <path d="M108,120 v22 M126,120 v22 M144,120 v22 M162,120 v22 M180,120 v22 M198,120 v22 M216,120 v22 M234,120 v22 M252,120 v22 M270,120 v22 M288,120 v22 M306,120 v22 M324,120 v22 M342,120 v22 M360,120 v22 M378,120 v22 M396,120 v22 M414,120 v22 M432,120 v22 M450,120 v22 M468,120 v22 M486,120 v22 M504,120 v22 M522,120 v22 M540,120 v22 M558,120 v22 M576,120 v22 M594,120 v22 M612,120 v22 M630,120 v22 M648,120 v22" fill="none" stroke="currentColor" stroke-width="0.5"/>
  <rect x="90" y="120" width="576" height="22" fill="none" stroke="currentColor" stroke-width="0.5"/>
  <text x="306" y="32" text-anchor="middle" font-size="11">copy16(): 16 bytes from d</text>
  <path d="M162 62 V70" stroke="currentColor"/>
  <text x="162" y="84" text-anchor="middle" font-weight="bold">d</text>
  <text x="333.0" y="112" text-anchor="middle" font-size="11">the 13 bytes past ll</text>
  <path d="M216 142 V150" stroke="currentColor"/>
  <text x="216" y="164" text-anchor="middle" font-weight="bold">d + 3</text>
  <text x="90" y="188" text-anchor="start">The next sequence writes from d + 3 on, over the 13 bytes.</text>
</svg>
</div>
<figcaption>The fixed copy for <code>ll = 3</code>: 16 bytes written, 3 of them kept. Grey are bytes of the page written before.</figcaption>
</figure>

Such a **wild copy** writes past the last byte the sequence needs and reads past the last literal. So
`seqlz_decode()` takes the fast path only where there is room in the page, in the literals and in the
input, one compare per buffer against limits computed once per page
([chapter 14](#ch-decode--rooms)); near the end of the page the slow path copies less at a time. The
encoder keeps such room too ([chapter 10](#ch-encoder)).

::: measured 16 bytes, not 32
Copying 32 bytes of every match made the mean faster and the p99 slower, because the slowest pages
have many short matches and each paid for 32 bytes ([chapter 12](#ch-dec-basics--copy-match)). Copying
byte by byte up to the end of the page made the slowest pages 10 times slower than `lz4`: with offset
1 every byte waited for the store before it
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).
:::

When the offset is below 16, the bytes a copy of 16 reads overlap the bytes it writes, and an 8-byte
load must not read bytes the store before it has not written yet. [Chapter 12](#ch-dec-basics--copies)
shows how the copies handle offsets below 8 and from 8 on.

## Shifts by 64, and what the kernel's sanitizers add {#ch-cpu--ubsan}

Bit readers meet undefined behaviour whenever a field can have 0 bits: "the top `x` bits" is
`v >> (64 - x)`, a shift by 64 for `x = 0`, and x86-64 then shifts by 0 and returns `v`, not 0.
`seqlz_br_value()` shifts by 1 and then by `63 - x` instead ([chapter 12](#ch-dec-basics--bit-reader)).
The token table's entries hold their shift as `(64 - raw_bits) & 63`, 0 for class 0, which has no
offset bits ([chapter 7](#ch-internal)). `__builtin_ctzll(0)` is undefined too, `rep bsf` gives 64 on
CPUs with `tzcnt` and nothing defined on older ones; `seqlz_match_len()` ORs in a bit where the XOR
could be 0 ([chapter 9](#ch-matcher--match-len)), and the literal streams' marker bit is never shifted
out. And the fast path's limits `lit_fast` and `in_fast` are `unsigned long`, not pointers, because
"16 bytes before the end" of a short input would be a pointer before the buffer.

The kernel can check for undefined behaviour at run time with **UBSan**. `CONFIG_UBSAN_SHIFT` checks
every shift whose count the compiler cannot prove to be in range, `CONFIG_UBSAN_BOUNDS` every array
index it cannot prove to be inside the array. Fedora's x86-64 kernel enables both, and the project
builds its benchmark copies of every codec with the same flags, `-fsanitize=shift` and
`-fsanitize=bounds-strict`. A check is a compare and a branch, on every shift and index in a hot loop.
These two functions differ only in the mask, compiled with gcc 16.2.1, `-O2 -fsanitize=shift
-fsanitize-undefined-trap-on-error`, the last flag only to make a failed check a trap instead of a
call:

```c
u64 put(u64 acc, unsigned int n)        { return acc << n; }
u64 put_masked(u64 acc, unsigned int n) { return acc << (n & 63U); }
```

```text
put:                            put_masked:
    cmpl    $63, %esi               movq    %rdi, %rax
    ja      .L3                     movl    %esi, %ecx
    movq    %rdi, %rax              salq    %cl, %rax
    movl    %esi, %ecx              ret
    salq    %cl, %rax
    ret
```

With the mask the check is gone, and the mask itself costs nothing: x86-64's shift uses only the low
6 bits of the count anyway, so the compiler drops the `and`. That is why the encoder's shifts are
written `<< (n & 63U)` even where `n` is never 64, and why `enc[]` in `struct value_table` has
{{ch03.enc_len_symbols}} entries for {{const.LEN_SYMBOLS}} or {{k16.const.LEN_SYMBOLS}} symbols: a
power of 2, so that `s & (ENC_LEN_SYMBOLS - 1U)` proves every index in range
([chapter 7](#ch-internal), [chapter 10](#ch-encoder)). Masked shifts and indices took the compressor
from 25 225 to 25 064 cycles per page, with 4.7% fewer instructions
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).
The decoder had 65 such checks in an early version, `lz4`'s 18, but not in its hot path: without them,
the instructions per page were the same
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-lz4s-matches-huffman-coded-sequences-with-static-tables)).
