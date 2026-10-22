# The decoder's tools: prefetches, copies and a bit reader {#ch-dec-basics}

`src/seqlz_decompress.c` is the decoder, {{ch12.lines}} lines. Its first 213 lines, this chapter, are
what every decoder of an LZ format needs in some form: copies of 8 and 16 bytes, a copy for the
literals, one for matches that overlap the bytes they write, and a reader for the bitstream; the
prefetch of the tables is in `seqlz_internal.h`, which the decoder of the coded literals uses too.
Lines 215 to 300 turn deltas back into literals ([chapter 13](#ch-literals-dec--undelta)), the rest is
`seqlz_decode()`, the loop over the sequences of a page ([chapter 14](#ch-decode)). The coded literals
are decoded in a file of their own, `seqlz_decompress_literals.c` ([chapter 13](#ch-literals-dec)).

@listing src/seqlz_decompress.c 1-5

Everything but `seqlz_decode()` is `static`. The copies are `__always_inline`, the kernel's spelling of
`inline __attribute__((__always_inline__))`, because a copy of 8 bytes that becomes a call costs more
than the copy. The other helpers are plain `inline`: with `-O2` and `-O3`, and the kernel copy is built
with `-O3`, they are inlined too, with `-Os` gcc keeps `prefetch_lines()`, `seqlz_br_refill()` and
`seqlz_br_value()` as functions. The tests and the fuzzers reach all of them through `seqlz_decode()`.

## The tables are prefetched, 8 lines per iteration {#ch-dec-basics--prefetch}

When a swap-in comes, whatever ran since the last one may have pushed the decoder's tables out of the
caches. Then each first lookup into a line is a miss of about 100 ns, and prefetches issued back to back
let these misses overlap instead of one after the other ([chapter 3](#ch-cpu--prefetch)).
<!-- undated: a typical latency, explained in chapter 3 -->

@listing src/seqlz_internal.h 113-138

`L1_CACHE_BYTES` is the kernel's size of a cache line, 64 on x86-64 and arm64. `prefetch()` is `prfm` on
arm64 and `prefetcht0` on x86-64, the latter with a patch to the kernel ([chapter 5](#ch-compat--prefetch)).
The inner loop has constant bounds, and the compiler unrolls it to 8 prefetches in a row. The token
table takes {{ch12.tok.rounds}} rounds, each length table {{ch12.ll.rounds}} and a literal table
{{ch12.lit.rounds}}: {{ch12.lines_raw}} cache lines for a page with raw literals, {{ch12.lines_coded}},
{{ch12.kib_coded}} KiB, for one with coded literals, which prefetches the one literal table it uses. A
page with a table of its own prefetches none: it builds its table ([chapter 13](#ch-literals-dec--own)).
The tables do not grow with the page, so it is the same with 16 KiB pages.
@assert F("k16.ch12.lines_coded") == ch12.lines_coded and ch12.lines_coded - ch12.lines_raw == ch12.lit.cachelines

::: why 8 lines per round, not 1
With one line per round, the loop's add, compare and branch were more instructions than the
prefetches. On the Mi 9T's little Cortex-A55, 8 lines per round took the decoder from 16 809 to 16 547
instructions per page and from 16 676 to 16 247 cycles
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).
Measured again in October 2026, the plain loop made the big Cortex-A76 read pages with coded literals
cold in 8.15 to 8.84 µs over the rounds, against 7.38 to 7.99 µs for two builds with 8 lines
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
Why 8 and not all 128 lines of the token table without a loop is not recorded.
:::

Chapter 3 has what the prefetches are worth on the A55, 14.5 µs per cold read. On the A76 they once
cost 1.2 µs, and for a while only in-order cores prefetched the token table. Since 10th October 2026
every core runs the same loop with the prefetch ([chapter 17](#ch-decisions--one-loop-for-all-cores)),
and the cost did not show again: cold reads in 7.44 and 7.65 µs with it, 8.07 and 7.60 µs without, in
two runs
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16)).

::: warn The line of end - 1
The loop prefetches the line of `p` and the lines 64, 139, ... bytes on. Where `p` is not at the start
of a line, the table touches one line more than the loop reaches. In `struct seqlz_tables` on a 64-bit
build the token table is at offset {{ch12.tok.off}}, `ll.decode` and `ml.decode` at
{{ch12.ll.off|grp}} and {{ch12.ml.off|grp}}, {{ch12.ll.mod}} bytes past a multiple of 64, and the
literal tables at {{ch12.lit.off|grp}}, {{ch12.lit1.off|grp}} and so on, {{ch12.lit.mod}} bytes past
one. The harness aligns the struct to a page, so there the last {{ch12.ll.tail}} entries of each length
table and the last {{ch12.lit.tail}} of each literal table are in a line of their own. The kernel's
copy is one static object, which C aligns to its widest member, the `u64` of `lit_cost`; a kernel
build puts it at a multiple of 32 bytes with gcc for x86-64 and of 8 with gcc for arm64 and clang for
x86-64. So there any table can start inside a line, and the literal tables always do.
@assert ch12.ml.mod == ch12.ll.mod and ch12.tok.mod == 0 and ch12.ll.last_entry - ch12.ll.first_extra + 1 == ch12.ll.tail and ch12.lit.last_entry - ch12.lit.first_extra + 1 == ch12.lit.tail and ch12.lit.mod % 8 != 0

Until 11th October 2026 the decoder left that line out (#179). With canonical codes the last entries
belong to the longest codes, the rarest symbols, so it cost a miss only on a rare code. Line 31 is one
`prefetch()` more per table; where the table starts a line, it asks for a line the loop asked for
already. Aligning the tables to 64 bytes instead would need a struct aligned to 64, which the tests and
tools that get it from `malloc()` do not have. On x86-64 the extra prefetch made no difference in a quick
bench; it was not measured on the phone (commit `bafef14`).
:::

@listing src/seqlz_internal.h 140-146

The loop forms the pointers `q + k` up to `q + size - 64` only when `size` is a multiple of 512;
otherwise its last round would form pointers past `end` and prefetch lines behind the table. These
asserts stop the build when a table stops being one, e.g. length codes of at most 6 bits, 64 entries of
4 bytes. `sizeof_field(type, member)` is the kernel's `sizeof(((type *)0)->member)`.

## Copies of 8 and 16 bytes, and what overlap each allows {#ch-dec-basics--copies}

@listing src/seqlz_decompress.c 7-29

`__builtin_memcpy()` of a constant size is one load and one store of any alignment. It is the builtin and
not `memcpy()` because with `CONFIG_FORTIFY_SOURCE` the kernel's `memcpy()` can become a call
([chapter 6](#ch-header--summary)).

What matters is the order of loads and stores. `copy8()` loads 8 bytes, then stores them, so its source
may end in bytes the store just before it wrote, as long as `s + 8 <= d`. `copy16()` loads both halves
before it stores either, so its source and destination must not overlap at all; it is only for
literals, which come from the compressed page or the scratch.

A match with offset 10 and length 16 after `0123456789` must write `0123456789012345`: the format copies
byte by byte ([chapter 2](#ch-compression--overlap)), so the last 6 bytes repeat the first 6 of the match
itself. The first `copy8()` stores `01234567` at `d`. The second loads `d - 2` to `d + 5`, `89` and the
`012345` just stored, and writes `89012345` at `d + 8`. One `copy16()` from `d - 10` would have loaded
`d` to `d + 5` before anything was written there.

::: cpu A load right after a store to the same bytes
A store waits in a store buffer before it reaches the cache. A load of bytes a recent store wrote takes
them from there, **store forwarding**, but on most cores only when all its bytes come from one store.
With offsets from 9 to 15, the second `copy8()` reads bytes of the store just before and of older
memory, and waits until the store reached the cache. `ls_bad_status2.stli_other`, a counter of the
Ryzen 9 7950X, counted 359 such loads per page for `seqlz-1`'s decoder and 363 for `lz4`'s in
September 2026
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
Offsets below 8 avoid it: they store one register again and again, with no load after the first.
:::

## copy_literals(): 16 bytes at a time, also past the end {#ch-dec-basics--literals}

@listing src/seqlz_decompress.c 31-59

The caller, the slow path of `seqlz_decode()`, has checked that the `nl` literals are there and fit into
the page. `s_end` is not the end of the literals but of the memory behind them that may be read,
`lit_bound` in [chapter 14](#ch-decode): the end of the compressed page for raw literals, of the scratch
for coded ones. Copying 16 bytes where `nl` is smaller is the wild copy of
[chapter 3](#ch-cpu--wild-copy): the next sequence writes its match over the extra bytes.

The first `copy16()` runs whenever 16 bytes fit on both sides, also for `nl = 0`; as the comment says, a
loop that runs once or not at all, depending on the page, is a branch that mispredicts. The `while` loop
takes 16 bytes more as long as they fit, so the byte loop only runs within 15 bytes of the end of the
page or of the readable input, e.g. for the last sequence of a page. The casts make each compare one of
two unsigned numbers, the same on 32-bit and 64-bit builds; the differences are never negative.

## Offsets below 8: one register, stored again and again {#ch-dec-basics--pattern}

A match may start fewer bytes back than it is long and then repeats what it just wrote: offset 1 after
an `a` with length 6 writes `aaaaaa`, which is how a run of zeros compresses to a few bits. `copy8()` is
right only from offset 8 on. For offsets 1 to 7 the decoder builds the repeated pattern in a register.

@listing src/seqlz_decompress.c 61-65

@listing src/seqlz_decompress.c 67-86

`repeat_pattern(d, off)` returns the 8 bytes a byte-by-byte copy would write at `d`. The load takes 8
bytes from `d - off`; the first `off` are the pattern, the rest are at `d` and after, not written yet.
The mask keeps the low `8 * off` bits: for `off = 3`, `~0ULL >> {{ch12.rep.3.shift}}` is
`{{ch12.rep.3.mask}}`. The multiply by `repeat[off]` puts a copy of the pattern at every byte where the
constant has a 1. `repeat[3]` is `{{ch12.rep.3.mul}}`, ones in bytes 0, 3 and 6, and the pattern `abc`
is `0x636261`: the three copies do not overlap, so no carry crosses a byte, and the third copy's `c`
falls off the top. The result is `{{ch12.rep.3.prod}}`, the bytes `a b c a b c a b`. Some more, each
computed with `repeat_pattern()` on the `off` bytes `a`, `ab`, ... before `d`:

| `off` | pattern | `repeat[off]` | bytes at `d` | `repeat_step[off]` |
| --- | --- | --- | --- | --- |
| 1 | `{{ch12.rep.1.pat}}` | `{{ch12.rep.1.mul}}` | `{{ch12.rep.1.bytes}}` | {{ch12.rep.1.step}} |
| 3 | `{{ch12.rep.3.pat}}` | `{{ch12.rep.3.mul}}` | `{{ch12.rep.3.bytes}}` | {{ch12.rep.3.step}} |
| 5 | `{{ch12.rep.5.pat}}` | `{{ch12.rep.5.mul}}` | `{{ch12.rep.5.bytes}}` | {{ch12.rep.5.step}} |
| 7 | `{{ch12.rep.7.pat}}` | `{{ch12.rep.7.mul}}` | `{{ch12.rep.7.bytes}}` | {{ch12.rep.7.step}} |
@assert ch12.rep[3].step == 6 and [ch12.rep[k].step for k in (1, 2, 4)] == [8, 8, 8]

`repeat_step[off]` is the largest multiple of `off` up to 8, how far on the same 8 bytes fit again: for
`off = 3` the bytes at `d + 6` start with `a` again. So after the first store at `d`, stores of the same
register at `d + 6`, `d + 12`, ... write the rest of the match, without another load. For offsets 1, 2
and 4 the step is the full 8. Entry 0 of both tables is a placeholder: offset 0 is invalid and never gets
here, and its mask would be a shift by 64. The load and store are little endian on every CPU because the
multiply needs the pattern's first byte in the lowest byte ([chapter 3](#ch-cpu--endian)).

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 300" role="img" aria-label="A match with offset 3 and length 20: the register abcabcab is stored at d, d plus 6, d plus 12 and d plus 18, each store overlapping the one before by 2 bytes">
  <g font-size="12">
    <text x="10" y="34">page</text>
    <rect x="100" y="20" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="106" y="34">a</text><rect x="120" y="20" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="126" y="34">b</text><rect x="140" y="20" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="146" y="34">c</text>
    <rect x="160" y="20" width="520" height="20" fill="none" stroke="currentColor" stroke-dasharray="3 3"/><text x="300" y="34">not written yet</text>
    <text x="96" y="14" font-size="11">d - 3</text><text x="160" y="14" font-size="11">d</text>
    <text x="10" y="78">pat</text>
    <g class="acc"><rect x="160" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text class="on-fill" x="166" y="78">a</text><rect x="180" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text class="on-fill" x="186" y="78">b</text><rect x="200" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text class="on-fill" x="206" y="78">c</text><rect x="220" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text class="on-fill" x="226" y="78">a</text><rect x="240" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text class="on-fill" x="246" y="78">b</text><rect x="260" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text class="on-fill" x="266" y="78">c</text><rect x="280" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text class="on-fill" x="286" y="78">a</text><rect x="300" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text class="on-fill" x="306" y="78">b</text></g>
    <text x="330" y="78" font-size="11">repeat_pattern(d, 3) = {{ch12.rep.3.prod}}</text>
    <text x="10" y="122">k = 0</text><rect x="160" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="166" y="122">a</text><rect x="180" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="186" y="122">b</text><rect x="200" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="206" y="122">c</text><rect x="220" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="226" y="122">a</text><rect x="240" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="246" y="122">b</text><rect x="260" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="266" y="122">c</text><rect x="280" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="286" y="122">a</text><rect x="300" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="306" y="122">b</text>
    <text x="10" y="152">k = 6</text><rect x="280" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="286" y="152">a</text><rect x="300" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="306" y="152">b</text><rect x="320" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="326" y="152">c</text><rect x="340" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="346" y="152">a</text><rect x="360" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="366" y="152">b</text><rect x="380" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="386" y="152">c</text><rect x="400" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="406" y="152">a</text><rect x="420" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="426" y="152">b</text>
    <text x="10" y="182">k = 12</text><rect x="400" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="406" y="182">a</text><rect x="420" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="426" y="182">b</text><rect x="440" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="446" y="182">c</text><rect x="460" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="466" y="182">a</text><rect x="480" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="486" y="182">b</text><rect x="500" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="506" y="182">c</text><rect x="520" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="526" y="182">a</text><rect x="540" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="546" y="182">b</text>
    <text x="10" y="212">k = 18</text><rect x="520" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="526" y="212">a</text><rect x="540" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="546" y="212">b</text><rect x="560" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="566" y="212">c</text><rect x="580" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="586" y="212">a</text><rect x="600" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="606" y="212">b</text><rect x="620" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="626" y="212">c</text><rect x="640" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="646" y="212">a</text><rect x="660" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="666" y="212">b</text>
    <text x="10" y="262">result</text>
    <rect x="160" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="166" y="262">a</text><rect x="180" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="186" y="262">b</text><rect x="200" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="206" y="262">c</text><rect x="220" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="226" y="262">a</text><rect x="240" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="246" y="262">b</text><rect x="260" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="266" y="262">c</text><rect x="280" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="286" y="262">a</text><rect x="300" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="306" y="262">b</text><rect x="320" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="326" y="262">c</text><rect x="340" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="346" y="262">a</text><rect x="360" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="366" y="262">b</text><rect x="380" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="386" y="262">c</text><rect x="400" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="406" y="262">a</text><rect x="420" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="426" y="262">b</text><rect x="440" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="446" y="262">c</text><rect x="460" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="466" y="262">a</text><rect x="480" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="486" y="262">b</text><rect x="500" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="506" y="262">c</text><rect x="520" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="526" y="262">a</text><rect x="540" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="546" y="262">b</text>
    <g class="mark"><rect x="560" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text class="on-fill" x="566" y="262">c</text><rect x="580" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text class="on-fill" x="586" y="262">a</text><rect x="600" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text class="on-fill" x="606" y="262">b</text><rect x="620" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text class="on-fill" x="626" y="262">c</text><rect x="640" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text class="on-fill" x="646" y="262">a</text><rect x="660" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text class="on-fill" x="666" y="262">b</text></g>
    <text x="160" y="290" font-size="11">d .. d + 19: the match, 20 bytes</text><text x="680" y="290" font-size="11" text-anchor="end">d + 20 .. d + 25: past it</text>
  </g>
</svg>
</div>
<figcaption>A match of offset 3 and length 20: one register, stored 4 times 6 bytes apart; the stores overlap by 2 bytes, and the last one writes 6 bytes past the match, which the next sequence overwrites.</figcaption>
</figure>

## copy_match(): the slow path's match copy {#ch-dec-basics--copy-match}

`copy_match()` copies the matches that do not take the fast path of [chapter 14](#ch-decode): those
with a length value, up to a page long, and those near the end of the page. The fast path has its own
copy, `copy_match_fast()`.

@listing src/seqlz_decompress.c 88-105

The caller has checked that `d - off` is inside the page and `d + len` at most `d_end`. The last sentence
of the first paragraph has a measurement behind it: the first decoder of seqlz, in September 2026,
copied a match that ran up to the end of the page byte by byte, all of it. On a page that ends in a long
run of zeros that is up to 4000 single bytes with offset 1, each load waiting for the store before it,
and the slowest 1% of pages took 5700 ns, where `lz4` took 500
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-lz4s-matches-huffman-coded-sequences-with-static-tables)).

@listing src/seqlz_decompress.c 106-123

`off & 7U` changes nothing, `off` is below 8 here. It is left from the older
`step = off >= 8 ? 8U : repeat_step[off & 7U]`, which commit `b163902` moved into this branch. With
fewer than 8 bytes left in the page, the byte loop at the end copies all of the match. The comment on
lines 111 to 114 bounds the load of `repeat_pattern()`: it reads up to `d - off + 7`, at most `d + 6`,
inside the page also when the page is nearly full. For `off = 3, len = 20` the stores are at `k = 0, 6,
12, 18`, as in the figure. For the example page's match of [chapter 4](#ch-format--example),
`off = 2, len = 4094`, the loop stops at `k = 4088`, where 8 more bytes would not fit, and the byte loop
copies the last 6.
@assert ch10.ab.s1.ml == 4094 and ch10.ab.s1.off == 2

::: measured Stores only, no loads, for short offsets
Before 24th September 2026 the code built the first 8 bytes in a register as now, and then copied `step`
bytes on by loading what the store before had written: the same 8 bytes as the register. Commit
`8e8f0d1` stores the register instead. Decode cycles per page went from 7750 to 7276 in a loop over 2000
pages on the PC, cold p50 from 1405 to 1380 ns in the harness; in the kernel VM the change was within
the scatter. A copy of the first 4 bytes one by one, as `lz4` does it, was slower: the load after four
single-byte stores waits for them
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
The format has changed since; the numbers say which way the change went, not what it is worth now.
:::

@listing src/seqlz_decompress.c 124-145

From offset 8 on, `copy8()` is right: each load ends at `d + k - off + 7`, before the bytes its store
writes. The second of the first two copies reads what the first wrote, e.g. `d - 2` to `d + 5` for
`off = 10`, the example above. `k = 16` also when `len` was shorter: the extra bytes are inside the page,
and the next sequence overwrites them. The byte loop, shared by both branches, is the format's own
definition of a match, and runs only within the last 7 bytes of the page or for a match on a page with
fewer than 8 or 16 bytes left.

::: why 16 bytes first, not 32
79% of the matches are at most 16 bytes, 32 bytes cover 91%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).
When this function still copied every match, 32 bytes first made the median page faster and the slow
ones slower, p99 in a decode loop on the PC from 3570 to 4150 ns: the slowest pages have many short
matches, and each copied 32 bytes
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
Today the fast path takes the short matches, and this function gets mostly those with a length value,
{{ch12.ml_tok}} bytes and more. Measured again in a swap-in in the kernel VM, Ryzen 9 7950X at 4.5 GHz,
gcc 16.2.1, 32 bytes first was 3.5 ns faster on the mean of 6 boots, less than the boots scatter, so 16
stayed
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained)).
:::
@assert ch12.ml_tok == ch03.token_ml_max + 1

Until 10th October 2026 a second loop after both branches ran only for offsets of 8 and more. Moved into
its branch, it gave the same bytes and moved the decoder's timing no more than moving its code to
another address does
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#copy_match-without-its-dead-branch-within-what-the-codes-place-moves-kept)).

## The bit reader: 64 bits, most significant bit first {#ch-dec-basics--bit-reader}

After the literals, a compressed page has the bitstream: per sequence the token's code, the offset bits
and the length values, with no alignment to bytes ([chapter 4](#ch-format--layout)).

@listing src/seqlz_decompress.c 147-161

`bits` is **left-aligned**: the next bit of the stream is bit 63, so the next `n` bits as a number are
`bits >> (64 - n)`, and a code indexes its table with the register's top bits. `count` says how many of
the top bits are real; below them can be bits of a byte that is only partly in, or zeros. `p` is the
first byte not yet in whole.

`count` is an `int` on purpose. The decoder does not check before every read: past the end of the
stream it gets zeros, and `count` goes negative. One check at the end of the page finds that
([chapter 14](#ch-decode--end)). As the comment says, zeros instead of real bits can give wrong lengths
and offsets but never a wrong memory access, because each is checked against the room where it is used.

### seqlz_br_refill(): one load of 8 bytes, and count ends at 56 to 63

@listing src/seqlz_decompress.c 163-185

While 8 bytes of the stream are left, the refill loads 8 at once, `seqlz_br_refill_8()`, which the
fast path of [chapter 14](#ch-decode--next-token) calls by itself, as it knows that 8 bytes are there; in the last 7
it goes byte by byte,
because zram hands the decoder a buffer of exactly the compressed size, and the byte after it may belong
to another allocation. `get_unaligned_be64()` puts the byte at `p` into the top 8 bits, the stream's
order; on x86-64 and arm64 it is a load and a byte swap. `v >> count` puts it right below the `count`
real bits, and what does not fit falls off the bottom.

The next two lines need a worked example. Write `count` as $8a + b$, with $b$ from 0 to 7. Then
`63 - count` is $8(7 - a) + (7 - b)$, so `(63 - count) >> 3` is $7 - a$, the bytes that fit in whole,
and `p` moves on by them. The new count is $8a + b + 8(7 - a) = 56 + b$, and `count | 56` is $56 + b$
too, because $8a$ is at most 56 and has only bits that 56 has. So after a refill `count` is 56 to 63, and
the rest of the decoder can count on 56 bits; Python confirms the identity for all 64 values of `count`.
The bits of the partial byte at the bottom are not counted. The next refill loads that byte again, from
`p`, and shifts it to the same place, so the OR writes the same bits over them and needs no mask.

A made-up stream, `dc 65 ef db 8a 11 22 33 44 55 66 77 88 99 aa bb ...`, run through
`seqlz_br_refill()` and `seqlz_br_drop()`:

| step | `bits` | `count` | `p` |
| --- | --- | --- | --- |
| start | `0x0000000000000000` | 0 | 0 |
| refill: `v = {{ch12.br.1.v}}`, `v >> {{ch12.br.1.c0}}` | `{{ch12.br.1.bits}}` | {{ch12.br.1.c0}} \| 56 = {{ch12.br.1.count}} | {{ch12.br.1.p0}} + {{ch12.br.1.num}} / 8 = {{ch12.br.1.p}} |
| drop 15 bits | `{{ch12.br.drop1.bits}}` | {{ch12.br.drop1.count}} | {{ch12.br.drop1.p}} |
| refill: `v = {{ch12.br.2.v}}`, `v >> {{ch12.br.2.c0}} = {{ch12.br.2.vs}}` | `{{ch12.br.2.bits}}` | {{ch12.br.2.c0}} \| 56 = {{ch12.br.2.count}} | {{ch12.br.2.p0}} + {{ch12.br.2.num}} / 8 = {{ch12.br.2.p}} |
| drop 23 bits | `{{ch12.br.drop2.bits}}` | {{ch12.br.drop2.count}} | {{ch12.br.drop2.p}} |
| refill: `v = {{ch12.br.3.v}}`, `v >> {{ch12.br.3.c0}}` | `{{ch12.br.3.bits}}` | {{ch12.br.3.c0}} \| 56 = {{ch12.br.3.count}} | {{ch12.br.3.p0}} + {{ch12.br.3.num}} / 8 = {{ch12.br.3.p}} |

The first refill counts 7 bytes: `0x33` is at the bottom, not counted, and after the drop of 15 bits at
bits 22 to 15, `{{ch12.br.drop1.low}}`. The second refill loads from `p = {{ch12.br.2.p0}}`, so `v`
starts with `0x33`, and `v >> {{ch12.br.2.c0}}`, `{{ch12.br.2.vs}}`, puts it at bits 22 to 15 again,
followed by `0x44` and the top 7 bits of `0x55`. `p` moves on by 2 and stops at `0x55`.
@assert ch12.br[1].p == 7 and ch12.br.drop1.low == "0x198000" and ch12.br[2].vs == "0x19a22a" and ch12.br[2].p == 9

@anim refill wide The example of the table, one operation per step: each refill loads, shifts, ORs, moves p and sets count, and the decoder drops 15 and then 23 bits between them. Computed in the browser with 64-bit integers, as the code does it.

The fast half needs `count` from 0 to 63 for its shift. It never sees a negative one, which only happens
after the stream ran out, and never 64, which only the byte half reaches, after which `end - p` stays
below 8. The byte half puts one byte below the counted bits per round and stops when no whole byte fits,
when the stream is used up, or when `count` is negative, where `56 - count` would be a shift past 56 and,
below -7, by 64 or more. The example page of [chapter 4](#ch-format--example) has a bitstream of
{{ab.b.bytes}} bytes, `{{ab.b.hex}}`, so its first refill is the byte half: `bits = {{ch12.ab.bits}}`,
`count = {{ch12.ab.count}}`, `p = end`, and every later refill does nothing.
@assert ab.b.bytes < 8 and ch12.ab.at_end == 1

### seqlz_br_drop(): taking bits

@listing src/seqlz_decompress.c 187-192

The `(int)n` keeps the subtraction signed, so `count` can go negative. `n` is never 64: the largest drop
is a token's code with its offset bits, $11 + 14$ bits with 16 KiB pages.

### seqlz_br_value(): a length value, with two shifts where one would do

@listing src/seqlz_decompress.c 194-213

A length value is a symbol of the `ll` or `ml` table and then `x` extra bits, added to a base
([chapter 4](#ch-format--length-values)). One lookup with the next 8 bits gives the code's length `n` in
bits 0 to 3, `x` in bits 4 to 7 and the base in bits 8 to 23 ([chapter 7](#ch-internal--value-table)).
The `x` bits after the code would be `(bits << n) >> (64 - x)`, a shift by 64 for `x = 0`, which x86-64
does as a shift by 0 ([chapter 3](#ch-cpu--ubsan)). `>> 1` and then `>> (63 - x)` gives 0 for `x = 0`
and the same as one shift by `64 - x` otherwise, without a branch.

The match length value of the example page, with the 4 KiB tables: after the escape's code, the token's
12 bits and the offset, {{ch12.ab.tokdrop}} bits, the register is `{{ch12.ab.bits2}}` with
`count = {{ch12.ab.count2}}`.

- The index is the top 8 bits, {{ch12.ab.idx}}, and with `t` the `ml` table, `t->decode[{{ch12.ab.idx}}]` is `{{ch12.ab.entry}}`.
- `n = {{ch12.ab.n}}`: the code `{{ch12.ab.code}}`, symbol {{ch12.ab.sym}}. `x = {{ch12.ab.x}}`, and the
  base is {{ch12.ab.base}}, $2^{ {{ch12.ab.x}} }$.
- `bits << {{ch12.ab.n}}` is `{{ch12.ab.shl}}`, `>> 1` is `{{ch12.ab.shr1}}`, `>> {{ch12.ab.shr2}}` is
  {{ch12.ab.xval}}, the bits `{{ch12.ab.xbits}}`.
- The value is `{{ch12.ab.base}} + {{ch12.ab.xval}} = {{ch12.ab.value}}`, {{ch12.ab.dropped}} bits are
  dropped and `count = {{ch12.ab.count3}}`, the zeros at the end of the last byte.
@assert ch12.ab.code == ml.code[ch12.ab.sym] and ch12.ab.base == 2 ** ch12.ab.x and ch12.ab.count3 == ab.b.pad and ch12.ab.escaped == 1

`seqlz_decode()` adds the token's {{ch12.ml_tok}}, and the match is {{ch12.ab.match}} bytes, as the
format's example says. A direct symbol: the code `{{ch12.ml0.code}}` gives
`t->decode[{{ch12.ml0.index}}] = {{ch12.ml0.entry}}`, `n = {{ch12.ml0.n}}`, `x = 0`, and the value
{{ch12.ml0.base}}, the shift by 63 gives 0 for the extra bits.
@assert ch12.ml0.x == 0 and ml.code[ch12.ml0.base] == ch12.ml0.code and ch12.ab.match == 4094

The comment's bound, `SEQLZ_MAX_BITS + SEQLZ_BLOCK_BITS`, 20 or 22 bits, has a bit to spare: a code
has at most 8 bits, a value at most {{ch10.len_extra_max}} extra bits with 4 KiB pages and
{{k16.ch10.len_extra_max}} with 16 KiB pages, and the caller refills before, which leaves at least 56.
The largest base, {{k16.ch12.base_max|grp}} with 16 KiB pages, fits the entry's 16 bits.
@assert F("k16.ch12.base_max") < 65536

These three functions and a token lookup that [chapter 14](#ch-decode--next-token) writes inline are
all the decoder does with the bitstream. The literal streams of [chapter 13](#ch-literals-dec) have a
reader without a `count`, because 8 of them have to fit into the registers at once.
