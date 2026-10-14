# Decoding eight streams side by side {#ch-literals-dec}

A page with coded literals has them Huffman coded in 8 streams, literal `i` in stream `i % 8`
([chapter 11](#ch-literals-enc) wrote them). `seqlz_decode()` first calls `decode_literals()`, which
decodes all of them into the scratch, `SEQLZ_SCRATCH` bytes of work memory, and returns where the
sequences' bitstream starts. From then on the loop of [chapter 14](#ch-decode) reads the literals from
the scratch as if the page had them raw.

Decoding a Huffman code is a chain: look up the next 10 bits, write the byte, shift the stream by the
code's length, and only then are the next code's bits known. 8 streams are 8 such chains with nothing
between them, and an out-of-order CPU works on them at the same time ([chapter 3](#ch-cpu--chains)).
The page layout is in [chapter 4](#ch-format--layout), the literal table's entries in
[chapter 7](#ch-internal--lit-table).

## A stream is a pointer and 64 bits with a 1 at the end {#ch-literals-dec--sentinel}

@listing src/seqlz_decompress.c 251-259

Each code shifts `bits` left by its length, and the 1 that the refill set in bit 0 moves up with it. So
its position, `__builtin_ctzll(bits)`, is the number of bits used since `ip`: one instruction on x86-64,
two on arm64. This **sentinel** replaces the `count` of the sequences' bit reader
([chapter 12](#ch-dec-basics--bit-reader)). With a count, 8 streams are 24 values, and x86-64 has 16
general purpose registers, some of which the loop needs for the table and the output. With the
sentinel the 8 `bits` stay in registers, and the 8 pointers, which only a refill needs, go to the stack
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
zstd keeps its 4 Huffman streams the same way and reads them from the end backwards; seqlz reads
forwards, as it reads the sequences.

The comment's last sentence is the invariant: a refill leaves at least 56 bits above the 1, the codes
between two refills take fewer, so they never reach the 1, and `bits` is never 0, for which
`__builtin_ctzll()` is undefined.

One stream in three states, from the page of [chapter 11](#ch-literals-enc--example): {{letters.lits}}
literals, 200 random letters and a zero byte, coded with literal table {{ch13.set}}. Stream 0 starts at
byte {{ch13.s0.start}} of the compressed page with the bytes `{{ch13.s0.bytes}}`.
@assert letters.lits == 201 and ch13.last_lit_byte == 0 and ch13.r.valid_before + ch13.r.new_bits == ch13.r.valid_after

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 250" font-size="12" role="img" aria-label="the 64 bits of stream 0 after a refill, after 5 codes, and after the next refill, with the sentinel 1 moving up and down">
  <text x="100" y="30">after the first refill: {{ch13.s0.bits0}}, ctz = 0</text>
  <rect x="100" y="38" width="{{ch13.fig.w0}}" height="28" fill="none" stroke="currentColor"/>
  <text x="{{ch13.fig.c0}}" y="57" text-anchor="middle">63 bits of the stream, from ip = byte {{ch13.s0.start}}</text>
  <g class="acc">
    <rect x="667" y="38" width="9" height="28" class="fill-acc" stroke="currentColor"/>
  </g>
  <text x="688" y="57">1</text>
  <text x="100" y="80">bit 63</text>
  <text x="676" y="80" text-anchor="end">bit 0</text>

  <text x="100" y="110">after 5 codes of {{ch13.c.bits}} bits: {{ch13.c.after}}, ctz = {{ch13.r.used}}</text>
  <rect x="100" y="118" width="{{ch13.fig.w1}}" height="28" fill="none" stroke="currentColor"/>
  <text x="{{ch13.fig.c1}}" y="137" text-anchor="middle">{{ch13.r.valid_before}} bits not used yet</text>
  <g class="acc">
    <rect x="{{ch13.fig.s1}}" y="118" width="9" height="28" class="fill-acc" stroke="currentColor"/>
  </g>
  <rect x="{{ch13.fig.z1}}" y="118" width="{{ch13.fig.wz1}}" height="28" class="fill-sheet" stroke="currentColor"/>
  <text x="{{ch13.fig.cz1}}" y="137" text-anchor="middle">{{ch13.r.used}} zeros shifted in</text>
  <text x="{{ch13.fig.l1}}" y="162" text-anchor="middle">bit {{ch13.r.used}}</text>

  <text x="100" y="192">next refill: ip += {{ch13.r.used}} &gt;&gt; 3 = {{ch13.r.bytes}}, shift by {{ch13.r.used}} &amp; 7 = {{ch13.r.shift}}: {{ch13.r.bits}}</text>
  <rect x="100" y="200" width="{{ch13.fig.w2}}" height="28" fill="none" stroke="currentColor"/>
  <text x="{{ch13.fig.c1}}" y="219" text-anchor="middle">the same {{ch13.r.valid_before}} bits, then {{ch13.r.new_bits}} new ones</text>
  <g class="acc">
    <rect x="{{ch13.fig.s2}}" y="200" width="9" height="28" class="fill-acc" stroke="currentColor"/>
  </g>
  <rect x="{{ch13.fig.z2}}" y="200" width="{{ch13.fig.wz2}}" height="28" class="fill-sheet" stroke="currentColor"/>
  <text x="{{ch13.fig.l2}}" y="244" text-anchor="middle">bit {{ch13.r.shift}}</text>
  <line x1="{{ch13.fig.s1}}" y1="200" x2="{{ch13.fig.s1}}" y2="228" stroke="currentColor" stroke-dasharray="2 2"/>
</svg>
</div>
<figcaption>Stream 0's 64 bits: the sentinel 1 (accent) marks how many bits were used since ip; a refill moves ip by the whole bytes and shifts by the rest.</figcaption>
</figure>

## lit_refill(): 8 new bytes, ip moved by the whole bytes used {#ch-literals-dec--refill}

The file has `lit_load_tail()` first, because `lit_refill()` calls it; we go the other way round.

@listing src/seqlz_decompress.c 274-289

`end` is the end of the whole compressed page, not of the stream. `p` moves by the whole bytes used, and
`<< (used & 7U)` drops the bits of `p`'s first byte that were used already, which also puts the new
sentinel at the right place for the new `ip`. `| 1U` overwrites the last bit of the 8th byte; that bit
is not lost, because the next refill moves at most 7 bytes on and loads that byte again. After a refill
`63 - (used & 7)` bits are valid, at least 56, so `SEQLZ_LIT_ROUNDS`, `56 / SEQLZ_LIT_BITS`, is 5 codes
of at most 10 bits between refills ([chapter 6](#ch-header--literals)). The only branch is
`end - p >= 8`, which is the same for every refill but the last few of a page.
@assert ch13.k.rounds == 5 and ch13.k.streams == 8 and ch13.k.lit_bits == 10

A stream starts with `bits = 1`, so its first refill loads from its start without a shift. In the
example, stream 0's first 8 bytes end in an odd byte, `| 1` changes nothing, and the bits are
`{{ch13.s0.bits0}}`. 5 codes, `{{ch13.c.0.lit}}{{ch13.c.1.lit}}{{ch13.c.2.lit}}{{ch13.c.3.lit}}{{ch13.c.4.lit}}`,
take {{ch13.c.bits}} bits, and the next refill is:
@assert ch13.s0.odd == 1

```text
used   = ctz({{ch13.c.after}}) = {{ch13.r.used}}
p      = ip + {{ch13.r.used}} >> 3 = ip + {{ch13.r.bytes}}                 byte {{ch13.r.at}} of the page
load   = {{ch13.r.load_hex}} -> {{ch13.r.load}}
| 1    = {{ch13.r.or1}}
<< {{ch13.r.shift}}   = {{ch13.r.bits}}                    {{ch13.r.used}} & 7 = {{ch13.r.shift}}, the sentinel at bit {{ch13.r.shift}}
```

The top {{ch13.r.valid_before}} bits are the same before and after, and {{ch13.r.new_bits}} new ones
follow. The shift drops `{{ch13.r.dropped}}`, the first {{ch13.r.shift}} bits of byte {{ch13.r.at}},
which were the end of the code of `{{ch13.c.4.lit}}`.
@assert ch13.c.last_split == 1 and ch13.c.last_prev_byte + 1 == ch13.r.at

### lit_load_tail(): the last bytes, out of the loop

@listing src/seqlz_decompress.c 260-272

With fewer than 8 bytes left, an 8-byte load would read past the compressed page, which may be the end
of a mapping. `lit_load_tail()` reads what is there and zeros for the rest. `noinline` keeps its byte
loop out of the copies of `lit_refill()` in the two unrolled loops over the streams, and `__cold` has the compiler place it away from the
loop and lay out its call as the unlikely branch. It runs only where the sequences' bitstream after the
streams is short. On the example page it has {{ch13.bitstream}} bytes, and stream 7, which starts at
byte {{ch13.s7.start}} of {{ch13.n}}, has its last refill {{ch13.s7.before_end}} bytes before the end,
through `lit_load_tail()`.
@assert ch13.s7.tail == 1 and ch13.bitstream == letters.b.bytes and ch13.n == letters.coded.len

## lit_decode(): one literal, or one that does not count {#ch-literals-dec--decode}

@listing src/seqlz_decompress.c 291-304

The top 10 bits index the table directly ([chapter 2](#ch-compression--table-decoding)); the index is
below 1024 for any `bits`, so a damaged page cannot make this read go wrong. `mask` is 63 or 0. With 63,
`e & 63` is the code length, and the mask tells the compiler that the shift is below 64. With 0, the
literal is decoded and written but the stream does not move, "decode, but do not advance" without a
branch, which the end of `decode_literals()` needs.

## The sizes of the 8 streams {#ch-literals-dec--sizes}

@listing src/seqlz_decompress.c 306-329

From byte 3 on, the header has `width` bytes with the 8 stream sizes, `width` bits each, lowest bit first
([chapter 4](#ch-format--layout)). A 4-byte load always holds a field: it starts at most 7 bits into its
first byte and has at most 12 bits. The furthest load, for `k = 7` with `width = 12`, is at byte
`84 / 8 = 10` and reads bytes 10 to 13, past the 12 bytes of sizes and possibly past the end of a short
page. So the sizes are copied into `h` first, whose bytes behind them are zero. The example page has
`width = {{ch13.width}}` and the size bytes `{{ch13.size_hex}}`:

| `k` | bit `k * {{ch13.width}}` | byte | shift | `le32` at that byte | size |
| --- | --- | --- | --- | --- | --- |
| 0 | {{ch13.sz.0.bit}} | {{ch13.sz.0.byte}} | {{ch13.sz.0.shift}} | `{{ch13.sz.0.le}}` | `{{ch13.sz.0.low}} & {{ch13.mask}} = {{ch13.sz.0.size}}` |
| 7 | {{ch13.sz.7.bit}} | {{ch13.sz.7.byte}} | {{ch13.sz.7.shift}} | `{{ch13.sz.7.le}}` | `{{ch13.sz.7.le_short}} >> {{ch13.sz.7.shift}} = {{ch13.sz.7.size}}` |
@assert ch13.sz[0].shift == 0 and ch13.sz[7].past == 1

The zeros at the top of the last load are `h`'s, not bytes of the page. The streams start at
{{ch13.starts}}, {{ch13.total}} bytes in all.

## Does a stream end where its size says? {#ch-literals-dec--ends}

@listing src/seqlz_decompress.c 331-347

The format wants a stream's codes to end in its last byte, with the bits after the last code 0
([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#coded-literals)). The bits
used are the bytes `ip` moved past, times 8, plus the sentinel's position; `left` is a `long` so that a
stream that used more than its size gives a negative number. The trailing bits are read from the page,
not from `b`: if the stream's last byte was the 8th of its last load, the sentinel sits where its lowest
bit was.

Stream 0 of the example has {{ch13.e.size}} bytes, {{ch13.e.size_bits}} bits, and used
{{ch13.e.used}}: `left = {{ch13.e.left}}`, and its last byte, `{{ch13.e.last_val}}`, has
`{{ch13.e.last_val}} & {{ch13.e.mask}} = {{ch13.e.masked}}`. Three damaged versions of the page, each
rejected by `seqlz_decode()` with `-EINVAL` when the book is built:

| change | what `lit_stream_ends()` sees |
| --- | --- |
| byte {{ch13.e.last}} is `{{ch13.e.bad_val}}` | `{{ch13.e.bad_val}} & {{ch13.e.mask}} = {{ch13.e.bad_masked}}`: a bit after the codes is set |
| stream 0's size is {{ch13.e.size1}} | `left = {{ch13.e.size1_bits}} - {{ch13.e.used}} = {{ch13.e.left1}}`: the codes ran into the next stream |
| stream 0's size is {{ch13.e.size2}} | `left = {{ch13.e.size2_bits}} - {{ch13.e.used}} = {{ch13.e.left2}}`: a whole byte after the codes |
@assert ch13.e.masked == 0 and ch13.e.bad_masked != 0 and ch13.e.left1 < 0 and ch13.e.left2 > 7

::: why The exact check, and what it cost
Until 5th October 2026 the last round decoded 5 literals in every stream, also behind its last literal,
and the check allowed for those bits: a stream one byte too short was decoded with the next stream's
bits and accepted. Now the codes must fit the size exactly, a rule the format can state without the
bytes that follow. On the Mi 9T's A55 it cost 1.1% of the decode cycles per page, 16 357 to 16 540, and
nothing on the A76 (commit `acfd49b`). zram stores no checksum, so this check and the end of the
sequences' bitstream are what tell a damaged page from a valid one.
:::

## decode_literals(), from the header to the bitstream {#ch-literals-dec--main}

@listing src/seqlz_decompress.c 349-358

`n_lit` is the page's u16 without the `SEQLZ_LIT_CODED` bit. A static function with one caller is
normally inlined; inlined, the 8 streams competed for registers with the loop over the sequences, and
made pages with raw literals slower too, 12% when the comment still had a number on 7th October 2026,
with a setup that is not recorded. Until 10th October the function was also aligned to 64 bytes; measured
again, that made less than 0.02 µs of difference in the kernel VM, and it was removed (commit `bad6224`).

@listing src/seqlz_decompress.c 359-377

A page of 2 bytes has no byte 2 and gets width 5, which the check below rejects. The streams are arrays
`ip[]`, `start[]`, `sz[]` and `b[]`; with constant indices after unrolling, the compiler keeps them in
registers like any variable. Until 10th October 2026 they were 8 variables each, `i0` to `i7` and `b0` to
`b7`. The arrays made the function 105 lines shorter and read coded pages 0.1 µs faster on the A55, within
the noise of code placement in the kernel VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).

@listing src/seqlz_decompress.c 378-390

Bits 6 and 7 of byte 2 are reserved: a decoder that accepted them now could not tell old pages from new
ones later. `n_lit` must fit the scratch. Any value of bits 0 to 2 is a valid table. The prefetch of the
2 KiB table goes out before the sizes are read, so its misses overlap with that work; the table never
starts a line, so it covers {{ch13.lt.lines}} lines, the last one asked for by the prefetch of `end - 1`
([chapter 12](#ch-dec-basics--prefetch)). `total` is at most $8 \times 4095$, so the check of the streams
against `src_len` cannot wrap, and after it every stream ends inside the input. Whether the bitstream
after them is long enough is the sequence loop's business.
@assert ch13.lt.mod8 != 0 and ch13.lt.lines == 33 and ch13.lt.prefetches == 33 and ch13.lt.loop_prefetches == 32

### Full rounds: 8 streams, 5 literals each

@listing src/seqlz_decompress.c 391-405

Stream `i` in step `j` of the round at `k` holds literal `k + 8 * j + i`, so the scratch gets the literals
in page order with no shuffle. The loops have constant bounds, and the compiler unrolls them into 8
refills and 40 lookups, where each `b[i]` depends only on the `b[i]` before it: 8 chains.

@anim streams wide The {{slab.lits}} literals of chapter 16's page, dealt out to 8 streams and decoded a round at a time, or in one stream. Literals, code lengths and stream sizes come from the real codec; the count of dependent steps is a model, one step per decoded literal.

### The rest: the same steps for all 8 streams

@listing src/seqlz_decompress.c 406-433

After one more refill, at most `steps + 1 <= 5` codes follow. In the last step,
`(0U - (r > i)) & 63U` is 63 for the `r` streams that have a literal left and 0 for the others, which
decode one too, write it to `out[k + i]` and stay where they were, so that `lit_stream_ends()` sees
exactly their own codes. The example page has {{letters.lits}} literals: rounds at
`k = {{ch13.rounds}}`, then `rest = {{ch13.rest}}`, `steps = {{ch13.steps}}`, `r = {{ch13.rest_r}}`.
Stream 0 decodes literal {{ch13.last_lit}}, the zero byte, with mask 63, and streams 1 to 7 decode
whatever their next 10 bits say into `out[{{ch13.extra_first}}]` to `out[{{ch13.extra_last}}]`.
@assert ch13.rest_r == 1 and ch13.steps == 0 and ch13.last_lit_byte == 0

The last step writes at most `out[n_lit + 7]`, {{ch13.max_write}} on 4 KiB pages, inside the scratch
of a page plus 16 bytes, {{const.SCRATCH}}. The 16 spare bytes are there for the 16-byte copies of the
sequence loop, which may read past the last literal; a valid page never takes them as literals
([chapter 14](#ch-decode)).

A loop over only the streams with a literal left would end after `r` iterations, 0 to 7, different on
every page, and mispredict. The same steps for all streams cost at most 8 lookups and no branch on the
page. Measured on the Mi 9T on 5th October 2026 (commit `8dc0341`), median of 7 runs of 20 000 pages:
the A55 decoded a page in 16 620 cycles this way against 16 531 with a loop per stream, the A76 in 7420
against 7480, all within their spread. So the form is kept for its reason and for the exact size
check, not for a measured gain.

## Why nothing reads or writes outside its buffer {#ch-literals-dec--safety}

`seqlz_decode()` is safe for any input ([chapter 14](#ch-decode--safety)). For `decode_literals()`:

- **Reads stay in the input.** The header is read after `src_len` is checked, the sizes from a copy. A
  refill loads 8 bytes only when 8 are left before the end of the whole input, else `lit_load_tail()`
  reads up to it. Table indices are 10 bits, the table number 3.
- **Reading past a stream is allowed, and caught.** A refill near the end of stream 3 loads bytes of
  stream 4 or of the bitstream, still inside the input. A stream that uses them as codes fails
  `lit_stream_ends()`. The format allows exactly this
  ([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#coded-literals)). A damaged
  stream's `ip` can even move past `end`; its refills then read nothing, and the check rejects it.
- **Writes stay in the scratch.** Every write is `out[at]` with `at < n_lit + 8`, and `n_lit` is at most
  a page.
- **The work is bounded.** No loop depends on the codes: `n_lit / 40` rounds and one tail.

The fuzz targets allocate the scratch and the page at their exact sizes, so AddressSanitizer reports a
read or write of one byte too far ([chapter 15](#ch-zram-glue--fuzzing)).

## What decoding the literals costs {#ch-literals-dec--cost}

::: measured 8 streams against 4
When `seqlz-fast-lit` got its 8 literal tables on 24th September 2026, the streams went from 4 to 8.
With 4, a literal took about 2.9 cycles. With 8, the decode loop over 2000 pages in the cache went from
9070 to 8676 cycles per page (median of 5, userspace; `seqlz-fast` took 7316), and cold reads in the
kernel from 2940 to 2860 ns at p50 and from 4810 to 4531 ns at p99
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
Measured again on 5th October 2026 in the Mi 9T's own kernel, 4 streams saved 2.7 bytes per page; the
A76 read 0.26 µs slower, the A55 1.1 µs faster, both within the spread, and the PC's hot loop decoded
2.8% slower
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#3-4-literal-streams-instead-of-8-27-bytes-no-faster-not-kept)).
On 10th October in the kernel VM, 4 streams read 1.8% slower with gcc 16.2.1 and 0.7% with clang
22.1.8, for 2.0 bytes per page less
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
A format change for 2 to 3 bytes and no time is not worth it. That the format has 8 streams and 8
literal tables is a coincidence.
:::

On the A55, an in-order core, a round is 312 instructions for 40 literals, in userspace with clang 21
from NDK r30: per literal a shift for the index, the load, a shift for the byte, the store and the shift
of the stream, plus 2.8 for the refills and the stream pointers on the stack. At about one instruction
per cycle, a page with coded literals takes about 8400 cycles more than with raw literals, 8 per
literal, and `decode_literals()` has 14% of the decoder's samples, with no hot spot. Putting the 8 loads
of a step first saved 2.8% of the A55's decode cycles, and was not built into the kernel module
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#8-coded-literals-on-the-a55-78-instructions-per-literal-at-one-per-cycle)).

The fixed cost per page counts more there: the 2 KiB table comes from memory, and the literals go
through the scratch before the sequences copy them. That is why the encoder codes literals only where
they save {{ch11.coded_min}} bytes and $\frac{1}{16}$ of themselves ([chapter 11](#ch-literals-enc--rule)). Decoding the
literals into the end of the output page instead, which saves the scratch, was built: on 4 KiB pages it
made cold reads 0.08 to 0.22 µs slower in the kernel VM, and it was not kept
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#16-kib-pages-tuned-c5-does-not-hold-the-literals-in-the-page-would-fix-it-not-kept)).
