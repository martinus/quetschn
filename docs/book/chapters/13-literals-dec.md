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

## A page's own table, built in the page {#ch-literals-dec--own}

With code 8 in byte 2 the page brings its own literal table, as a list of code lengths in the fixed
code `OWN` ([chapter 4](#ch-format--own)). `own_table()` reads that list and writes the same kind of
decode table a fixed literal table has, 1024 entries of `code length | byte << 8`, so the rest of
`decode_literals()` does not know the difference. It needs 2 KiB for that table and 3 KiB more on the
way, and takes neither from the work memory the project counts per CPU.

@listing src/seqlz_decompress.c 349-353

The decode table goes into the output page, `dst`: nothing writes the page before all literals are
decoded into the scratch, and then the table is no longer needed. The buckets, one row of 256 bytes
for each code length 1 to 10 and one row for "none", go into the scratch, which the literals overwrite
once the table is built. The `static_assert` checks that both fit, {{own.dt.bytes|grp}} bytes in a page,
and one more for a page at an odd address, and {{own.dt.bucket_bytes|grp}} in the scratch. `OWN_STEP_MAX` is the most bits one step can take, 16,
a run's code of up to 8 bits and its up to 8 bits more.
@assert own.dec.step_max == 16 and own.dt.bytes == 2048 and own.dt.bucket_bytes == 3072

@listing src/seqlz_decompress.c 355-381

The comment says why it is a function of its own: inlined into `decode_literals()`, its locals made
that function's stack frame larger than the kernel allows with KASAN, which puts a red zone around
each local ([chapter 15](#ch-zram-glue--check) has the limit and the frames `check.sh` reports).

### The loop: up to two lengths per lookup

@listing src/seqlz_decompress.c 382-396

The description is read through `bits`, a 64-bit window whose top `avail` bits are the next ones, as
the sequences' bit reader does it ([chapter 12](#ch-dec-basics--bit-reader)). When fewer than 16 are
left, it loads 8 bytes big-endian at `p + at` and keeps the `n` whole bytes that fit below 64:
`(63 - avail) >> 3` is 7 when `bits` is empty, and the mask `~(~0ULL >> (avail + 8n))` clears what is
below them. `avail + 8n` is at most 63, so the shift is never by 64, which C leaves undefined. Near
the end of the input, `lit_load_tail()` reads what is there and zeros after it
([below](#ch-literals-dec--refill)); `min(at, len)` keeps the pointer in the page even when `at` has
run past its end.

@listing src/seqlz_decompress.c 397-410

`top` is the next 8 bits, and `quad[top]` ([chapter 7](#ch-internal--own)) says what they hold. Bit 31
means a run that does not fit an entry, e.g. a run of 32: symbol 15's 8-bit code and 5 bits more. Then
`decode[top]` has the symbol and the code's length, and the run's `nb - 1` bits are read from right
after the code: `bits <<= d >> 8` drops the code, and `(bits >> 1) >> (64 - nb)` is the top `nb - 1`
bits. Two shifts, because for a run of 1, `nb` is 1, and one shift by 64 would be undefined; `bits >>
1` has a 0 on top, so a shift by 63 gives the 0 that `read(0)` is. `b` is the next byte value, and a
run only moves it on.

@listing src/seqlz_decompress.c 411-425

A step with lengths takes the first one, `l1`, and the run before it, and puts byte `b` into the bucket
of length `l1`. A complete code may end after this length, and the bits after it are then the zero
padding, which `quad[]` would read as more lengths: `two` is whether the code space has room
left after `l1`. Only then does the step take the second length and its run, else `l2` is
`OWN_QUAD_NONE`, and `k`, the bits the step takes, is the end of the first or of the second. The
second byte goes into bucket `l2` either way, and for `OWN_QUAD_NONE` that is the row nobody reads, its
`full >> 11` is 0 code space, and `b` moves on by `l2 != OWN_QUAD_NONE`, 0. So one length or two take
the same instructions, and the loop has one branch per step besides the refill, the one for a long
run. `& 255U` keeps a bucket's index in its row for any input; a valid table has at most 256 bytes.

The text page's table of [chapter 4](#ch-format--own), step by step:

| step | `top` | `quad[top]` | what the step does | `b` | `space` |
| --- | --- | --- | --- | --- | --- |
| 0 | `{{own.dec.0.top}}` | `{{own.dec.0.e}}` | a run alone, of {{own.dec.0.run}}, {{own.dec.0.used}} bits | | {{own.dec.0.space}} |
| 1 | `{{own.dec.1.top}}` | `{{own.dec.1.e}}` | length {{own.dec.1.l1}} for byte {{own.dec.1.b1}}, the space, {{own.dec.1.used}} bits | {{own.dec.1.b1}} | {{own.dec.1.space}} |
| 2 | `{{own.dec.2.top}}` | `{{own.dec.2.e}}` | a run alone, of {{own.dec.2.run}}, {{own.dec.2.used}} bits | | {{own.dec.2.space}} |
| 3 | `{{own.dec.3.top}}` | `{{own.dec.3.e}}` | length {{own.dec.3.l1}} for byte {{own.dec.3.b1}}, `a`, {{own.dec.3.used}} bits | {{own.dec.3.b1}} | {{own.dec.3.space}} |
| 4 | `{{own.dec.4.top}}` | `{{own.dec.4.e}}` | a run alone, of {{own.dec.4.run}}, {{own.dec.4.used}} bits | | {{own.dec.4.space}} |
| ... | | | | | |
| 19 | `{{own.dec.19.top}}` | `{{own.dec.19.e}}` | length {{own.dec.19.l1}} for byte {{own.dec.19.b1}}, `u`, {{own.dec.19.used}} bits | {{own.dec.19.b1}} | {{own.dec.19.space}} |
@assert own.dec.steps == 20 and own.dec["0"].kind == "alone" and own.dec["1"].kind == "one" and own.dec["4"].run == 2 and own.dec["19"].space == 1024 and own.dec.bits == own.text.desc_bits

The text page is a poor case for `quad[]`: its lengths of 3 and 4 bits have codes of 7 bits, so no
entry has two of them, and its runs of 2 have one bit after a 5-bit code that, with the next length,
does not fit 8 bits either. The pages `OWN` was made for have lengths of 7 to 10 bits, with codes of 2
and 3 bits, where an entry such as `quad[0xd1]`, a run of 1 and the lengths 9 and 10, does three
symbols in one lookup ([chapter 7](#ch-internal--own)).

### The checks, at the end

@listing src/seqlz_decompress.c 426-429

The loop checks nothing about the bytes, so it ends for any input: each step takes at least one code,
and past the end of the input every code is `{{own.code.8}}`, a length of 9 bits, 2 per 8 zero bits,
until the code space is full. Then `used`, the bits read, decides. The table is invalid if the code
space overflowed, if more than 256 byte values were named, if the bits ran past the input, or if the
bits after the last symbol in its byte are not 0: they are the top `8 - used % 8` bits of `bits`. On
the text page `used` is {{own.dec.bits}}, so {{own.text.desc_pad}} bits of the last byte must be 0, and
the table ends after {{own.text.dbytes}} bytes. These are the rules of [chapter 4](#ch-format--own),
which `fuzz/seqlz_diff_fuzz.c` checks against the reference decoder.

### Writing the entries, length by length

@listing src/seqlz_decompress.c 430-449

The buckets hold, for each length, the byte values with that length, in order of their value, which is
the order of their canonical codes. Writing all entries of length 1, then of length 2, and so on, from
the start of `lt`, puts each byte at the range its code indexes: a code of `l` bits fills
$1024 \gg l$ entries that start with it ([chapter 2](#ch-compression--table-decoding)). The entry is
the same for all of them, `l | byte << 8`, and the multiply by `0x0001000100010001` copies it into the
4 lanes of a `u64`, so one 8-byte store writes 4 entries. Codes of 1 to 8 bits have 512 down to 4
entries, a multiple of 4.

@listing src/seqlz_decompress.c 450-463

Codes of 9 bits have 2 entries, one 4-byte store, and codes of 10 bits one. A loop for each, and none
with a branch on how many entries a byte has: with one loop over all lengths and that branch, level 3
read 1.5% slower warm than the experiment before it, in userspace on the Mi 9T's Cortex-A76; with a
loop per length, 2.7% faster, on 15th October 2026
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-a-lazy-matcher-and-a-literal-table-of-the-pages-own-zstd-3s-memory-in-less-than-half-its-time-on-the-phone-kept)). On the text page:

| byte | code | entries | entry |
| --- | --- | --- | --- |
| the space | `{{own.text.code.20}}` | {{own.dt.20.first}} to {{own.dt.20.last}} | `{{own.dt.20.entry}}` |
| `l` | `{{own.text.code.6c}}` | {{own.dt.6c.first}} to {{own.dt.6c.last}} | `{{own.dt.6c.entry}}` |
| `u` | `{{own.text.code.75}}` | {{own.dt.75.first}} to {{own.dt.75.last}} | `{{own.dt.75.entry}}` |
| `a` | `{{own.text.code.61}}` | {{own.dt.61.first}} to {{own.dt.61.last}} | `{{own.dt.61.entry}}` |
| ... | | | |
| `t` | `{{own.text.code.74}}` | {{own.dt.74.first}} to {{own.dt.74.last}} | `{{own.dt.74.entry}}` |
@assert own.dt["20"].first == 0 and own.dt["74"].last == 1023

The table is built for one page and not kept: 2 KiB of stores for every read of such a page, where a
page with a fixed table loads its 2 KiB table, which `decode_literals()` prefetches
([below](#ch-literals-dec--main)).

::: measured Building a table of the page's own: 1.7 µs on the A76
The 3657 own tables of one phone dump, built alone in userspace, 20 times each: the first version, a
lookup per symbol, the bytes sorted by length, then the fill, took 2543 to 2677 ns per table on the Mi
9T's Cortex-A76 and about 7 µs on the A55. A bucket per length instead of the sort took that to about
2.1 µs on the A76, two lengths per lookup and the checks after the loop to 1689 to 1740 ns, 36% less
than the first version; reading the description was about 60% of the first version's time. A lookup of
10 bits instead of 8 was 5% to 6% faster with its table warm, and not kept: 4 KiB of table are 64 more
cache lines for a cold read, which was not measured. Against a description by code length, level 3
reads 2.9% slower on the A76 and 1.0% on the A55, for pages 0.5% smaller
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-pages-own-literal-table-in-the-order-of-the-bytes-decoded-two-lengths-at-a-time-05-smaller-than-by-code-length-reads-29-slower-on-the-a76-kept)).
:::

## decode_literals(), from the header to the bitstream {#ch-literals-dec--main}

@listing src/seqlz_decompress.c 465-474

`n_lit` is the page's u16 without the `SEQLZ_LIT_CODED` bit. A static function with one caller is
normally inlined; inlined, the 8 streams competed for registers with the loop over the sequences, and
made pages with raw literals slower too, 12% when the comment still had a number on 7th October 2026,
with a setup that is not recorded. Until 10th October the function was also aligned to 64 bytes; measured
again, that made less than 0.02 µs of difference in the kernel VM, and it was removed (commit `bad6224`).

@listing src/seqlz_decompress.c 475-493

A page of 2 bytes has no byte 2 and gets width 5, which the check below rejects. The streams are arrays
`ip[]`, `start[]`, `sz[]` and `b[]`; with constant indices after unrolling, the compiler keeps them in
registers like any variable. Until 10th October 2026 they were 8 variables each, `i0` to `i7` and `b0` to
`b7`. The arrays made the function 105 lines shorter and read coded pages 0.1 µs faster on the A55, within
the noise of code placement in the kernel VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).

@listing src/seqlz_decompress.c 494-519

Bit 7 of byte 2 and the codes 9 to 15 are reserved: a decoder that accepted them now could not tell
old pages from new ones later. `n_lit` must fit the scratch. Code 8 builds the page's own table
([above](#ch-literals-dec--own)) into `page`, the output page that `seqlz_decode()` passes down for
this, from its first even address: the table is read as `u16`, and a caller may decode into a page at
an odd address, which zram does not, but a test does. Then `q`, the start of the streams, moves behind
the table's description. The codes 0 to 7 are a
fixed table; `& (SEQLZ_LIT_SETS - 1U)` changes none of them and keeps the index inside `t->lit[]`
whatever the value. The prefetch of the 2 KiB table goes out before the sizes are read, so its misses
overlap with that work; the table never starts a line, so it covers {{ch13.lt.lines}} lines, the last
one asked for by the prefetch of `end - 1` ([chapter 12](#ch-dec-basics--prefetch)). The sizes are
read from byte 3 on, the streams start at `q`. `total` is at most $8 \times 4095$, and `q - s` at most
a page, so the check of the streams against `src_len` cannot wrap in 64 bits, and after it every stream
ends inside the input. Whether the bitstream after them is long enough is the sequence loop's
business.
@assert ch13.lt.mod8 != 0 and ch13.lt.lines == 33 and ch13.lt.prefetches == 33 and ch13.lt.loop_prefetches == 32

### Full rounds: 8 streams, 5 literals each

@listing src/seqlz_decompress.c 520-534

Stream `i` in step `j` of the round at `k` holds literal `k + 8 * j + i`, so the scratch gets the literals
in page order with no shuffle. The loops have constant bounds, and the compiler unrolls them into 8
refills and 40 lookups, where each `b[i]` depends only on the `b[i]` before it: 8 chains.

@anim streams wide The {{slab.lits}} literals of chapter 16's page, dealt out to 8 streams and decoded a round at a time, or in one stream. Literals, code lengths and stream sizes come from the real codec; the count of dependent steps is a model, one step per decoded literal.

### The rest: the same steps for all 8 streams

@listing src/seqlz_decompress.c 535-562

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
  reads up to it. Table indices are 10 bits, a fixed table's number 3 bits. A page's own table is read
  from the input only up to its end, and the steps past it read zeros
  ([above](#ch-literals-dec--own)).
- **Reading past a stream is allowed, and caught.** A refill near the end of stream 3 loads bytes of
  stream 4 or of the bitstream, still inside the input. A stream that uses them as codes fails
  `lit_stream_ends()`. The format allows exactly this
  ([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#coded-literals)). A damaged
  stream's `ip` can even move past `end`; its refills then read nothing, and the check rejects it.
- **Writes stay in the scratch.** Every write is `out[at]` with `at < n_lit + 8`, and `n_lit` is at most
  a page. `own_table()` writes its buckets at a masked index in the scratch, and its decode table only
  once the code is complete, exactly 1024 entries, into the page.
- **The work is bounded.** No loop depends on the codes: `n_lit / 40` rounds and one tail. The loop of
  `own_table()` depends on them, and ends anyway: every step takes a code, and every 8 bits past the
  input are two lengths of 9 bits, which fill the code space after at most 512 of them.

The fuzz targets allocate the scratch and the page at their exact sizes, so AddressSanitizer reports a
read or write of one byte too far ([chapter 15](#ch-zram-glue--fuzzing)).

## What decoding the literals costs {#ch-literals-dec--cost}

::: measured 8 streams against 4
When `seqlz-2` got its 8 literal tables on 24th September 2026, the streams went from 4 to 8.
With 4, a literal took about 2.9 cycles. With 8, the decode loop over 2000 pages in the cache went from
9070 to 8676 cycles per page (median of 5, userspace; `seqlz-1` took 7316), and cold reads in the
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
