# seqlz_decode(): one table lookup per sequence {#ch-decode}

`seqlz_decode()` turns a compressed page back into its 4096 bytes, or 16 384 with 16 KiB pages. It reads the header, has the coded
literals decoded into the scratch if the page has any ([chapter 13](#ch-literals-dec)), and then runs
one loop over the sequences. Each round of the loop looks up one token in the token table, takes the
offset out of the bits after it, and copies the literals and the match. There is no count of the
sequences, and the last one has no token: the loop ends when the literals left fill the rest of the
page, and copies them.

The loop has two paths. The **fast path** is for a sequence without a length value that is far from
the end of the page, of the literals and of the input: it copies a fixed 16 bytes of literals and 16 to
40 bytes of match without loops and without checks of room, and it looks up the next token before it
copies. The **slow path** is for everything else: length values, every check, loops, and the end of the
page. On a desktop's zram pages, 178.5 of the 207 sequences of an average page take the fast path
([explored-designs.md, "The fast path's condition"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-fast-paths-condition-its-22-misses-per-page-are-the-length-values-four-ways-around-them-slower-not-kept)).

<pre class="mermaid">
flowchart TB
    S["seqlz_decode():<br/>prefetch tables, header,<br/>literals raw or decoded"] --> N0["tok = next_token()"]
    N0 --> E{"escape?"}
    E -->|yes| X["refill, read 12 bits,<br/>tok = token_entry(idx, 0)"]
    E -->|no| O
    X --> O["offset from the entry,<br/>drop code and offset bits"]
    O --> C{"no length value,<br/>d &lt;= d_fast,<br/>lit &lt;= lit_fast,<br/>br.p &lt;= in_fast?"}
    C -->|"yes: fast path"| F["next_token_fast():<br/>next token first,<br/>refill every second time"]
    F --> FC["copy16() literals,<br/>check off,<br/>copy_match_fast()"]
    FC --> E
    C -->|"no: slow path"| L["ll value if any,<br/>check nl, copy_literals()"]
    L --> M["ml value if any,<br/>check off and len,<br/>copy_match()"]
    M --> D{"literals left =<br/>bytes left?"}
    D -->|yes| END["last sequence:<br/>copy_literals() the literals left,<br/>then the end checks"]
    D -->|no| NT["tok = next_token(), skip = 0"]
    NT --> E
</pre>

The code starts with the helpers that only the loop uses: the fast path's match copy, the two ways
to get the next token, and the room the fast path needs. The helpers of the slow path, `copy_literals()`,
`copy_match()` and the bit reader, are in [chapter 12](#ch-dec-basics).

## copy_match_fast(): the match without loops {#ch-decode--copy-match-fast}

@listing src/seqlz_decompress.c 435-458

On the fast path the match is at most {{ch14.fast_len}} bytes long: a token holds `ml - 4` up to 31, and 31 means
that a length value follows, which the fast path does not take. The caller guarantees room for {{ch14.cmf.room}} bytes
at `d`, see `FAST_PAGE_ROOM` below, so the function writes without looking at the end of the page.

For an offset of 8 or more, lines 451 and 452 copy 16 bytes, the next two copies follow if `len > 16`,
and one more if `len > 32`: 16, 32 or 40 bytes, always at least `len`. Each `copy8()` loads before it
stores ([chapter 12](#ch-dec-basics--copies)), and with `off >= 8` each load ends before the bytes its
store writes. With `8 <= off < 16` the second load reads bytes the first store wrote, which is what the
comment on lines 445 and 446 means: that is the overlapping match the format asks for, and it is right
because the stores come in order.

Line 449 computes `m = d - off` once, and every copy reads from `m` plus a constant. The comment says
why: clang computed `d + 8 - off` anew for every load when the code was written as `copy8(d + 8, d + 8
- off)`. On the Mi 9T's little Cortex-A55, which runs at most two instructions per cycle, that change took
the decoder from 17 842 to 17 542 instructions per page
([explored-designs.md, "seqlz's decoder for an in-order core"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).

Why branches on `len` and not always 40 bytes: 79% of the matches are at most 16 bytes, and more copies
are not free. On the Mi 9T, 32 bytes always and 40 bytes always both read slower than the code as it is:
warm reads 0.1 µs slower on the A76 and 0.5 to 0.8 µs on the A55, cold reads 1.1 and 1.2 µs slower on
the A76 ([explored-designs.md, "Six choices made on the PC", item 6](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#6-the-fast-paths-copies-as-they-are)).
Measured again in a swap-in in the kernel VM, where the branch predictor has not seen the page, the
copies without branches took out 14 mispredictions per page and were still about 30 ns slower per
decode, 250 ns at the swap-in's p99
([explored-designs.md, "A branch or more work, again in a swap-in"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained)).
The likely reason is in that entry: the added copies load bytes that the stores just before them wrote,
and such a load waits.

@listing src/seqlz_decompress.c 459-475

For an offset below 8 the function uses the pattern register of `repeat_pattern()`
([chapter 12](#ch-dec-basics--pattern)): 8 bytes that repeat every `step` bytes. Lines 467 to 471 store
it 5 times without a loop, at `d`, `d + step`, up to `d + 4 * step`. The smallest step is {{ch14.cmf.min_step}}, for `off =
{{ch14.cmf.min_step_off}}`, and then the 5 stores cover $4 \times {{ch14.cmf.min_step}} + 8 = {{ch14.cmf.five_stores}}$ bytes, which is the "at least 28" of the comment. For
`off` 1, 2 and 4 the step is 8 and the 5 stores cover {{ch14.cmf.five_stores_8}} bytes, every match of the fast path. The loop
on lines 472 and 473 runs only for a longer match with a step below 8: e.g. `off = 5, len = 34` stores at
25 and 30 and stops at 35. Over all offsets from 1 to 7 and lengths from 4 to {{ch14.fast_len}}, the furthest byte
written is `d + {{ch14.cmf.max_byte}}`, found by running `copy_match_fast()` on all of them when the book is built; that is
where the {{ch14.cmf.room}} bytes of room come from.

@assert ch14.cmf.min_step == 5 and ch14.cmf.min_step_off == 5 and ch14.cmf.step1 == ch14.cmf.step2 == ch14.cmf.step4 == 8 and ch14.cmf.room == 40

`repeat_step[off]` needs no `& 7U` here: this is the `else` of `off >= 8U`, so `off` is below 8. And
`off` is not 0, which would index the placeholder entry: the caller rejected 0 before it called.

Before 10th October 2026 a loop instead of the 5 stores was tried again, for simpler code. In the kernel
VM it was 0.5 to 0.7% slower with both gcc and clang, more than a second copy of the same code moved
([explored-designs.md, "Five simplifications"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)),
so the 5 stores stayed.

## The next token: refill only when the bits might run out {#ch-decode--next-token}

@listing src/seqlz_decompress.c 477-490

`next_token()` is how the slow path, and the first sequence, get the next token's entry. Line 487 checks
whether the register has fewer bits than a token and its offset can take: an 11-bit code and up to {{ch14.max_off_bits}}
offset bits with 4 KiB pages, {{ch14.need}}, or up to {{k16.ch14.max_off_bits}} with 16 KiB pages, {{k16.ch14.need}}. Only then does it refill. Line 489
looks up the top 11 bits, `bits >> 53`, an index from 0 to 2047, in the token table. Every index is a
valid entry, because the token code is complete ([chapter 2](#ch-compression)): there is no check for
bits that are no code.

The `(int)` cast on line 487 matters. `SEQLZ_TOKEN_BITS + QUETSCHN_PAGE_BITS` is unsigned. Without the
cast, C would convert `count` to unsigned for the compare, and a negative `count`, a page that took more
bits than it has, would compare as a number above 4 billion and skip the refill. That would still be
safe, the refill would do nothing there anyway, but the compare would not say what it means.

The comment on lines 478 to 480 is the reason for the check. A refill puts a load in front of the
token's lookup: the lookup's index comes from `bits`, which the refill changes. If the refill ran for
every sequence, every lookup would wait for a load from the compressed page, a miss when the page is
cold. With the check, most lookups use bits that are already in the register, and the refill's load
happens about once every two or three sequences: a refill leaves at least 56 bits, a sequence on the
fast path takes at most {{ch14.fast_seq_bits}} of them with 4 KiB pages.

::: measured Refill below 23 bits instead of every sequence
September 2026, `seqlz-fast` in the kernel VM, Ryzen 9 7950X at 4.5 GHz, cold and with another page
read first: p50 / p99 of a read went from 2811 / 4501 to 2610 / 4300 ns, warm from 2290 / 4049 to
2129 / 3790 ns. The same change made a hot loop over 2000 pages slower, 6883 to 7115 cycles per page,
with 39 more mispredictions per page: the check is a branch on data. The kernel's numbers decided
([explored-designs.md, "zram: prefetch the compressed data", the paragraph "A refill only when the bits might run out"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
The format of that time had raw offset bits of the same widths, but the bit order and the tables were
different.
:::

### next_token_fast(): the next token before the copies

@listing src/seqlz_decompress.c 492-520

The fast path calls this function right after it took the current sequence's bits, before it copies
anything. Two ideas are in it, one per paragraph of the comment.

**The next token before the copies.** On the fast path no length value follows, so the next token's
code starts right after the offset bits, and its entry can be looked up as soon as the current sequence's
bits are dropped. Loading the entry takes a few cycles from L1 and much longer from memory. An
out-of-order core would run the copies while it waits anyway. An in-order core such as the Cortex-A55
would not: if the lookup came after the copies, it would wait for it at the top of the next round. With
the lookup first, the copies, 32 to 56 bytes, fill that wait.

::: measured The next token before the copies, on the Mi 9T
3rd October 2026, phone pages, the userspace harness, the code before and after alternating, 5 runs
per core, the difference to `lz4` in ns: the little core's cold p50 went from 2169 to 2007, the big
core's from 456 to 330; at p99, 7364 to 7178 and 3204 to 3011. On x86-64 the decode loop took 5800
instead of 5967 cycles per page, with the same instructions
([explored-designs.md, "The next token before the copies"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-next-token-before-the-copies-reads-faster-on-both-phone-cores-kept)).
:::

**The refill every second fast sequence, without a check.** The check in `next_token()` is a branch
whose outcome depends on the code lengths of the sequences before: whether 23 bits are left after
three short codes and one long one. A branch predictor learns that only for a page it has seen, and a
swap-in decodes a page once. In a fault, that one branch was the largest single source of mispredictions
in the decoder, 32 per page
([explored-designs.md, "Which branches"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#which-branches-three-in-the-sequence-loop-are-58-of-the-130-misses-of-a-page-seen-once)).

So the fast path refills on a schedule instead: every second fast sequence, whatever is left. `*skip`
alternates between 0 and 1 on line 518, and the branch on line 510 alternates with it, a pattern every
predictor gets right. Lines 511 to 516 are the fast half of `seqlz_br_refill()`
([chapter 12](#ch-dec-basics--bit-reader)) without its check of the bytes left. `skip` is passed as a
pointer so that the function can change the caller's variable; after inlining it is a register.

Why the bits never run short, with numbers. After a refill `count` is at least 56. The sequence in
whose `next_token_fast()` the refill runs has dropped its own bits already, on line 626. Between this
refill and the next one, two fast sequences take their bits: the next one, whose `next_token_fast()`
does not refill, and the one after it, which drops its bits on line 626 before its own
`next_token_fast()` refills. Each takes its token's code, at most 11 bits, and its offset bits:

| page size | most offset bits | one fast sequence | two | left of 56 |
| --- | --- | --- | --- | --- |
| 4 KiB | {{ch14.max_off_bits}}, class 3 | {{ch14.fast_seq_bits}} | {{ch14.fast_two_bits}} | {{ch14.fast_left}} |
| 16 KiB | {{k16.ch14.max_off_bits}}, class 3 | {{k16.ch14.fast_seq_bits}} | {{k16.ch14.fast_two_bits}} | {{k16.ch14.fast_left}} |

@assert ch14.class3_bits == ch14.max_off_bits and F("k16.ch14.class3_bits") == F("k16.ch14.max_off_bits")

The comment counts 14 offset bits for both page sizes, the larger case. The peek at the next token
needs 11 bits; after the first of the two sequences at least {{k16.ch14.fast_after_one}} are left, after the second the refill
comes first. An escaped token does not break the count: the escape refills on its own before it reads
its 12 bits, see below, which only adds bits.

After a slow sequence the schedule starts over: the slow path sets `skip = 0`, so the first fast
sequence after it refills. That is needed because the slow path's `next_token()` only guarantees {{ch14.need}} or
{{k16.ch14.need}} bits, not 56.

The comment's last sentence names the two preconditions of the load on line 511, which has no checks.
"8 bytes of input are there": the fast path is only taken while `br.p <= in_fast`, 8 bytes before the
end. "count is at least 0": the bits taken since the last refill or `next_token()` are within the bounds
above, so `count` cannot have gone negative, and `v >> count` is a defined shift.

::: measured The refill without its branch
7th October 2026, x86-64 in userspace, gcc 16.2.1, 19 577 of a desktop's zram pages, each decoded
once: branch misses per page went from 130 to 101, cycles from about 6960 to about 6700. Refilling on
every fast sequence instead took out the branch too, but its loads cost 500 cycles more on a page the
predictor knows, and was not kept. In the kernel VM, `MODE=swap`, CPU 2 of the Ryzen 9 7950X at 4.5 GHz,
3 boots each: `zcomp_decompress()` of `seqlz-fast-lit` took 1935 to 1941 ns instead of 2010 to 2011 with
gcc, 1976 to 1983 instead of 2020 to 2040 with clang 22.1.8. A second series of 3 boots, with the code
as it was kept and gcc, gave 1936 to 1942 ns instead of 1995 to 2008. On the Mi 9T the big Cortex-A76 decoded a
page seen once 3241 to 3272 ns instead of 3309 to 3340, and the little Cortex-A55 got slower, so at
first it kept a loop of its own
([explored-designs.md, "The refill without its branch"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-refill-without-its-branch-every-second-fast-sequence-63-ns-less-per-swap-in-on-x86-64-and-the-a76-kept)).
Since 10th October 2026 every core runs this one loop ([chapter 17](#ch-decisions--one-loop-for-all-cores)).
:::

## The fast path's three rooms {#ch-decode--rooms}

@listing src/seqlz_decompress.c 522-529

The fast path copies without asking whether there is room, so it is entered only where there is, and
these three constants say how much.

- **`FAST_PAGE_ROOM`, {{ch14.room.page}} bytes of page.** A fast sequence writes the 16 bytes of `copy16()` at `d`, then
  moves `d` on by at most {{ch14.fast_nl}} literals, then writes at most {{ch14.cmf.room}} bytes of match: at most {{ch14.cmf.max_write}} bytes from where
  it started. {{ch14.room.page}} is the round number above {{ch14.cmf.max_write}}. The fast path of September 2026, which copied 16 or 32
  bytes of match, already needed 64 bytes
  ([explored-designs.md, "zram: prefetch the compressed data", the paragraph "A fast path in seqlz's decoder"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
- **`FAST_LIT_ROOM`, {{ch14.room.lit}} bytes of literals.** One `copy16()` reads 16 bytes at `lit`.
- **`FAST_IN_ROOM`, {{ch14.room.input}} bytes of input.** The refill in `next_token_fast()` loads 8 bytes at `br.p`.

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

## Setup: the header, the tables, and where the literals are {#ch-decode--setup}

@listing src/seqlz_decompress.c 531-545

The parameters are the ones of `seqlz.h` ([chapter 6](#ch-header)): the tables, the compressed page
and its length, the output page of `SEQLZ_PAGE` bytes, and the scratch of `SEQLZ_SCRATCH` bytes for coded
literals, or `NULL`. The return value follows the kernel's convention: 0 for success, a negative error
number, `-EINVAL`, for input that is not a valid page.

`s_end` is one past the last byte of the input, `d_end` one past the last byte of the page. `lit`,
`lit_end` and `lit_bound` are the literals: the next one, one past the last real one, and one past the
last byte that may be read where the literals are. `d_fast`, `lit_fast` and `in_fast` are the limits of
the fast path. `last` is the repeat offset, 1 at the start of every page, as the format says
([chapter 4](#ch-format--offsets)). `skip` starts at 0, so the first fast sequence refills.

Line 544 rejects input shorter than the 2-byte header before anything reads it.

@listing src/seqlz_decompress.c 546-553

The tables are prefetched first, before the header is even read: {{ch14.pf.token}} prefetches for the token table
and {{ch14.pf.len}} for each length table, the loop's and one for the line of the table's last byte
([chapter 12](#ch-dec-basics--prefetch), whose warn box says why). Everything until the first token's
lookup, reading the header and possibly decoding the literals, then runs while those lines are on their
way. The comment says why that matters most on the little cores: an in-order core stops at every miss it
did not see coming. `sizeof(t->token.decode)` is the array's size in bytes, {{ch14.token_bytes}}.

Line 553 reads the 2-byte header with the kernel's `get_unaligned_le16()`: little endian, at any address.
The input buffer has no alignment guarantee worth relying on, and the kernel helper says what is meant.
It replaced a helper of seqlz's own on 10th October 2026, within the noise of the measurement in the
kernel VM and on the phone
([explored-designs.md, "The kernel copy's choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16)).

@listing src/seqlz_decompress.c 554-576

The top bit of the header, `SEQLZ_LIT_CODED`, `0x8000`, says which of the two layouts the page has
([chapter 4](#ch-format--layout)).

**Coded literals**, lines 555 to 567. `n_lit &= SEQLZ_LIT_CODED - 1U` keeps the low 15 bits, the
number of literals. Without a scratch, line 559, a page with coded literals is not valid: zram at level 1
writes only raw literals and can call the decoder without one ([chapter 15](#ch-zram-glue)).
`decode_literals()` decodes the 8 streams into the scratch and returns where the bitstream starts, or
`NULL` for a page whose header or streams are not valid ([chapter 13](#ch-literals-dec)); it also rejects
`n_lit > SEQLZ_PAGE`. The bit reader then starts at `q` and ends at the end of the input. The
compound literal `(struct seqlz_bit_reader){ .p = q, .end = s_end }` sets the fields not named, `bits` and
`count`, to 0. The literals are in the scratch: `lit_end` is after the last one, and `lit_bound` is the
end of the scratch, `SEQLZ_SCRATCH` = 4096 + 16 bytes. Those 16 bytes exist for exactly this: the
16-byte copies of the fast path may read up to 15 bytes past the last literal.

**Raw literals**, lines 569 to 575. The literals are the `n_lit` bytes right after the header, and the
bitstream follows them. Line 570 checks that they are in the input. The sum is done in `u64`, so it
cannot wrap whatever `n_lit` is; with at most 15 bits in `n_lit` it would not wrap in 32 bits either,
but then the reader has to know that. Here `lit_bound` is the end of the input: past the literals are the
bitstream's bytes, which are in the buffer and may be read, though they are not literals.

One rule of the format is not checked here: a raw page with more than 4096 literals. A page has room
for at most 4096 literals, so such a page cannot use them all: the literals left are always more than
the bytes the page still needs, the loop never finds its end, and a check of room in the slow path
rejects the page.

@listing src/seqlz_decompress.c 578-601

`d_fast` is a pointer, 64 bytes before the end of the page, which is always inside the page. The other
two limits are numbers of type `unsigned long`, and the comment says why: with a short input,
`lit_bound - 16` would be a pointer before the start of the buffer. C allows a pointer to point into an
array or one past its end, nothing else; computing one before the start is undefined behaviour, also
if it is never used to read. As numbers the same subtraction is plain arithmetic, and the compare
`(unsigned long)lit <= lit_fast` is then false, as it should be. In the kernel, `unsigned long` has the
size of a pointer on every architecture.

`in_fast` gets one more guard, line 587: if the input is shorter than 8 bytes, it is 0. Then no pointer
`br.p` compares below or equal to it, and the fast path, whose refill loads 8 bytes without a check, is
never taken. Without the guard, `s_end - 8` would be below `s`, and `br.p >= s` would fail the compare as
well, unless the subtraction wrapped around address 0; the 0 states the rule without depending on
where the buffer is.

Lines 591 to 600 prepare the end. The page needs `d_end - d` more bytes, and `lit_end - lit` literals
are left. When the two are equal, no match fits any more, since every match has at least 4 bytes, and
the literals left are the last sequence, which has no token ([chapter 4](#ch-format--end)). Comparing
the two differences costs two subtractions per sequence. `d - lit == d_end - lit_end` is the same
equation, and its right side never changes, so line 597 computes it once, as `end_gap`. `d` and `lit`
point into different buffers, the page and the input or the scratch, and C defines the difference of
two pointers only within one array, so both sides are numbers, as `lit_fast` and `in_fast` above.

Lines 599 and 600 take care of the one page that ends before its first token: a page of 4096 literals
and no match, where the literals left fill the page from the start. Its bitstream is empty, and the
`goto` jumps to the copy after the loop. With raw literals such a page is 4098 bytes and never stored;
with coded literals it can be smaller than a page, and the format allows it.

Line 601 gets the first token's entry. `count` is 0, so `next_token()` refills first.

## The loop: escape, then the offset without a branch {#ch-decode--offset}

@listing src/seqlz_decompress.c 602-617

The loop has no condition; it ends with the `break` on line 688 when the literals left fill the rest of
the page, or with a `return -EINVAL`. Every round is one sequence with a match.

Lines 605 to 617 handle the escape first. Only {{ch14.tokens_coded}} of the {{const.TOKEN_SYMBOLS}} tokens have a code of their own in the
4 KiB tables; the others are sent as the escape's code, `{{tok.code.3072}}`, followed by the token's number in 12
bits ([chapter 4](#ch-format--token)). The escape's entry has bit 31 set, `TOK_ESCAPE_AT`, so `tok >>
TOK_ESCAPE_AT` is 1 for it and 0 for every other entry, and only the code's length in its drop field,
{{tok.len.3072}} ([chapter 7](#ch-internal)). Line 609 drops the escape's code. Line 610 refills: the 12 bits may not
all be in the register, since `next_token()` only made sure of 23 or 25 bits before the escape's code
was looked up. Line 611 takes the top 12 bits, line 613 drops them.

12 bits can hold up to 4095, and the tokens end at 3071. Line 614 rejects the {{ch14.no_token}} numbers that are no
token. Then line 616 computes the token's entry with `token_entry(idx, 0)`, the same function that
filled the table ([chapter 7](#ch-internal)), with a code length of 0: the code and the 12 bits are
already dropped, so the entry's shift and drop count only the offset bits. From here on, an escaped token
is the same as any other.

An example with the 4 KiB tables: token 513 is {{ch14.t513.ll}} literal, a match of {{ch14.t513.ml}}, class {{ch14.t513.cls}}, and has no code. It
is sent as `{{tok.code.3072}}`, `{{ch14.t513.bin}}` (513) and then {{ch14.t513.raw_bits}} offset bits. `token_entry(513, 0)` is `{{ch14.t513.entry}}`:
`TOK_N` {{ch14.t513.N}}, `TOK_BACK` {{ch14.t513.BACK}}, `TOK_DROP` {{ch14.t513.DROP}}, `ll` {{ch14.t513.ll}}, `ml` {{ch14.t513.ml}}, no length value. The offset is then the top {{ch14.t513.raw_bits}}
bits of the register.

@listing src/seqlz_decompress.c 618-629

This is the step [chapter 7](#ch-internal) built the token entry for: the offset in three shifts and a
select, with every number taken from the entry. Right after the lookup, the register has the token's
code on top, `TOK_N(tok)` bits, then the offset bits.

- `br.bits << TOK_N(tok)` shifts the code out, so the offset bits are on top.
- `>> back` with `back = 64 - raw_bits` moves them down to the bottom: the offset bits as a number.
- The cast to `unsigned int` keeps the low 32 bits, of which the offset has at most 14.
- `<< TOK_SHIFT(tok)` multiplies by 2 for class 1 and by 8 for classes 4 and 5, which store the offset
  divided by 2 and 8; for the other classes the shift is 0.

For class 0, the repeat offset, there are no offset bits. `back` is then 0 and not 64, because
`token_entry()` stores `(64 - raw_bits) & 63`: a shift by 64 is undefined in C. With `back = 0`, `raw` is
some part of the register, a meaningless number, and line 627 does not use it: `off = back ? raw : last`.
Line 626 drops the code and the offset bits together, `TOK_DROP(tok)` = code length + offset bits.

Three tokens from the 4 KiB tables, each with its real code and entry, printed by a program compiled
against `src/`. The register holds the code, the offset bits, and then made-up bits `0101...`:

| | class {{ch14.tk.a.cls}}: token {{ch14.tk.a.s}} | class {{ch14.tk.b.cls}}: token {{ch14.tk.b.s}} | class {{ch14.tk.c.cls}}: token {{ch14.tk.c.s}} |
| --- | --- | --- | --- |
| sequence | `ll` {{ch14.tk.a.ll}}, `ml` {{ch14.tk.a.ml}}, offset {{ch14.tk.a.off}} | `ll` {{ch14.tk.b.ll}}, `ml` {{ch14.tk.b.ml}}, offset {{ch14.tk.b.off}} | `ll` {{ch14.tk.c.ll}}, `ml` {{ch14.tk.c.ml}}, the last offset |
| code | `{{ch14.tk.a.code}}`, {{ch14.tk.a.n}} bits | `{{ch14.tk.b.code}}`, {{ch14.tk.b.n}} bits | `{{ch14.tk.c.code}}`, {{ch14.tk.c.n}} bits |
| offset bits | `{{ch14.tk.a.ob}}`, {{ch14.tk.a.raw_bits}} bit: {{ch14.tk.a.ob_val}} | `{{ch14.tk.b.ob}}`, {{ch14.tk.b.raw_bits}} bits: {{ch14.tk.b.ob_val}} | none |
| entry | `{{ch14.tk.a.entry}}` | `{{ch14.tk.b.entry}}` | `{{ch14.tk.c.entry}}` |
| `TOK_N` / `TOK_BACK` / `TOK_SHIFT` / `TOK_DROP` | {{ch14.tk.a.N}} / {{ch14.tk.a.BACK}} / {{ch14.tk.a.SHIFT}} / {{ch14.tk.a.DROP}} | {{ch14.tk.b.N}} / {{ch14.tk.b.BACK}} / {{ch14.tk.b.SHIFT}} / {{ch14.tk.b.DROP}} | {{ch14.tk.c.N}} / {{ch14.tk.c.BACK}} / {{ch14.tk.c.SHIFT}} / {{ch14.tk.c.DROP}} |
| `br.bits` | `{{ch14.tk.a.bits}}` | `{{ch14.tk.b.bits}}` | `{{ch14.tk.c.bits}}` |
| `<< TOK_N` | `{{ch14.tk.a.shl}}` | `{{ch14.tk.b.shl}}` | `{{ch14.tk.c.shl}}` |
| `>> back` | `{{ch14.tk.a.shr}}` | `{{ch14.tk.b.shr}}` | `{{ch14.tk.c.shr}}` |
| `raw` | {{ch14.tk.a.ob_val}} << {{ch14.tk.a.SHIFT}} = {{ch14.tk.a.raw}} | {{ch14.tk.b.ob_val}} << {{ch14.tk.b.SHIFT}} = {{ch14.tk.b.raw}} | `{{ch14.tk.c.raw32}}`, not used |
| `off` | {{ch14.tk.a.off}} | {{ch14.tk.b.off}} | `last` |

@assert ch14.tk.a.SHIFT == 1 and ch14.tk.a.raw_bits == 1 and ch14.tk.c.BACK == 0 and ch14.tk.c.n == 4

The index into the token table is the top 11 bits of `br.bits`: {{ch14.tk.a.index}} for the first, {{ch14.tk.b.index}} for the second,
{{ch14.tk.c.index}} for the third. A code of {{ch14.tk.c.n}} bits fills {{ch14.tk.c.fill}} entries of the table, all with the same value: token {{ch14.tk.c.s}}'s
entry is at every index from {{ch14.tk.c.idx_lo}} to {{ch14.tk.c.idx_hi}}, which is why the made-up bits after the code do not matter for
the lookup.

What the compiler makes of it, checked with gcc 16.2.1 at `-O2` for x86-64: the shifts, the masks and a
`cmovne` for the select, no branch. And the `& 63U` of `TOK_N()` costs no instruction there: x86-64 uses
only the low 6 bits of a shift count anyway, and gcc shifts by the whole entry.

Before 3rd October 2026 the entry had 2 bytes and the decoder computed the class, the number of offset
bits, the mask and the shift from the token at run time. On the Cortex-A55, 18 of about 60 instructions
per sequence went into that; the entries of 4 bytes took the decoder from 16 547 to 15 539 instructions
per page there, and the length value flag in the entry, bit 30, to 15 129
([explored-designs.md, "seqlz's decoder for an in-order core"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).

Lines 628 and 629 take `ll` and `ml` out of the entry. `TOK_ML` is `ml` itself, from 4 to 35, not `ml -
4`.

## The fast path {#ch-decode--fast}

@listing src/seqlz_decompress.c 630-659

The condition on lines 638 to 640 has four parts, and each one is what lets the code below skip a check:

1. `!(tok & (1U << TOK_VALUE_AT))`: no length value follows. Then `nl` is at most 14 and `len` at most
   34, because 15 and 35 are exactly the values that need a length value, and the entry's bit 30 is set
   for them. One test instead of two compares; without the bit, the A55 read raw pages 0.2 µs slower
   ([explored-designs.md, "Five simplifications"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
2. `d <= d_fast`: 64 bytes of page, for the at most 54 this sequence writes.
3. `(unsigned long)lit <= lit_fast`: 16 readable bytes at `lit`.
4. `(unsigned long)br.p <= in_fast`: 8 bytes of input for the refill.

The comment on lines 630 to 637 adds a consequence: the page's last match, the one after which the
literals left fill the page, never takes the fast path. After it fewer than 8 bits of the input are
left, the padding of the last byte. The fast path needs `br.p <= in_fast`, so 8 bytes of input are not
loaded yet, and at least 64 bits are not used. So the fast path needs no end check of its own, and
every page ends on the slow path, where the checks are. On a damaged page the literals left could
match the bytes left right after a fast sequence. The loop then goes on, every further match puts the
literals left further ahead of the bytes left by its length, the two never meet again, and a check of
room rejects the page.

In the body, line 641 looks up the next token first, as explained above. Line 650 copies 16 bytes of
literals, and lines 651 and 652 move both pointers by the real `nl`. Line 653 remembers the offset for a
later class 0.

Line 654 is the offset check, the same as in the slow path: the match must start inside the bytes
written so far, `1 <= off <= d - dst`. It is one unsigned compare: `off - 1U >= (unsigned int)(d - (u8
*)dst)`. For `off = 0`, `off - 1U` wraps around to 4 294 967 295, which is never below the bytes written,
so 0 is rejected without a compare of its own. With 24 bytes written: `off = 24` gives `23 >= 24`, false,
the match may start at the first byte of the page; `off = 25` gives `24 >= 24`, true, invalid. The cast
`(u8 *)dst` is there because `dst` is a `void *`, and C has no arithmetic on those.

Then `copy_match_fast()` and `d += len`, and `continue` goes to the next round with the entry the lookup
already brought.

The comment on lines 642 to 649 explains a check that is missing on purpose: `nl` is not compared with
the literals left. A page could claim more literals in its tokens than its header has. Then `lit` moves
past `lit_end`, and the bytes after the literals are copied as if they were literals: the bitstream's
bytes for raw literals, the scratch's spare bytes for coded ones. Nothing is read outside memory that
may be read, because `lit <= lit_bound - 16` before the copy, and the copy reads 16 bytes. And the page is
still rejected: the next slow sequence checks `lit > lit_end` first, and only the slow path ends the
loop. The comment gives the saving, 3 instructions per sequence, counted when the check left the fast path on
3rd October 2026 (commit `5c769f5`).

::: warn The check that moved
The first fast path, in September 2026, needed a check of `nl` that no test covered: without it, `lit`
runs past the literals, and the slow path then computed `lit_end - lit` as a negative number, cast to
unsigned, so every `nl` passed, and `copy_literals()` read behind the input. A test builds such a page
since then
([explored-designs.md, "zram: prefetch the compressed data", the paragraph "A fast path in seqlz's decoder"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
When the check was taken out of the fast path on purpose, on 3rd October 2026, the `lit > lit_end` on
the slow path came with it; without that, AddressSanitizer reported a heap overflow in the existing test
([explored-designs.md, "seqlz's decoder for an in-order core"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).
:::

## The slow path: length values, and every check {#ch-decode--slow}

@listing src/seqlz_decompress.c 660-669

A sequence ends up here if a length value follows, or if it is near one of the three ends.

Lines 661 to 664: an `ll` of 15 in the token means 15 or more, and the rest follows as a length value
([chapter 4](#ch-format--length-values)). The refill before `seqlz_br_value()` makes sure of the 20 or 22
bits it may take. The bitstream has the offset bits before the length values, which matches the code:
the offset bits were dropped with the token on line 626.

Lines 665 to 667 are the checks of the literals. The order matters, and C's `||` evaluates left to right
and stops at the first true part:

- `lit > lit_end`: the fast path has gone past the literals, see above. This has to come first: with
  `lit` past `lit_end`, `lit_end - lit` is negative, and cast to `unsigned int` a number near 4 billion
  that every `nl` would pass.
- `nl > (unsigned int)(lit_end - lit)`: more literals than are left.
- `nl > (unsigned int)(d_end - d)`: more literals than fit into the page.

Line 668 copies them with `copy_literals()` ([chapter 12](#ch-dec-basics--literals)), which may read
up to `lit_bound`, not just `lit_end`, for its 16-byte copies. No test of the end follows: every token
has a match, only the last sequence has none, and the last sequence has no token.

Until 11th October 2026 it had one, and here the decoder checked `if (d == d_end)`: if the literals
had filled the page, the token had to have `ml - 4 = 0` and class 0, else the page was invalid. The
token said what the decoder knew already, and cost about 1.2 bytes per page.

@listing src/seqlz_decompress.c 670-691

Line 671: an `ml` of 35 in the entry, `SEQLZ_ML_CAP + 4`, means 35 or more, and the rest follows as a
length value. With 4 KiB pages a length value is at most {{ch14.max_value}}, so `len` is at most {{ch14.max_len}} here, more than a
page; the check below catches that.

@assert ch14.max_len > 4096


Line 675 sets the repeat offset before the checks; if they fail, the function returns and `last` does not
matter. Lines 677 to 679 check the offset as on the fast path, and that the match fits into the page.
`copy_match()` copies it ([chapter 12](#ch-dec-basics--copy-match)).

Lines 682 to 688 look for the end. If `d - lit` is `end_gap`, the literals left are exactly the bytes
the page still needs, and the loop ends without reading another token. Otherwise the next token comes
through `next_token()`, with the check of the bits left, and `skip = 0` restarts the refill schedule of
the fast path.

## The end: a page must end where its bits end {#ch-decode--end}

@listing src/seqlz_decompress.c 692-704

The loop leaves only through the `break` on line 688, or never enters, from line 600. Either way the
literals left are exactly the bytes the page still needs, and line 694 copies them with the slow
path's `copy_literals()`: the last sequence. On the page of `ab` they are 0, on
[chapter 16](#ch-trace)'s page too. A `memcpy()` here, a call instead of a second inlined copy loop,
made the swap-in 0.02 µs slower in the kernel VM, at the edge of what the boots of one kernel differ by
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-last-sequence-without-a-token-12-bytes-per-page-less-kept)).
After the copy the page is full and every literal is used, by the condition the loop ended on, so there
is nothing to check about them. The checks on lines 700 to 702 make sure the input had nothing else to
say:

| check | what it means | an example it rejects |
| --- | --- | --- |
| `br.count < 0` | the sequences took no more bits than the bitstream has | a page cut short: the reader filled in zeros |
| `br.p != br.end` | every byte of the bitstream went into the register | 3 more bytes after the last code |
| `br.count >= 8` | fewer than 8 bits of the last byte are left | a whole byte after the last code, also a zero byte |
| `br.count > 0 && (br.end[-1] & ((1U << br.count) - 1U))` | the bits left are 0 | a bit 1 in the padding of the last byte |

A header with more literals than the sequences use fails before: the literals left never become the
bytes left, so the loop reads tokens past the end of the bitstream until a check of room fails.

The last two need a closer look. When `br.p == br.end`, the `count` bits left in the register are the
last `count` bits of the last byte of the input, `br.end[-1]`. `(1U << count) - 1U` is a mask of the low
`count` bits; for `count = 3` it is `0b111`. Reading them from the page and not from the register avoids
a shift by `64 - count`, which would be 64 for `count = 0`; the `count > 0 &&` skips the read when no bits
are left. And `br.end[-1]` is in the input: with `count > 0` some byte was read.

Why so strict, and what it costs, is in [chapter 4](#ch-format--end): zram stores no checksum, and these
checks catch damage to the bytes after the last code. `lz4` also rejects input it did not use. The checks
came on 8th October 2026; the compressor always wrote pages that pass, since it fills the last byte with
zero bits.

## Why no input makes it read or write out of bounds {#ch-decode--safety}

The kernel calls `seqlz_decode()` on whatever zram has stored, and if that memory is damaged, the
decoder must not make it worse. The project's constraint C6 is that the decoder is safe and bounded in
time for any input; a decoder that is not was one of the reasons a compressor for zram was not merged
before ([plan.md, §1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#1-goal-and-success-criteria)).
Here is the argument, access by access, for a page with any bytes and any `src_len`.

**Reads of the input.**

- The header: `src_len >= 2` on line 544 before the 2-byte read.
- Raw literals: `2 + n_lit <= src_len` on line 570, and every read of literals stays below `lit_bound =
  s_end`, see below.
- Coded literals and their header: `decode_literals()` checks them ([chapter 13](#ch-literals-dec)).
- The bitstream: the 8-byte loads run only while 8 bytes are left, by the check in `seqlz_br_refill()` or
  by `br.p <= in_fast`. They move `br.p` by at most 7, so it never passes `end`. The byte loop checks `p <
  end` for every byte. A page that wants more bits gets zeros and a negative `count`.
- `br.end[-1]` at the end: only with `count > 0`, after a byte was read.

**Reads of the literals.** On the fast path `lit <= lit_bound - 16` before a 16-byte read, and `lit`
moves by at most 14, so it stays below `lit_bound` even when it passes `lit_end`. On the slow path, `lit
<= lit_end` is checked first, `nl` at most the literals left, and `copy_literals()` reads 16 bytes only
when 16 are readable before `lit_bound`. `lit_bound` is the end of the input or the end of the scratch.

**Reads of the tables.** The token table is indexed by 11 bits, 0 to 2047, its size. The length tables by
8 bits, 0 to 255. An escaped token's number is checked against 3072 before `token_entry()` uses it, and
that function computes and reads no table.

**Reads of the page itself, for matches.** `off >= 1` and `off <= d - dst` are checked before each match,
so the first byte read is inside the page. The copies for `off >= 8` read only bytes before the ones they
write, inside the page. `repeat_pattern()` reads `d - off` to `d - off + 7`, below `d + 8`, inside the
room too. The bytes it reads at `d` and after are whatever the output buffer held, but they are masked
away.

**Writes.** The page: the fast path writes at most 54 bytes from a `d` at least 64 bytes before
`d_end`. `copy_literals()` and `copy_match()` check the room for each copy of 16 or 8 bytes and copy
the rest byte by byte, and the slow path checked `nl` and `len` against the room before. The scratch:
`decode_literals()` writes up to 8 bytes past the literals, inside its 16 spare bytes
([chapter 13](#ch-literals-dec)). Nothing else is written.

**Undefined behaviour that is not a memory access.** Every shift count is from 0 to 63: `back` is 0 and
not 64 for class 0, `seqlz_br_value()` shifts twice, the refill shifts by a `count` from 0 to 63 and the
byte loop only while `count` is from 0 to 56. Pointers are only formed inside their buffers; the limits
that could fall before a buffer are numbers. Counts that may go negative are `int`.

**Time.** Every round of the loop writes a match of at least 4 bytes, and the checks make sure each
write is inside the page. So after at most $4096 / 4 = 1024$ rounds the page is full, also for input
that is not valid, which ends earlier with `-EINVAL`; then one copy for the last sequence. Each round does a bounded amount of work
plus copies that are bounded by the bytes they write, and `decode_literals()` is bounded by the 4096
literals a page can have. The slowest valid page found for the decoder is made up: a match of 4 bytes at
an offset below 8 in every sequence, every token escaped, 1022 sequences. It takes 107 099 instructions
on arm64 and 71 468 cycles on the Mi 9T's A55, about 40 µs at 1.8 GHz. Counted in instructions, the
costliest valid pages found take 1.59 times the p99 of real pages to decode with `seqlz-fast-lit`; the
same search found 2.58 times for `lz4`. Timed on the A55, the slowest made-up page took 1.33 times the
p99 of real pages, `lz4`'s slowest 1.57 times its own
([explored-designs.md, "The worst case"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-worst-case-the-slowest-pages-found-cost-13-times-the-p99-of-real-ones-as-for-lz4)).

**How it is checked.** An argument like the one above is easy to get wrong, so the repository runs the
decoder on input nobody wrote: fuzz targets under the compiler's sanitizers, and a second decoder written
from `docs/format.md` alone that must agree with it ([chapter 15](#ch-zram-glue--fuzzing)). Before the
format was declared ready, AFL++, a fuzzer that mutates inputs toward code they have not reached yet,
ran 12 instances for 2 hours on 9th October 2026: 1.42 billion inputs on the decoder, 1.94 billion on
the decoder against the reference, nothing found
([plan.md, Phase 4](https://github.com/martinus/quetschn/blob/main/docs/plan.md#phase-4-reference-implementation-format-spec-fuzzing-20-weeks)).

## The example page, sequence by sequence {#ch-decode--example}

Below, `seqlz_decode()` runs on two pages, one statement at a time: the format's example of
[chapter 4](#ch-format--example), and the made-up page of [chapter 16](#ch-trace), which has coded
literals and takes the fast path most of the time. It is the function of this chapter, recorded while
it decoded the pages when the book was built, so every number in it is what the code computes. For
each sequence it also shows what it costs in bits, against the same sequence in `lz4`'s format. Step
with the buttons or the arrow keys, and with shift from one sequence to the next.

@stepper decode

The same in prose, for the example page:

The {{ab.len}} bytes `{{ab.hex}}` are the format's example ([chapter 4](#ch-format--example)): a
4 KiB page of `ab` repeated 2048 times. Here is what `seqlz_decode()` does with them, from a copy of the
function with `printf()`s in it.

**Setup.** `src_len = {{ab.len}} >= 2`. The header is `0x0002`: raw literals, `n_lit = {{ab.lits}}`. `2 + 2 <= {{ab.len}}`, so `lit =
s + 2`, `lit_end = s + 4`, `lit_bound = s_end = s + {{ab.len}}`, and the bit reader starts at `s + 4` with the {{ab.b.bytes}}
bytes `{{ab.b.hex}}`. `d_fast = dst + {{ch14.ab.dfast}}`. `lit_fast = s + {{ab.len}} - 16`, as a number {{ch14.ab.lit_fast_below}} below `s`.
`in_fast = s_end - 8 = s + 1`, since `src_len >= 8`. `end_gap = d_end - lit_end`, as numbers. `n_lit` is not
4096, so the loop starts.

@assert ab.len == 9 and ab.lits == 2 and ab.b.bytes == 5

**The first token.** `count = 0 < {{ch14.need}}`, so `next_token()` refills: fewer than 8 bytes are left, so the
byte loop runs {{ch14.ab.loads}} times, `bits = {{ch14.ab.bits}}`, `count = {{ch14.ab.count}}`, `p = end`. The index is the top 11
bits, `{{ch14.ab.idx_bin}}`, {{ch14.ab.idx}}, and the entry is `{{ch14.ab.entry}}`: bit 31 is set, the escape, and its drop is
{{ch14.ab.esc_n}}, the length of the escape's code. The page's token has no code of its own.

@assert ch14.ab.escaped == 1 and ch14.ab.value_bit == 1 and ch14.ab.ml == 35

**Sequence 1.** An escape ([the loop](#ch-decode--offset)). Line 609 drops its {{ch14.ab.esc_n}} bits, `count = {{ch14.ab.esc_count}}`.
The refill of line 610 finds `p == end` and adds nothing. The next 12 bits, `{{ch14.ab.esc_bits}}`, are the token, {{ch14.ab.tok}},
below 3072, and after they are dropped `count = {{ch14.ab.esc_count2}}`. `token_entry({{ch14.ab.tok}}, 0)` builds its entry, `{{ch14.ab.entry2}}`:
code length {{ch14.ab.N}}, since the code is gone already, `ll` {{ch14.ab.ll}}, `ml` {{ch14.ab.ml}}, class {{ch14.ab.cls}}, `back` {{ch14.ab.BACK}},
shift {{ch14.ab.SHIFT}}, drop {{ch14.ab.DROP}}, length value bit set. `raw = ((bits << {{ch14.ab.N}}) >> {{ch14.ab.BACK}}) << {{ch14.ab.SHIFT}}`: the {{ch14.ab.raw_n}} bit `{{ch14.ab.raw_bin}}`, {{ch14.ab.raw_sent}},
times 2, {{ch14.ab.raw}}. Drop {{ch14.ab.DROP}}, `count = {{ch14.ab.count2}}`.
@assert ch14.ab.raw_n == 1 and ch14.ab.SHIFT == 1 and ch14.ab.raw == 2
`off = {{ch14.ab.raw}}`, `nl = {{ch14.ab.ll}}`, `len = {{ch14.ab.ml}}`. The fast path is not taken, for three reasons at once: a length value
follows, `lit` is above `lit_fast`, and `br.p` is above `in_fast`. On the slow path, `nl` is not 15. The
checks pass: `lit == lit_end - 2`, `2 <= 2`, `2 <= 4096`. `copy_literals()` finds only {{ch14.ab.readable}} readable bytes
after `lit`, so it copies `a b` byte by byte. `d = dst + 2`. `len == 35`, so a match length value
follows: the refill finds `p == end` and adds nothing, and `seqlz_br_value()` reads the code `{{ch14.ab.vcode}}`,
symbol {{ch14.ab.vsym}}, and {{ch14.ab.vx}} extra bits, {{ch14.ab.vextra}}: the value is {{ch14.ab.value}} ([chapter 12](#ch-dec-basics--bit-reader)),
`count = {{ch14.ab.count3}}`, `len = 35 + {{ch14.ab.value}} = {{ch14.ab.len}}`. `last = 2`. The checks: `2 - 1 = 1 >= 2` is false, the offset is
fine; `{{ch14.ab.len}} <= {{ch14.ab.room}}`, the match fits. `copy_match()` with `off = 2` stores the pattern `abababab` every 8
bytes from `d` up to `d + {{ch14.ab.store_end}}`, and copies the last {{ch14.ab.bytewise}} bytes one by one. `d = d_end`.

**The end.** `lit == lit_end` and `d == d_end`, so `d - lit` is `end_gap`: 0 literals left for 0 bytes,
the last sequence, `break`. No second token is read. `copy_literals()` copies 0 bytes.

**The end checks.** `count = {{ch14.ab.count3}}`, `p == end`, `count < 8`, and the {{ch14.ab.count3}} bits left are 0, the rest of the
last byte. The function returns 0, and the page is `abab...ab`.

@assert ch14.ab.count3 == ab.b.pad

The example never takes the fast path; it has one sequence with a token, and that one is escaped
and has a length value. [Chapter 16](#ch-trace)'s page shows the fast path at work. With coded literals it compresses
to {{slab.coded.len}} bytes, and its decode goes like this at the start and at the end, recorded from the loop
of `seqlz_decode()` when the book was built:

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

{{ch14.fast}} of its {{ch14.seqs}} sequences with a token take the fast path. The slow ones have a length value, or come
in the last 8 bytes of the input, where the fast path's refill would read past it; the last sequence
has no token at all.

@assert ch14.seqs == slab.tokens and ch14.fast + ch14.slow == ch14.seqs and ch16.slow_novalue_past_in == 1

## What is left {#ch-decode--limits}

In a real swap-in the decoder of `seqlz-fast-lit` takes 2.0 µs per page in the kernel VM, against 1.4 µs
for `lz4`, and most of the difference is branch mispredictions on a page the CPU has not seen: about 110
more per page than when the same page was decoded just before
([explored-designs.md, "The decoder in a fault, found"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
The refill's branch is gone from the fast path. The next largest are the fast path's condition, 22 to 24
misses per page, and the test `off >= 8`, about 20. The condition misses once for every sequence with a
length value, and which sequences have one is data; four ways around it were all slower
([explored-designs.md, "The fast path's condition"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-fast-paths-condition-its-22-misses-per-page-are-the-length-values-four-ways-around-them-slower-not-kept)).
One copy for every offset instead of the test `off >= 8` took out 12 misses per page and cost 10 to 19
instructions per fast sequence, which was more
([explored-designs.md, "The offset below 8"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-offset-below-8-one-copy-for-every-offset-saves-12-misses-per-page-and-costs-more-not-kept)).
Fewer length values would need a wider token, a change of the format that was not tried. And the one
loop for all cores costs the in-order Cortex-A55 up to about 0.3 µs per warm read, the price of not
asking the CPU which core it is.
