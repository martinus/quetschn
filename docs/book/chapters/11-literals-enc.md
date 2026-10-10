# Coding the literals: one table of eight, eight streams {#ch-literals-enc}

When `compress_page()` is done, `dst` holds a complete page with raw literals: the u16 with the
number of literals, the literals themselves, and the bitstream of the sequences behind them
([chapter 10](#ch-encoder)). That page is already valid, and it is what `seqlz-fast` stores. For
`seqlz-fast-lit`, `seqlz_compress()` then calls `code_literals()`, which may turn the same bytes into
a page with Huffman coded literals: it prices the literals in all 8 literal tables, takes the
cheapest, decides whether coding them saves enough, and if so rewrites `dst` in place. The sequences'
bitstream does not change at all; it only moves down, behind the coded literals.

This chapter covers lines 320 to 593 of `src/seqlz_compress.c`. The helpers come first in the file,
`lit_put()` at the top and `code_literals()` at the bottom. We go through them in the order
`code_literals()` uses them instead, because each helper makes sense only once you know what its
caller needs. The format of a page with coded literals is in
[chapter 4](#ch-format); the tables and the packed `lit_cost` array are in
[chapter 7](#ch-internal--lit-cost), and how `build_lit()` makes the codes is in [chapter 8](#ch-codes). The
decoder's side of this chapter is [chapter 13](#ch-literals-dec).

## What code_literals() does to dst {#ch-literals-enc--overview}

@listing src/seqlz_compress.c 511-537

The comment says what the function decides and why it does not code every page. "1/16 of them plus
`SEQLZ_LIT_CODED_MIN` bytes" is the rule we meet in
[its own section below](#ch-literals-enc--rule). "Decoding coded literals costs time for every
byte" is the reason there is a rule at all: a page with coded literals is read slower than the same
page with raw literals, so a few bytes saved are not worth it.

The second paragraph of the comment is the memory trick of this function. The coded literals go
where the raw literals were, into `d` itself. Writing over bytes you are still reading does not
work, so the raw literals are read from a copy. There are two callers, and they bring the copy
differently:

- `seqlz_compress()` passes `raw = NULL` and `spare = st->literals`. `st` is the per-CPU
  `struct seqlz_state`, the matcher's hash table. Once the matcher is done with the page, the table
  is garbage, and the raw literals are copied into it.
- `seqlz_encode()`, for tests and tools, gets the literals from its caller as an array, so it passes
  `raw = literals` and `spare = NULL`. Nothing is copied.

So `d` needs no room beyond `len`, the length of the page with raw literals. The function returns the
new length, or `len` when the literals stay raw.

The declarations:

- `n_literals` is read from the first 2 bytes of the page with `get_unaligned_le16()`
  ([chapter 5](#ch-compat)). The page has raw literals at this point, so the top bit, `SEQLZ_LIT_CODED`,
  is 0 and the u16 is the plain count.
- `bitstream` is the length of the sequences' bitstream: what is left of `len` after the 2-byte
  header and the literals. In the page with raw literals it starts at `d + SEQLZ_HEADER + n_literals`.
- `literals` points at the raw literals inside `d`, at `d + 2`. The pricing reads them there. Only when
  the page will be coded does the pointer move to the copy.
- `bits = ~0U` is the largest `unsigned int`, so the first table's sum is always smaller. `set` will
  be the chosen table, `sizes[]` the 8 stream sizes in bytes, `coded` their sum, `width` the bits per
  size in the header, and `header` the header's bytes.
- `even` and `odd` are the per-stream sums of code lengths, explained with `lit_cost()` next. The
  `= { 0 }` clears all of them; `lit_cost()` adds into them.
- `q[]` has 9 pointers: the start of each of the 8 streams, and in `q[8]` the end of the last one.

Here is the whole operation on a page we will take apart at the end of the chapter: 200 letters
drawn at random from `etaoinshrdlu` and a space, then 3896 zero bytes. The matcher finds 201
literals (the 200 letters and the first zero) and one match that copies the zero byte to the end of
the page.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 280" role="img" aria-label="dst before and after code_literals(): the raw literals are copied into the matcher's table, the coded streams are written over their old place, and the bitstream moves down behind them">
  <text x="20" y="22">dst after compress_page(): 209 bytes, raw literals</text>
  <rect x="20" y="30" width="60" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="50" y="50" text-anchor="middle">c9 00</text>
  <rect x="80" y="30" width="440" height="30" fill="none" stroke="currentColor"/>
  <text x="300" y="50" text-anchor="middle">201 raw literals</text>
  <g class="acc">
    <rect x="520" y="30" width="80" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.25"/>
    <text x="560" y="50" text-anchor="middle">bitstream</text>
  </g>
  <text x="20" y="74" text-anchor="middle">0</text>
  <text x="80" y="74" text-anchor="middle">2</text>
  <text x="520" y="74" text-anchor="middle">203</text>
  <text x="600" y="74" text-anchor="middle">209</text>

  <line x1="300" y1="78" x2="300" y2="106" stroke="currentColor"/>
  <path d="M296 100 L300 108 L304 100 Z" fill="currentColor"/>
  <text x="308" y="96">memcpy()</text>
  <text x="20" y="104">spare = st-&gt;literals</text>
  <rect x="170" y="110" width="270" height="30" class="fill-sheet" stroke="currentColor" stroke-dasharray="4 3"/>
  <text x="305" y="130" text-anchor="middle">a copy of the 201 raw literals</text>

  <line x1="300" y1="140" x2="300" y2="196" stroke="currentColor"/>
  <path d="M296 190 L300 198 L304 190 Z" fill="currentColor"/>
  <text x="308" y="172">lit_streams()</text>

  <g class="acc">
    <line x1="560" y1="60" x2="490" y2="196" stroke="currentColor"/>
    <path d="M484 190 L489 199 L492 189 Z" fill="currentColor"/>
    <text x="536" y="130">memmove()</text>
  </g>

  <text x="20" y="194">dst after code_literals(): 151 bytes</text>
  <rect x="20" y="200" width="60" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="50" y="220" text-anchor="middle">c9 80</text>
  <rect x="80" y="200" width="30" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="95" y="220" text-anchor="middle">00</text>
  <rect x="110" y="200" width="60" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="140" y="220" text-anchor="middle">sizes</text>
  <rect x="170" y="200" width="35" height="30" fill="none" stroke="currentColor"/>
  <rect x="205" y="200" width="35" height="30" fill="none" stroke="currentColor"/>
  <rect x="240" y="200" width="35" height="30" fill="none" stroke="currentColor"/>
  <rect x="275" y="200" width="35" height="30" fill="none" stroke="currentColor"/>
  <rect x="310" y="200" width="35" height="30" fill="none" stroke="currentColor"/>
  <rect x="345" y="200" width="35" height="30" fill="none" stroke="currentColor"/>
  <rect x="380" y="200" width="35" height="30" fill="none" stroke="currentColor"/>
  <rect x="415" y="200" width="35" height="30" fill="none" stroke="currentColor"/>
  <text x="187" y="220" text-anchor="middle">s0</text>
  <text x="222" y="220" text-anchor="middle">s1</text>
  <text x="257" y="220" text-anchor="middle">s2</text>
  <text x="292" y="220" text-anchor="middle">s3</text>
  <text x="327" y="220" text-anchor="middle">s4</text>
  <text x="362" y="220" text-anchor="middle">s5</text>
  <text x="397" y="220" text-anchor="middle">s6</text>
  <text x="432" y="220" text-anchor="middle">s7</text>
  <g class="acc">
    <rect x="450" y="200" width="80" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.25"/>
    <text x="490" y="220" text-anchor="middle">bitstream</text>
  </g>
  <rect x="530" y="200" width="70" height="30" fill="none" stroke="currentColor" stroke-dasharray="2 3"/>
  <text x="565" y="220" text-anchor="middle">unused</text>
  <text x="20" y="244" text-anchor="middle">0</text>
  <text x="80" y="244" text-anchor="middle">2</text>
  <text x="110" y="244" text-anchor="middle">3</text>
  <text x="170" y="244" text-anchor="middle">8</text>
  <text x="205" y="244" text-anchor="middle">27</text>
  <text x="240" y="244" text-anchor="middle">44</text>
  <text x="275" y="244" text-anchor="middle">61</text>
  <text x="310" y="244" text-anchor="middle">78</text>
  <text x="345" y="244" text-anchor="middle">95</text>
  <text x="380" y="244" text-anchor="middle">112</text>
  <text x="415" y="244" text-anchor="middle">129</text>
  <text x="450" y="244" text-anchor="middle">145</text>
  <text x="530" y="244" text-anchor="middle">151</text>
  <text x="170" y="262" text-anchor="middle">q[0]</text>
  <text x="450" y="262" text-anchor="middle">q[8]</text>
</svg>
</div>
<figcaption>One page in dst before and after code_literals(), not to scale: the raw literals are copied out, the 8 streams are written over their old place, and the bitstream moves down.</figcaption>
</figure>

The offsets under the second row are the values of `q[0]` to `q[8]` for this page, measured from
`d`. Bytes 2 to 150 of the first row are overwritten: by byte 2, by the 5 bytes of sizes, by the
streams, and by the bitstream at its new place. Bytes 151 to 208 keep what they held, but they are no
longer part of the page.

## Pricing all 8 tables with one add per literal {#ch-literals-enc--pricing}

The first thing `code_literals()` does is `lit_cost(t, literals, n_literals, even, odd)`. It answers:
for every stream and every literal table, how many bits do this stream's literals take with that
table's codes?

The obvious way is a loop over the 8 tables, and inside it a loop over the literals that adds
`seqlz_lit_sets[table][byte]`: 8 loads and 8 adds per literal. The first version in the project was
a histogram of the literals followed by 8 times 256 products, and it cost 1400 ns on pages with few
literals ([explored-designs.md, "The encoder prices all 8 tables at
once"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
`lit_cost()` does it with one 64-bit add per literal instead.

::: cpu SWAR: several small numbers in one register
SWAR means "SIMD within a register". A 64-bit integer can hold 8 independent bytes, and one ordinary
`+` adds all 8 byte pairs at once, as long as no byte sum exceeds 255: a byte that overflows carries
into the byte above it and spoils it. Nothing in the CPU knows about the lanes; the code has to make
sure they never overflow. The kernel may not use the CPU's real vector registers in this code
([chapter 3](#ch-cpu)), so SWAR is the way to add many small numbers at once there.
:::

`t->lit_cost[b]` is that 64-bit number for byte value `b`: its code length in table 0 in bits 0 to 7,
in table 1 in bits 8 to 15, and so on up to table 7 in bits 56 to 63. `build_lit_sets()` fills it
once at start-up ([chapter 8](#ch-codes)), and the array is 256 times 8 bytes, 2 KiB. Its second
index, `LIT_COST_WORDS`, is `(8 + 7) / 8 = 1`: the code is written for any number of tables, a u64 per
8 of them, but with 8 tables every loop over `w` runs once. Read `[w]` as `[0]` in what follows.

@listing src/seqlz_compress.c 384-399

The comment states the plan. The sums go per stream, into `even` and `odd`, which hold 16-bit lanes:
`even[j]` has the sums of stream `j` for tables 0, 2, 4 and 6, `odd[j]` for tables 1, 3, 5 and 7.
The adds themselves go into 8-bit lanes of a temporary `x`, at most `LIT_COST_RUN` literals per
stream, and then into the wider lanes.

`LIT_COST_RUN` is `255 / SEQLZ_LIT_BITS`, `255 / 10 = 25` in integer division. A code has at most 10
bits, so 25 literals add at most 250 to a lane, which fits a byte; a 26th could make it 260 and
overflow. `lanes = 0x00ff00ff00ff00ff` keeps every second byte, the comment says how it is used: to
split 8 lanes of 8 bits into 2 times 4 lanes of 16 bits.

@listing src/seqlz_compress.c 400-422

The outer loop walks over the literals in chunks of `SEQLZ_LIT_STREAMS * LIT_COST_RUN`, 8 times 25 =
200 literals, 25 per stream. For each chunk:

- `x` is 8 u64, one per stream, cleared. It is declared inside the loop, so the `= { 0 }` runs once per
  chunk.
- `end` is where the chunk ends: `k + 200`, or `n_literals` if fewer than 200 are left. The test is
  written as `n_literals - k < 200` and not as `k + 200 > n_literals`; both are fine here, because
  `k <= n_literals` and no sum can wrap.
- The first inner loop takes 8 literals at a time, `literals[k]` to `literals[k + 7]`, one for each
  stream: literal `k + j` goes to stream `j`. Since `k` is always a multiple of 8 at this point,
  `k + j` modulo 8 is `j`, which is the format's rule that literal `i` belongs to stream `i % 8`.
  Each literal is one load of `t->lit_cost[byte]` and one 64-bit add.
- The second inner loop takes the rest of the chunk one literal at a time, into stream `k % 8`. It
  runs only in the last chunk, and only when `n_literals` is not a multiple of 8: 200 is a multiple of
  8, so every earlier chunk ends with `k == end`.
- The last loop moves each stream's 8 byte lanes into its 16-bit lanes. `x[j][w] & lanes` keeps bytes
  0, 2, 4 and 6 of `x`, the sums of tables 0, 2, 4 and 6, each now in the low half of a 16-bit lane
  whose high half is 0. `x[j][w] >> 8 & lanes` first moves every byte down by one byte, so that the
  sums of tables 1, 3, 5 and 7 sit where 0, 2, 4 and 6 were, then keeps those. Both are added into the
  running sums. `>>` binds tighter than `&` in C, so the second expression is `(x >> 8) & lanes`.

A 16-bit lane holds up to 65535. A stream has at most every 8th literal of a page: 512 literals of
at most 10 bits on 4 KiB pages, 5120 bits, and 2048 of them on 16 KiB pages, 20480 bits. Both fit.

### A worked example with real code lengths

`docs/seqlz.md` prices the 4 literals `65 20 65 00`, the letter `e`, a space, `e` again and a zero
byte. Here are their `lit_cost` entries from the tables of this commit, printed by a test program,
with table 7 in the top byte:

```text
byte 65: lengths 4 9 10 5 9 9 10 10   lit_cost 0x0a0a0909050a0904
byte 20: lengths 4 7  7 6 7 9  8  8   lit_cost 0x0808090706070704
byte 00: lengths 8 2  2 2 2 9  3  8   lit_cost 0x0803090202020208
```

If these 4 literals were all of stream 0, `x[0]` would grow like this:

```text
after 65:  0x0a0a0909050a0904
after 20:  0x121212100b111008
after 65:  0x1c1c1b19101b190c
after 00:  0x241f241b121d1b14
```

Read the last one byte by byte from the right: table 0 has `0x14 = 20` bits, table 1 `0x1b = 27`,
table 2 `0x1d = 29`, table 3 `0x12 = 18`, table 4 `0x1b = 27`, table 5 `0x24 = 36`, table 6
`0x1f = 31`, table 7 `0x24 = 36`. Those are the sums in the table of
[`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-literals-one-table-per-page-in-8-streams),
from 4 adds instead of 32. Then the split:

```text
x                = 0x241f241b121d1b14
x & lanes        = 0x001f001b001d0014    even: tables 6, 4, 2, 0 = 31, 27, 29, 20
x >> 8 & lanes   = 0x002400240012001b    odd:  tables 7, 5, 3, 1 = 36, 36, 18, 27
```

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 240" role="img" aria-label="the 8 byte lanes of x split into the 16-bit lanes of even and odd">
  <text x="20" y="58">x</text>
  <text x="20" y="128">even</text>
  <text x="20" y="198">odd</text>
  <text x="125" y="32" text-anchor="middle">table 7</text>
  <text x="195" y="32" text-anchor="middle">table 6</text>
  <text x="265" y="32" text-anchor="middle">table 5</text>
  <text x="335" y="32" text-anchor="middle">table 4</text>
  <text x="405" y="32" text-anchor="middle">table 3</text>
  <text x="475" y="32" text-anchor="middle">table 2</text>
  <text x="545" y="32" text-anchor="middle">table 1</text>
  <text x="615" y="32" text-anchor="middle">table 0</text>
  <rect x="90" y="40" width="70" height="28" fill="none" stroke="currentColor"/>
  <rect x="160" y="40" width="70" height="28" class="fill-sheet" stroke="currentColor"/>
  <rect x="230" y="40" width="70" height="28" fill="none" stroke="currentColor"/>
  <rect x="300" y="40" width="70" height="28" class="fill-sheet" stroke="currentColor"/>
  <rect x="370" y="40" width="70" height="28" fill="none" stroke="currentColor"/>
  <rect x="440" y="40" width="70" height="28" class="fill-sheet" stroke="currentColor"/>
  <rect x="510" y="40" width="70" height="28" fill="none" stroke="currentColor"/>
  <rect x="580" y="40" width="70" height="28" class="fill-sheet" stroke="currentColor"/>
  <g class="mark">
    <text x="125" y="59" text-anchor="middle">24</text>
    <text x="265" y="59" text-anchor="middle">24</text>
    <text x="405" y="59" text-anchor="middle">12</text>
    <text x="545" y="59" text-anchor="middle">1b</text>
  </g>
  <text x="195" y="59" text-anchor="middle">1f</text>
  <text x="335" y="59" text-anchor="middle">1b</text>
  <text x="475" y="59" text-anchor="middle">1d</text>
  <text x="615" y="59" text-anchor="middle">14</text>

  <rect x="90" y="110" width="140" height="28" fill="none" stroke="currentColor" stroke-width="2"/>
  <rect x="230" y="110" width="140" height="28" fill="none" stroke="currentColor" stroke-width="2"/>
  <rect x="370" y="110" width="140" height="28" fill="none" stroke="currentColor" stroke-width="2"/>
  <rect x="510" y="110" width="140" height="28" fill="none" stroke="currentColor" stroke-width="2"/>
  <rect x="160" y="110" width="70" height="28" class="fill-sheet" stroke="none"/>
  <rect x="300" y="110" width="70" height="28" class="fill-sheet" stroke="none"/>
  <rect x="440" y="110" width="70" height="28" class="fill-sheet" stroke="none"/>
  <rect x="580" y="110" width="70" height="28" class="fill-sheet" stroke="none"/>
  <text x="125" y="129" text-anchor="middle">00</text>
  <text x="195" y="129" text-anchor="middle">1f</text>
  <text x="265" y="129" text-anchor="middle">00</text>
  <text x="335" y="129" text-anchor="middle">1b</text>
  <text x="405" y="129" text-anchor="middle">00</text>
  <text x="475" y="129" text-anchor="middle">1d</text>
  <text x="545" y="129" text-anchor="middle">00</text>
  <text x="615" y="129" text-anchor="middle">14</text>
  <text x="160" y="154" text-anchor="middle">table 6: 31</text>
  <text x="300" y="154" text-anchor="middle">table 4: 27</text>
  <text x="440" y="154" text-anchor="middle">table 2: 29</text>
  <text x="580" y="154" text-anchor="middle">table 0: 20</text>

  <rect x="90" y="180" width="140" height="28" fill="none" stroke="currentColor" stroke-width="2"/>
  <rect x="230" y="180" width="140" height="28" fill="none" stroke="currentColor" stroke-width="2"/>
  <rect x="370" y="180" width="140" height="28" fill="none" stroke="currentColor" stroke-width="2"/>
  <rect x="510" y="180" width="140" height="28" fill="none" stroke="currentColor" stroke-width="2"/>
  <text x="125" y="199" text-anchor="middle">00</text>
  <text x="265" y="199" text-anchor="middle">00</text>
  <text x="405" y="199" text-anchor="middle">00</text>
  <text x="545" y="199" text-anchor="middle">00</text>
  <g class="mark">
    <text x="195" y="199" text-anchor="middle">24</text>
    <text x="335" y="199" text-anchor="middle">24</text>
    <text x="475" y="199" text-anchor="middle">12</text>
    <text x="615" y="199" text-anchor="middle">1b</text>
  </g>
  <text x="160" y="224" text-anchor="middle">table 7: 36</text>
  <text x="300" y="224" text-anchor="middle">table 5: 36</text>
  <text x="440" y="224" text-anchor="middle">table 3: 18</text>
  <text x="580" y="224" text-anchor="middle">table 1: 27</text>
</svg>
</div>
<figcaption>x with one byte lane per table, and the two masks that widen it: even keeps the shaded bytes in place, odd shifts the others down by one byte.</figcaption>
</figure>

### Why one sum per stream, and not one per page

To choose a table, one sum over all literals would do. The 8 sums per table are for what comes after
the choice: each stream is written as whole bytes, rounded up on its own, and the header stores each
stream's size. `code_literals()` has to know all 8 sizes before it writes the first stream, because
stream 1 starts where stream 0 ends. With one lane per stream, the exact size of every stream in
the chosen table falls out of the pricing, and no second pass over the literals is needed. The cost
is 8 accumulators `x[j]` instead of one, and the 8 widenings per chunk.

::: measured What the pricing costs
In the kernel, a build that prices the literals and then stores the page with raw literals wrote a
page in 6920 ns at p50 and 10 529 ns at p99, against 6890 and 10 490 ns for `seqlz-fast`, which does
not price. That is 30 to 40 ns per write, about 0.5% of a write, measured in the kernel in September
2026. In userspace loops over 2000 pages the pricing was at most 400 cycles, 1.5% of a write.
Choosing the table from every 4th literal would save little: the sizes still need a pass over all
literals with the chosen table, which costs about as much as pricing all 8 tables, and it was not
kept. Writing the
streams is what costs: about 0.6 ns per coded literal in the kernel. Source: [explored-designs.md,
"seqlz-fast-lit: one of 8 literal tables per
page"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page).
:::

## Reading one sum out of its lane: lit_bits() {#ch-literals-enc--lit-bits}

@listing src/seqlz_compress.c 374-382

`lit_bits()` returns the bits of stream `j` in table `k`. The parameters `u64 even[][LIT_COST_WORDS]`
are pointers to arrays of one u64, which is how C passes a 2-D array.

- `k & 1U ? odd : even` picks the array: odd table numbers are in `odd`. Both operands of `?:` are
  the same pointer type, so the result can be indexed directly with `[j][k / 8]`. `k / 8` is the u64
  word, always 0 with 8 tables.
- `k % 8 / 2` is the 16-bit lane inside that word: tables 0 and 1 are in lane 0, 2 and 3 in lane 1,
  4 and 5 in lane 2, 6 and 7 in lane 3. `16U * ...` is the shift that brings the lane down to bits 0
  to 15, and `& 0xffffU` drops the lanes above it.

For `k = 5`: `odd`, word 0, lane `5 % 8 / 2 = 2`, shift 32. In the example above `odd` is
`0x002400240012001b`; shifted right by 32 it is `0x00240024`, and `& 0xffff` gives `0x24 = 36`, table
5's sum. The cast to `unsigned int` happens after the mask, so nothing is lost.

## The table with the fewest bits, and the 8 sizes {#ch-literals-enc--choice}

Back in `code_literals()`, after the pricing:

@listing src/seqlz_compress.c 538-554

The first loop adds the 8 streams' bits for each table and keeps the smallest total. Because the
compare is `b < bits`, a tie keeps the lower table number. That is a choice of the encoder only; the
decoder takes whatever table byte 2 names. By the comment in `seqlz_lit_sets.c`, the most used table
comes first, so a tie with table 0 goes to it.

The choice compares bits, not bytes. The page takes the 8 sizes rounded up to whole bytes, so a
table with fewer bits can still take a few bytes more after rounding, fewer than 7. The code does not
look for that case.

The second loop computes, for the chosen table:

- `sizes[j]`, the bytes of stream `j`: its bits rounded up with `(bits + 7U) / 8U`. 147 bits are
  `154 / 8 = 19` bytes, 144 bits are 18.
- `coded`, the sum of the 8 sizes: the bytes of all streams, without the header.
- `all`, the bitwise OR of all sizes. Its highest set bit is the highest set bit of the largest
  size, which is all `width` needs. OR saves a compare per stream against computing the maximum.

## The width of a size, from one count of leading zeros {#ch-literals-enc--width}

@listing src/seqlz_compress.c 555-561

The header stores each stream size in `width` bits, `w` in the format: as few as the largest size
needs, but at least `SEQLZ_SIZE_BITS_MIN`, 5 ([chapter 6](#ch-header)).

`all |= 1U << (SEQLZ_SIZE_BITS_MIN - 1U)` sets bit 4, the value 16. A number with bit 4 set needs at
least 5 bits, so this is the minimum of 5 without a compare. It has a second job: `__builtin_clz(0)`
is undefined, and after the OR `all` is never 0.

`__builtin_clz(all)` counts the zero bits above the highest set bit of the 32-bit `all`, so
`32 - clz` is the number of bits `all` needs, its bit width. The cast to `unsigned int` is because
`__builtin_clz()` returns `int`. Three examples:

| largest size | `all`, with every size the largest | `clz` | `width` | header bytes |
| --- | --- | --- | --- | --- |
| 19 bytes, the example page | `0b10011` = 19 | 27 | 5 | 8 |
| 100 bytes | `0b1110100` = 116 | 25 | 7 | 10 |
| 640 bytes, the largest a 4 KiB page can have | `0b1010010000` | 22 | 10 | 13 |

Smaller sizes can set more low bits in `all`, which changes nothing above its highest bit. `width` is at most 12 on 16 KiB
pages, where a stream has at most 2560 bytes. `header = SEQLZ_LIT_HEADER(width)` is `3 + width`: 2
bytes of u16, byte 2, and `width` bytes that hold 8 sizes of `width` bits each, `8 * width` bits.

::: why Sizes in w bits and not 2 bytes each
Until 5th October 2026 every size had 2 bytes, a header of 19 bytes. Most streams are below 128
bytes: on the two desktop dumps the largest stream of a page with coded literals was below 128 bytes
on 85% and 69% of the pages. With `w` bits per size the header is mostly 9 to 11 bytes, and pages are
5.2 and 5.5 bytes smaller on average on the two desktop dumps, zsmalloc cost in userspace. Widths of 3
and 4 bits are left out: they would pay only on pages whose streams are all below 16 bytes, and such
pages rarely pass the rule below. Source: [explored-designs.md, "Stream sizes in as many bits as the
largest needs,
kept"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#stream-sizes-in-as-many-bits-as-the-largest-needs-kept).
:::

## Coded only where it saves 1/16 of the literals and 51 bytes {#ch-literals-enc--rule}

@listing src/seqlz_compress.c 562-563

The page keeps its raw literals unless `coded + 51 < n - n / 16`: the 8 streams plus 51 bytes have to
be smaller than 15/16 of the raw literals. Everything is `unsigned int`; `n - n / 16` cannot wrap,
because `n / 16 <= n`. For `n = 0` the right side is 0 and the page stays raw, which is right:
there is nothing to code.

What the rule means in bytes: the page with raw literals has `2 + n` bytes before the bitstream, the
coded one `3 + w + coded`. If the rule holds, the page shrinks by `n - coded - 1 - w`, which is more
than `51 + n / 16 - 1 - w`: more than 38 bytes plus 1/16 of the literals, with the largest `w` of 12.

Two different reasons are behind the two terms.

**The 1/16.** zsmalloc stores a page in a size class, and classes are at least 16 bytes apart
([chapter 1](#ch-zram)). A page that is a few bytes smaller often lands in the same class and saves
nothing, while every read of it pays for decoding the literals. Coding the literals whenever they
save anything at all gave less than 0.1 points (0.1% of a page) more on the dumps of September 2026,
in userspace ([`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-literals-one-table-per-page-in-8-streams)).

**The 51 bytes.** A page with coded literals costs a fixed time to read, whatever the number of
literals: the decoder has to bring the page's 2 KiB literal table into the cache, and it decodes the
literals into a buffer of work memory first ([chapter 13](#ch-literals-dec)). On a phone's little
core, which has few cache misses in flight, that fixed cost is several µs.

::: measured 51 bytes instead of 19, on the Mi 9T and on the PC
Before 5th October 2026 the minimum was 19 bytes. In the Mi 9T's own kernel, 20 000 pages of the
second phone dump, clocks fixed, a minimum of 51 coded 24.9% instead of 38.8% of the pages. Pages
got 7.9 bytes larger on average, and the time per page written (write + 0.34 × cold read) got 2.7 µs
shorter on the Cortex-A55 and 1.1 µs shorter on the Cortex-A76. 2/16 of the literals instead of 1/16
coded about as few pages, for 12.3 bytes and less time saved. In the kernel VM on a Ryzen 9 7950X, 51
costs 11 bytes per page on the whole first desktop dump for 0.1 µs, within what two boots of the same
build differ. 51 is the best of the three minimums measured, 19, 51 and 115, not a measured optimum.
Measured again with the prefetches of items 4 and 7 of "Six choices" (the compressed data prefetched, the
tables only where it helps), 51 saved 0.8 µs on the A55 and 0.4 µs on the A76, both within the noise
there; it stayed. Sources:
[explored-designs.md, "Coded literals only where they save 51
bytes"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-only-where-they-save-51-bytes-27-µs-per-page-written-less-on-the-a55-kept)
and [item 9 of "Six
choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#9-the-51-bytes-of-coded-literals-only-where-they-save-51-bytes-again-with-item-4s-and-7s-prefetches).
:::

The rule does not count the header of `3 + w` bytes. That is deliberate: when the sizes shrank from
2 bytes to `w` bits, counting the smaller header would have coded 64.2% instead of 53.6% of the pages on
the first desktop dump, and on the phone's pages it would have saved 2 bytes per page for 251 ns more
per page written on the A55 than the rule that was kept. So the rule kept the old 19 bytes as its constant,
and the same pages as before got coded with a smaller header. The 51 of today replaced that 19.

## Writing over the raw literals, safely {#ch-literals-enc--in-place}

@listing src/seqlz_compress.c 564-574

From here on the page gets coded literals.

`literals = raw ? raw : memcpy(spare, literals, n_literals)` makes `literals` point at a copy that
nothing below writes to. `memcpy()` returns its first argument, so with `raw == NULL` the line copies
the raw literals out of `d` into `spare` and points `literals` there. Pages that stay raw never get
here, so they are never copied.

`q[0] = d + header` is where stream 0 starts, right after the header, and each `q[j + 1]` is
`q[j] + sizes[j]`. `q[8]` is the end of stream 7. Then `lit_streams()` writes the streams, with the
chosen table's encoder entries `t->lit[set].enc`.

The comment before the `memcpy()` is the safety argument for the whole in-place rewrite, and the
first of the two `static_assert`s after the function makes it a compile-time fact:

@listing src/seqlz_compress.c 588-592

`static_assert(condition, message)` is checked by the compiler; if the condition is false the build
fails with the message ([chapter 5](#ch-compat)). It costs nothing at run time.

The first one says `SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MAX) <= SEQLZ_HEADER + SEQLZ_LIT_CODED_MIN`, that
is `3 + 12 = 15 <= 2 + 51 = 53`. With it, the proof that the streams end before the bitstream is
three lines:

```text
streams end at     d + header + coded
header           <= 15 <= 2 + 51                     (the static_assert)
coded + 51        < n - n/16 <= n                     (the rule, since the page is coded)
so header + coded < 2 + 51 + n - 51 = 2 + n,  and d + 2 + n is where the bitstream starts
```

So no stream byte lands on the bitstream, and it can stay where it is while the streams are written.
In the example page, the streams end at byte 145 and the bitstream starts at byte 203.

The writes are bounded on the other side too: every store of `lit_streams()` stays between `q[0]` and
`q[8]`, as we will see in `lit_flush()`. The u16, byte 2 and the sizes are written last, after
`lit_streams()`, over bytes that were raw literals.

The second `static_assert` is about the copy. `struct seqlz_state` ([chapter 6](#ch-header)) is a union
of the matcher's `u16 table[1 << SEQLZ_HASH_BITS]` and `u8 literals[SEQLZ_PAGE]`. A union is as large as
its largest member, so `literals` is there for free only if the table is at least a page. With 4 KiB
pages the table has 4096 entries of 2 bytes, 8192 bytes; with 16 KiB pages 8192 entries, 16 384 bytes,
exactly a page. `sizeof_field(type, member)` is the kernel's macro for the size of a member without an
object. The assert keeps the work memory per CPU at the table's size: the staging area for the raw
literals costs no byte of it.

::: why The matcher's table, and not a second buffer or the end of dst
The first version moved the raw literals and the bitstream to the end of `dst` and wrote the coded
literals from its front, so all three had to fit at once. zram gives the compressor two pages, which
is plenty; zswap gives exactly one page, and there pages with coded literals failed to fit more
often, which cost 1.6% to 2.9% more memory on four dumps. With the raw literals in the matcher's
table it costs 0.08% to 0.50%, and in the kernel VM writes got 0.08 to 0.18 µs faster. In zram's two
pages the output is the same byte for byte, checked on 80 000 pages of four dumps. Source:
[explored-designs.md, "Coded literals from the matcher's
table"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-from-the-matchers-table-a-buffer-of-one-page-costs-01-to-05-instead-of-16-to-29-kept).
:::

## Four streams at a time: lit_streams() {#ch-literals-enc--streams}

`lit_streams()` writes the 8 streams. Each literal's code goes to the stream of its index modulo 8,
most significant bit first, which is how the decoder reads them ([chapter 13](#ch-literals-dec)).

@listing src/seqlz_compress.c 424-443

The comment says why the loop is shaped the way it is. Appending a code to a stream is a shift and an
OR into the stream's accumulator: `acc = acc << len | code`. The next code of the same stream needs
the result, so all codes of one stream form a chain where every step waits for the one before. When
the streams were written one after the other, the CPU had nothing else to do while it waited, and
that cost about 3.5 cycles per literal when it was measured with commit `34080eb` ([explored-designs.md, "Numbers that
were only in the codec's
comments"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).
With 4 accumulators, 4 independent chains run side by side: an out-of-order CPU works on the shift
of stream 1 while stream 0's is still in flight ([chapter 3](#ch-cpu)).

The outer loop runs twice: `half = 0` writes streams 0 to 3, `half = 4` streams 4 to 7. For each half:

- `p0` to `p3` are where the next bytes of the 4 streams go, starting at `q[half]` to `q[half + 3]`.
- `e0` to `e3` are their ends: stream 0 ends where stream 1 starts, so `e0 = p1` (taken before `p1`
  moves), and the last one ends at `q[half + 4]`. For `half = 4` that is `q[8]`.
- `a0` to `a3` are the 64-bit accumulators, the newest bits in the low end.
- `c0` to `c3` count the bits still in an accumulator from before its last flush, 0 to 7.
- `s0` to `s3` collect the sum of the encoder entries since the last flush, see `lit_put()`.

Why 4 at a time and not all 8 is not written down in the repository. Before commit `34080eb` the format
had 4 streams, written one after the other. On the way to that commit, the encoder first wrote the 4
streams in one loop, and then the 8 streams as that loop twice. A guess, not measured: each
stream needs 5 live values, and 8 streams would be 40, far more than the 16 general registers of
x86-64. The kernel measurements of the step from one loop of 4 streams to "8 streams, 4 in one loop,
twice" are in the steps table of [explored-designs.md, "seqlz-fast-lit: one of 8 literal tables per
page"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page):
7590 ns at p50 and 12 030 ns at p99 per write, on the second desktop dump, against 7530 and 12 030 ns
with 4 streams in one loop, two other changes earlier.

### Blocks of 32 literals

@listing src/seqlz_compress.c 444-460

One block is 32 literals of the page, `k` to `k + 31`, and from it this half's 4 streams get 4
literals each. For `half = 0` and `k = 0`, the inner loop runs `j = 0, 8, 16, 24` and stream 0 gets
literals 0, 8, 16 and 24, stream 1 gets 1, 9, 17 and 25, and so on. For `half = 4`, `k` starts at 4,
and stream 4 gets literals 4, 12, 20 and 28. The 4 `lit_put()` calls in one iteration are the 4
chains; each depends only on its own accumulator from the iteration before.

After 4 literals per stream, each stream is flushed: its whole bytes go to memory. 4 codes of at most
10 bits are 40 bits, plus at most 7 left from the flush before, 47 bits, which fit the 64-bit
accumulator. 5 codes would fit too, 7 + 50 = 57 bits, 6 would not; the repository does not say why
the blocks have 4 and not 5. With 4, a block is 32 literals, and `k` moves on by a power of 2.

The loop condition `k + 28U < n_literals` keeps whole blocks only. The highest literal a block uses
is `k + 27`, from `j = 24` and the 4th stream's `+ 3`, so a block needs `k + 27 < n`; the code asks
for one more, `k + 28 < n`. Either way, after the loop fewer than 29 literals are left from
`k` on, and of those, each of this half's streams has at most 4: e.g. stream 0 can only have `k`,
`k + 8`, `k + 16` and `k + 24`. That is what the comment means, and what the tail below relies on: the
rest fits the accumulator without a flush in between.

### lit_put(): one code into an accumulator

This is the first helper of the file, at line 320; it is used only here.

@listing src/seqlz_compress.c 320-334

`enc[b]` is the literal table's encoder entry for byte `b`, built by `build_lit()`
([chapter 8](#ch-codes)): the code's length in the low byte, the code itself from bit 8 on. Lengths
are 1 to 10, codes below `1 << 10`.

- `acc << (e & 63U) | e >> 8` shifts the accumulator left by the code's length and puts the code into
  the freed low bits. `e & 63U` is the length: the low byte holds at most 10, and the code above it
  starts at bit 8, so masking with 63 gives the same number. The mask is there so that the compiler
  can see the shift count is below 64. A shift of a u64 by 64 or more is undefined behaviour in C, and
  a kernel built with `CONFIG_UBSAN_SHIFT` adds a check and a branch to every shift it cannot prove
  in range ([chapter 7](#ch-internal) has the comment that says so). `e >> 8` is the code. `<<` binds
  tighter than `|`, so no parentheses are needed.
- `*sum += e` adds the whole entry, code and length, to the stream's sum.

The sum is the trick the comment describes. The entries have the length in the low byte and the code
from bit 8 up. Adding them, the low byte of the sum is the sum of the lengths modulo 256: the codes
add only into bits 8 and higher, and a carry only ever goes upward, never into the low byte. Between
two flushes a stream gets at most 4 codes of at most 10 bits, 40, so the low byte is the exact number
of bits added. The rest of `sum` is garbage, and an `unsigned int` that wraps around is defined in C.
`lit_flush()` reads `*sum & 255U`. So the length of each code is counted with one add per literal,
and the mask is applied once per flush instead of once per literal.

Stream 0 of the example page, in table 0, gets literals 0, 8, 16 and 24, the letters `o`, `d`, `h`
and `n`:

| literal | `enc[b]` | code | `acc` after | `sum` after | low byte |
| --- | --- | --- | --- | --- | --- |
| `o` | `0x805` | `01000`, 5 bits | `0x8` | `0x805` | 5 |
| `d` | `0x1a06` | `011010`, 6 bits | `0x21a` | `0x220b` | 11 |
| `h` | `0x5907` | `1011001`, 7 bits | `0x10d59` | `0x7b12` | 18 |
| `n` | `0x705` | `00111`, 5 bits | `0x21ab27` | `0x8217` | 23 |

`acc` now holds the 23 bits `01000 011010 1011001 00111`, oldest on top.

### lit_flush(): whole bytes to memory, big-endian

@listing src/seqlz_compress.c 350-372

`lit_flush()` is the literal streams' version of `enc_flush()` ([chapter 10](#ch-encoder)), with one
difference: the sequences' bitstream is written backwards from the end of `dst`, a stream forwards from
its start.

- `n = *cnt + (*sum & 255U)` is the number of bits in the accumulator that are not yet in memory as
  whole bytes: the up to 7 bits left over from the last flush plus the bits added since.
- `acc << ((64U - n) & 63U)` moves those `n` bits to the top of the u64; older bits above them fall
  out. The `& 63U` is again for the shift count; it matters only for `n = 0`, where `64 - 0 = 64`
  would be undefined and the mask makes it a shift by 0. `n` is 0 only at a stream's last flush when
  its bits ended on a byte boundary; `p` is then already at `end`, and `store_tail()` writes 0 bytes.
- `__builtin_bswap64()` reverses the byte order of the 64-bit value. Then `put_unaligned_le64(w, p)`
  stores it little endian, lowest byte first. The two reversals cancel: the store puts the original
  top byte at `p`, the next one at `p + 1`, which is what a big-endian store of the shifted
  accumulator would do. The stream's first bit ends up as the top bit of the byte at `p`, the order in
  which the decoder reads.
- `end - p >= 8`: if 8 bytes fit before the end of this stream, it writes all 8. Only the first
  `(n + 7) / 8` of them carry bits; the rest are zeros from the shift, and they land in this stream's
  own room, which later flushes overwrite. Near the end it calls `store_tail()`,
  which writes only the bytes that carry bits, `(n + 7) >> 3`, so it never touches the next stream.
- `p += n >> 3` moves on by the whole bytes only. A last byte that is not full was written, and is
  written again, with more bits in it, by the next flush, because those bits are still in `acc`.
- `*cnt = n & 7U` is how many bits that partial byte has; `*sum = 0` starts the next count.

::: why One byte swap before the branch
The obvious form is `put_unaligned_be64()` in the first branch and a big-endian byte loop in the
second. On arm64, swapping the bytes inside both branches gave `code_literals()` other machine
code, so the code swaps once, in a register, before the branch, and both branches store the
same `w` (commit `9d64639`, and the comment in [chapter 6](#ch-header) on why the builtin is used).
On a big-endian machine `put_unaligned_le64()` swaps the bytes itself, so the bytes in memory are
the same on every machine; CI checks that on s390x.
:::

For stream 0 of the example, the first flush:

```text
n        = 0 + 23 = 23
acc<<41  = 0x43564e0000000000        the 23 bits on top, zeros below
w        = 0x00000000004e5643        bswap64
stored   = 43 56 4e 00 00 00 00 00   at q[0], byte 8 of the page
p       += 23 >> 3 = 2               cnt = 23 & 7 = 7
```

The bytes `43 56` are final. `4e` holds 7 bits of stream 0, `0100111`, and a 0 that is not a bit yet;
the next flush writes this byte again with its eighth bit. In the finished page, stream 0 starts with
`43 56 4e e0`.

### store_tail(): at most 7 bytes at the end of a stream

@listing src/seqlz_compress.c 336-348

`store_tail()` writes the first `n` bytes of what `put_unaligned_le64(w, p)` would write, for `n` below
8. It stores the u64 into a local array and copies `n` bytes, a loop of up to 7 stores, slower than one
store, which is fine: it runs at most once or twice per stream, at its end. It is not
`__always_inline`, so the compiler decides whether to inline it; `gcc -O2` for x86-64 does.

A stream never needs more room than its size: `sizes[j]` was computed from the exact bits of its
literals, the stream writes those bits and nothing else, and its last flush writes `(n + 7) / 8` bytes
that end exactly at `e`. The bits after the last code in that last byte are the zeros the shift
brought in, which is what the format requires: a stream ends in its last byte, with the trailing bits
0 ([chapter 4](#ch-format)).

### The tail: at most 4 literals per stream, one switch

@listing src/seqlz_compress.c 461-490

After the blocks, fewer than 29 literals are left from `k` on, and the loop takes them one by one.
`k % SEQLZ_LIT_STREAMS` says which stream a literal belongs to. Cases 0 and 4 both go to `a0`, 1 and 5
to `a1`, and so on, because `a0` is stream 0 when `half = 0` and stream 4 when `half = 4`. The same
switch serves both halves.

The literals of the other half must be skipped, and that is `if ((k & 3U) == 3U) k += 4;`. After this
half's 4th literal of a group of 8, at `k % 8 == 3` or `k % 8 == 7`, `k` jumps over the next 4, and
the loop's `k++` adds the fifth. For `half = 0` and the example's `n = 201`, the blocks end at
`k = 192`, and the tail takes:

```text
k = 192  case 0  stream 0
k = 193  case 1  stream 1
k = 194  case 2  stream 2
k = 195  case 3  stream 3, then k += 4 -> 199, k++ -> 200
k = 200  case 0  stream 0
k = 201  stop
```

For `half = 4` the blocks run from `k = 4` up to `k = 164`, and the tail starts at `k = 196`, which is
case 4, stream 4. After 199, `k += 4` makes it 203, past the end. `k` can step past `n` this way, but
the loop condition `k < n_literals` stops it, and `k` is far from wrapping around.

The `switch` runs for at most 16 literals per half, 4 per stream, so its cost does not matter next to
the blocks above it. The 4 final `lit_flush()` calls write each
stream's last bits, with `store_tail()` when fewer than 8 bytes are left before its end.

## The header: u16, byte 2 and the sizes {#ch-literals-enc--header}

@listing src/seqlz_compress.c 575-581

With the streams in place, the header is written over the first bytes of the old raw literals.

- `put_unaligned_le16((u16)(SEQLZ_LIT_CODED | n_literals), d)` sets the top bit of the u16 that
  counts the literals. `SEQLZ_LIT_CODED` is `0x8000`; a page has at most 16 384 literals, which need
  15 bits, so bit 15 is free ([chapter 6](#ch-header)). For 201 literals the u16 is `0x80c9`, the
  bytes `c9 80`.
- `d[2]` is the table in bits 0 to 2, `set`, and `width - 5` in bits 3 to 5, shifted by
  `SEQLZ_LIT_WIDTH_AT`, 3. `width` is 5 to 12, so `width - 5` is 0 to 7, 3 bits. Bits 6 and 7 stay 0;
  the decoder rejects a page where they are not ([chapter 13](#ch-literals-dec)). Table 3 with width 7
  would be `3 | 2 << 3 = 0x13`. The `static_assert`s in `seqlz_internal.h` check that table and
  width fit their 3 bits ([chapter 7](#ch-internal)).
- `lit_sizes(d + SEQLZ_LIT_HEADER(0), sizes, width)` writes the sizes from byte 3 on;
  `SEQLZ_LIT_HEADER(0)` is `3 + 0`.

### lit_sizes(): 8 numbers of w bits, lowest bit first

@listing src/seqlz_compress.c 492-509

The 8 sizes are one number of `8 * width` bits, little endian: size 0 in the lowest `width` bits,
size 1 in the next `width`, and so on (the format's `S` in
[format.md, "Layout of a compressed page"](https://github.com/martinus/quetschn/blob/main/docs/format.md#layout-of-a-compressed-page)).
`lit_sizes()` builds it in a small accumulator:

- `acc |= (u64)sizes[j] << cnt` puts the next size above the `cnt` bits that are still waiting.
- The inner `for` adds `width` to `cnt` and, while 8 or more bits are waiting, writes the low byte and
  shifts it out.

`cnt` stays below 8 after the inner loop, and below `8 + 12 = 20` after the add, so `acc` never holds
more than 20 bits; a u64 is more than enough, and the cast to u64 before the shift keeps the shift in
64 bits. After 8 sizes, `8 * width` bits have gone in, a whole number of bytes, so the last inner loop
leaves `cnt` at 0 and nothing behind: exactly `width` bytes, as the comment promises.

The example's sizes are 19, 17, 17, 17, 17, 17, 17 and 16 with `width = 5`:

```text
j=0  acc = 19                cnt  5
j=1  acc |= 17 << 5          cnt 10  -> write 0x33, cnt 2
j=2  acc |= 17 << 2          cnt  7
j=3  acc |= 17 << 7          cnt 12  -> write 0xc6, cnt 4
j=4  acc |= 17 << 4          cnt  9  -> write 0x18, cnt 1
j=5  acc |= 17 << 1          cnt  6
j=6  acc |= 17 << 6          cnt 11  -> write 0x63, cnt 3
j=7  acc |= 16 << 3          cnt  8  -> write 0x84, cnt 0
```

5 bytes, `33 c6 18 63 84`: in binary, little end first, `10011` (19), `10001` (17), and so on.

## The bitstream moves down {#ch-literals-enc--memmove}

@listing src/seqlz_compress.c 582-586

`coded += header` makes `coded` the offset where the coded literals end: header plus streams. The
bitstream, `bitstream` bytes at `d + SEQLZ_HEADER + n_literals`, moves down to there, and the new
length is `coded + bitstream`.

It is `memmove()`, not `memcpy()`, because source and destination can overlap: the bitstream moves
down by `2 + n - coded`, more than 38 bytes, and a bitstream is often longer than that. `memmove()` is
defined for overlapping ranges, `memcpy()` is not.

In the example, the bitstream of 6 bytes moves from byte 203 to byte 145, and the page goes from 209
to 151 bytes. One more copy of the bitstream could be saved: the encoder already copies it once, from
the back of `dst` to behind the raw literals ([chapter 10](#ch-encoder)). Copying it only once, to
behind the coded literals, was built and measured: the writes got up to 0.3 µs slower in the kernel
VM and 0.8 µs on the A55, `seqlz-fast` too, because gcc laid out the matcher's loop differently
([explored-designs.md, "The bitstream copied once, behind the literals raw or
coded"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-bitstream-copied-once-behind-the-literals-raw-or-coded-writes-up-to-03-µs-slower-in-the-vm-and-08-µs-on-the-a55-not-kept)).
The copy of 400 to 600 bytes on a third of the pages stayed.

## The example page, byte by byte {#ch-literals-enc--example}

The page of this chapter: 200 letters, each drawn from `etaoinshrdlu` and a space by the C library's
classic linear congruential generator, `seed = seed * 1103515245 + 12345` with `seed = 1`, taking
`(seed >> 16) % 13`; then 3896 zero bytes. The first 24 letters are `oh  asridistliuthtoo hha`. The
matcher finds 2 sequences: 201 literals followed by a match of 3895 bytes at offset 1, and the last
sequence with no literals. Compressed with `coded = false` the page has 209 bytes; with
`coded = true`, 151:

```text
000  c9 80 00 33 c6 18 63 84 43 56 4e e0 00 96 ca 9f
010  45 9b 28 68 cf a1 4f a6 cb f7 80 b2 65 be 53 18
020  e6 71 a6 90 8b 22 57 0e fa 9f 03 38 05 21 68 5b
030  87 53 8d 25 6e 21 4e 0a d0 b5 b4 16 40 05 a1 c3
040  34 74 34 e3 45 68 a7 4a 0c 0a 07 0c e0 00 2b 80
050  75 81 dc 42 0e 72 95 a5 38 e2 94 d3 43 65 f0 51
060  ac 98 5f b2 7b 2e 20 54 f8 8a 50 b7 1a 29 cc a0
070  4b f6 52 65 2c eb 69 f5 ac 05 62 82 be 76 9f 52
080  40 32 ca 63 24 4a 50 10 7c c0 90 6a ca 56 fb e9
090  b2 d6 dc 75 ef 14 8a
```

The offsets on the left are hexadecimal. Taken apart by hand:

- `c9 80` is the u16 `0x80c9`. The top bit is set, coded literals; `0x80c9 & 0x7fff = 0xc9 = 201`
  literals.
- `00` is byte 2: table `0x00 & 7 = 0`, width `5 + (0x00 >> 3 & 7) = 5`, and bits 6 and 7 are 0.
  Table 0 is the one that gives `e` and the space 4 bits, the table for text.
- `33 c6 18 63 84` are the 8 sizes. As one little-endian number, `0x846318c633`, cut into 5-bit
  fields from the bottom: 19, 17, 17, 17, 17, 17, 17, 16. Their sum is 137.
- The streams start at byte `3 + 5 = 8`: stream 0 at 8, stream 1 at 27, then 44, 61, 78, 95, 112 and
  129, and they end at `8 + 137 = 145`, `0x91`.
- Stream 0 begins `43 56 4e e0`, in bits `01000011 01010110 01001110 11100000`. Table 0's codes cut it
  into `01000` (`o`), `011010` (`d`), `1011001` (`h`), `00111` (`n`), `011100` (`l`): literals 0, 8, 16,
  24 and 32 of the page.
- Stream 0 ends with byte 26, `0x1a`, which is `80`. It has 147 bits, 18 whole bytes and 3 bits, so
  the last byte holds `100` and 5 zero bits.
- `d6 dc 75 ef 14 8a` from byte 145 on is the sequences' bitstream, the same 6 bytes that followed the
  raw literals in the 209-byte page.

Did it pass the rule? The pricing gave these bits per stream:

| table | stream 0 to 7 | total |
| --- | --- | --- |
| 0 | 147 135 130 129 132 132 130 127 | 1062 |
| 3 | 159 152 150 148 148 150 152 150 | 1209 |
| 1 | 213 216 212 210 214 218 221 215 | 1719 |
| others | | 1749 to 1954 |

Table 0 wins. Its sizes add up to `coded = 137`, and `137 + 51 = 188` is less than
`201 - 201 / 16 = 189`. One byte more and this page would have kept its raw literals. It saves 58
bytes, 28% of the page.

::: try Compress a page of your own
In a scratch directory, with the repository in `$R`:

```c
#include <stdio.h>
#include "seqlz_internal.h"

static struct seqlz_tables t;
static struct seqlz_state st;
static u8 page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE];

int main(void)
{
	unsigned int k, len, seed = 1;

	seqlz_tables_init(&t, &seqlz_default_own);
	for (k = 0; k < 200; k++) {
		seed = seed * 1103515245u + 12345u;
		page[k] = "etaoinshrdlu "[(seed >> 16) % 13];
	}
	len = seqlz_compress(&t, &st, page, dst, sizeof(dst), true);
	for (k = 0; k < len; k++)
		printf("%02x%c", dst[k], k % 16 == 15 ? '\n' : ' ');
	printf("\n%u bytes\n", len);
	return 0;
}
```

Build it with `gcc -O2 -I$R/src ex.c $R/src/seqlz_{codes,compress,decompress,default_tables,lit_sets}.c`.
With 190 letters instead of 200 the literals stay raw, with 195 they are coded.
:::

## What writing the literals costs {#ch-literals-enc--cost}

The pricing is cheap; writing the streams is not. On the Cortex-A55, `code_literals()` takes about
5400 cycles, 3 µs, per page in the hot loop of the userspace benchmark: `seqlz-fast-lit` compresses a
page in 47 640 cycles, `seqlz-fast` in 42 241. Most pages pay part of that even when they stay raw:
in that sample 14 582 of 19 387 pages kept their raw literals, about 404 each, and all of them were
priced in all 8 tables first. Pricing only every 8th literal first, to send hopeless pages back early,
made compression slower, because the literals of most raw pages come close to paying ([explored-designs.md,
item 8 of "Six
choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#8-coded-literals-on-the-a55-78-instructions-per-literal-at-one-per-cycle)).
On the phone this is where `seqlz-fast`, which never calls `code_literals()`, gains: in an earlier
measurement on the Mi 9T's little core, `seqlz-fast-lit` stored 55 bytes per page less than
`seqlz-fast` and took 4.2 µs more per page written ([explored-designs.md, "A second phone
dump"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-second-phone-dump-after-12-hours-of-use-seqlz-fast-reads-faster-than-lzo-rle-at-p99)).
Which of the two a phone should use depends on how it values a byte against a µs, and that is not
settled.
