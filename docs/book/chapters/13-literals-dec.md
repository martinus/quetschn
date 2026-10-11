# Decoding eight streams side by side {#ch-literals-dec}

A page with coded literals has its literals Huffman coded in 8 streams, literal `i` in stream `i % 8`
([chapter 11](#ch-literals-enc) wrote them). Before the decoder can copy a single sequence, it needs
the literals as plain bytes. So `seqlz_decode()` first calls `decode_literals()`, which decodes all
of them into the scratch, a buffer of `SEQLZ_SCRATCH` bytes of work memory, and returns where the
sequences' bitstream starts. From then on the main loop ([chapter 14](#ch-decode)) reads the literals
from the scratch as if the page had stored them raw.

Decoding one Huffman code is a chain: look up the next 10 bits in the literal table, write the byte,
shift the stream by the code's length. Only after the shift are the next code's bits known. 8
streams are 8 such chains with nothing between them, and the CPU works on them at the same time.
This chapter covers lines 245 to 427 of `src/seqlz_decompress.c`: the representation of a stream,
its refill, the decode of one literal, the header with the stream sizes, the check that each stream
ends where its size says, and `decode_literals()` itself. The page layout is in
[chapter 4](#ch-format), the literal table's entries in [chapter 7](#ch-internal--lit-table), and the
prefetch and the sequences' bit reader, `seqlz_br_refill()` and friends, in
[chapter 12](#ch-dec-basics).

## A stream is a pointer and 64 bits with a 1 at the end {#ch-literals-dec--sentinel}

@listing src/seqlz_decompress.c 245-253

The comment describes how one stream is kept while it is decoded, in two values:

- `ip`, a pointer into the page, to the byte the 64 bits were loaded from;
- `bits`, a u64: the 8 bytes at `ip` read big-endian, so that the stream's next bit is the top bit,
  with the lowest bit set to 1, and shifted left by the bits of the byte at `ip` that were used
  already.

Each decoded code shifts `bits` left by its length. Zeros come in from the right, and the 1 that the
refill put at the bottom moves up by the same amount. So the position of that 1, counted from bit 0,
is always the number of bits used since `ip`: the bits used in `ip`'s first byte when it was loaded,
plus every code since. `__builtin_ctzll(bits)`, count trailing zeros, finds it in one instruction on
x86-64 and two on arm64. The 1 is called a sentinel: it marks the end of the valid bits, so that the stream
needs no separate counter.

The sequences' bit reader of [chapter 12](#ch-dec-basics) has a counter, `count`, next to its 64 bits
and its pointer: three values. For one bitstream that is fine. For 8 streams it would be 24 values,
and x86-64 has 16 general purpose registers, some of which the loop needs for other things: the
table, the output, the loop counter. The comment's "With a counter, 8 streams did not fit into the
registers of x86-64" is that observation. With the sentinel, the 8 `bits` values stay in registers,
and the 8 pointers, which only a refill needs, can live on the stack ([explored-designs.md, "8 streams
decode
faster"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).

The idea is zstd's: its fast decoder of Huffman coded literals keeps each of its 4 streams the same
way, a pointer and 64 bits with a marker bit. zstd reads its streams from the end backwards; seqlz
reads forwards, most significant bit first, as it reads the sequences' bitstream.

The last sentence of the comment is the invariant the whole design rests on: a refill leaves at
least 56 valid bits above the 1, and between two refills the codes never take more than that, so
they never reach the 1, and `bits` is never 0. Both matter. If a code used the 1, the count would be
wrong; and `__builtin_ctzll(0)` is undefined.

Here is one stream in three states, from the page that [chapter 11](#ch-literals-enc--example) built:
201 literals, 200 random letters and a zero byte, coded with literal table 0. Stream 0 starts at byte
8 of the compressed page with the bytes `43 56 4e e0 00 96 ca 9f 45 9b 28`.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 250" font-size="12" role="img" aria-label="the 64 bits of stream 0 after a refill, after 5 codes, and after the next refill, with the sentinel 1 moving up and down">
  <text x="100" y="30">after the first refill: 0x43564ee00096ca9f, ctz = 0</text>
  <rect x="100" y="38" width="567" height="28" fill="none" stroke="currentColor"/>
  <text x="383" y="57" text-anchor="middle">63 bits of the stream, from ip = byte 8</text>
  <g class="acc">
    <rect x="667" y="38" width="9" height="28" class="fill-acc" stroke="currentColor"/>
  </g>
  <text x="688" y="57">1</text>
  <text x="100" y="80">bit 63</text>
  <text x="676" y="80" text-anchor="end">bit 0</text>

  <text x="100" y="110">after 5 codes of 29 bits: 0x0012d953e0000000, ctz = 29</text>
  <rect x="100" y="118" width="306" height="28" fill="none" stroke="currentColor"/>
  <text x="253" y="137" text-anchor="middle">34 bits not used yet</text>
  <g class="acc">
    <rect x="406" y="118" width="9" height="28" class="fill-acc" stroke="currentColor"/>
  </g>
  <rect x="415" y="118" width="261" height="28" class="fill-sheet" stroke="currentColor"/>
  <text x="545" y="137" text-anchor="middle">29 zeros shifted in</text>
  <text x="410" y="162" text-anchor="middle">bit 29</text>

  <text x="100" y="192">next refill: ip += 29 &gt;&gt; 3 = 3, shift by 29 &amp; 7 = 5: 0x0012d953e8b36520</text>
  <rect x="100" y="200" width="522" height="28" fill="none" stroke="currentColor"/>
  <text x="253" y="219" text-anchor="middle">the same 34 bits, then 24 new ones</text>
  <g class="acc">
    <rect x="622" y="200" width="9" height="28" class="fill-acc" stroke="currentColor"/>
  </g>
  <rect x="631" y="200" width="45" height="28" class="fill-sheet" stroke="currentColor"/>
  <text x="626" y="244" text-anchor="middle">bit 5</text>
  <line x1="406" y1="200" x2="406" y2="228" stroke="currentColor" stroke-dasharray="2 2"/>
</svg>
</div>
<figcaption>Stream 0's 64 bits: the sentinel 1 (accent) marks how many bits were used since ip; a refill moves ip by the whole bytes and shifts by the rest.</figcaption>
</figure>

## lit_refill(): 8 new bytes, ip moved by the whole bytes used {#ch-literals-dec--refill}

The file has `lit_load_tail()` first, because `lit_refill()` calls it. We go the other way round.

@listing src/seqlz_decompress.c 268-283

`lit_refill()` takes a stream's pointer by address, its bits by value, and `end`, the end of the
whole compressed page, `s + src_len`, not the end of the stream. It returns the new bits and moves
`*ip`.

- `used = __builtin_ctzll(bits)` is the number of bits used since `*ip`, the sentinel's position.
- `p = *ip + (used >> 3)` moves the pointer by the whole bytes among them. `used >> 3` is
  `used / 8`.
- If 8 bytes are left before `end`, `get_unaligned_be64(p)` loads them, big-endian, so the byte at `p`
  is the top byte and its first bit the top bit ([chapter 5](#ch-compat--endian)). Else
  `lit_load_tail()` builds the same value from fewer bytes. `end - p` is a signed `ptrdiff_t`, so a
  `p` past `end` gives a negative number and also takes the tail path.
- `| 1U` sets the lowest bit, the new sentinel. That replaces the last bit of the 8th byte; that bit is
  not lost, because the next refill moves `ip` at most 7 bytes on and loads that byte again.
- `<< (used & 7U)` drops the `used % 8` bits of the first byte that were already used. The sentinel
  moves up with them, to bit `used & 7`, which is the right count for the new `ip`.

After a refill the valid bits are bits 63 down to `(used & 7) + 1`, which is `63 - (used & 7)`, at least
56. `SEQLZ_LIT_ROUNDS` in `seqlz.h` is `56 / SEQLZ_LIT_BITS`, `56 / 10 = 5`: 5 codes of at most 10 bits
are 50 bits, which always fit ([chapter 6](#ch-header)). The decoder refills each stream once per 5
literals of it.

The first refill of a stream starts from `bits = 1`. Its `ctz` is 0, so `p` is the stream's start and
nothing is shifted. In the example, stream 0's first 8 bytes are `43 56 4e e0 00 96 ca 9f`; the last
byte is odd already, so `| 1` changes nothing, and the bits are `0x43564ee00096ca9f`. Then 5 codes:

| top 10 bits | entry of table 0 | code length | literal |
| --- | --- | --- | --- |
| `0100001101` | `0x6f05` | 5 | `o` |
| `0110101011` | `0x6406` | 6 | `d` |
| `1011001001` | `0x6807` | 7 | `h` |
| `0011101110` | `0x6e05` | 5 | `n` |
| `0111000000` | `0x6c06` | 6 | `l` |

The 5 codes took 29 bits, and the bits are now `0x0012d953e0000000`, with the sentinel at bit 29. The
next refill:

```text
used   = ctz(0x0012d953e0000000) = 29
p      = ip + 29 >> 3 = ip + 3                 byte 11 of the page
load   = e0 00 96 ca 9f 45 9b 28 -> 0xe00096ca9f459b28
| 1    = 0xe00096ca9f459b29
<< 5   = 0x0012d953e8b36520                    29 & 7 = 5, the sentinel at bit 5
```

The top 34 bits are the same before and after: the refill drops nothing that was not used, and
appends the 24 bits that follow. The shift by 5 drops the first 5 bits of byte 11, `11100`, which
were the end of the code of `l`, `0` from byte 10 and `11100` from byte 11.

::: cpu A refill without a branch on the stream's data
`lit_refill()` has one `?:`, on `end - p >= 8`. That is true for every refill except near the end of
the input, so the branch predictor guesses it right. The rest is a `ctz`, an add, a load, an OR and a
shift, with no decision that depends on the codes. The refills come at fixed points, once per 5
literals of a stream, and the loop around them has a constant trip count, so no refill has to ask
whether enough bits are left.
:::

### lit_load_tail(): the last bytes, out of the loop

@listing src/seqlz_decompress.c 254-266

When fewer than 8 bytes of the input are left at `ip`, an 8-byte load would read past the end of the
compressed page, which `seqlz_decode()` promises never to do ([chapter 6](#ch-header)): the bytes
after it belong to someone else, or are not mapped at all.
`lit_load_tail()` copies the bytes that are there into a zeroed array of 8 and loads that, so the
missing bytes read as zeros. The loop condition `ip + k < end` stops at the end; if `ip` is already at
or past `end`, it copies nothing.

Two attributes keep it out of the hot code. `lit_refill()` is `__always_inline` and is used in two
loops over the 8 streams, which the compiler can unroll; `noinline` stops it from copying the byte loop
of `lit_load_tail()` into each of those copies, where it would almost never run. `__cold` tells the
compiler that the function is rarely called, so it can place it away from the hot loop and lay out
the branch that calls it as the unlikely one ([chapter 5](#ch-compat)).

When does it run? The streams are followed by the sequences' bitstream, so a stream's refill comes
within 8 bytes of the end only on pages whose bitstream is short. The example page is one: its
bitstream has 6 bytes, the page 151, and stream 7 starts at byte 129 with 16 bytes. Its last refill
is at byte 144, 7 bytes before the end, and goes through `lit_load_tail()`.

## lit_decode(): one literal, or one that does not count {#ch-literals-dec--decode}

@listing src/seqlz_decompress.c 285-298

`lt` is the decode table of the page's literal table, 1024 entries of a u16 each
([chapter 7](#ch-internal--lit-table)), built by `build_lit()` ([chapter 8](#ch-codes)). An entry is
`length | byte << 8`. A code of $l$ bits fills all $2^{10-l}$ entries whose index starts with the
code, so one lookup with the next 10 bits finds the code, whatever bits follow it. That is the table
decoding of [chapter 2](#ch-compression--table-decoding).

- `bits >> (64U - SEQLZ_LIT_BITS)` is `bits >> 54`, the top 10 bits, a number below 1024. The index is
  in range for any value of `bits`, so a damaged page cannot make this read go wrong.
- `*out = (u8)(e >> 8)` writes the byte.
- `bits << (e & mask)` moves the stream on. `mask` is 63 or 0.

With `mask = 63`, `e & 63` is the code length, 1 to 10. The length sits in bits 0 to 7 and the byte
from bit 8 on, so masking with 63 gives the same number, and it tells the compiler that the shift is
below 64, as in the encoder ([chapter 11](#ch-literals-enc--streams)).

With `mask = 0`, the shift is by 0: the literal is decoded and written, but the stream stays where it
was. That is a way to say "decode, but do not advance" without a branch, used once per page at the
end of `decode_literals()`. A caller computes `mask` with arithmetic, not with an `if`.

## The sizes of the 8 streams {#ch-literals-dec--sizes}

@listing src/seqlz_decompress.c 300-323

The header of a page with coded literals has, from byte 3 on, `width` bytes that hold the 8 stream
sizes, `width` bits each, the lowest bit first ([chapter 4](#ch-format)). `lit_stream_sizes()` reads
them, and computes where each stream starts.

- `h` is 16 bytes, zeroed. `SEQLZ_SIZE_BITS_MAX + 4U` is `12 + 4`.
- `__builtin_memcpy(h, s + SEQLZ_LIT_HEADER(0), width)` copies the `width` header bytes from byte 3
  of the page into it. `SEQLZ_LIT_HEADER(0)` is `3`.
- Size `k` starts at bit `k * width` of that little-endian number. `k * width / 8` is the byte it
  starts in, `k * width % 8` the bit within that byte. `get_unaligned_le32()` loads 4 bytes from there,
  the shift drops the bits below the field, and `(1U << width) - 1U` keeps `width` bits.
- `start[k] = q + total` is where stream `k` begins, `q` being the first byte after the header, and
  `total` grows by each size. The function returns the sum of all 8 sizes.

A 4-byte load holds the field: the field starts at most 7 bits into the first byte and has at most
12 bits, 19 bits in all, below 32. The furthest load is for `k = 7` with `width = 12`, at byte
`84 / 8 = 10`, reading bytes 10 to 13. The page has only `width` bytes of sizes, and right after them,
in a short page, can come the end of the input. That is why the sizes are copied first, as the comment
says: in `h`, bytes 10 to 15 exist and are zero.

The example page has `width = 5` and the size bytes `33 c6 18 63 84`:

| `k` | bit `k * 5` | byte | shift | `le32` at that byte | size |
| --- | --- | --- | --- | --- | --- |
| 0 | 0 | 0 | 0 | `0x6318c633` | `0x33 & 31 = 19` |
| 1 | 5 | 0 | 5 | `0x6318c633` | 17 |
| 2 | 10 | 1 | 2 | `0x846318c6` | 17 |
| 3 | 15 | 1 | 7 | `0x846318c6` | 17 |
| 4 | 20 | 2 | 4 | `0x00846318` | 17 |
| 5 | 25 | 3 | 1 | `0x00008463` | 17 |
| 6 | 30 | 3 | 6 | `0x00008463` | 17 |
| 7 | 35 | 4 | 3 | `0x00000084` | `0x84 >> 3 = 16` |

The zeros at the top of the last loads are `h`'s zeros, not bytes of the page. The starts are 8, 27,
44, 61, 78, 95, 112 and 129, and the total is 137.

## Does a stream end where its size says? {#ch-literals-dec--ends}

@listing src/seqlz_decompress.c 325-341

The format says a stream's codes must end in its last byte: they take at most `8 * size` bits and
fewer than 8 bits less, and the bits after the last code are 0 ([format.md, "Coded
literals"](https://github.com/martinus/quetschn/blob/main/docs/format.md#coded-literals)).
`lit_stream_ends()` checks that after all literals are decoded.

- The bits a stream used are the bytes `ip` moved past since `start`, times 8, plus the sentinel's
  position, `__builtin_ctzll(b)`.
- `left` is the stream's size in bits minus that. It is a `long`, so that a stream that used more
  than its size gives a negative number instead of wrapping around. `8L` makes the products `long`
  too.
- `left >= 0 && left <= 7`: the stream used its last byte, and not more.
- `left == 0 ||`: the codes ended exactly at a byte boundary, nothing to check. Else the low `left`
  bits of the stream's last byte must be 0. `(1U << left) - 1U` is a mask of `left` ones, and
  `start[sz - 1U]` is the stream's last byte. With `left >= 1` the stream has at least one byte, so
  `sz - 1` does not wrap.

The trailing bits are read from the page, not from `b`. The comment says why: if the stream's last
byte was the 8th byte of its last load, its lowest bit is where the refill put the sentinel, and `b`
cannot tell whether the page had a 0 or a 1 there.

The example, stream 0: size 19, so 152 bits. At the end `ip` is 17 bytes past the start and the
sentinel is at bit 11, so 147 bits were used, and `left = 152 - 147 = 5`. The last byte is byte 26 of
the page, `0x80`, and `0x80 & 0x1f = 0`: the stream is valid. Three damaged versions of the page, each
rejected by `seqlz_decode()` with `-EINVAL` in a test program:

| change | what `lit_stream_ends()` sees |
| --- | --- |
| byte 26 is `0x81` | `left = 5`, `0x81 & 0x1f = 1`: a bit after the codes is set |
| stream 0's size is 18 | `left = 144 - 147 = -3`: the codes ran into the next stream |
| stream 0's size is 20 | `left = 160 - 147 = 13`: a whole byte after the codes |

A stream without literals, on a page with fewer than 8, has used 0 bits: its `left` is `8 * size`,
valid only for size 0.

::: why The exact check, and what it cost
Until 5th October 2026 the last round decoded 5 literals in every stream, also behind each stream's
last literal, and the check allowed for those bits: up to 50 bits past a stream's size, so a stream
one byte too short was decoded with the next stream's bits and accepted. Now the codes must fit the
size exactly, which is also a rule the format can state without the bytes that follow. On the Mi 9T
it cost 1.1% of the A55's decode cycles per page, 16 357 to 16 540, and nothing on the A76 (commit
`acfd49b`). zram stores no checksum, so this check and the end of the sequences' bitstream are what
tell a damaged page from a valid one.
:::

## decode_literals(), from the header to the bitstream {#ch-literals-dec--main}

@listing src/seqlz_decompress.c 343-352

`decode_literals()` takes the tables, the compressed page `s` with its length, the number of literals
`n_lit` from the page's u16 (`seqlz_decode()` has cleared the top bit, `SEQLZ_LIT_CODED`), and `out`,
the scratch. It returns a pointer to the first byte of the sequences' bitstream, or `NULL` for an
invalid page.

It is `noinline`. Its only caller is `seqlz_decode()`, and a static function with one caller is
normally inlined. Inlined, its 8 streams and their pointers would compete for registers with the loop
over the sequences in the same function, and the compiler's choices there made pages with raw
literals slower too, which never call it. When the comment still had its number, on 7th October 2026,
it said pages with raw literals were 12% slower that way; the setup of that measurement is not
recorded. Until 10th October the function was also aligned to 64 bytes, after two kernels with the
same decoder source read pages 250 ns apart at p50; measured again, the alignment made less than
0.02 µs of difference in the VM, and it was removed (commit `bad6224`).

### The width, and the first checks

@listing src/seqlz_decompress.c 353-371

- `end` is one past the last byte of the input. Every read of this function stays below it.
- `width` comes from byte 2, bits 3 to 5: `(s[2] >> SEQLZ_LIT_WIDTH_AT) & 7`, where 7 is written as
  `SEQLZ_SIZE_BITS_MAX - SEQLZ_SIZE_BITS_MIN`, plus 5. `src_len > 2U ?` guards the read of `s[2]`: a
  page of 2 bytes has no byte 2. It then gets width 5, and the check below rejects it, because
  `SEQLZ_LIT_HEADER(5)` is 8 bytes.
- `q` is the first byte after the header, where stream 0 starts.
- `ip[]`, `start[]`, `sz[]` and `b[]` are the 8 streams: pointer, start, size, bits. As arrays, the
  code is written once for all 8 streams. Until 10th October 2026 they were 8 variables each,
  `i0` to `i7` and `b0` to `b7`, for fear that arrays would not stay in registers; that was never
  measured. Measured, the arrays read coded pages 0.1 µs faster on the A55 and were within the noise
  of code placement in the kernel VM, and the function got 105 lines shorter ([explored-designs.md,
  "Five
  simplifications"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
  With constant indices after unrolling, the compiler can keep array elements in registers like any
  variable.
- `i`, `k`, `j`, `rest`, `steps` and `r` are loop counters and the tail's numbers; `total` the sum of
  the sizes; `lt` the decode table.

@listing src/seqlz_decompress.c 372-384

The checks come before anything is read from the streams:

- `src_len < SEQLZ_LIT_HEADER(width)`: the header itself must fit. `||` stops here for a short page,
  before `s[2]` is read in the next test.
- `s[2] >> SEQLZ_LIT_ZERO_AT`: bits 6 and 7 of byte 2 must be 0. They are reserved for a later format,
  and a decoder that accepted them now could not tell old pages from new ones then.
- `n_lit > SEQLZ_PAGE`: the u16 has 15 bits for the count, up to 32 767, and the scratch has room for
  a page plus 16 bytes.

`lt = t->lit[s[2] & (SEQLZ_LIT_SETS - 1U)].decode` picks the literal table by bits 0 to 2. Any value
of those 3 bits is a valid table, so there is nothing to check.

`prefetch_lines(lt, sizeof(t->lit[0].decode))` asks for the 2 KiB table at once, 32 prefetches 64
bytes apart ([chapter 12](#ch-dec-basics--prefetch)). A literal table never starts at a cache line, so
it covers 33 lines, and the last one, with entries 1022 and 1023, is not asked for; chapter 12's warn
box has the offsets. In a swap-in the table is often not in the cache, and
the lookups below would otherwise miss one line after the other. The prefetch goes out before the
sizes are read, so the misses overlap with that work. On the Mi 9T's A55, the in-order core, cold
reads were 14.5 µs slower without the decoder's table prefetches ([explored-designs.md, item 4 of "Six
choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#six-choices-made-on-the-pc-measured-on-the-phone-the-token-table-and-the-prefetches-matter-the-rest-does-not)).

Then the sizes and starts, and each stream's pointer at its start with `b[i] = 1`: only the sentinel,
at bit 0, so the first refill loads from `start[i]` without a shift.

`SEQLZ_LIT_HEADER(width) + total > src_len`: the streams must fit in the input. `total` is a u64 and
at most $8 \times 4095$, so the sum cannot wrap. After this check every `start[i] + sz[i]` is at most `end`,
which is what makes reading each stream's last byte in `lit_stream_ends()` safe. The bitstream after
the streams may be empty here; whether it is long enough is the sequence loop's business.

### Full rounds: 8 streams, 5 literals each

@listing src/seqlz_decompress.c 385-399

A round is 40 literals, 5 from each stream, `SEQLZ_LIT_STREAMS * SEQLZ_LIT_ROUNDS`. The loop runs while
a whole round is left.

- First every stream is refilled. After that each has at least 56 bits, enough for its 5 codes.
- Then 5 steps; in each step every stream decodes one literal. Stream `i` in step `j` of the round
  that starts at literal `k` holds literal `k + 8 * j + i`, so `out[at]` is the literal's place in
  page order: the scratch gets the literals in the order the sequences use them, with no shuffle
  afterwards.

The loops over `j` and `i` have constant bounds, 5 and 8, and the compiler can unroll them into 40
lookups and 8 refills in a row. Each `b[i] = lit_decode(...)` depends only on the `b[i]` before it.
That is the point of the whole format choice: the 8 chains are independent, and an out-of-order CPU
starts the lookup of stream 1 while stream 0's is still waiting for the cache ([chapter 3](#ch-cpu)).

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 330" font-size="12" role="img" aria-label="one stream is one chain of 40 dependent lookups; 8 streams are 8 chains of 5 that run side by side">
  <text x="20" y="22">1 stream</text>
  <g>
    <rect x="40" y="40" width="44" height="26" fill="none" stroke="currentColor"/>
    <text x="62" y="58" text-anchor="middle">0</text>
    <rect x="40" y="90" width="44" height="26" fill="none" stroke="currentColor"/>
    <text x="62" y="108" text-anchor="middle">1</text>
    <rect x="40" y="140" width="44" height="26" fill="none" stroke="currentColor"/>
    <text x="62" y="158" text-anchor="middle">2</text>
    <rect x="40" y="190" width="44" height="26" fill="none" stroke="currentColor"/>
    <text x="62" y="208" text-anchor="middle">3</text>
    <rect x="40" y="240" width="44" height="26" fill="none" stroke="currentColor"/>
    <text x="62" y="258" text-anchor="middle">4</text>
    <line x1="62" y1="66" x2="62" y2="88" stroke="currentColor"/>
    <line x1="62" y1="116" x2="62" y2="138" stroke="currentColor"/>
    <line x1="62" y1="166" x2="62" y2="188" stroke="currentColor"/>
    <line x1="62" y1="216" x2="62" y2="238" stroke="currentColor"/>
    <line x1="62" y1="266" x2="62" y2="288" stroke="currentColor"/>
    <text x="62" y="306" text-anchor="middle">...</text>
    <text x="62" y="322" text-anchor="middle">39</text>
    <text x="105" y="58">each waits</text>
  </g>
  <text x="180" y="22">8 streams: 8 chains side by side</text>
  <g class="acc">
    <rect x="180" y="30" width="520" height="20" class="fill-acc" fill-opacity="0.2" stroke="currentColor"/>
    <text x="440" y="45" text-anchor="middle">lit_refill() of all 8 streams</text>
  </g>
  <text x="203" y="76" text-anchor="middle">s0</text>
  <text x="268" y="76" text-anchor="middle">s1</text>
  <text x="333" y="76" text-anchor="middle">s2</text>
  <text x="398" y="76" text-anchor="middle">s3</text>
  <text x="463" y="76" text-anchor="middle">s4</text>
  <text x="528" y="76" text-anchor="middle">s5</text>
  <text x="593" y="76" text-anchor="middle">s6</text>
  <text x="658" y="76" text-anchor="middle">s7</text>
  <g>
    <rect x="183" y="90" width="40" height="26" class="fill-sheet" stroke="currentColor"/><text x="203" y="108" text-anchor="middle">0</text>
    <rect x="248" y="90" width="40" height="26" class="fill-sheet" stroke="currentColor"/><text x="268" y="108" text-anchor="middle">1</text>
    <rect x="313" y="90" width="40" height="26" class="fill-sheet" stroke="currentColor"/><text x="333" y="108" text-anchor="middle">2</text>
    <rect x="378" y="90" width="40" height="26" class="fill-sheet" stroke="currentColor"/><text x="398" y="108" text-anchor="middle">3</text>
    <rect x="443" y="90" width="40" height="26" class="fill-sheet" stroke="currentColor"/><text x="463" y="108" text-anchor="middle">4</text>
    <rect x="508" y="90" width="40" height="26" class="fill-sheet" stroke="currentColor"/><text x="528" y="108" text-anchor="middle">5</text>
    <rect x="573" y="90" width="40" height="26" class="fill-sheet" stroke="currentColor"/><text x="593" y="108" text-anchor="middle">6</text>
    <rect x="638" y="90" width="40" height="26" class="fill-sheet" stroke="currentColor"/><text x="658" y="108" text-anchor="middle">7</text>

    <rect x="183" y="140" width="40" height="26" fill="none" stroke="currentColor"/><text x="203" y="158" text-anchor="middle">8</text>
    <rect x="248" y="140" width="40" height="26" fill="none" stroke="currentColor"/><text x="268" y="158" text-anchor="middle">9</text>
    <rect x="313" y="140" width="40" height="26" fill="none" stroke="currentColor"/><text x="333" y="158" text-anchor="middle">10</text>
    <rect x="378" y="140" width="40" height="26" fill="none" stroke="currentColor"/><text x="398" y="158" text-anchor="middle">11</text>
    <rect x="443" y="140" width="40" height="26" fill="none" stroke="currentColor"/><text x="463" y="158" text-anchor="middle">12</text>
    <rect x="508" y="140" width="40" height="26" fill="none" stroke="currentColor"/><text x="528" y="158" text-anchor="middle">13</text>
    <rect x="573" y="140" width="40" height="26" fill="none" stroke="currentColor"/><text x="593" y="158" text-anchor="middle">14</text>
    <rect x="638" y="140" width="40" height="26" fill="none" stroke="currentColor"/><text x="658" y="158" text-anchor="middle">15</text>

    <rect x="183" y="190" width="40" height="26" fill="none" stroke="currentColor"/><text x="203" y="208" text-anchor="middle">16</text>
    <rect x="248" y="190" width="40" height="26" fill="none" stroke="currentColor"/><text x="268" y="208" text-anchor="middle">17</text>
    <rect x="313" y="190" width="40" height="26" fill="none" stroke="currentColor"/><text x="333" y="208" text-anchor="middle">18</text>
    <rect x="378" y="190" width="40" height="26" fill="none" stroke="currentColor"/><text x="398" y="208" text-anchor="middle">19</text>
    <rect x="443" y="190" width="40" height="26" fill="none" stroke="currentColor"/><text x="463" y="208" text-anchor="middle">20</text>
    <rect x="508" y="190" width="40" height="26" fill="none" stroke="currentColor"/><text x="528" y="208" text-anchor="middle">21</text>
    <rect x="573" y="190" width="40" height="26" fill="none" stroke="currentColor"/><text x="593" y="208" text-anchor="middle">22</text>
    <rect x="638" y="190" width="40" height="26" fill="none" stroke="currentColor"/><text x="658" y="208" text-anchor="middle">23</text>

    <rect x="183" y="240" width="40" height="26" fill="none" stroke="currentColor"/><text x="203" y="258" text-anchor="middle">24</text>
    <rect x="248" y="240" width="40" height="26" fill="none" stroke="currentColor"/><text x="268" y="258" text-anchor="middle">25</text>
    <rect x="313" y="240" width="40" height="26" fill="none" stroke="currentColor"/><text x="333" y="258" text-anchor="middle">26</text>
    <rect x="378" y="240" width="40" height="26" fill="none" stroke="currentColor"/><text x="398" y="258" text-anchor="middle">27</text>
    <rect x="443" y="240" width="40" height="26" fill="none" stroke="currentColor"/><text x="463" y="258" text-anchor="middle">28</text>
    <rect x="508" y="240" width="40" height="26" fill="none" stroke="currentColor"/><text x="528" y="258" text-anchor="middle">29</text>
    <rect x="573" y="240" width="40" height="26" fill="none" stroke="currentColor"/><text x="593" y="258" text-anchor="middle">30</text>
    <rect x="638" y="240" width="40" height="26" fill="none" stroke="currentColor"/><text x="658" y="258" text-anchor="middle">31</text>

    <rect x="183" y="290" width="40" height="26" fill="none" stroke="currentColor"/><text x="203" y="308" text-anchor="middle">32</text>
    <rect x="248" y="290" width="40" height="26" fill="none" stroke="currentColor"/><text x="268" y="308" text-anchor="middle">33</text>
    <rect x="313" y="290" width="40" height="26" fill="none" stroke="currentColor"/><text x="333" y="308" text-anchor="middle">34</text>
    <rect x="378" y="290" width="40" height="26" fill="none" stroke="currentColor"/><text x="398" y="308" text-anchor="middle">35</text>
    <rect x="443" y="290" width="40" height="26" fill="none" stroke="currentColor"/><text x="463" y="308" text-anchor="middle">36</text>
    <rect x="508" y="290" width="40" height="26" fill="none" stroke="currentColor"/><text x="528" y="308" text-anchor="middle">37</text>
    <rect x="573" y="290" width="40" height="26" fill="none" stroke="currentColor"/><text x="593" y="308" text-anchor="middle">38</text>
    <rect x="638" y="290" width="40" height="26" fill="none" stroke="currentColor"/><text x="658" y="308" text-anchor="middle">39</text>
  </g>
  <g>
    <line x1="203" y1="116" x2="203" y2="138" stroke="currentColor"/><line x1="268" y1="116" x2="268" y2="138" stroke="currentColor"/><line x1="333" y1="116" x2="333" y2="138" stroke="currentColor"/><line x1="398" y1="116" x2="398" y2="138" stroke="currentColor"/><line x1="463" y1="116" x2="463" y2="138" stroke="currentColor"/><line x1="528" y1="116" x2="528" y2="138" stroke="currentColor"/><line x1="593" y1="116" x2="593" y2="138" stroke="currentColor"/><line x1="658" y1="116" x2="658" y2="138" stroke="currentColor"/>
    <line x1="203" y1="166" x2="203" y2="188" stroke="currentColor"/><line x1="268" y1="166" x2="268" y2="188" stroke="currentColor"/><line x1="333" y1="166" x2="333" y2="188" stroke="currentColor"/><line x1="398" y1="166" x2="398" y2="188" stroke="currentColor"/><line x1="463" y1="166" x2="463" y2="188" stroke="currentColor"/><line x1="528" y1="166" x2="528" y2="188" stroke="currentColor"/><line x1="593" y1="166" x2="593" y2="188" stroke="currentColor"/><line x1="658" y1="166" x2="658" y2="188" stroke="currentColor"/>
    <line x1="203" y1="216" x2="203" y2="238" stroke="currentColor"/><line x1="268" y1="216" x2="268" y2="238" stroke="currentColor"/><line x1="333" y1="216" x2="333" y2="238" stroke="currentColor"/><line x1="398" y1="216" x2="398" y2="238" stroke="currentColor"/><line x1="463" y1="216" x2="463" y2="238" stroke="currentColor"/><line x1="528" y1="216" x2="528" y2="238" stroke="currentColor"/><line x1="593" y1="216" x2="593" y2="238" stroke="currentColor"/><line x1="658" y1="216" x2="658" y2="238" stroke="currentColor"/>
    <line x1="203" y1="266" x2="203" y2="288" stroke="currentColor"/><line x1="268" y1="266" x2="268" y2="288" stroke="currentColor"/><line x1="333" y1="266" x2="333" y2="288" stroke="currentColor"/><line x1="398" y1="266" x2="398" y2="288" stroke="currentColor"/><line x1="463" y1="266" x2="463" y2="288" stroke="currentColor"/><line x1="528" y1="266" x2="528" y2="288" stroke="currentColor"/><line x1="593" y1="266" x2="593" y2="288" stroke="currentColor"/><line x1="658" y1="266" x2="658" y2="288" stroke="currentColor"/>
  </g>
  <text x="140" y="108" text-anchor="end">j = 0</text>
  <text x="140" y="158" text-anchor="end">j = 1</text>
  <text x="140" y="208" text-anchor="end">j = 2</text>
  <text x="140" y="258" text-anchor="end">j = 3</text>
  <text x="140" y="308" text-anchor="end">j = 4</text>
</svg>
</div>
<figcaption>The literals of one round, by page index: with one stream every lookup waits for the one before; with 8, a box waits only for the box above it, and the 8 boxes of a row can run at once.</figcaption>
</figure>

### The rest: the same steps for all 8 streams

@listing src/seqlz_decompress.c 400-427

After the full rounds fewer than 40 literals are left. `rest` is how many, `steps = rest / 8` how many
complete steps of all 8 streams they make, and `r = rest % 8` how many streams have one literal more.

- All streams are refilled once more. At most `steps + 1 <= 5` codes follow, so the 56 bits suffice.
- `steps` full steps, as in the rounds above, and `k` moves past them.
- One last step for all 8 streams, with `mask = (0U - (r > i)) & 63U`. `r > i` is 1 or 0. `0U - 1U`
  wraps to `0xffffffff` in unsigned arithmetic, which is well defined in C, and `& 63` makes it 63;
  `0U - 0U` is 0. So streams 0 to `r - 1`, the ones that have a literal left, decode it and move on;
  the others decode one too, write it to `out[k + i]`, and stay where they were.
- Every stream's end is checked with `lit_stream_ends()`, and the function returns `q + total`, the
  byte after the last stream: the start of the sequences' bitstream.

The example page has 201 literals. The rounds run for `k = 0, 40, 80, 120, 160`, since `160 + 40 = 200
<= 201`, and stop at `k = 200`. Then `rest = 1`, `steps = 0`, `r = 1`. In the last step stream 0 decodes
literal 200, the zero byte, with mask 63. Streams 1 to 7 decode whatever their next 10 bits say into
`out[201]` to `out[207]` (streams 1 to 6 got `20`, a space, stream 7 got `64`, a `d`) and do not move
on, so that
`lit_stream_ends()` sees exactly their own codes.

Those extra bytes land behind the literals. The highest index the last step writes is `k + 7`, and
`k = n_lit - r`, so at most `n_lit + 7`, at most `SEQLZ_PAGE + 7` = 4103 on 4 KiB pages. The scratch is
`SEQLZ_SCRATCH`, a page plus 16 bytes, 4112 ([chapter 6](#ch-header)). The 16 spare bytes are there for
the sequence loop's 16-byte copies, which may read past the last literal; the last step's writes use
the same room. A valid page never takes them as literals. The slow path of the sequence loop checks
every `ll` against the number of literals left; the fast path does not, and a page whose literals run
past `lit_end` there is rejected by the slow path at its end, which checks `lit > lit_end`
([chapter 14](#ch-decode)).

Why decode into the spare bytes, instead of a loop over the remaining streams only? A loop "for each
stream with a literal left" ends after `r` iterations, and `r` is different on every page, 0 to 7. A
branch predictor cannot learn that, and each page would pay a misprediction or more at the loop's end.
The same steps for all streams cost 8 lookups at most, and no decision that depends on the page,
except `r > i`, which is arithmetic. Measured on the Mi 9T (commit `8dc0341`), median of 7 runs of
20 000 pages: the A55 decoded a page in 16 620 cycles this way against 16 531 with a loop per stream,
the A76 in 7420 against 7480, all within their spread. So the form was chosen for its reason, not for a
measured gain; it also keeps the size check exact without the loop.

## Why nothing reads or writes outside its buffer {#ch-literals-dec--safety}

`seqlz_decode()` promises to be safe for any input ([chapter 6](#ch-header)). For `decode_literals()`
that comes down to four facts.

**Reads stay inside the input.** The header is read only after `src_len >= SEQLZ_LIT_HEADER(width)` is
checked, and the sizes from a copy. A refill loads 8 bytes only when `end - p >= 8`, where `end` is the
end of the whole input; else `lit_load_tail()` reads the bytes before `end` and nothing else. A table
index is the top 10 bits of a u64, always below 1024, and the table number is 3 bits, always below 8.

**Reading past a stream's end is allowed, and caught.** A refill near the end of stream 3 loads bytes
of stream 4 or of the bitstream. That is still inside the input, and harmless as long as the stream
does not use those bits as codes. If it does, because the page is damaged or made up, `lit_stream_ends()`
finds that it used more bits than its size, and the page is rejected. The format allows exactly this:
"A decoder may read past the end of a stream, as long as it checks this" ([format.md, "Coded
literals"](https://github.com/martinus/quetschn/blob/main/docs/format.md#coded-literals)). A stream of
a damaged page can even move its `ip` a few bytes past `end`; its refills then read nothing, and the
check rejects it, because the streams were checked to end inside the input.

**Writes stay inside the scratch.** All writes go to `out[at]` with `at < n_lit + 8`, and
`n_lit <= SEQLZ_PAGE` was checked.

**The work is bounded.** The loops run `n_lit / 40` rounds and at most one tail, and `n_lit` is at most
a page. No loop depends on the codes, so no input makes the function run longer than a page of
literals takes.

The fuzz targets of the repository check this on every input they make, with a scratch and page
allocated at their exact sizes, so that AddressSanitizer reports a read or write of a single byte too
far ([chapter 6](#ch-header) on `seqlz_decode()`'s contract).

## What decoding the literals costs {#ch-literals-dec--cost}

::: measured 8 streams against 4
When `seqlz-fast-lit` got its 8 literal tables on 24th September 2026, the streams went from 4 to 8.
With 4, a literal took about 2.9 cycles to decode: four chains of a shift, a table load and a shift.
With 8, the decode loop over 2000 pages in the cache went from 9070 to 8676 cycles per page (median of
5, userspace; `seqlz-fast` took 7316), and in the kernel cold reads went from 2940 to 2860 ns at p50
and from 4810 to 4531 ns at p99. The header grew by 8 bytes then, 0.1 to 0.2 points, when a size took
2 bytes ([explored-designs.md, "seqlz-fast-lit: one of 8 literal tables per
page"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).

Measured again on 5th October 2026 in the Mi 9T's own kernel, 4 streams saved 2.7 bytes per page on
the phone's pages; the A76 read 0.26 µs slower, the A55 1.1 µs faster, both within the spread between
rounds, and the PC's hot loop decoded 2.8% slower, 7627 against 7418 cycles per page
([item 3 of "Six
choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#3-4-literal-streams-instead-of-8-27-bytes-no-faster-not-kept)).
And on 10th October in the kernel VM, next to a second copy of the same source to measure where the
code lands: 4 streams read 1.8% slower with gcc 16.2.1 and 0.7% slower with clang 22.1.8, for 0.1% to
0.2% less memory ([explored-designs.md, "Five
simplifications"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
A format change for 2.7 bytes and no time is not worth it, so the format stays at 8. The 8 streams
have nothing to do with the 8 literal tables; that both are 8 is a coincidence.
:::

On the Cortex-A55, an in-order core, the loop of a round is 312 instructions for 40 literals, in
userspace built with clang 21 from NDK r30. Per literal that is a shift for the index, the load, a
shift for the byte, the store and the shift of the stream, 5 instructions, plus 2.8 for the refills
and the stream pointers, which do not fit into registers there and go to the stack. The A55 runs
them at about one per cycle, and a page with coded literals takes about 8400 cycles more to decode
than the same pages with raw literals, about 8 cycles per literal. `decode_literals()` has 14% of the
decoder's samples, spread over its instructions with no hot spot. Putting the 8 loads of a step first
and their uses after saved 2.8% of the decode cycles on the A55, and was not built into the kernel
module ([explored-designs.md, item 8 of "Six
choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#8-coded-literals-on-the-a55-78-instructions-per-literal-at-one-per-cycle)).

The fixed cost per page matters more on that core than the cost per literal: the 2 KiB table has to
come from memory, and the literals go through the scratch before the sequences copy them into the
page. That is why the encoder codes literals only where they save 51 bytes and $\frac{1}{16}$ of themselves
([chapter 11](#ch-literals-enc--rule)). Decoding the literals into the end of the output page instead,
which would save the scratch's memory, was built: on 4 KiB pages it made cold reads 0.08 to 0.22 µs
slower in the kernel, and it was not kept ([explored-designs.md, "16 KiB pages,
tuned"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#16-kib-pages-tuned-c5-does-not-hold-the-literals-in-the-page-would-fix-it-not-kept)).
