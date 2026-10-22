# Coding the literals: one table of eight or the page's own, eight streams {#ch-literals-enc}

When `compress_page()` is done, `dst` holds a valid page with raw literals ([chapter 10](#ch-encoder)),
which is what level 1, `seqlz-1`, stores. From level 2 on, `seqlz_compress()` then calls
`code_literals()`: it prices the literals in all 8 literal tables, takes the cheapest, at level 3 also
builds a table of the page's own where that promises to save more, decides whether coding them saves
enough, and if so rewrites `dst` in place. Level 3 first stores the literals as deltas to earlier
bytes of the page where that looks cheaper, in `delta_literals()`, and `code_own()` codes the deltas. The
sequences' bitstream does not change; it only moves down behind the coded literals. This chapter is
lines 322 to 486 and 542 to 1157 of `src/seqlz_compress.c` and `code_own()`, in the order
`code_literals()` uses the helpers rather than file order. The format is in
[chapter 4](#ch-format--layout), the tables and `lit_cost` in [chapter 7](#ch-internal--lit-cost), the
decoder's side in [chapter 13](#ch-literals-dec).

## What code_literals() does to dst {#ch-literals-enc--overview}

@listing src/seqlz_compress.c 1038-1080

The coded literals go where the raw ones were, so the raw literals are read from a copy. The callers
bring it differently: `seqlz_compress()` at level 2 passes `raw = NULL` and `spare = st->literals`, the
per-CPU matcher's hash table, which is garbage once the matcher is done; `seqlz_encode()` has the
caller's literals and passes them as `raw`. At level 3 with deltas, `code_own()` passes as `raw` the
deltas that `delta_literals()` wrote into the matcher's state, and which delta in `delta`; those are
the literals that get priced and coded, so `literals` is `raw` where there is one, while `d` still
holds them as they are. So `d` needs no room beyond `len`. `n_literals` is the
plain count, as the page has raw literals here, and `bits = ~0U` makes the first table's sum smaller.
`own` is `NULL` below level 3, and becomes `NULL` for a page with fewer than
{{own.enc.min_lits}} literals or more than `OWN_LITERALS_MAX`, {{own.enc.max_lits}} on 4 KiB pages, so
on all of them; from then on `own` alone says whether to try a table of the page's own
([below](#ch-literals-enc--own)). For one, `lit_cost()` also counts how often each byte occurs, into
`own->freq`, which has to start at 0.

The page this chapter takes apart at the end: {{ch11.letters}} letters drawn at random from
`etaoinshrdlu` and a space, then {{ch11.zeros}} zero bytes. The matcher finds {{ch11.n}} literals,
the letters and the first zero, and one match that copies the zero byte to the end of the page.
@assert ch11.n == ch11.letters + 1 and ch11.off == 1

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 280" font-size="12" role="img" aria-label="dst before and after code_literals(): the raw literals are copied into the matcher's table, the coded streams are written over their old place, and the bitstream moves down behind them">
  <text x="20" y="22">dst after compress_page(): {{letters.len}} bytes, raw literals</text>
  <rect x="20" y="30" width="60" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="50" y="50" text-anchor="middle">{{ch11.raw.u16}}</text>
  <rect x="80" y="30" width="440" height="30" fill="none" stroke="currentColor"/>
  <text x="300" y="50" text-anchor="middle">{{ch11.n}} raw literals</text>
  <g class="acc">
    <rect x="520" y="30" width="80" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.25"/>
    <text x="560" y="50" text-anchor="middle">bitstream</text>
  </g>
  <text x="20" y="74" text-anchor="middle">0</text>
  <text x="80" y="74" text-anchor="middle">2</text>
  <text x="520" y="74" text-anchor="middle">{{ch11.raw.bs}}</text>
  <text x="600" y="74" text-anchor="middle">{{letters.len}}</text>

  <line x1="300" y1="78" x2="300" y2="106" stroke="currentColor"/>
  <path d="M296 100 L300 108 L304 100 Z" fill="currentColor"/>
  <text x="308" y="96">memcpy()</text>
  <text x="20" y="104">spare = st-&gt;literals</text>
  <rect x="170" y="110" width="270" height="30" class="fill-sheet" stroke="currentColor" stroke-dasharray="4 3"/>
  <text x="305" y="130" text-anchor="middle">a copy of the {{ch11.n}} raw literals</text>

  <line x1="300" y1="140" x2="300" y2="196" stroke="currentColor"/>
  <path d="M296 190 L300 198 L304 190 Z" fill="currentColor"/>
  <text x="308" y="172">lit_streams()</text>

  <g class="acc">
    <line x1="560" y1="60" x2="490" y2="196" stroke="currentColor"/>
    <path d="M484 190 L489 199 L492 189 Z" fill="currentColor"/>
    <text x="536" y="130">memmove()</text>
  </g>

  <text x="20" y="194">dst after code_literals(): {{letters.coded.len}} bytes</text>
  <rect x="20" y="200" width="60" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="50" y="220" text-anchor="middle">{{ch11.u16}}</text>
  <rect x="80" y="200" width="30" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="95" y="220" text-anchor="middle">{{ch11.byte2}}</text>
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
  <text x="170" y="244" text-anchor="middle">{{ch11.q.0}}</text>
  <text x="205" y="244" text-anchor="middle">{{ch11.q.1}}</text>
  <text x="240" y="244" text-anchor="middle">{{ch11.q.2}}</text>
  <text x="275" y="244" text-anchor="middle">{{ch11.q.3}}</text>
  <text x="310" y="244" text-anchor="middle">{{ch11.q.4}}</text>
  <text x="345" y="244" text-anchor="middle">{{ch11.q.5}}</text>
  <text x="380" y="244" text-anchor="middle">{{ch11.q.6}}</text>
  <text x="415" y="244" text-anchor="middle">{{ch11.q.7}}</text>
  <text x="450" y="244" text-anchor="middle">{{ch11.q.8}}</text>
  <text x="530" y="244" text-anchor="middle">{{letters.coded.len}}</text>
  <text x="170" y="262" text-anchor="middle">q[0]</text>
  <text x="450" y="262" text-anchor="middle">q[8]</text>
</svg>
</div>
<figcaption>One page in dst before and after code_literals(), not to scale: the raw literals are copied out, the 8 streams are written over their old place, and the bitstream moves down.</figcaption>
</figure>

The offsets under the second row are `q[0]` to `q[8]` for this page, from `d`. Bytes
{{letters.coded.len}} to {{ch11.raw.last}} keep what they held, but are no longer part of the page.

## Pricing all 8 tables with one add per literal {#ch-literals-enc--pricing}

For every stream and every literal table, `lit_cost()` sums how many bits the stream's literals take.
The obvious loops are 8 loads and 8 adds per literal; the first version in the project, a histogram
followed by 8 times 256 products, cost 1400 ns on pages with few literals
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
`t->lit_cost[b]` has byte `b`'s code length in table $j$ in byte lane $j$, so one 64-bit add sums all 8
tables, SWAR ([chapter 3](#ch-cpu--swar)); the kernel may not use the vector registers here.
`LIT_COST_WORDS` is 1 with 8 tables, so read `[w]` as `[0]`.

@listing src/seqlz_compress.c 606-652

`hist` is `NULL` at level 2, and the two `if (hist)` are then the same branch for the whole page,
which the CPU predicts; at level 3 the same pass counts the bytes for the page's own table. Literal
`k + j` goes to `hist[j & 1]`, and with `k` a multiple of 8, `j & 1` is the literal's own parity: even
literals into one histogram, odd ones into the other. Text repeats bytes, `ee` or a run of spaces,
and two increments of the same counter in a row are a chain: the second load waits for the first
store. With two histograms, two neighbours never share one; `own_code()` adds them up later. The
counts are `u16`: a page has at most as many literals as bytes, 16 384 on 16 KiB pages. Counting in
this pass, instead of one of its own, made the own table cost writes 20.2% instead of 23.9% in
userspace on the Mi 9T's Cortex-A76, on 15th October 2026
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-a-lazy-matcher-and-a-literal-table-of-the-pages-own-zstd-3s-memory-in-less-than-half-its-time-on-the-phone-kept)).

The adds go into the 8-bit lanes of `x`, 25 literals per stream, `LIT_COST_RUN`: 25 codes of at most
10 bits sum to at most 250, a 26th could overflow into the next lane. Then each stream's 8 byte lanes
are widened into the 16-bit lanes of `even`, tables 0, 2, 4 and 6, and `odd`, tables 1, 3, 5 and 7:
`x & lanes` keeps bytes 0, 2, 4 and 6 in place, `x >> 8 & lanes` moves the other four down by a byte
first. The chunks are 200 literals, a multiple of 8, so `k` is a multiple of 8 in the first inner
loop and literal `k + j` is stream `j`; only the last chunk takes a rest one literal at a time. A
16-bit lane holds the {{ch11.stream_bits}} bits of a stream at most, {{k16.ch11.stream_bits}} on 16 KiB
pages.
@assert F("k16.ch11.stream_bits") <= 65535

The 4 literals `65 20 65 00`, `e`, a space, `e` and a zero byte, have these `lit_cost` entries, table
7 in the top byte:

```text
{{ch11.cost.65}}
{{ch11.cost.20}}
{{ch11.cost.00}}
```

As stream 0, `x[0]` after the 4 adds, and the split:

```text
x                = {{ch11.x}}
x & lanes        = {{ch11.even}}    even: tables 6, 4, 2, 0 = {{ch11.sum.6.dec}}, {{ch11.sum.4.dec}}, {{ch11.sum.2.dec}}, {{ch11.sum.0.dec}}
x >> 8 & lanes   = {{ch11.odd}}    odd:  tables 7, 5, 3, 1 = {{ch11.sum.7.dec}}, {{ch11.sum.5.dec}}, {{ch11.sum.3.dec}}, {{ch11.sum.1.dec}}
```

4 adds instead of 32. The same on stream 0 of [chapter 16](#ch-trace)'s page, {{ch11.slab.s0}}
literals in chunks of 25:

@anim pricing The sums of stream 0 of chapter 16's page in x[0], one add per literal, and their move into the 16-bit lanes of even[0] and odd[0] after each chunk; or, as a choice, the same adds in 8-bit lanes only. Code lengths, literals and the sums of all 8 streams come from the real tables and lit_cost().

One sum per page would do to choose the table. The 8 sums per table are for after the choice: each
stream is rounded up to whole bytes on its own, and `code_literals()` needs all 8 sizes before it
writes the first stream, since stream 1 starts where stream 0 ends. So the sizes fall out of the
pricing, and no second pass over the literals is needed.

::: measured What the pricing costs
In the kernel, a build that prices the literals and then stores the page with raw literals wrote a
page in 6920 ns at p50 and 10 529 ns at p99, against 6890 and 10 490 ns for `seqlz-1`, which does
not price: 30 to 40 ns per write, about 0.5%, September 2026. In userspace loops over 2000 pages the
pricing was at most 400 cycles, 1.5% of a write. Choosing the table from every 4th literal would save
little, as the sizes still need a pass over all literals. Writing the streams is what costs, about
0.6 ns per coded literal in the kernel
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
:::

## Reading one sum out of its lane: lit_bits() {#ch-literals-enc--lit-bits}

@listing src/seqlz_compress.c 596-604

Odd tables are in `odd`, and `k % 8 / 2` is the 16-bit lane: tables 0 and 1 in lane 0, 2 and 3 in lane
1, and so on. For `k = 5`, `odd` shifted right by 32 is `{{ch11.odd32}}`, and `& 0xffff` gives
`0x{{ch11.sum.5.hex}} = {{ch11.sum.5.dec}}`, table 5's sum in the example above.

## The table with the fewest bits, and the 8 sizes {#ch-literals-enc--choice}

@listing src/seqlz_compress.c 1081-1095

A tie keeps the lower table number, which matters only to the encoder; the decoder takes whatever
byte 2 names. The choice compares bits, not bytes, so a table with fewer bits can take a few bytes
more after the 8 sizes are rounded up; the code does not look for that case.

## The width of a size, from one count of leading zeros {#ch-literals-enc--width}

@listing src/seqlz_compress.c 1096-1099

With deltas, the byte that says which comes before the sizes, and `header` counts it:
`(unsigned int)(delta != 0)` is 1 or 0 without a branch.

@listing src/seqlz_compress.c 1025-1036

`size_width()` is called twice at level 3, for the fixed table's sizes and for the page's own; it
comes from the file's part before `code_literals()`. `all`, the OR of the sizes, has the highest set bit
of the largest one, which is all the width needs, without a compare per stream. Setting bit 4 makes
the width at least 5, `SEQLZ_SIZE_BITS_MIN`, without a compare, and keeps `all` from being 0, for
which `__builtin_clz()` is undefined. `32 - clz` is then the bit width:

| largest size | `all`, the sizes ORed, with bit 4 set | `clz` | `width` | header bytes |
| --- | --- | --- | --- | --- |
| {{ch11.s0.bytes}} bytes, the example page | `{{ch11.max.bin}}` = {{ch11.max.all}} | {{ch11.max.clz}} | {{ch11.width}} | {{ch11.header}} |
| {{ch11.lit_stream_max}} bytes, the largest a 4 KiB page can have | `0b1010010000` | 22 | 10 | 13 |
@assert ch11.lit_stream_max == 640 and ch11.max.all >= ch11.s0.bytes and 0b1010010000 == 640 | 16

Until 5th October 2026 every size had 2 bytes, a header of 19 bytes ([chapter 4](#ch-format--layout)).
On a desktop's zram pages the largest stream of a page with coded literals was below 128 bytes on 77%
of the pages, the mean of two dumps, so the header is now mostly 9 to 11 bytes
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#stream-sizes-in-as-many-bits-as-the-largest-needs-kept)).

## Level 3: a table of the page's own {#ch-literals-enc--own}

`enc` holds the fixed table's codes so far. At level 3 a table of the page's own may take its place:
Huffman-like code lengths for exactly the bytes this page has, sent in front of the streams
([chapter 4](#ch-format--own)). This section follows `code_literals()` through it on the text page of
chapter 4, {{own.enc.n_lit}} literals of 12 letters and the space, whose best fixed table is table
{{own.enc.set}}, at {{own.enc.set_bits_per_lit}} bits per literal.
@assert own.enc.set == ch04.text.set and own.enc.n_lit == ch04.text.n

### Which pages get one

@listing src/seqlz_compress.c 1100-1120

`own_code()` gets `header + coded`, what the fixed table costs, as `limit`, and returns the bytes of
the table's description, or 0 where it does not pay. Two checks decide, the first one cheap and at
the end of `own_code()`. Its `bits` is the sum of the code lengths over all literals, counted on the
way; divided by 8 it is at most the sum of the 8 streams' bytes, each rounded up, and the smallest
header has 8 bytes. Only a page that passes with these lower bounds pays for `own_sizes()`, one more
pass over the literals, and the exact check here. Both want `OWN_SAVES_MIN` bytes less than the fixed
table:

| | own table | fixed table {{own.enc.set}} |
| --- | --- | --- |
| first check | $8 + {{own.enc.d}} + {{own.enc.bits_bytes}} + 16 = {{own.enc.first_lhs}}$ | $\le {{own.enc.limit}}$ |
| second check | ${{own.enc.own_header}} + {{own.enc.d}} + {{own.enc.own_coded}} + 16 = {{own.enc.second_lhs}}$ | $\le {{own.enc.limit}}$ |
@assert own.enc.first_lhs <= own.enc.second_lhs <= own.enc.limit and own.enc.saves_min == 16

So the page takes its own table: header, description and streams are {{own.enc.total}} bytes instead of
{{own.enc.fixed_total}}. `desc`, `width`, `header`, `coded`, `sizes` and `enc` change over to it, and the
rest of `code_literals()` writes the page as for a fixed table, with the description in the header,
behind the sizes, which come behind the delta's byte if there is one. With deltas, `header` already counts that byte, so the right
side of the second check takes it off again, `- (delta ? 1U : 0U)`: both tables would need it, and the
page's own table has to save the same 16 bytes as without deltas.

### The work memory

@listing src/seqlz_compress.c 741-776

A table of the page's own needs counts, lengths, codes and room for the description,
{{own.enc.work_size|grp}} bytes in `struct own_work`. The compressor's work memory is the 8 KiB of the
matcher's table, and nothing else is free per CPU, so `own_work()` puts the struct at byte
{{own.enc.work_at}} of it ([chapter 10](#ch-encoder--entry)); below that byte goes the copy of the raw
literals. A page with more literals than that would overwrite it, so `OWN_LITERALS_MAX` is the limit,
4096 on 4 KiB pages, where no page has more. On 16 KiB pages it is 12 288 of 16 384. Pages with fewer
than 512 literals, `OWN_LITERALS_MIN`, gained 1.15 bytes per page together on one phone dump, counted
offline, and in the kernel VM 512 was on the hull, where a table from 2048 literals on was too. With
it, about a fifth of the pages got a table of their own, and its description took about 0.45 bytes per
byte with a code ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-a-lazy-matcher-and-a-literal-table-of-the-pages-own-zstd-3s-memory-in-less-than-half-its-time-on-the-phone-kept)).
The code gives no reason for the 16 bytes of `OWN_SAVES_MIN`. The description has room for 512 bytes, a power of 2 so that `own_put()` can
mask its index, and 8 more for its last 8-byte store.
@assert own.enc.work == 4096 and own.enc.work_size <= own.enc.work and own.enc.max_lits == 4096 and own.enc.min_lits == 512 and own.enc.desc_room == 512

### own_code(): which bytes, and what the entropy promises

@listing src/seqlz_compress.c 878-910

`w->freq[0]` and `w->freq[1]` are the even and the odd literals' counts, and this loop adds them, 4
bytes at a time: `own_lanes()` loads 4 counts as the 16-bit lanes of a `u64`, one add sums 4 pairs,
and the sums go back into `freq[0]`. In the same step it marks which of the 4 bytes occur, a bit per
byte value in `present[4]`, 256 bits, without a branch:

@listing src/seqlz_compress.c 856-860

The comment's trick, for the bytes `0x60` to `0x63`, the backtick and `a` to `c`, of the text page,
where only `a` occurs:

```text
c                    = {{own.enc.pr.c}}   the 4 sums, a in lane 1
(c & low) + low      = {{own.enc.pr.plus}}   bit 15 of a lane set where its low 15 bits are not 0
(c | ...) & ~low >> 15 = {{own.enc.pr.tops}}   one bit per lane that is not 0, at 0, 16, 32 or 48
* 0x0001000200040008 = {{own.enc.pr.mul}}
>> 48 & 15           = {{own.enc.pr.nibble}}               bit 1: byte 0x61, a
```

The multiplier is $2^{48} + 2^{33} + 2^{18} + 2^3$, and it moves the bit of lane $j$, at $16j$, to
$48 + j$: the 4 products that land in bits 48 to 51 are the 4 lane bits, and the other 12 products
land at other bits, never two on one, so no carry reaches bits 48 to 51. The `c |` covers a lane whose
bit 15 is set by the count itself, which a page of at most 16 384 literals never has.

@listing src/seqlz_compress.c 911-927

The bytes that occur, in order of their value: `__builtin_ctzll()` is the lowest set bit of `m`, and
`m &= m - 1` clears it. On the way the loop sums $f \log_2 f$ over the bytes, in 1/256 bits, with
`log2_256()`:

@listing src/seqlz_compress.c 842-854

For $f \ge 1$, `e` is the position of the top bit, the integer part of $\log_2 f$. `f << (31 - e)` moves
the top bit to bit 31, and the 5 bits below it, `>> 26 & 31`, say where $f / 2^e$ lies between 1 and
2, in 32 steps; `log2_frac[]` has $\log_2$ of the middle of each step, in 1/256. For the space's
{{own.enc.b.20.freq}}: `e = {{own.enc.log.e}}`, `f << 22 = {{own.enc.log.shifted}}`, the 5 bits are
{{own.enc.log.idx}}, `log2_frac[{{own.enc.log.idx}}] = {{own.enc.log.frac}}`, so `log2_256()` gives
${{own.enc.log.e}} \cdot 256 + {{own.enc.log.frac}} = {{own.enc.b.20.log}}$, where $256 \log_2 608$ is
{{own.enc.b.20.log_exact}}: a table of 32 bytes and no division, about 1/50 bit off.
@assert own.enc.b["20"].freq == 608 and own.enc.log.e == 9

@listing src/seqlz_compress.c 928-940

With $n$ literals and $f_b$ of byte $b$, $n \log_2 n - \sum_b f_b \log_2 f_b$ is $n$ times the
entropy, the bits per literal that no prefix code can beat ([chapter 2](#ch-compression--entropy)).
`h / 2048` turns 1/256 bits into bytes, and `n * 115 / 256` adds 0.45 bytes of description for each
byte with a code. If that estimate, with the smallest header and `OWN_SAVES_MIN`, is not below
`limit`, or the page would stay raw anyway ([below](#ch-literals-enc--rule)), no code is built: on
many pages the fixed table is close enough, and this is where they leave. On the text page, `h` is
{{own.enc.h}}, {{own.enc.h_bits}} bits per literal, {{own.enc.h_bytes}} bytes, plus {{own.enc.desc_est}}
for {{own.enc.n}} bytes with a code: `est` = {{own.enc.est}}, and
${{own.enc.est}} + 8 + 16 = {{own.enc.est_lhs}}$ is far below `limit` = {{own.enc.limit}}.
@assert own.enc.est_lhs < own.enc.limit and own.enc.n == 13

### own_lengths(): close to Huffman, without sorting

A Huffman code gets the code lengths right, but it needs the bytes sorted by count, and a tree. The
comment's way is cheaper: start with lengths that are about right, then repair them in an order that
a counting sort gives.

@listing src/seqlz_compress.c 778-816

A byte that is a fraction $p = f / n$ of the literals would ideally get $-\log_2 p$ bits. The first
loop gives it the shortest `l` with $f \cdot 2^l \ge n$, which is $\lceil -\log_2 p \rceil$: the count
of leading zeros gives `l` within one, `(f << l) < n_literals` corrects it, `l += !l` would keep a
byte that is all the literals from 0 bits, which `own_code()`'s test for 2 bytes or more rules out
before, and lengths above 10 bits are cut to 10. Since each
$2^{-l} \le p$, the lengths sum to at most 1, and only the cut can overfill the code.

$f \cdot 2^l / n$ is then between 1 and 2, and it is what the code calls the gain: shortening the byte
to `l - 1` bits saves `f` bits and takes $2^{-l}$ more of the code space, so $f \cdot 2^l$ is the bits
saved per code space. `inv` turns the division into a multiply, computed once with `div_u64()`
([chapter 5](#ch-compat--min)), and `q` is 16 times the ratio, 16 to 31,
so `q - 16` is a bucket from 0 to 15. Bytes cut to 10 bits get bucket 0. Then a counting sort, `at[]`,
lists the bytes by bucket in `by_gain[]`, within a bucket by byte value. For the text page:

| byte | count | `l` | $f \cdot 2^l$ | $f \cdot 2^l / n$ | bucket | final length |
| --- | --- | --- | --- | --- | --- | --- |
| space | {{own.enc.b.20.freq}} | {{own.enc.b.20.l0}} | {{own.enc.b.20.f2l}} | {{own.enc.b.20.ratio}} | {{own.enc.b.20.gain}} | {{own.enc.b.20.len}} |
| `l` | {{own.enc.b.6c.freq}} | {{own.enc.b.6c.l0}} | {{own.enc.b.6c.f2l}} | {{own.enc.b.6c.ratio}} | {{own.enc.b.6c.gain}} | {{own.enc.b.6c.len}} |
| `n` | {{own.enc.b.6e.freq}} | {{own.enc.b.6e.l0}} | {{own.enc.b.6e.f2l}} | {{own.enc.b.6e.ratio}} | {{own.enc.b.6e.gain}} | {{own.enc.b.6e.len}} |
| `u` | {{own.enc.b.75.freq}} | {{own.enc.b.75.l0}} | {{own.enc.b.75.f2l}} | {{own.enc.b.75.ratio}} | {{own.enc.b.75.gain}} | {{own.enc.b.75.len}} |
| `e` | {{own.enc.b.65.freq}} | {{own.enc.b.65.l0}} | {{own.enc.b.65.f2l}} | {{own.enc.b.65.ratio}} | {{own.enc.b.65.gain}} | {{own.enc.b.65.len}} |
| `a` | {{own.enc.b.61.freq}} | {{own.enc.b.61.l0}} | {{own.enc.b.61.f2l}} | {{own.enc.b.61.ratio}} | {{own.enc.b.61.gain}} | {{own.enc.b.61.len}} |
| `d` | {{own.enc.b.64.freq}} | {{own.enc.b.64.l0}} | {{own.enc.b.64.f2l}} | {{own.enc.b.64.ratio}} | {{own.enc.b.64.gain}} | {{own.enc.b.64.len}} |
@assert own.enc.b["20"].l0 == 3 and own.enc.b["6c"].l0 == 4 and own.enc.b["6c"].len == 3 and own.enc.b["75"].len == 3 and own.enc.b["6e"].len == 4 and own.enc.b["6e"].gain == own.enc.b["75"].gain and own.enc.changed == 2

For the space, `clz({{own.enc.b.20.freq}}) - clz({{own.enc.n_lit}})` is ${{own.enc.clz_space}} - {{own.enc.clz_n}} = 2$,
and ${{own.enc.b.20.freq}} \cdot 4 < {{own.enc.n_lit}}$ makes it {{own.enc.b.20.l0}}. The 13 first lengths, one of 3 bits and 12 of 4, fill {{own.enc.space0}} of the 1024
units of a 10-bit code space: {{own.enc.room0}} are left.

@listing src/seqlz_compress.c 817-840

An overfull code gets longer codes from bucket 0 up: lengthening a byte frees the least room where it
costs the fewest bits per room. Room left is handed out from the top bucket down, while a byte's
shorter code still fits. On the text page the code is not overfull, and the second loop runs once
through `by_gain[]` from its end: `l`, the last byte of bucket 3, gets 3 bits and takes 64 of the 128
units, the space has 3 already and would need 128 for 2, and `u`, the last byte of bucket 2, takes the
other 64. That fills the code. `n` occurs {{own.enc.nu_diff}} times more than `u` and stays at 4 bits:
both are in bucket 2, and within a bucket the loop goes by byte value, from the top. A Huffman code
would not leave those {{own.enc.nu_diff}} bits; the buckets save the sort. The comment above the
second loop is about a byte that just got shorter and is then among the longest: the room left is
always a multiple of the longest code's, so another round of the `while` fills it.

### Canonical codes, and the description

@listing src/seqlz_compress.c 942-953

The [canonical codes](#ch-compression--canonical-codes), as `first_codes()` makes them for the fixed
tables ([chapter 8](#ch-codes--first-codes)), and in the same loop the bits all literals will take,
`bits`, {{own.enc.bits}} on the text page, {{own.enc.bits_per_lit}} bits per literal: close to the
entropy's {{own.enc.h_bits}}. `enc[]` has the layout of `struct lit_table`'s, so `lit_streams()` takes
either. The `& 15U` and `& 255U` masks change no valid value; they keep every index inside its array
whatever the value, at the cost of an `and`.

@listing src/seqlz_compress.c 955-983

The description goes through the bytes with a code in order of their value, and writes the run of
bytes without a code before each one, then its length, as symbols of `OWN` from `t->own.enc`. `prev`
starts at `~0U`, so for the first byte `b - prev - 1` wraps to `b`, the run from byte 0. A run of
`run` is `nb = 32 - clz(run)` bits wide, symbol `SEQLZ_OWN_RUN + nb`, and its code goes out together
with the `nb - 1` bits below the run's top bit, one put: for the run of 32 before the space, `nb = 6`,
the code `{{own.code.15}}` and `00000`. At the end the bits left in `acc` go out as whole bytes, the
last one filled with zeros. On the text page that is {{own.enc.d}} bytes, the ones
[chapter 4](#ch-format--own) reads symbol by symbol. The last lines are the first check of
`code_literals()` above, with these bytes and `bits`: the text page passes it with
{{own.enc.first_lhs}} bytes against {{own.enc.limit}}.
@assert own.enc.d == own.text.dbytes

@listing src/seqlz_compress.c 862-876

`own_put()` collects bits in a `u64` as `enc_put()` does for the sequences
([chapter 10](#ch-encoder--put)), but forwards. Once 32 bits or more are waiting, it stores 8 bytes
big-endian, the waiting bits on top, and moves on by 4 bytes: the first 4 bytes are final, the other 4
hold the rest, which the next store writes again with more bits behind them. A put adds at most 16
bits to fewer than 32, so 48 bits fit. `& 63` is a no-op for `cnt` from 32 on, and `& (OWN_DESC_ROOM -
1)` keeps the store inside `desc[]` for any input, which the 8 bytes behind `OWN_DESC_ROOM` make room
for.

### own_sizes(): eight sums in eight registers

@listing src/seqlz_compress.c 985-1023

The bytes of each stream, as `lit_cost()` gives them for the fixed tables, but from `w->len`, one
load per literal. Eight named sums, not `sizes[k % 8]`: each is a chain of adds of its own, the 8 run
side by side, and the compiler keeps them in registers, where an array indexed in a loop could go
through memory. It runs only on pages that passed the first check above. On the text page the 8
streams take {{own.enc.own_coded}} bytes with a width of {{own.enc.own_width}} bits, against
{{own.enc.set_coded}} with table {{own.enc.set}}.

## Level 3: the literals as deltas, priced like the tables {#ch-literals-enc--deltas}

The format lets a page store each literal as its delta to an earlier byte, the literal minus that
byte modulo 256, one of three ways for the whole page ([chapter 6](#ch-header--literals)). Which way
codes a page's literals in the fewest bits is a question of the same kind as which fixed table does,
and `delta_literals()` answers it the same way, with the sums of `lit_cost` of
[above](#ch-literals-enc--pricing): four sums, the literals as they are and with each delta, in all 8
fixed tables at once. It runs once the page is matched, on the records that `encode_seq()` wrote in
front of each sequence's literals ([chapter 10](#ch-encoder--records)), and before `code_literals()`.
The page of pointers of [chapter 9](#ch-matcher--back) is the example: its {{delta.ptr.n_lit}}
literals at level 3 are mostly the low byte of a pointer right after a match at offset 8.

@listing src/seqlz_compress.c 322-329

Not every literal is priced, only every {{delta.step}}th, which is enough to tell the choices apart. The
comment has the two measured neighbours, both measured with the XOR that came before the deltas, with
the same pricing: every literal made pages almost no smaller and the encoder slower; every 4th made
pages 1.0 byte larger than every 5th on ten samples of 20 000 pages, because in fields of 4 or 8 bytes
every 4th literal of a sequence is the same byte of each field, so the sample saw the same bytes of
every field and missed the others
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-xors-its-literals-with-an-earlier-byte-of-the-page-38-fewer-bytes-than-seqlz-3-of-15-october-kept),
21st October 2026). The `static_assert` is for the code below, which adds one word of `lit_cost` per
literal and would miss tables if there were more than 8.

@listing src/seqlz_compress.c 331-357

These are `lit_cost()`'s two steps for one sum instead of one per stream: `delta_spill()` moves 8 lanes
of 8 bits into 4 lanes of 16 bits in `even` and 4 in `odd`, with the mask `0x00ff00ff00ff00ff`, and
`delta_bits()` reads table `k`'s lane back out, as `lit_bits()` does ([below](#ch-literals-enc--lit-bits)).
`s`, `even` and `odd` have 4 elements, one per choice, indexed by the delta's value: 0 is none,
`SEQLZ_DELTA_4`, `SEQLZ_DELTA_8` and `SEQLZ_DELTA_LAST` are 1 to 3.

@listing src/seqlz_compress.c 370-387

`delta_literals()` does two things: it prices, and it moves the literals down over the records, which
the page needs whatever it picks. `out` is where the next literal goes in `dst`, `to` where the next
delta goes in `deltas`, the matcher's state. The comment's last sentence is an assumption the code
makes: the delta is chosen by the fixed tables, also for a page that then gets a table of its own.
`sub_bytes()`, which comes before it in the file, is explained where it is used,
[further down](#ch-literals-enc--sub-bytes).

@listing src/seqlz_compress.c 388-417

The loop walks the records: `p` is a record, `p + LIT_REC` its literals, and the next record follows
them. For each priced literal at position `at` it loads the byte, and the byte each delta would be
to: 4 back from the 5th literal of the sequence on, 8 back from the 9th, `last` back from position
`last` on, else 0, which leaves the byte as it is. These are the rules of the format, so the price is
of the literals the decoder would get. `b - b4` is an `unsigned int` and wraps to near $2^{32}$ when
`b4` is the larger byte; the cast to `u8` keeps its low 8 bits, which is the difference modulo 256,
e.g. `0x00 - 0x56` is `0xaa`, and keeps the index inside the 256 entries of `lit_cost`. The price is as cheap as the XOR's that came before: a subtraction where there was a XOR. Then 4 adds, one per choice,
each an add of all 8 tables in one `u64`. `n` counts the literals in the 8-bit lanes: after
`LIT_COST_RUN`, 25, of at most 10 bits a lane could overflow, so they move to the 16-bit lanes first.
`j` carries the step over to the next sequence, so the priced literals are every {{delta.step}}th of
the page's literals, not of each sequence, where a sequence of 1 literal would be priced every time.
The page of pointers has {{delta.ptr.sampled}} priced literals of {{delta.ptr.n_lit}}.
@assert delta.ptr.sampled == (delta.ptr.n_lit + delta.step - 1) // delta.step

@listing src/seqlz_compress.c 418-428

The cheapest of 4 times 8 sums wins. A delta costs its byte in the header, 8 bits, and the sums count
every {{delta.step}}th literal only, so the byte counts as $8 / 5$, {{delta.penalty}} in integer division.
A tie keeps the lower number, so no deltas win over deltas that save nothing. For the page of
pointers, in bits of the priced literals, each with its best fixed table:

| choice | none | `SEQLZ_DELTA_4` | `SEQLZ_DELTA_8` | `SEQLZ_DELTA_LAST` |
| --- | --- | --- | --- | --- |
| bits, with {{delta.penalty}} for the delta's byte | {{delta.ptr.price.0}} | {{delta.ptr.price.1}} | {{delta.ptr.price.2}} | {{delta.ptr.price.3}} |
@assert delta.ptr.pick == 3 and delta.ptr.price[3] < min(delta.ptr.price[0], delta.ptr.price[1], delta.ptr.price[2])

`SEQLZ_DELTA_LAST` wins. The fixed tables underrate it: the deltas are
{{delta.ptr.distinct.deltas}} values, {{delta.ptr.h.10}} of them `0x10`, which no fixed table codes in
few bits, and the page's own table then takes the page from {{delta.ptr.l3plain.len}} bytes without
deltas to {{delta.ptr.l3.len}} with them ([chapter 6](#ch-header--literals)).

@listing src/seqlz_compress.c 429-453

The second pass goes over the records again. The comment is the argument for the word copies below,
which write up to 7 bytes past a sequence's last literal: those bytes land where the next sequence's
literals go next, or on the first 2 bytes of the next record, as every record so far, at least one,
has moved the literals {{delta.rec}} bytes down. So the loop reads the next record, `pos`, `ll` and
`last`, before it copies, while `in`, `pin`, `n_lit`, `at` and `dist` keep the current one. Past the
last sequence the bytes written are below the old end of the literals plus 2, inside the room the
encoder's check left before `p` ([chapter 10](#ch-encoder--emit)).

@listing src/seqlz_compress.c 454-459

The copy moves each sequence's literals down by the {{delta.rec}} bytes of every record so far, 8 bytes
at a time with no tail loop: up to 7 bytes past the last literal are written, and the next sequence's
literals go over them. As `out` is at least {{delta.rec}} bytes below `in`, each load reads its 8 bytes
before the store can change them, and the next load starts above what the store wrote, so a forward
copy of words is safe where it overlaps; `memcpy()` is not, and was a call per sequence, as the
comment above says. Without deltas the work ends here.

### sub_bytes(): 8 subtractions in one word {#ch-literals-enc--sub-bytes}

The deltas are written 8 at a time, and a plain subtraction of two `u64` is not 8 subtractions of
bytes: where a byte of the first word is smaller than the byte of the second, the subtraction borrows
1 from the byte above, and that byte comes out 1 too small. The format wants each byte on its own,
the same as the byte loop writes. `sub_bytes()` is the helper for that, back in file order before
`delta_literals()`:

@listing src/seqlz_compress.c 359-368

`a | high` sets the top bit of every byte of `a`, and `b & ~high` clears it in every byte of `b`. Now
each byte of the first word is at least `0x80` and each byte of the second at most `0x7f`, so no byte
can borrow from the one above, and the low 7 bits of each byte of the difference are those of the
byte's own difference: a borrow out of bit 6 only ever reaches bit 7 of the same byte. Bit 7 is wrong,
though. In a subtraction bit 7 of the result is bit 7 of `a`, XOR bit 7 of `b`, XOR the borrow out of
bit 6; the code used 1 and 0 in place of `a`'s and `b`'s bit, so it has 1 XOR the borrow. XOR with 1,
`a`'s bit and `b`'s bit fixes that, and `(a ^ ~b) & high` is exactly those three in bit 7 of each
byte: `a ^ ~b` is `a ^ b ^ 1` bit by bit. Two words whose low byte borrows, the bytes `00 01` minus
`10 00`:

```text
a                     {{delta.sub.a}}
b                     {{delta.sub.b}}
a | high              {{delta.sub.a_hi}}
b & ~high             {{delta.sub.b_lo}}
difference            {{delta.sub.diff}}
(a ^ ~b) & high       {{delta.sub.fix}}
sub_bytes(a, b)       {{delta.sub.r}}      bytes f0 01: 0x00 - 0x10 = 0xf0, 0x01 - 0x00 = 0x01
a - b                 {{delta.sub.plain}}      bytes f0 00: the borrow took byte 1's 1
```

@assert delta.sub.r == "0x00000000000001f0" and delta.sub.plain == "0x00000000000000f0"

Compiled for arm64 by clang with `-O2`, `sub_bytes()` is 7 instructions where the XOR was 1, the
masks as immediates of `orr`, `and` and `eor`. The decoder's `add4()` and `add8()` are the same trick
for an addition ([chapter 13](#ch-literals-dec--undelta)).

@listing src/seqlz_compress.c 460-486

`x` is the distance of the delta, `j` the number of literals at the start of the sequence that stay as
they are: 4 or 8 for `SEQLZ_DELTA_4` and `SEQLZ_DELTA_8`, for `SEQLZ_DELTA_LAST` those whose position
is below `x`, near the start of the page. The rest become deltas 8 bytes at a time with `sub_bytes()`
if the 8 bytes past the sequence's last literal are still in the page and in `deltas`, else byte by
byte, with the cast to `u8` for the modulo 256. The encoder reads the bytes it subtracts from the page
as it is, so a distance of 4 is no problem for a word of 8; the decoder, which has only the bytes it
has already restored, works in words of 4 there ([chapter 13](#ch-literals-dec--undelta)). Here are
the first 12 literals of the page of pointers, as they are and as deltas with `SEQLZ_DELTA_LAST`:

```text
as they are   {{delta.ptr.plain}}
deltas        {{delta.ptr.deltas}}
```

The first sequence's 9 literals have `last` 1, so a distance of 4: the first 4 stay, `12` becomes
`12 - 10 = 02`, `00` becomes `00 - 56 = aa` modulo 256, and so on. From the second sequence on, each
literal is the low byte of a pointer minus the one 8 bytes before: `30 - 20 = 10`, `40 - 30 = 10`,
`50 - 40 = 10`. The low bytes go up by `0x10`, so the delta of every low byte is `0x10`, also from
`0xf0` to `0x00`, where `0x00 - 0xf0` is `0x10` modulo 256. A XOR of the same bytes was `0x10` only every second time,
`0x30` every fourth, `0x70` every eighth: the bits that change when a counter in the top 4 bits counts
up by one. Of the page's {{delta.ptr.n_lit}} literals, {{delta.ptr.xor.h.10}} XORed were `0x10` and
{{delta.ptr.h.10}} deltas are.
@assert delta.ptr.plain == "10 00 56 34 12 7f 00 00 20 30 40 50" and delta.ptr.deltas == "10 00 56 34 02 7f aa cc 0e 10 10 10" and F("delta.ptr.xor.h.10") > F("delta.ptr.xor.h.30") > F("delta.ptr.xor.h.70") and F("delta.ptr.h.10") > F("delta.ptr.xor.h.10")

### code_own(): the deltas first {#ch-literals-enc--code-own}

@listing src/seqlz_compress.c 1167-1184

`code_own()` is level 3's call of `code_literals()`. With a delta it codes the deltas in the matcher's
state, as `raw`, with no `spare`, since that copy is the one it reads. If the deltas stay raw, the page
is still unchanged, `n == len`, and the plain literals in `dst` get their own try: `spare` is the
matcher's state again, and the plain literals are copied over the deltas there. So a page never stores
deltas as raw literals; the format would not allow them, as only coded literals have a byte 2.

## Coded only where it saves 1/16 of the literals and {{ch11.coded_min}} bytes {#ch-literals-enc--rule}

@listing src/seqlz_compress.c 1121-1122

The page with raw literals has `2 + n` bytes before the bitstream, the coded one `3 + w + e + desc +
coded`, with `e` 1 with deltas, else 0, and `desc` 0 for a fixed table. If the rule holds, the page
shrinks by more than {{ch11.save_min}} bytes plus $\frac{1}{16}$ of the literals, with the largest `w`
of 12 and no deltas, and by one byte less with deltas. For `n = 0` the right side is 0 and the page stays raw. The two
terms have two reasons.

**The $\frac{1}{16}$.** zsmalloc stores a page in a size class, and classes are at least 16 bytes
apart ([chapter 1](#ch-zram)). A page a few bytes smaller often lands in the same class and saves
nothing, while every read of it pays for decoding the literals. Coding the literals wherever they save
anything at all gave less than 0.1 points more on a desktop's zram pages of September 2026, in
userspace
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).

**The 51 bytes.** A page with coded literals costs a fixed time to read, whatever the number of
literals: the decoder brings the page's 2 KiB literal table into the cache and decodes the literals
into a buffer first ([chapter 13](#ch-literals-dec)). On a phone's little core, which has few cache
misses in flight, that is several µs.

::: measured 51 bytes instead of 19, on the Mi 9T and on the PC
Before 5th October 2026 the minimum was 19 bytes. In the Mi 9T's own kernel, 20 000 of the phone's
zram pages, clocks fixed, a minimum of 51 coded 24.9% instead of 38.8% of the pages. Pages
got 7.9 bytes larger on average, and the time per page written ($\text{write} + 0.34 \times \text{cold read}$, the reads per write of then) got 2.7 µs
shorter on the Cortex-A55 and 1.1 µs shorter on the Cortex-A76. $\frac{2}{16}$ of the literals instead of $\frac{1}{16}$
coded about as few pages, for 12.3 bytes and less time saved. On desktop pages 51 costs 9 bytes per
page, for 0.1 µs in the kernel VM on a Ryzen 9 7950X, within what two boots of the same build
differ. 51 is the best of the three minimums measured, 19, 51 and 115, not a measured optimum.
Measured again with the prefetches of items 4 and 7 of "Six choices", 51 saved 0.8 µs on the A55 and
0.4 µs on the A76, both within the noise there; it stayed. Sources:
[explored-designs.md, "Coded literals only where they save 51
bytes"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-only-where-they-save-51-bytes-27-µs-per-page-written-less-on-the-a55-kept)
and [item 9 of "Six
choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#9-the-51-bytes-of-coded-literals-only-where-they-save-51-bytes-again-with-item-4s-and-7s-prefetches).
:::

The rule does not count the header of `3 + w` bytes, nor the delta's byte, on purpose. When the sizes shrank from 2 bytes to
`w` bits, counting the smaller header would have coded 64.7% instead of 56.3% of a desktop's pages, and
on the phone's pages saved 2 bytes per page for 251 ns more per page written on the A55, with 0.34 reads per write
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#stream-sizes-in-as-many-bits-as-the-largest-needs-kept)).
So the rule kept the old 19 bytes as its constant, which today's {{ch11.coded_min}} replaced.

## Writing over the raw literals, safely {#ch-literals-enc--in-place}

@listing src/seqlz_compress.c 1123-1133

`memcpy()` returns its first argument, so with `raw == NULL` the line copies the raw literals into
`spare` and points `literals` there; pages that stay raw are never copied. With deltas, `raw` is the
copy of the deltas in the matcher's state, and nothing is copied. `q[0]` is right after the
header, and each stream starts where the one before ends. The first `static_assert` after the function
makes the comment's argument a compile-time fact:

@listing src/seqlz_compress.c 1155-1159

```text
streams end at     d + header + coded,  header = 3 + w + e + desc
3 + w + e        <= {{ch11.header_max}} + 1 <= 2 + {{ch11.coded_min}}                 (the static_assert)
desc + coded + {{ch11.coded_min}} < n - n/16 <= n                  (the rule, since the page is coded)
so header + coded < 2 + {{ch11.coded_min}} + n - {{ch11.coded_min}} = 2 + n,  and d + 2 + n is where the bitstream starts
```

So no stream byte lands on the bitstream; in the example the streams end at byte {{ch11.q.8}} and the
bitstream starts at byte {{ch11.raw.bs}}. Every store of `lit_streams()` stays between `q[0]` and
`q[8]` (`lit_flush()` below), and the u16, byte 2 and the sizes are written after it, over bytes that
were raw literals. The second assert keeps the copy free: `struct seqlz_state` is a union of the
matcher's table and `u8 literals[SEQLZ_PAGE]`, and with the table at least a page,
{{ch11.hash_bytes}} bytes on 4 KiB pages and {{k16.ch11.hash_bytes|grp}} on 16 KiB pages, the
literals cost no byte of work memory.

::: why The matcher's table, and not a second buffer or the end of dst
The first version moved the raw literals and the bitstream to the end of `dst` and wrote the coded
literals from its front, so all three had to fit at once. zram gives the compressor two pages, which
is plenty; zswap gives exactly one page, and there pages with coded literals failed to fit more
often, which cost 2.0% more memory. With the raw literals in the matcher's table it costs 0.23%, and
in the kernel VM writes got 0.12 µs faster. In zram's two pages the output is the same byte for byte,
checked on 80 000 desktop and phone pages
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-from-the-matchers-table-a-buffer-of-one-page-costs-01-to-05-instead-of-16-to-29-kept)).
:::

## Four streams at a time: lit_streams() {#ch-literals-enc--streams}

@listing src/seqlz_compress.c 654-673

Appending a code is `acc = acc << len | code`, and the next code of the same stream needs the result:
one stream is one chain where every step waits for the one before. Written one stream after the
other, that cost about 3.5 cycles per literal, measured with commit `34080eb`
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).
With 4 accumulators, 4 chains run side by side on an out-of-order CPU ([chapter 3](#ch-cpu--chains)).
The outer loop runs twice, streams 0 to 3 and 4 to 7. `e0` to `e3` are the ends, each stream ending
where the next starts. Each stream has 5 live values, `p`, `e`, `a`, `c` and `s`.

Why 4 at a time and not 8 is not written down. A guess, not measured: 8 streams would be 40 live
values, far more than the 16 general registers of x86-64. In the kernel, "8 streams, 4 in one loop,
twice" wrote a page in 7590 ns at p50 and 12 030 ns at p99 on a desktop's zram pages, against 7530 and
12 030 ns with the 4 streams of the format before in one loop
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).

### Blocks of 32 literals

@listing src/seqlz_compress.c 674-690

A block is 32 literals of the page, 4 for each stream of this half: for `half = 0` and `k = 0`, stream
0 gets literals 0, 8, 16 and 24, stream 1 gets 1, 9, 17 and 25; for `half = 4`, `k` starts at 4. 4
codes of at most 10 bits and 7 bits left over are 47 bits, which fit; 5 would fit too, $7 + 50 = 57$,
and the repository does not say why 4. The highest literal of a block is `k + 27`, and the condition
asks for one more, so after the loop each stream of this half has at most 4 literals left, which fit
without a flush.

### lit_put(): one code into an accumulator

@listing src/seqlz_compress.c 542-556

`e & 63U` is the code's length: the low byte holds at most 10, and the mask lets the compiler see a
shift below 64, for UBSan ([chapter 7](#ch-internal--decode-tables)). `*sum += e` adds the whole
entry, code and length: the codes add only into bits 8 and up, and a carry never goes down, so the
low byte of the sum is the sum of the lengths, exact as long as it stays below 256. Between two
flushes it is at most $4 \times 10 = 40$. `lit_flush()` reads `*sum & 255U`, one mask per flush
instead of one per literal. Stream 0 of the example page, in table {{ch11.set}}, gets literals 0, 8,
16 and 24:

| literal | `enc[b]` | code | `acc` after | `sum` after | low byte |
| --- | --- | --- | --- | --- | --- |
| `{{ch11.put.0.ch}}` | `{{ch11.put.0.enc}}` | `{{ch11.put.0.code}}`, {{ch11.put.0.len}} bits | `{{ch11.put.0.acc}}` | `{{ch11.put.0.sum}}` | {{ch11.put.0.low}} |
| `{{ch11.put.1.ch}}` | `{{ch11.put.1.enc}}` | `{{ch11.put.1.code}}`, {{ch11.put.1.len}} bits | `{{ch11.put.1.acc}}` | `{{ch11.put.1.sum}}` | {{ch11.put.1.low}} |
| `{{ch11.put.2.ch}}` | `{{ch11.put.2.enc}}` | `{{ch11.put.2.code}}`, {{ch11.put.2.len}} bits | `{{ch11.put.2.acc}}` | `{{ch11.put.2.sum}}` | {{ch11.put.2.low}} |
| `{{ch11.put.3.ch}}` | `{{ch11.put.3.enc}}` | `{{ch11.put.3.code}}`, {{ch11.put.3.len}} bits | `{{ch11.put.3.acc}}` | `{{ch11.put.3.sum}}` | {{ch11.put.3.low}} |

### lit_flush(): whole bytes to memory, big-endian

@listing src/seqlz_compress.c 572-594

The streams' version of `enc_flush()` ([chapter 10](#ch-encoder--flush)), but forwards: `n` bits are
not in memory yet, the shift puts them on top, and `__builtin_bswap64()` followed by a little-endian
store is a big-endian store, so the stream's first bit is the top bit of the byte at `p`, the order the
decoder reads. The mask matters only for `n = 0`, at a stream's last flush when its bits ended on a
byte boundary; `p` is then at `end`, and `store_tail()` writes 0 bytes. With 8 bytes of room the flush
writes all 8, the zeros past the bits landing in the stream's own room for later flushes to overwrite;
near the end `store_tail()` writes only the `(n + 7) >> 3` bytes that carry bits, so it never touches
the next stream. A partial last byte is written again by the next flush.

::: why One byte swap before the branch
The obvious form is `put_unaligned_be64()` in the first branch and a big-endian byte loop in the
second. On arm64, swapping the bytes inside both branches gave `code_literals()` other machine
code, so the code swaps once, in a register, before the branch, and both branches store the
same `w` (commit `9d64639`). On a big-endian machine `put_unaligned_le64()` swaps the bytes itself, so
the bytes in memory are the same on every machine; CI checks that on s390x.
:::

For stream 0 of the example, the first flush:

```text
n        = 0 + {{ch11.flush.n}} = {{ch11.flush.n}}
acc<<{{ch11.flush.shift}}  = {{ch11.flush.sh}}        the {{ch11.flush.n}} bits on top, zeros below
w        = {{ch11.flush.w}}        bswap64
stored   = {{ch11.flush.stored}}   at q[0], byte {{ch11.q.0}} of the page
p       += {{ch11.flush.n}} >> 3 = {{ch11.flush.moved}}               cnt = {{ch11.flush.n}} & 7 = {{ch11.flush.cnt}}
```

The bytes `{{ch11.flush.final}}` are final; `{{ch11.flush.part}}` holds {{ch11.flush.cnt}} bits of
stream 0, `{{ch11.flush.partbits}}`, and the next flush writes it again with its last 3 bits.
@assert ch11.flush.cnt == 5 and ch11.flush.moved == 2

### store_tail(): at most 7 bytes at the end of a stream

@listing src/seqlz_compress.c 558-570

It runs once or twice per stream, at its end, so a loop of up to 7 stores is fine; `gcc -O2` for
x86-64 inlines it anyway. `sizes[j]` came from the exact bits of the stream, so its last flush ends
exactly at `e`, and the bits after the last code are the zeros of the shift, as the format requires
([chapter 4](#ch-format--end)).

### The tail: at most 4 literals per stream, one switch

@listing src/seqlz_compress.c 691-720

Cases 0 and 4 both go to `a0`, which is stream 0 for `half = 0` and stream 4 for `half = 4`, so one
switch serves both halves. After this half's 4th literal of a group of 8, `k += 4` skips the other
half's 4. For `half = 0` and the example's `n = {{ch11.n}}`, the blocks end at `k = {{ch11.tail0}}`,
and the tail takes:

```text
{{ch11.tr.0}}
{{ch11.tr.1}}
{{ch11.tr.2}}
{{ch11.tr.3}}
{{ch11.tr.4}}
{{ch11.tr.5}}
```
@assert ch11.tr.lines == 6

For `half = 4` the tail starts at `k = {{ch11.tail4}}`, stream 4, and after {{ch11.tr4.last}} the
skip makes `k` {{ch11.tr4.jump}}, past the end, where the loop condition stops it.
@assert ch11.tail4 % 8 == 4 and ch11.tr4.jump > ch11.n

## The header: u16, byte 2 and the sizes {#ch-literals-enc--header}

@listing src/seqlz_compress.c 1134-1148

The u16 gets `SEQLZ_LIT_CODED`: {{ch11.n}} literals are `{{ch11.u16hex}}`, the bytes `{{ch11.u16}}`.
Byte 2 is the literals' code in bits 0 to 3, `width - 5` in bits 4 to 6, and in bit 7 whether they are
deltas, e.g. table 3 with width 7 is `3 | 2 << 4 = 0x23`, the page's own table with width
{{own.text.w}} `8 | {{own.text.w5}} << 4 = {{own.text.t}}`, and the page of pointers, own table, width
{{delta.ptr.l3.w}} and deltas, `8 | 0 << 4 | 1 << 7 = {{delta.ptr.l3.t}}`. The delta's byte goes right behind
byte 2, at `SEQLZ_LIT_HEADER(0)`, byte 3, and the sizes behind it, one byte later with a delta than
without. The description comes after the sizes, at `SEQLZ_LIT_HEADER(width)` plus the delta's byte,
and `q[0]` already left room for all of them, since `header` counts them.
@assert delta.ptr.l3.w == 5

@listing src/seqlz_compress.c 722-739

The 8 sizes are one number of `8 * width` bits, little endian, size 0 in the lowest `width` bits
([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#layout-of-a-compressed-page)).
`acc` holds at most $7 + 12 = 19$ bits, and after 8 sizes `8 * width` bits, whole bytes, have gone out,
exactly `width` bytes. The example's sizes, {{ch11.sizes}} with `width = {{ch11.width}}`, become the
{{ch11.width}} bytes `{{ch11.sizes.hex}}`: in binary, little end first, `10010` (18), `10001` (17),
and so on.
@assert ch11.width == 5 and F("ch11.sizes").startswith("18, 17,")

## The bitstream moves down {#ch-literals-enc--memmove}

@listing src/seqlz_compress.c 1149-1153

`memmove()`, as the bitstream moves down by more than {{ch11.save_min}} bytes and is often longer than
that, so source and destination overlap. In the example, {{ch11.bs}} bytes move from byte
{{ch11.raw.bs}} to byte {{ch11.q.8}}, and the page goes from {{letters.len}} to
{{letters.coded.len}} bytes. The encoder copied the bitstream once already, from the back of `dst`.
Copying it only once, to behind the coded literals, made writes 0.2 µs slower in the kernel VM and
0.8 µs on the A55, `seqlz-1` too, because gcc laid out the matcher's loop differently
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-bitstream-copied-once-behind-the-literals-raw-or-coded-writes-up-to-03-µs-slower-in-the-vm-and-08-µs-on-the-a55-not-kept)).
The copy of 400 to 600 bytes on a third of the pages stayed.

## The example page, byte by byte {#ch-literals-enc--example}

{{ch11.letters}} letters, each drawn from `etaoinshrdlu` and a space by the C library's classic linear
congruential generator, `seed = seed * 1103515245 + 12345` from `seed = 1`, taking
`(seed >> 16) % 13`, then {{ch11.zeros}} zero bytes. The first 24 letters are `{{ch11.first24}}`.
{{letters.seqs}} sequences: {{ch11.n}} literals and a match of {{ch11.ml}} bytes at offset
{{ch11.off}}, then the last sequence with no literals. At level 1 the page has {{letters.len}} bytes,
at level 2 {{letters.coded.len}}, and at level 3 the same: {{ch11.n}} literals are too few for a table of
their own.

```text
{{ch11.dump.0}}
{{ch11.dump.1}}
{{ch11.dump.2}}
{{ch11.dump.3}}
{{ch11.dump.4}}
{{ch11.dump.5}}
{{ch11.dump.6}}
{{ch11.dump.7}}
{{ch11.dump.8}}
{{ch11.dump.9}}
```
@assert ch11.dump_lines == 10

- `{{ch11.u16}}` is `{{ch11.u16hex}}`: coded literals, `{{ch11.u16hex}} & 0x7fff = {{ch11.n}}` of
  them.
- `{{ch11.byte2}}` is code `0x{{ch11.byte2}} & 15 = {{ch11.set}}`, table {{ch11.set}}, and width
  `5 + (0x{{ch11.byte2}} >> 4 & 7) = {{ch11.width}}`. Table {{ch11.set}} is one of the two tables for
  text: `e` {{lit.3.len.101}} bits, the space {{lit.3.len.32}}.
@assert ch11.set == 3 and lit[3].len[0x65] == 4 and lit[0].len[0x65] == 4 and letters.l3.len == letters.coded.len and ch11.n < own.enc.min_lits
- `{{ch11.sizes.hex}}` are the 8 sizes, {{ch11.sizes_comma}}, {{ch11.coded}} bytes in all. The
  streams start at byte {{ch11.q.0}} and end at {{ch11.q.8}}, `{{ch11.q8hex}}`.
- Stream 0 begins `{{ch11.s0.head4}}`, `{{ch11.s0.head4bin}}`, which table {{ch11.set}}'s codes cut
  into {{ch11.s0.codes5}}: literals 0, 8, 16, 24 and 32. It has {{ch11.s0.bits}} bits, so its last
  byte, `{{ch11.s0.lasthex}}`, holds `{{ch11.s0.lastbits}}` and {{ch11.s0.zeros}} zero bits.
- `{{letters.b.hex}}` from byte {{ch11.q.8}} on is the sequences' bitstream, the same {{ch11.bs}}
  bytes that followed the raw literals.

The pricing gave:

| table | stream 0 to 7 | total |
| --- | --- | --- |
| {{ch11.rank.0.table}} | {{ch11.rank.0.bits}} | {{ch11.rank.0.total}} |
| {{ch11.rank.1.table}} | {{ch11.rank.1.bits}} | {{ch11.rank.1.total}} |
| {{ch11.rank.2.table}} | {{ch11.rank.2.bits}} | {{ch11.rank.2.total}} |
| others | | {{ch11.others.min}} to {{ch11.others.max}} |

`{{ch11.coded}} + {{ch11.coded_min}} = {{ch11.rule.lhs}}` is less than
`{{ch11.n}} - {{ch11.n}} / 16 = {{ch11.rule.rhs}}`, by 7 bytes; a few letters fewer and the page would
have kept its raw literals. It saves {{ch11.saves}} bytes, {{ch11.saves_pct}}% of the {{letters.len}}
bytes with raw literals.
@assert ch11.rule.lhs + 7 == ch11.rule.rhs

::: try Compress a page of your own
In a scratch directory, with the repository's path in `$R` (`R=/path/to/quetschn`):

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
	len = seqlz_compress(&t, &st, page, dst, sizeof(dst), 2);
	for (k = 0; k < len; k++)
		printf("%02x%c", dst[k], k % 16 == 15 ? '\n' : ' ');
	printf("\n%u bytes\n", len);
	return 0;
}
```
@program ch11-letters

Build it with `gcc -O2 -I$R/src ex.c $R/src/seqlz_{codes,compress,decompress,default_tables,lit_sets}.c`.
It prints the page above, 16 bytes to a line, and ends with:

@output ch11-letters 10-11

With {{ch11.lraw}} letters instead of 200 the literals stay raw, with one more they are coded.
@assert ch11.lraw < 200 and ch11.lraw_coded == 1
:::

## What writing the literals costs {#ch-literals-enc--cost}

The pricing is cheap, writing the streams is not. On the Cortex-A55, `code_literals()` takes about
5400 cycles, 3 µs, per page in the hot loop of the userspace benchmark: `seqlz-2` compresses a
page in 47 640 cycles, `seqlz-1` in 42 241. Most pages pay part of that even when they stay raw:
in that sample 14 582 of 19 387 pages kept their raw literals, about 404 each, all priced in all 8
tables first. Pricing only every 8th literal first, to send hopeless pages back early, made
compression slower, because the literals of most raw pages come close to paying
([item 8 of "Six choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#8-coded-literals-on-the-a55-78-instructions-per-literal-at-one-per-cycle)).
This is where `seqlz-1`, which never calls `code_literals()`, gains on the phone: in an earlier
measurement on the Mi 9T's little core, `seqlz-2` stored 55 bytes per page less and took
4.2 µs more per page written, with 0.34 reads per write
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-second-phone-dump-after-12-hours-of-use-seqlz-fast-reads-faster-than-lzo-rle-at-p99)).
Which of the two a phone should use depends on how it values a byte against a µs, and that is not
settled.

Level 3 adds a third choice. On 15th October 2026, in the Mi 9T's own kernel on the Cortex-A76 and
19 775 pages of a phone session, it stored a page in 952.0 bytes of zsmalloc memory, against 997.0 at
level 2 and 951.4 for `zstd` 3, and wrote one in about 16 µs, against 12 µs at level 2 and 36 µs for
`zstd` 3
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-a-lazy-matcher-and-a-literal-table-of-the-pages-own-zstd-3s-memory-in-less-than-half-its-time-on-the-phone-kept)).
Those 4 µs are the lazy step, the extension backwards and the table together. The deltas, on 21st
October 2026, in userspace on the Mi 9T with the kernel's compile flags, added about 4 µs per page
written at level 3 on the A76 and 8 µs on the A55, for 4.6% fewer bytes on ten samples of 20 000
pages, 873.6 instead of 915.6 per page: the XOR before them cost 3.6 µs and 7.4 µs, the subtraction
0.1 to 0.3 µs more on the A76
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-stores-its-literals-as-deltas-not-xors-084-fewer-bytes-than-the-xor-kept)).

