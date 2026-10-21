# seqlz_decode(): one table lookup per sequence {#ch-decode}

`seqlz_decode()` turns a compressed page back into its `SEQLZ_PAGE` bytes. It reads the header, has the
coded literals decoded into the scratch if the page has them ([chapter 13](#ch-literals-dec)), and runs
one loop over the sequences: one lookup in the token table, the offset from the bits after the code,
the copies of the literals and the match. There is no count of the sequences, and the last one has no
token: the loop ends when the literals left fill the rest of the page.

The loop has two paths. The **fast path** takes a sequence without a length value that is far from the
end of the page, of the literals and of the input: fixed copies without loops and without checks of
room, and the next token's lookup before the copies. The **slow path** takes everything else, with every
check. On 7th October 2026, with the tables and offset classes of then, 178.5 of the 207 sequences of an
average desktop zram page took the fast path
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-fast-paths-condition-its-22-misses-per-page-are-the-length-values-four-ways-around-them-slower-not-kept)).

The loop is in `decode_page()`, and `seqlz_decode()` calls it with `xored` the constant `true` or
`false`: two inlined copies, one of which undoes level 3's XOR of the literals after each copy of them
([below](#ch-decode--xored)). The rest of this chapter is about `decode_page()`, which is
`seqlz_decode()` for every page that is not XORed.

<pre class="mermaid">
flowchart TB
    S["decode_page():<br/>prefetch tables, header,<br/>literals raw or decoded"] --> N0["tok = next_token()"]
    N0 --> E{"escape?"}
    E -->|yes| X["refill, read 12 bits,<br/>tok = token_entry(idx, 0)"]
    E -->|no| O
    X --> O["offset from the entry,<br/>drop code and offset bits"]
    O --> C{"no length value,<br/>d &lt;= d_fast,<br/>lit &lt;= lit_fast,<br/>br.p &lt;= in_fast?"}
    C -->|"yes: fast path"| F["next_token_fast():<br/>next token first,<br/>refill every second time"]
    F --> FC["copy16() literals,<br/>unxor_fast() if XORed,<br/>check off,<br/>copy_match_fast()"]
    FC --> E
    C -->|"no: slow path"| L["ll value if any,<br/>check nl, copy_literals(),<br/>unxor_literals() if XORed"]
    L --> M["ml value if any,<br/>check off and len,<br/>copy_match()"]
    M --> D{"literals left =<br/>bytes left?"}
    D -->|yes| END["last sequence:<br/>copy_literals() the literals left,<br/>then the end checks"]
    D -->|no| NT["tok = next_token(), skip = 0"]
    NT --> E
</pre>

The file first has the helpers only this loop uses: the fast path's match copy, the two ways to get the
next token, and the room the fast path needs. The slow path's helpers are in
[chapter 12](#ch-dec-basics).

## copy_match_fast(): the match without loops {#ch-decode--copy-match-fast}

@listing src/seqlz_decompress.c 650-673

On the fast path a match has at most {{ch14.fast_len}} bytes, since the token's longest `ml` means that a
length value follows. The caller guarantees {{ch14.cmf.room}} bytes of room at `d`. From offset 8 on the
copies are 16, 32 or 40 bytes, always at least `len`, and each `copy8()` loads before it stores, so for
`8 <= off < 16` a load reads what the store before wrote, the overlap the format asks for
([chapter 12](#ch-dec-basics--copies)). `m = d - off` once, because clang computed `d + 8 - off` anew
for every load: on the Mi 9T's little Cortex-A55 that took the decoder from 17 842 to 17 542
instructions per page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).

::: measured Branches on len, not always 40 bytes
On the Mi 9T, 32 bytes always and 40 bytes always both read slower than the code as it is: warm reads
by 0.1 µs on the A76 and 0.5 to 0.8 µs on the A55, cold reads by 1.1 and 1.2 µs on the A76
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#6-the-fast-paths-copies-as-they-are)).
Measured again in a swap-in in the kernel VM, where the predictor has not seen the page, the copies
without branches took out 14 mispredictions per page and were still about 30 ns slower per decode, 250
ns at the swap-in's p99, most likely because the added loads read bytes the stores just before them
wrote
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained)).
:::

@listing src/seqlz_decompress.c 674-690

Below offset 8, the pattern register of `repeat_pattern()` ([chapter 12](#ch-dec-basics--pattern)) is
stored 5 times without a loop. The smallest step is {{ch14.cmf.min_step}}, for
`off = {{ch14.cmf.min_step_off}}`, and then the 5 stores cover
$4 \times {{ch14.cmf.min_step}} + 8 = {{ch14.cmf.five_stores}}$ bytes, the comment's "at least 28"; with
step 8, for offsets 1, 2 and 4, they cover {{ch14.cmf.five_stores_8}}, every match of the fast path. The
loop runs only for a longer match with a smaller step, e.g. `off = 5, len = 34` stores at 25 and 30.
Run on all offsets 1 to 7 and lengths 4 to {{ch14.fast_len}} when the book is built, the furthest byte
written is `d + {{ch14.cmf.max_byte}}`: that is the {{ch14.cmf.room}} bytes of room. The caller has
rejected offset 0, so `repeat_step[off]` needs no `& 7U` here. A loop instead of the 5 stores, tried
again for simpler code before 10th October 2026, was 0.5 to 0.7% slower in the kernel VM with gcc and
clang
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
@assert ch14.cmf.min_step == 5 and ch14.cmf.min_step_off == 5 and ch14.cmf.step1 == ch14.cmf.step2 == ch14.cmf.step4 == 8 and ch14.cmf.room == 40

## The next token: refill only when the bits might run out {#ch-decode--next-token}

@listing src/seqlz_decompress.c 692-705

The slow path and the first sequence get their token here. It refills only below an 11-bit code plus the
longest offset, {{ch14.need}} bits with 4 KiB pages and {{k16.ch14.need}} with 16 KiB pages. The index
is the top 11 bits, and every index is a valid entry because the token code is complete
([chapter 2](#ch-compression--complete)). The `(int)` keeps the compare signed, so a negative `count`
reads as what it is. A refill puts a load in front of the lookup, whose index comes from `bits`; with the
check, most lookups use bits already in the register, and the refill's load comes about every two or
three sequences.

::: measured Refill below 23 bits instead of every sequence
September 2026, `seqlz-1` in the kernel VM, Ryzen 9 7950X at 4.5 GHz: cold reads, with another page
read first, went from 2811 / 4501 to 2610 / 4300 ns at p50 / p99. A hot loop over 2000 pages got slower,
6883 to 7115 cycles per page, with 39 more mispredictions: the check is a branch on data. The kernel's
numbers decided
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
The format of then had other tables and another bit order.
:::

### next_token_fast(): the next token before the copies

@listing src/seqlz_decompress.c 707-735

**The next token before the copies.** No length value follows on the fast path, so the next token's code
starts right after the offset bits, and its entry can be loaded before the copies. An out-of-order core
would overlap the two anyway; an in-order core such as the A55 would wait for the lookup at the top of the
next round ([chapter 3](#ch-cpu--in-order)). On 3rd October 2026, phone pages in the userspace harness,
the little core's cold p50 against `lz4` went from 2169 to 2007 ns more, the big core's from 456 to 330
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-next-token-before-the-copies-reads-faster-on-both-phone-cores-kept)).

**The refill every second fast sequence, without a check.** Whether 23 bits are left depends on the code
lengths before, which the predictor learns only for a page it has seen, and a swap-in decodes a page
once. In a page seen once that branch was the largest single source of mispredictions in the decoder
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#which-branches-three-in-the-sequence-loop-are-58-of-the-130-misses-of-a-page-seen-once)).
So the fast path refills on a schedule: `*skip` alternates, and a branch that alternates is one every
predictor gets right. The refill is the fast half of `seqlz_br_refill()` without the check of bytes
left.

Why the bits never run short: after a refill `count` is at least 56, and until the next one two fast
sequences drop their code and offset bits, the second one before its own `next_token_fast()` refills.

| page size | most offset bits | one fast sequence | two | left of 56 |
| --- | --- | --- | --- | --- |
| 4 KiB | {{ch14.max_off_bits}}, class 3 | {{ch14.fast_seq_bits}} | {{ch14.fast_two_bits}} | {{ch14.fast_left}} |
| 16 KiB | {{k16.ch14.max_off_bits}}, class 3 | {{k16.ch14.fast_seq_bits}} | {{k16.ch14.fast_two_bits}} | {{k16.ch14.fast_left}} |
@assert ch14.class3_bits == ch14.max_off_bits and F("k16.ch14.class3_bits") == F("k16.ch14.max_off_bits")

The peek at the next token needs 11 of them, and after the first sequence at least
{{k16.ch14.fast_after_one}} are left. An escape refills on its own before its 12 bits, which only adds
bits. After a slow sequence, whose `next_token()` guarantees only {{ch14.need}} or {{k16.ch14.need}}
bits, `skip = 0` restarts the schedule. The load has its two preconditions from the fast path's
condition: `br.p <= in_fast` leaves 8 bytes of input, and the bounds above keep `count` at 0 or more.

::: measured The refill without its branch
7th October 2026, x86-64 in userspace, gcc 16.2.1, 19 577 desktop zram pages each decoded once: branch
misses per page from 130 to 101, cycles from about 6960 to about 6700. In the kernel VM, `MODE=swap`,
CPU 2 of the Ryzen 9 7950X at 4.5 GHz, 3 boots each: `zcomp_decompress()` of `seqlz-2` took 1935
to 1941 ns instead of 2010 to 2011 with gcc, 1976 to 1983 instead of 2020 to 2040 with clang 22.1.8. On
the Mi 9T the A76 decoded a page seen once in 3241 to 3272 ns instead of 3309 to 3340; the A55 got
slower and kept a loop of its own until 10th October 2026, when every core got this one loop
([chapter 17](#ch-decisions--one-loop-for-all-cores),
[explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-refill-without-its-branch-every-second-fast-sequence-63-ns-less-per-swap-in-on-x86-64-and-the-a76-kept)).
:::

## The fast path's three rooms {#ch-decode--rooms}

@listing src/seqlz_decompress.c 737-744

A fast sequence writes the 16 bytes of `copy16()` at `d`, moves on by at most {{ch14.fast_nl}}
literals and writes at most {{ch14.cmf.room}} bytes of match, {{ch14.cmf.max_write}} bytes in all, below
the {{ch14.room.page}} of `FAST_PAGE_ROOM`. One `copy16()` reads 16 bytes of literals, and the refill of
`next_token_fast()` loads 8 bytes of input.
@assert ch14.room.page == 64 and ch14.room.lit == 16 and ch14.room.input == 8 and ch14.cmf.max_write == 54 and ch14.fast_nl == 14

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 330" role="img" aria-label="The three rooms of the fast path: in the page it starts no later than 64 bytes before the end and writes at most 54 bytes; in the literals it reads 16 bytes and starts no later than 16 bytes before lit_bound; in the input a refill loads 8 bytes and starts no later than 8 bytes before the end">
  <g font-size="12">
    <text x="10" y="20" font-weight="bold">the page, dst to d_end</text>
    <rect x="20" y="30" width="420" height="24" class="fill-sheet" stroke="currentColor"/>
    <text x="30" y="47">written so far, and room</text>
    <g class="mark"><rect x="440" y="30" width="240" height="24" class="fill-mark" stroke="currentColor"/></g>
    <text x="450" y="47" class="on-fill">last 64 bytes: slow path only</text>
    <text x="440" y="70" font-size="11">d_fast = d_end - 64</text>
    <text x="680" y="70" font-size="11" text-anchor="end">d_end</text>
    <g class="acc"><rect x="440" y="80" width="60.0" height="18" class="fill-acc" stroke="currentColor"/>
    <rect x="492.5" y="100" width="150.0" height="18" class="fill-acc" stroke="currentColor"/></g>
    <text x="434" y="94" font-size="11" text-anchor="end">copy16() of the literals, 16 bytes at d</text>
    <text x="434" y="114" font-size="11" text-anchor="end">copy_match_fast() at d + nl, nl &lt;= 14: up to 40 bytes</text>
    <text x="648.5" y="114" font-size="11">d + 54</text>
    <text x="10" y="150" font-weight="bold">the literals, lit to lit_bound</text>
    <rect x="20" y="160" width="520" height="24" class="fill-sheet" stroke="currentColor"/>
    <text x="30" y="177">literals not copied yet</text>
    <rect x="540" y="160" width="80" height="24" fill="none" stroke="currentColor" stroke-dasharray="3 3"/>
    <text x="546" y="177" font-size="11">readable</text>
    <g class="mark"><rect x="620" y="160" width="60" height="24" class="fill-mark" stroke="currentColor"/></g>
    <text x="540" y="200" font-size="11">lit_end</text>
    <text x="620" y="214" font-size="11" text-anchor="end">lit_fast = lit_bound - 16</text>
    <text x="680" y="200" font-size="11" text-anchor="end">lit_bound</text>
    <text x="20" y="232" font-size="11">raw literals: lit_bound = s_end, the bitstream lies behind lit_end; coded: lit_bound = scratch + {{const.SCRATCH}}</text>
    <text x="10" y="262" font-weight="bold">the bitstream, br.p to s_end</text>
    <rect x="20" y="272" width="630" height="24" class="fill-sheet" stroke="currentColor"/>
    <text x="30" y="289">bytes not yet in the register</text>
    <g class="mark"><rect x="650" y="272" width="30" height="24" class="fill-mark" stroke="currentColor"/></g>
    <text x="650" y="314" font-size="11" text-anchor="end">in_fast = s_end - 8</text>
    <text x="680" y="326" font-size="11" text-anchor="end">s_end</text>
  </g>
</svg>
</div>
<figcaption>Where the fast path may start: at least 64 bytes before the end of the page, of which it writes at most 54; at least 16 readable bytes of literals; at least 8 bytes of bitstream.</figcaption>
</figure>

## Two copies of the loop: decode_page() {#ch-decode--xored}

@listing src/seqlz_decompress.c 746-755

`decode_page()` is the whole decoder, with one parameter more than `seqlz_decode()`: `xored`, whether
the page's literals are XORed ([chapter 6](#ch-header--literals)). `seqlz_decode()` comes last in the
file, and is the only caller:

@listing src/seqlz_decompress.c 937-948

It reads byte 2 only for a page with coded literals, and only if there is a byte 2, `src_len > 2`; a
page with raw literals is never XORed, and bit 7 of its byte 2 is a literal. Each call passes a
constant, and `decode_page()` is `__always_inline`, so the compiler makes two copies of the loop: in
the one with `false` the `if (xored)` lines are gone, and a page without a XOR runs the loop as it was
before the XOR existed. In the one with `true` the loop undoes the XOR right after each sequence's
literals are copied, with `unxor_fast()` on the fast path and `unxor_literals()` on the slow path and
for the last sequence ([chapter 13](#ch-literals-dec--unxor)): the next match may copy from those
literals, so they have to be the page's own before it. `seqlz_decode()` checks only which copy to run;
`decode_literals()` reads and checks the XOR's byte.

::: why A second copy of the loop, not a test per sequence
A check of the XOR for every sequence in the one loop, and of the records in the encoder's, made levels
1 and 2 slower to read and about 6% slower to write on the Mi 9T's Cortex-A76, userspace, 21st October
2026. With the two copies they read as fast as before, within the run's spread, and level 3 reads
0.37 µs slower per page on the A76 and 1.0 µs on the A55 than without the XOR, the mean of two phone
dumps
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-xors-its-literals-with-an-earlier-byte-of-the-page-38-fewer-bytes-than-seqlz-3-of-15-october-kept)).
The cost is code size: the loop is in the decoder twice.
:::

## Setup: the header, the tables, and where the literals are {#ch-decode--setup}

@listing src/seqlz_decompress.c 756-767

The parameters and the return values, 0 or `-EINVAL`, are those of `seqlz.h`
([chapter 6](#ch-header--encode-decode)). `xor` is the page's XOR, which `decode_literals()` sets for a
page with coded literals and which stays 0 for raw ones. `lit_bound` is one past the last byte that may be read where
the literals are, which is more than `lit_end`. `last`, the repeat offset, starts at 1, as the format
says ([chapter 4](#ch-format--offsets)), and `skip = 0` has the first fast sequence refill.

@listing src/seqlz_decompress.c 768-775

The tables are prefetched before the header is read, {{ch14.pf.token}} prefetches for the token table
and {{ch14.pf.len}} for each length table ([chapter 12](#ch-dec-basics--prefetch)), so that the header
and the literals run while the lines are on their way. `get_unaligned_le16()` replaced a helper of
seqlz's own on 10th October 2026, within the noise in the kernel VM and on the phone
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16)).

@listing src/seqlz_decompress.c 776-798

`SEQLZ_LIT_CODED` picks the layout ([chapter 4](#ch-format--layout)). A page with coded literals needs a
scratch; zram at level 1 writes only raw literals and may call the decoder without one
([chapter 15](#ch-zram-glue)). `decode_literals()` also gets `d`, the output page, where a page with a
table of its own has that table built before the loop below writes the page
([chapter 13](#ch-literals-dec--own)), and `&xor` for the XOR's byte. Its literals end at `lit_end` in the scratch, and `lit_bound` is
the end of the scratch, 16 bytes more, for the 16-byte copies. Raw literals are right after the header, and
`lit_bound` is the end of the input: behind the literals is the bitstream, which may be read though it
is not literals. The `u64` sum cannot wrap; with 15 bits of `n_lit` neither could a 32-bit one, but then
the reader would have to know that.

A raw page with more than `SEQLZ_PAGE` literals is not rejected here. It needs no check: its literals left
are always more than the bytes left, the loop never finds its end, and a check of room rejects it.

@listing src/seqlz_decompress.c 800-823

`lit_fast` and `in_fast` are numbers because with a short input `lit_bound - 16` would be a pointer
before the buffer, which C does not allow even unused; `unsigned long` has the size of a pointer on every
architecture the kernel has. An input shorter than 8 bytes gets `in_fast = 0`, so the fast path, whose
refill loads 8 bytes unchecked, is never taken.

The end comes when the bytes the page still needs, `d_end - d`, are the literals left, `lit_end - lit`:
no match fits then, since every match has at least 4 bytes, and the literals left are the last sequence
([chapter 4](#ch-format--end)). `d - lit == d_end - lit_end` is the same equation with a right side that
never changes, so `end_gap` is computed once and the loop compares two pointers it moves anyway, as
numbers because they point into different buffers. A page of `SEQLZ_PAGE` literals ends before its first
token and jumps to the copy after the loop; with raw literals such a page is never stored, with coded ones
it can be smaller than a page. The first `next_token()` finds `count = 0` and refills.

## The loop: escape, then the offset without a branch {#ch-decode--offset}

@listing src/seqlz_decompress.c 824-839

The loop ends with the `break` when the literals left fill the page, or with `return -EINVAL`. Only
{{ch14.tokens_coded}} of the {{const.TOKEN_SYMBOLS}} tokens have a code of their own in the 4 KiB tables;
the others are the escape's code, `{{tok.code.3072}}`, and the token's number in 12 bits
([chapter 4](#ch-format--token)). The escape's entry has bit 31 set and only the code's length in its
drop field ([chapter 7](#ch-internal--token-entry)). The refill is needed because `next_token()` made sure
of only {{ch14.need}} or {{k16.ch14.need}} bits before the escape's code. Of the 4096 numbers 12 bits can
hold, {{ch14.no_token}} are no token. `token_entry(idx, 0)` builds the entry the table would have, with a
code length of 0 since the code and the 12 bits are dropped already; from here on, an escaped token is
like any other. E.g. token 513, {{ch14.t513.ll}} literal and a match of {{ch14.t513.ml}} at class
{{ch14.t513.cls}}, has no code and comes as `{{tok.code.3072}}`, `{{ch14.t513.bin}}` and
{{ch14.t513.raw_bits}} offset bit; `token_entry(513, 0)` is `{{ch14.t513.entry}}`.
@assert tok.len[513] == 0 and ch14.t513.N == 0

@listing src/seqlz_decompress.c 840-851

This is what [chapter 7](#ch-internal--token-entry) built the entry for: the offset in three shifts and a
select. `<< TOK_N` shifts the code out, `>> back`, with `back = 64 - raw_bits`, brings the offset bits to
the bottom, and `<< TOK_SHIFT` multiplies by 2 for class 1 and by 8 for classes 4 and 5. Class 0, the
repeat offset, has no offset bits, and `back` is stored as `(64 - raw_bits) & 63`, 0 and not 64; `raw` is
then a meaningless number that the select does not use. Three tokens from the 4 KiB tables, with made-up
bits `0101...` after the code and the offset bits:

| | class {{ch14.tk.a.cls}}: token {{ch14.tk.a.s}} | class {{ch14.tk.b.cls}}: token {{ch14.tk.b.s}} | class {{ch14.tk.c.cls}}: token {{ch14.tk.c.s}} |
| --- | --- | --- | --- |
| sequence | `ll` {{ch14.tk.a.ll}}, `ml` {{ch14.tk.a.ml}}, offset {{ch14.tk.a.off}} | `ll` {{ch14.tk.b.ll}}, `ml` {{ch14.tk.b.ml}}, offset {{ch14.tk.b.off}} | `ll` {{ch14.tk.c.ll}}, `ml` {{ch14.tk.c.ml}}, the last offset |
| code, offset bits | `{{ch14.tk.a.code}}`, `{{ch14.tk.a.ob}}` | `{{ch14.tk.b.code}}`, `{{ch14.tk.b.ob}}` | `{{ch14.tk.c.code}}`, none |
| `TOK_N` / `TOK_BACK` / `TOK_SHIFT` / `TOK_DROP` | {{ch14.tk.a.N}} / {{ch14.tk.a.BACK}} / {{ch14.tk.a.SHIFT}} / {{ch14.tk.a.DROP}} | {{ch14.tk.b.N}} / {{ch14.tk.b.BACK}} / {{ch14.tk.b.SHIFT}} / {{ch14.tk.b.DROP}} | {{ch14.tk.c.N}} / {{ch14.tk.c.BACK}} / {{ch14.tk.c.SHIFT}} / {{ch14.tk.c.DROP}} |
| `br.bits` | `{{ch14.tk.a.bits}}` | `{{ch14.tk.b.bits}}` | `{{ch14.tk.c.bits}}` |
| `<< TOK_N` | `{{ch14.tk.a.shl}}` | `{{ch14.tk.b.shl}}` | `{{ch14.tk.c.shl}}` |
| `>> back` | `{{ch14.tk.a.shr}}` | `{{ch14.tk.b.shr}}` | `{{ch14.tk.c.shr}}` |
| `off` | {{ch14.tk.a.ob_val}} << {{ch14.tk.a.SHIFT}} = {{ch14.tk.a.off}} | {{ch14.tk.b.ob_val}} << {{ch14.tk.b.SHIFT}} = {{ch14.tk.b.off}} | `last` |
@assert ch14.tk.a.SHIFT == 1 and ch14.tk.a.raw_bits == 1 and ch14.tk.c.BACK == 0 and ch14.tk.c.n == 4

The made-up bits do not change the lookup: token {{ch14.tk.c.s}}'s {{ch14.tk.c.n}}-bit code fills the
entries {{ch14.tk.c.idx_lo}} to {{ch14.tk.c.idx_hi}}. gcc 16.2.1 at `-O2` for x86-64 makes the select a
`cmovne`, no branch, and the `& 63U` of `TOK_N()` costs no instruction, since x86-64 uses only the low 6
bits of a shift count. Before 3rd October 2026 the entry had 2 bytes, and the decoder computed the class,
the offset bits and the shift at run time: 18 of about 60 instructions per sequence on the A55. The
entries of 4 bytes took the decoder there from 16 547 to 15 539 instructions per page, the length value
flag in bit 30 to 15 129
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).
`TOK_ML` is `ml` itself, 4 to 35.

## The fast path {#ch-decode--fast}

@listing src/seqlz_decompress.c 852-883

Each part of the condition lets the body skip a check. Bit 30 of the entry, `TOK_VALUE_AT`, says that no
length value follows, so `nl` is at most 14 and `len` at most 34; one test instead of two compares, and
without it the A55 read raw pages 0.2 µs slower
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
The other three are the rooms above.

The page's last match never takes the fast path: after it fewer than 8 bits are left, and the fast path
needs 8 bytes of input not yet loaded. So every page ends on the slow path, where the end is checked. On a
damaged page the literals left could equal the bytes left right after a fast sequence; then every further
match moves them apart by its length, they never meet again, and a check of room rejects the page.

The offset check is one unsigned compare for `1 <= off <= d - dst`: `off = 0` wraps to the largest
`unsigned int`. With 24 bytes written, `off = 24` gives `23 >= 24`, false, a valid offset; `off = 25`
gives `24 >= 24`, invalid.

With `xored`, `unxor_fast()` undoes the XOR on the 16 bytes `copy16()` wrote, before `last` becomes
this sequence's offset: the XOR of `SEQLZ_XOR_LAST` is by the offset of the sequence before.

`nl` is not checked against the literals left, which saved 3 instructions per sequence on 3rd October
2026 (commit `5c769f5`). A page whose tokens claim more literals than its header has moves `lit` past
`lit_end` and copies the bytes behind the literals, still inside `lit_bound`. The next slow sequence
checks `lit > lit_end` first, and only the slow path ends the loop, so the page is rejected.

::: warn The check that moved
The first fast path, in September 2026, needed a check of `nl` that no test covered: without it `lit`
ran past the literals, the slow path computed `lit_end - lit` as a negative number, cast to unsigned, so
every `nl` passed, and `copy_literals()` read behind the input. A test builds such a page since then
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
When the check left the fast path on purpose, the `lit > lit_end` of the slow path came with it; without
it, AddressSanitizer reported a heap overflow in that test
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).
:::

## The slow path: length values, and every check {#ch-decode--slow}

@listing src/seqlz_decompress.c 884-895

An `ll` of 15 means a literal length value follows the offset bits
([chapter 4](#ch-format--length-values)); the refill makes sure of the 20 or 22 bits `seqlz_br_value()`
may take. `lit > lit_end` has to come first: with `lit` past `lit_end`, the cast of `lit_end - lit` would
let every `nl` pass. `copy_literals()` may read up to `lit_bound` ([chapter 12](#ch-dec-basics--literals)).
With `xored`, `unxor_literals()` undoes the XOR on exactly the `nl` literals, again before `last`
changes.
Until 11th October 2026 the last sequence had a token too, with a match of nothing, and the decoder checked
for it here; it said what the decoder knew already and cost about 1.2 bytes per page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-last-sequence-without-a-token-12-bytes-per-page-less-kept)).

@listing src/seqlz_decompress.c 896-917

With 4 KiB pages a match length value is at most {{ch14.max_value}}, so `len` can be {{ch14.max_len}},
more than a page, which the check of room catches. `last` is set before the checks; if they fail, it does
not matter. After a slow sequence the next token comes through `next_token()`, with its check of the bits
left.
@assert ch14.max_len > 4096

## The end: a page must end where its bits end {#ch-decode--end}

@listing src/seqlz_decompress.c 918-935

When the loop is left, the literals left are exactly the bytes the page still needs, so the copy fills the
page and uses every literal. The XORed page undoes the XOR on them as on any sequence's literals, the
last sequence being no different in the format. Its two guards, `lit <= lit_end` and the `min()` with
the room left, change nothing on the way the loop gets here, where `lit` is at most `lit_end` and the
two counts are equal; they keep `unxor_literals()` inside the page without that argument. A `memcpy()` here, a call instead of a second inlined copy loop, made the
swap-in 0.02 µs slower in the kernel VM, at the edge of what the boots differ by
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-last-sequence-without-a-token-12-bytes-per-page-less-kept)).
The checks then make sure the input has nothing else to say:

| check | rejects |
| --- | --- |
| `br.count < 0` | a page cut short: the reader filled in zeros |
| `br.p != br.end` | bytes of the bitstream never read |
| `br.count >= 8` | a whole byte after the last code, also a zero byte |
| `br.count > 0 && (br.end[-1] & ...)` | a bit 1 in the padding of the last byte |

A header with more literals than the sequences use fails before, in a check of room. The padding is read
from `br.end[-1]` and not from the register, which would be a shift by `64 - count`, 64 for
`count = 0`. Why so strict is in [chapter 4](#ch-format--end): zram stores no checksum. The checks came
on 8th October 2026; the compressor always wrote pages that pass.

## Why no input makes it read or write out of bounds {#ch-decode--safety}

Constraint C6 of the project is a decoder that is safe and bounded in time for any input; a decoder that
was not was one of the reasons a compressor for zram was not merged before
([plan.md](https://github.com/martinus/quetschn/blob/main/docs/plan.md#1-goal-and-success-criteria)).
For any bytes and any `src_len`:

- **The input.** The header after `src_len >= 2`, raw literals after `2 + n_lit <= src_len`, coded ones
  in `decode_literals()`. The bitstream's 8-byte loads run only with 8 bytes left, by the check in
  `seqlz_br_refill()` or by `br.p <= in_fast`, and move `br.p` by at most 7; the byte loop checks every
  byte. `br.end[-1]` only after a byte was read.
- **The literals.** The fast path reads 16 bytes only at `lit <= lit_bound - 16` and moves `lit` by at most
  14; the slow path checks `lit` and `nl` first, and `copy_literals()` stays below `lit_bound`.
- **The tables.** 11 bits index 2048 token entries, 8 bits 256 length entries; an escaped token's number
  is checked before `token_entry()`, which reads no table.
- **The XOR.** `unxor_literals()` and `unxor_fast()` write only the literals just copied, or the
  16 bytes of the fast path, and read only bytes of the page before them
  ([chapter 13](#ch-literals-dec--safety)).
- **The page, for matches.** `1 <= off <= d - dst` before each match. `repeat_pattern()` reads up to
  `d + 6`, and its bytes at `d` and after are masked away.
- **Writes.** The fast path writes at most {{ch14.cmf.max_write}} bytes from at least
  {{ch14.room.page}} before `d_end`; the slow path checks `nl` and `len` against the room, and its copies
  check before each 8 or 16 bytes. The scratch gets up to 8 bytes past the literals, inside its 16 spare
  ones ([chapter 13](#ch-literals-dec--safety)).
- **Undefined behaviour.** Every shift count is 0 to 63, pointers are formed only inside their buffers,
  the limits that could fall before one are numbers, and counts that may go negative are `int`.
- **Time.** Every round writes a match of at least 4 bytes inside the page, so after at most
  {{ch14.max_rounds}} rounds the page is full, and each round's work is bounded by the bytes it writes.
@assert ch14.max_rounds == 1024

The slowest valid page found is made up: a match of 4 bytes at an offset below 8 in every sequence, every
token escaped, 1022 sequences. It takes 107 099 instructions on arm64 and 71 468 cycles on the A55, about
40 µs at 1.8 GHz. Counted in instructions, the costliest valid pages found take 1.59 times the p99 of real
pages with `seqlz-2`, against 2.58 times for `lz4`; timed on the A55, 1.33 against 1.57
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-worst-case-the-slowest-pages-found-cost-13-times-the-p99-of-real-ones-as-for-lz4)).
The argument is checked by fuzz targets under the sanitizers and a second decoder written from
`docs/format.md` alone ([chapter 15](#ch-zram-glue--fuzzing)). On 9th October 2026, 12 instances of AFL++
ran for 2 hours: 1.42 billion inputs on the decoder, 1.94 billion on the decoder against the reference,
nothing found
([plan.md](https://github.com/martinus/quetschn/blob/main/docs/plan.md#phase-4-reference-implementation-format-spec-fuzzing-20-weeks)).

## The example page, sequence by sequence {#ch-decode--example}

`decode_page()` on two pages, one statement at a time: the format's example of
[chapter 4](#ch-format--example), and the page of [chapter 16](#ch-trace), which has coded literals and
takes the fast path most of the time. It is this chapter's function, recorded while `seqlz_decode()`
decoded the pages when the book was built, in the copy with `xored` false, as neither page is XORed,
so the `if (xored)` lines never take a step. Each sequence comes with its cost in bits against `lz4`'s format. Step with the buttons
or the arrow keys, with shift from one sequence to the next.

@stepper decode

The example page, {{ab.len}} bytes, has raw literals, `n_lit = {{ab.lits}}`, and a bitstream of
{{ab.b.bytes}} bytes, so the first refill is the byte loop. Its one token is escaped, token
{{ch14.ab.tok}}: `ll` {{ch14.ab.ll}}, `ml` {{ch14.ab.ml}} with a length value, offset {{ch14.ab.raw}}.
It goes the slow path for three reasons at once: the length value, `lit` above `lit_fast`, and `br.p`
above `in_fast`. `copy_literals()` copies `a b` byte by byte, `seqlz_br_value()` reads
{{ch14.ab.value}} ([chapter 12](#ch-dec-basics--bit-reader)), and `copy_match()` stores `abababab` up
to `d + {{ch14.ab.store_end}}` and the last {{ch14.ab.bytewise}} bytes one by one. Then `d - lit` is
`end_gap`, 0 literals for 0 bytes, and the {{ch14.ab.count3}} bits left are the zero padding.
@assert ab.len == 9 and ab.lits == 2 and ab.b.bytes == 5 and ch14.ab.escaped == 1 and ch14.ab.value_bit == 1 and ch14.ab.ml == 35 and ch14.ab.count3 == ab.b.pad and ch14.ab.raw == 2

[Chapter 16](#ch-trace)'s page shows the fast path at work. With coded literals it compresses to
{{slab.coded.len}} bytes, and its decode goes like this at the start and at the end:

```text
{{ch14.trace.0}}
{{ch14.trace.1}}
{{ch14.trace.2}}
{{ch14.trace.3}}
{{ch14.trace.4}}
{{ch14.trace.5}}
{{ch14.trace.6}}
{{ch14.trace.7}}
...
{{ch14.trace.8}}
{{ch14.trace.9}}
{{ch14.trace.10}}
{{ch14.trace.11}}
{{ch14.trace.12}}
{{ch14.trace.13}}
```

{{ch14.fast}} of its {{ch14.seqs}} sequences with a token take the fast path. The slow ones have a length
value, or come in the last 8 bytes of the input.
@assert ch14.seqs == slab.tokens and ch14.fast + ch14.slow == ch14.seqs and ch16.slow_novalue_past_in == 1

## What is left {#ch-decode--limits}

In a swap-in in the kernel VM, decompression is 2.0 µs of `seqlz-2`'s cold swap-in, against
1.4 µs for `lz4` ([plan.md](https://github.com/martinus/quetschn/blob/main/docs/plan.md#11-the-score-memory-against-time-not-bars)),
and most of the difference is branch mispredictions on a page the CPU has not seen: about 110 more per page
than when the same page was decoded just before
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
The refill's branch is gone from the fast path. The next largest are the fast path's condition, about 23
misses per page, and the test `off >= 8`, 19. The condition misses once for every sequence with a length
value, which is data, and four ways around it were slower
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-fast-paths-condition-its-22-misses-per-page-are-the-length-values-four-ways-around-them-slower-not-kept)).
One copy for every offset instead of the test took out 12 misses per page and cost 10 to 19 instructions
per fast sequence, which was more
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-offset-below-8-one-copy-for-every-offset-saves-12-misses-per-page-and-costs-more-not-kept)).
Fewer length values would need a wider token, a change of the format that was not tried. And the one loop
for all cores costs the in-order A55 up to about 0.3 µs per warm read, the price of not asking the CPU
which core it is.
