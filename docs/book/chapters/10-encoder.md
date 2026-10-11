# The encoder writes the page from both ends {#ch-encoder}

The matcher of [chapter 9](#ch-matcher) finds a page's sequences; the encoder in
`src/seqlz_compress.c` writes them down in the format of [chapter 4](#ch-format). For each sequence it
copies the literals as they are, and it turns `ll`, `ml` and the offset into bits: the code of the
token, the offset's bits, and the length values where the token's fields are not enough. The two run as
one loop, so the encoder gets each sequence the moment it is found, and it never sees the page as a
whole.

That gives it a problem. A compressed page is the u16 with the number of literals, then all literals,
then the bitstream. Where the bitstream starts depends on how many literals there are, and that is known
only after the last sequence. The encoder's answer is to write from both ends of its output buffer: the
literals from the front, the bitstream backwards from the back, and at the end it copies the bitstream,
turned around, to right after the literals. This chapter goes through that machinery: lines 1 to 318 of
the file, and the three functions at its end, lines 594 to 628, that tie it together. Lines 320 to 593
code the literals with Huffman codes, which is [chapter 11](#ch-literals-enc).

## The file's head, and seqlz_find() {#ch-encoder--find}

@listing src/seqlz_compress.c 1-37

Line 1 is the licence line, with `//` as in every `.c` file ([chapter 5](#ch-compat--top)). Line 6 includes
`seqlz_internal.h` ([chapter 7](#ch-internal)) for the layout of `struct seqlz_tables`, which the
encoder reads its codes from, and line 8 the matcher.

Lines 10 to 37 are the first receiver of the matcher's sequences, the simple one. `struct find_ctx` is
an array to fill and a count. `find_emit()` has the signature of `emit_fn` from `page_lz.h`, so it can be
passed to `seqlz_match_page()`, and it stores the three numbers of each sequence into the next element.
It does not need the literals: `(void)literals;` on line 23 says so to the compiler, which would
otherwise warn about an unused parameter. The casts to `u16` on lines 24 to 26 are exact: `ll`, `ml`
and the offset are at most the page size, 4096 or 16384, and `u16` holds up to 65535. `ctx` arrives as
a `void *` and line 21 turns it back into the struct it is; in C that conversion needs no cast.

`seqlz_find()` sets up the context with a designated initializer, `{ .seq = seq }`, which sets every
field it does not name to 0, so `n` starts at 0. It runs the matcher on the page and returns how many
sequences it found. Its array must have room for `SEQLZ_MAX_SEQUENCES`, 1025 on 4 KiB pages; chapter 9
shows why no page has more. As said [there](#ch-matcher--emit), `find_emit()` is inlined into the matcher's
loop, so this costs no call per sequence.

`seqlz_find()` is not for zram. The table trainer (`bench/seqlz_train_main.cpp`) counts the symbols of
exactly these sequences, and the round trip fuzzer checks that `seqlz_encode()` of these sequences gives
the same bytes as `seqlz_compress()`. The kernel's copy does not have it: `port.py` cuts lines 10 to 37.

## Literals from the front, bits from the back {#ch-encoder--layout}

@listing src/seqlz_compress.c 39-74

The comment on lines 41 to 48 describes the layout, and this is what it looks like in the buffer `dst`
while the encoder works and after it is done:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 222" role="img" aria-label="While encoding, the literals grow from the front of dst after 2 bytes left for the u16, and the bitstream grows backwards from the end of dst; at the end the bitstream is copied, turned around, to right after the literals.">
  <text x="20" y="26" font-size="12">while encoding</text>
  <rect x="20" y="34" width="40" height="36" fill="none" stroke="currentColor" stroke-width="1" stroke-dasharray="4 3"/>
  <rect class="fill-sheet" x="60" y="34" width="240" height="36" stroke="currentColor" stroke-width="1"/>
  <rect x="300" y="34" width="170" height="36" fill="none" stroke="currentColor" stroke-width="1"/>
  <g class="mark"><rect class="fill-sheet" x="470" y="34" width="230" height="36" stroke="currentColor" stroke-width="1.5"/></g>
  <g font-size="12" text-anchor="middle">
    <text x="40" y="56">u16</text>
    <text x="180" y="56">literals, in order →</text>
    <text x="385" y="56">free, ≥ ll + 32</text>
  </g>
  <g class="mark" font-size="12" text-anchor="middle"><text x="585" y="56">← bitstream, byte 0 last</text></g>
  <g font-size="11">
    <text x="20" y="86">d</text>
    <text x="60" y="86" text-anchor="middle">d+2</text>
    <text x="300" y="86" text-anchor="middle">lit</text>
    <text x="470" y="86" text-anchor="middle">p</text>
    <text x="700" y="86" text-anchor="end">d+dst_cap</text>
  </g>
  <text x="20" y="136" font-size="12">after encoder_finish()</text>
  <g class="acc"><rect class="fill-sheet" x="20" y="144" width="40" height="36" stroke="currentColor" stroke-width="1.5"/></g>
  <rect class="fill-sheet" x="60" y="144" width="240" height="36" stroke="currentColor" stroke-width="1"/>
  <g class="mark"><rect class="fill-sheet" x="300" y="144" width="230" height="36" stroke="currentColor" stroke-width="1.5"/></g>
  <rect x="530" y="144" width="170" height="36" fill="none" stroke="currentColor" stroke-width="1" stroke-dasharray="4 3"/>
  <g font-size="12" text-anchor="middle">
    <g class="acc"><text x="40" y="166" font-size="10.5">n_lit</text></g>
    <text x="180" y="166">literals</text>
    <text x="615" y="166">not part of the page</text>
  </g>
  <g class="mark" font-size="12" text-anchor="middle"><text x="415" y="166">bitstream, byte 0 first →</text></g>
  <g font-size="11">
    <text x="20" y="196">d</text>
    <text x="300" y="196" text-anchor="middle">d+2+n_lit</text>
    <text x="530" y="196" text-anchor="middle">d+len</text>
  </g>
  <g class="mark" fill="none" stroke="currentColor" stroke-width="1.5">
    <path d="M 585 72 C 585 112, 430 104, 418 140"/>
    <polygon class="fill-mark" points="417,144 413,134 422,136"/>
  </g>
  <g class="mark" font-size="11"><text x="716" y="106" text-anchor="end">copied, byte order</text><text x="716" y="120" text-anchor="end">turned around</text></g>
</svg>
</div>
<figcaption>The buffer while encoding, and after encoder_finish(): the bitstream moves from the back to right after the literals.</figcaption>
</figure>

`lit` is where the next literal goes, `p` is where the bitstream ends, and the next byte of bits goes
right below `p`. Byte 0 of the bitstream, the first one the decoder reads, is the last byte of `dst`,
byte 1 the one before it, and so on. The two grow toward each other.

Here is the same with a real page, the slab of task records from [chapter 16](#ch-trace), one sequence
per step. The buttons give `dst` the two pages of zram, the smallest size the page fits into, and 1 byte
less.

@anim ends wide The buffer dst while seqlz compresses chapter 16's page with raw literals, after each sequence, then encoder_finish(). The numbers come from the real encoder, through anim/ends.c; the close-ups show the 16 bytes at each end.

::: why One pass, any buffer
The obvious alternatives cost something. Two passes, the matcher into an array and then the encoder,
were 6% slower ([chapter 9](#ch-matcher--emit)). Writing the bitstream into a second buffer needs more
work memory per CPU. Until 8th October 2026 the encoder wrote the bitstream forwards, starting behind
room for a page of literals, at `d + 2 + 4096`, and moved it down at the end; that needed a `dst` of two
pages and refused anything smaller. zram gives two pages. zswap, the kernel's compressed cache in front
of another swap device, gives one, and `lz4`, `lzo` and `zstd` take any buffer and fail when the page
does not fit ([#122](https://github.com/martinus/quetschn/issues/122)). The layout from both ends takes any `dst`,
writes the same bytes, and made writes in the kernel VM 0.16 to 0.24 µs faster per page; on the Mi 9T
they stayed within 0.22 µs
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-into-a-buffer-of-any-size-the-bitstream-from-the-back-the-same-bytes-in-zram-writes-2-faster-in-the-vm-kept),
commit `66aab11`).
:::

**The room check.** Lines 50 to 59: before each sequence the encoder checks that its literals and
`ENC_ROOM` bytes more fit between `lit` and `p`. If not, the page does not fit, nothing more is written,
and `seqlz_compress()` returns 0, as `lz4_compress_default()` does for a buffer that is too small. The 32
bytes are what one sequence can write beyond its literals, counted in bytes:

- The literal copies (lines 180 to 187) copy 16 bytes at a time and may write up to 16 bytes past the
  last literal, so the literals' side reaches at most `lit + ll + 16`.
- A sequence flushes the accumulator once or twice. Each flush stores 8 bytes right below `p` and then
  moves `p` down by the whole bytes in the accumulator, at most 7 (at most 58 bits, 60 on 16 KiB pages,
  see below). So two flushes write no lower than `p - 7 - 8 = p - 15`.

With `p - lit >= ll + 32`, the literals end at or below `p - 16`, and the bits start at or above `p - 15`:
they never meet, with 1 byte spare. Lines 52 to 57 of the comment give this count. Until 11th October
the comment said the flushes "move the bitstream down by at most 16 bytes, writing 8 bytes below it",
which read that way adds up to 40 and not to 32 (#180). "It can happen up to `ENC_ROOM` bytes before dst is full" means that a page can fail
although its compressed form would have fit with up to 32 bytes to spare. Running the compressor on
about 22 500 made-up pages, each into the smallest `dst` it would take, that smallest `dst` was at most
31 bytes larger than the compressed page; the page of pointers from chapter 9 compresses to 817 bytes
and needs a `dst` of 847.

**Why two pages are always enough.** Lines 59 to 64 are a proof, and it is worth doing the arithmetic.

1. Every sequence but the last covers at least 4 bytes of the page: its match is at least 4 bytes long.
2. A sequence's token and offset take at most 31 bits. The token's code has at most 11 bits
   (`SEQLZ_TOKEN_BITS`) and the offset at most 12 (class 3 on 4 KiB pages): 23. An escaped token is the
   escape's code, then the token in 12 bits, then the offset: `SEQLZ_MAX_ESCAPE_LEN` is defined in
   `seqlz.h` as `31 - 12 - 12 = 7`, the longest escape code that keeps this at 31, and
   `seqlz_tables_init()` marks tables with a longer escape as unfit for the encoder (`all_symbols`, see
   [chapter 8](#ch-codes)). The tables of today give the escape 4 bits, so the real maximum is 28.
3. Length values come on top, but only with long sequences. An `ll` value (at most $8 + 12 = 20$ bits)
   comes only with 15 literals or more, and those 15 bytes alone are worth $15 / 4 \times 31 = 116$ bits of the
   budget; an `ml` value only with a match of 35 bytes or more, worth 271 bits. So "31 bits per 4 bytes
   covered" holds for every sequence, with its length values.
4. The last sequence has no match and may cover 0 bytes, but it has no bits at all, not even a
   token.

| | 4 KiB pages | 16 KiB pages |
| --- | --- | --- |
| bits at most, `SEQLZ_PAGE / 4 * 31` | $1024 \times 31 = 31\,744$ | $4096 \times 31 = 126\,976$ |
| bytes of bitstream at most | 3968 | 15 872 |
| the u16 + every byte a literal + that bitstream | $2 + 4096 + 3968 = 8066$ | $2 + 16\,384 + 15\,872 = 32\,258$ |
| two pages | 8192 | 32 768 |
| left over, must be ≥ `ENC_ROOM` | 126 | 510 |

The bound adds the most literals and the most bits, which no page has at once, so the real margin is
larger. A page of random bytes, the worst case for literals, has almost no sequences.

**The accumulator.** Lines 66 to 72 describe how bits get to memory, which the next sections go through
line by line: bits are collected in a 64-bit variable and written 8 bytes at a time; then `p` moves down
by the whole bytes, and the at most 7 bits left over are written again with the next ones. The numbers at
the end of the comment are the budget of one sequence: at most 7 bits left over, plus at most 31 for
token and offset, plus at most 20 for a match length value (8 bits of code and 12 extra bits on 4 KiB
pages), is 58 bits, which fits into 64. On 16 KiB pages the extra bits are 14, and it is 60. A literal
length value would add 20 or 22 more and could overflow, so it gets a flush of its own.

## The encoder's state {#ch-encoder--state}

@listing src/seqlz_compress.c 76-84

Seven fields, and the comments say what each is. `t` is the tables, read only. `acc` and `cnt` are the
bits not yet written: `cnt` of them count, in the low bits of `acc`, the newest lowest. Bits above
`cnt` may hold old bits that were written already; they are shifted out before they could matter.
`p` and `lit` are the two ends of the free room in the diagram above. `src_end` is how far the literal
copies may read: the end of the page in the compressor, the end of the caller's literals in
`seqlz_encode()`. `last` is the repeat offset as the encoder sees it, so that it can tell class 0 from
the others. The matcher has its own copy of the same number, `last` in `seqlz_match_page()`, and both
start at 1.

In the compressor the struct lives on the stack of `compress_page()`, and the matcher's loop and all of
`encode_emit()` are inlined into it. The second round of work on the compressor found that the matcher
leaves the encoder no registers, so "all its state lives on the stack"
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).

## Bits into a 64-bit accumulator {#ch-encoder--put}

@listing src/seqlz_compress.c 86-109

`enc_put()` appends `n` bits: it shifts the accumulator left by `n` to make room, and ORs `v` into the low
bits. Two things the caller must make sure of: `v` has no bits set above bit `n - 1`, else they would
corrupt the bits before them, and `n < 64 - cnt`, else bits fall out of the top before they are written.
The comment states the second; every call below keeps both.

**The masks.** `n & 63U` does nothing for any `n` the encoder uses, all of them below 64. It is there for
the kernel's sanitizer: with `CONFIG_UBSAN_SHIFT` the compiler adds a check before every shift it cannot
prove below 64, and the mask lets it drop the check for one AND ([chapter 3](#ch-cpu--ubsan); the rule is in `seqlz_internal.h`,
[chapter 7](#ch-internal)).

`enc_put_code()` appends a code and then extra bits, in one put. `entry` is an element of a
`value_table`'s `enc[]`, which holds the code in its low 16 bits and the code's length in bits 16 to 19;
line 102 takes the length out. Line 104 masks the number of extra bits to 31, for the same reason as
above: `1U << n_extra` on line 107 must not shift by 32. The value put on lines 105 to 108 is the code
shifted left to make room for the extra bits, ORed with the low `n_extra` bits of `extra`;
`(1U << n_extra) - 1U` is a mask of `n_extra` ones. The code goes first because the decoder reads it
first: the bitstream is read from its most significant bit, and the newest bits are the lowest.

A worked example from the page of `ab` repeated, which has a match of 4094 bytes. `ml - 4` is 4090, more
than the 31 the token holds, so a length value follows: $4090 - 31 = 4059$. Its symbol is 23 with 11 extra
bits (`seqlz_len_symbol()`, [chapter 6](#ch-header)): 4059 is in the range 2048 to 4095, $2^{11}$
plus 11 bits. In the default tables symbol 23 of the `ml` table has the 6-bit code `111101`, so its
`enc[]` entry is `0x3d | 6 << 16`.

| | value |
| --- | --- |
| `len` | `0x6003d >> 16 & 15` = 6 |
| `extra & ((1U << 11) - 1U)` | 4059 & 2047 = 2011 = `11111011011` |
| `(u64)0x3d << 11` | `0x1e800` |
| value put | <code>0x1e800 &#124; 2011</code> = `0x1efdb`, 17 bits: `111101 11111011011` |

The top bit of 4059, the 2048, is not sent: the symbol says it.

## enc_flush(): whole bytes go below p {#ch-encoder--flush}

@listing src/seqlz_compress.c 111-123

Three lines. Line 120 shifts the accumulator left so that the `cnt` bits that count are at the top,
bits 63 and down, and the oldest of them is bit 63. Old bits above them fall out, and zeros come in at
the bottom. Then it stores the 64-bit word, little endian, into the 8 bytes right below `p`. A little-
endian store puts the lowest byte of the word first, at `p - 8`, and the top byte last, at `p - 1`. So
the oldest bit lands in bit 7 of the byte right below `p`, the next 7 bits fill that byte, and the bits
after them go to `p - 2`, `p - 3` and so on, downwards. Read from the end of `dst` backwards, the bytes
hold the bits in order, each byte from its top bit down: the order the decoder wants, once the bytes
are turned around at the end.

Line 121 moves `p` down by the whole bytes, `cnt >> 3`. Line 122 keeps the at most 7 bits that do not
fill a byte: `cnt & 7U`. They stay in `acc` and are written again by the next flush, which overwrites
the byte at `p - 1` with those bits and the ones after them. The store also writes up to 7 bytes below
the last full one; those are zeros or partial bits, below `p`, and the next flush overwrites them.

The first sequences of the page of pointers from chapter 9 show it with real numbers. The first sequence
is 9 literals and a match of 7 at offset 8. Its token (`ll` 9, `ml - 4` 3, class 1) is 569, with the
11-bit code `11011000101`, and the offset 8 in class 1 is `1000` in 4 bits: 15 bits,
`110110001011000`, so `acc = 0x6c58` and `cnt = 15`.

- Flush 1: `acc << 49` is `0xd8b0000000000000`. Stored little endian at `p - 8`, the bytes are `00 00
  00 00 00 00 b0 d8`: `d8` at `p - 1`, `11011000`, the first 8 bits; `b0` at `p - 2`, `1011000` and a
  0, the 7 bits left. `p` moves down by `15 >> 3 = 1`, `cnt` is 7.
- The second sequence, 1 literal and 7 bytes at offset 8 again, is token 49 in class 0, which has the
  4-bit code `0001` and no offset bits. `cnt` is 11, `acc` ends in the bits `1011000 0001`.
- Flush 2: `acc << 53` is `0xb020000000000000`. The store writes `b0` at the new `p - 1` (the same
  byte as before, now with all its bits) and `20` below it, `001` and zeros. `p` moves down by 1,
  `cnt` is 3.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 256" role="img" aria-label="The last 10 bytes of dst after the first, second and fourth flush of the page of pointers: d8 at end-1 first, then b0, then 22, each byte written once partly and again fully by the next flush.">
  <g font-size="11" text-anchor="middle">
    <text x="130" y="26">end-10</text><text x="190" y="26">end-9</text><text x="250" y="26">end-8</text><text x="310" y="26">end-7</text><text x="370" y="26">end-6</text><text x="430" y="26">end-5</text><text x="490" y="26">end-4</text><text x="550" y="26">end-3</text><text x="610" y="26">end-2</text><text x="670" y="26">end-1</text>
  </g>
  <g font-size="11">
    <text x="10" y="50">flush 1</text><text x="10" y="64">cnt 15 → 7</text>
    <text x="10" y="120">flush 2</text><text x="10" y="134">cnt 11 → 3</text>
    <text x="10" y="190">flush 4</text><text x="10" y="204">cnt 11 → 3</text>
  </g>
  <g fill="none" stroke="currentColor" stroke-width="1">
    <rect x="100" y="36" width="60" height="32" stroke-dasharray="4 3"/><rect x="160" y="36" width="60" height="32" stroke-dasharray="4 3"/>
    <rect x="220" y="36" width="60" height="32"/><rect x="280" y="36" width="60" height="32"/><rect x="340" y="36" width="60" height="32"/><rect x="400" y="36" width="60" height="32"/><rect x="460" y="36" width="60" height="32"/><rect x="520" y="36" width="60" height="32"/>
    <rect x="100" y="106" width="60" height="32" stroke-dasharray="4 3"/>
    <rect x="160" y="106" width="60" height="32"/><rect x="220" y="106" width="60" height="32"/><rect x="280" y="106" width="60" height="32"/><rect x="340" y="106" width="60" height="32"/><rect x="400" y="106" width="60" height="32"/><rect x="460" y="106" width="60" height="32"/>
    <rect x="100" y="176" width="60" height="32"/><rect x="160" y="176" width="60" height="32"/><rect x="220" y="176" width="60" height="32"/><rect x="280" y="176" width="60" height="32"/><rect x="340" y="176" width="60" height="32"/><rect x="400" y="176" width="60" height="32"/>
  </g>
  <g class="mark" stroke="currentColor" stroke-width="1.5">
    <rect class="fill-sheet" x="580" y="36" width="60" height="32"/>
    <rect class="fill-sheet" x="520" y="106" width="60" height="32"/>
    <rect class="fill-sheet" x="460" y="176" width="60" height="32"/>
  </g>
  <g class="acc" stroke="currentColor" stroke-width="2">
    <rect class="fill-sheet" x="640" y="36" width="60" height="32"/>
    <rect class="fill-sheet" x="580" y="106" width="60" height="32"/><rect class="fill-sheet" x="640" y="106" width="60" height="32"/>
    <rect class="fill-sheet" x="520" y="176" width="60" height="32"/><rect class="fill-sheet" x="580" y="176" width="60" height="32"/><rect class="fill-sheet" x="640" y="176" width="60" height="32"/>
  </g>
  <g font-size="13" text-anchor="middle">
    <text x="130" y="57">?</text><text x="190" y="57">?</text><text x="250" y="57">00</text><text x="310" y="57">00</text><text x="370" y="57">00</text><text x="430" y="57">00</text><text x="490" y="57">00</text><text x="550" y="57">00</text><text x="610" y="57">b0</text><text x="670" y="57">d8</text>
    <text x="130" y="127">?</text><text x="190" y="127">00</text><text x="250" y="127">00</text><text x="310" y="127">00</text><text x="370" y="127">00</text><text x="430" y="127">00</text><text x="490" y="127">00</text><text x="550" y="127">20</text><text x="610" y="127">b0</text><text x="670" y="127">d8</text>
    <text x="130" y="197">00</text><text x="190" y="197">00</text><text x="250" y="197">00</text><text x="310" y="197">00</text><text x="370" y="197">00</text><text x="430" y="197">00</text><text x="490" y="197">20</text><text x="550" y="197">22</text><text x="610" y="197">b0</text><text x="670" y="197">d8</text>
  </g>
  <g stroke="currentColor" stroke-width="2.5">
    <line x1="640" y1="30" x2="640" y2="76"/><line x1="580" y1="100" x2="580" y2="146"/><line x1="520" y1="170" x2="520" y2="216"/>
  </g>
  <g font-size="12"><text x="644" y="86">p</text><text x="584" y="156">p</text><text x="524" y="226">p</text></g>
  <g font-size="11">
    <g class="acc"><rect class="fill-sheet" x="100" y="236" width="14" height="12" stroke="currentColor"/><text x="120" y="246">a full byte, final</text></g>
    <g class="mark"><rect class="fill-sheet" x="300" y="236" width="14" height="12" stroke="currentColor"/><text x="320" y="246">bits left over, written again by the next flush</text></g>
  </g>
</svg>
</div>
<figcaption>The last bytes of dst after flushes 1, 2 and 4 of the page of pointers; p moves down one byte per 8 bits.</figcaption>
</figure>

The next sequences are the same token, 4 bits each: flush 3 leaves `cnt` at 7 and `p` where it was, with
the partial byte `0x22` at `p - 1`, and flush 4 completes it. The page's bitstream starts `d8 b0 22 22
22`, which is what `seqlz_compress()` writes for this page: 817 bytes with raw literals, of which 258 are
the bitstream for 511 sequences.

The same, put by put and flush by flush, for the first sequences of chapter 16's page:

@anim accum wide The accumulator acc, the value each enc_put() appends or the word enc_flush() stores, and the last bytes of dst, for the first 4 sequences of chapter 16's page. The values come from the encoder's own helpers, through anim/accum.c, which checks them against encode_emit().

**The shift count.** `(64U - e->cnt) & 63U` is 64 - `cnt` for `cnt` from 1 to 63. For `cnt` 0, 64 - 0 would
be a shift by 64, undefined, and the mask makes it 0. That happens: a sequence with an `ll` value and
without an `ml` value flushes twice, and the second time there may be no bits at all. Then the whole old
accumulator is stored below `p`, `p` does not move, and the next flush overwrites those 8 bytes: they
are below `p`, and nothing reads them.

Before commit `66aab11` the bitstream went forwards, and the flush had to swap the bytes of the word
first, a big-endian store; now it stores the word as the CPU has it
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-into-a-buffer-of-any-size-the-bitstream-from-the-back-the-same-bytes-in-zram-writes-2-faster-in-the-vm-kept)).
The byte swap moved to `encoder_finish()`, one per 8 bytes of the finished bitstream, once per page.

## Codes for length values and tokens {#ch-encoder--codes}

@listing src/seqlz_compress.c 125-152

`put_len_value()` writes a length value `v` with the table `t`, the one for `ll` or the one for `ml`.
`seqlz_len_symbol()` from `seqlz.h` gives the symbol, 0 to 24 on 4 KiB pages, and the number of extra
bits; the mask `& (ENC_LEN_SYMBOLS - 1U)`, 31, keeps the index into the 32 entries of `enc[]` provably in
range, for UBSan again. Then `enc_put_code()` puts the code and the extra bits. The extra bits are the
bits of `v` below its highest one, and `enc_put_code()` masks `v` to them, so `put_len_value()` can pass
`v` whole.

`token_code()` returns the code of a token and its length. The token table's `enc[]` has one `u16` per
token: the code in the low 12 bits and its length from bit 12 on (`TOK_ENC_LEN_AT`, chapter 7). An entry
is 0 exactly when the token has no code, because a token with a code has a length of at least 1 in the
top bits, even if its code is 0. So line 145 tests for the common case, a token with a code, and returns
`te >> 12` as the length and the low 12 bits as the code.

A token without a code is **escaped** (lines 149 to 151): the escape's code, followed by the token's own
number in 12 bits (`SEQLZ_ESCAPE_BITS`). Both go out as one code: the escape's code shifted left by 12,
ORed with the token, of the escape's length plus 12 bits. In the default tables the escape's code is
`0011`, 4 bits, and 576 of the 3072 tokens have a code of their own. Token 10, `ll` 10 with a match of 4
bytes at the repeat offset, is one without:

| | value |
| --- | --- |
| `t->token.enc[10]` | 0, no code |
| `ee = t->token.enc[3072]` | `0x4003`: length 4, code `0x3` |
| `*len` | 4 + 12 = 16 |
| returned | <code>0x3 &lt;&lt; 12 &#124; 10</code> = `0x300a`: `0011 000000001010` |

The decoder reads the escape's code, then 12 bits, and has the token, see [chapter 14](#ch-decode). A
token with a code is cheaper, `0001` for token 49, the pointer array's sequence.

Both functions are `__always_inline`: they run for every sequence, and inlined, the table loads and the
branch on line 145 sit in the encoder's code directly.

## encode_emit(), one sequence {#ch-encoder--emit}

`encode_emit()` is the second receiver of the matcher's sequences, the one the compressor uses. It is
`__always_inline` and has the signature of `emit_fn`, so the matcher's call to `emit()` becomes this
code, inlined, as chapter 9 showed. It is also called in a plain loop by `encode_raw()` below.

@listing src/seqlz_compress.c 154-174

Line 162 turns `ctx` back into the encoder. Line 164 declares the locals: `k` counts literals copied,
`tlen` is a code's length, `raw_bits` the number of offset bits, `cls` the offset class.

**The room check.** Line 171 is the check of the comment on lines 50 to 56. `e->p - e->lit` is the free
room in bytes, a pointer difference, signed. The cast to `size_t` makes the compare unsigned; the
difference is never negative, because `p` never goes below `lit` (see `encoder_init()` and the callers'
check that `dst_cap` is at least 34). If the room is too small, line 172 sets `p` to `lit` and the
function returns. That is the failure state, and it sticks: the free room is now 0, so every later
sequence fails the same check, writes nothing and returns, and `encoder_finish()` recognises `p == lit`
and returns 0. The matcher does not know; it runs to the end of the page. A page that does not fit takes
as long as one that does, which is fine, because with zram's two pages it never happens.

@listing src/seqlz_compress.c 175-188

**The literals.** Lines 180 to 183 copy the first 16 bytes without a loop, if the input has 16 bytes
left. That copy happens even if the sequence has 1 literal or none: `dst` has room for it (the 16 bytes of
`ENC_ROOM`), and the bytes past `ll` are overwritten by the next sequence's literals or are not part of
the page. Most sequences have few literals: in the page of pointers, 1. For them this is one fixed copy,
one 16-byte or two 8-byte moves depending on the CPU and the compiler flags, and no loop, whose exit branch would depend on `ll` and could mispredict, as the
comment says. `__builtin_memcpy()` with a constant 16 is always inlined; `seqlz.h` explains why seqlz
uses the builtin and not `memcpy()`, which can become a function call per copy with
`CONFIG_FORTIFY_SOURCE` ([chapter 6](#ch-header)).

Lines 184 and 185 copy 16 more bytes per round while there are literals left and the input has 16 bytes
more. Lines 186 and 187 copy the rest one byte at a time; that happens only near the end of the input,
where a 16-byte read would go past `src_end`. Line 188 moves `lit` on by the real count.

The casts `(unsigned int)(e->src_end - in)` turn the pointer difference, at most a page, into the type of
`k + 16U`, so that the compare is between two unsigned numbers of the same width.

@listing src/seqlz_compress.c 190-195

**The last sequence.** The matcher marks it with `ml` 0. Its literals are copied above, like those of
any sequence, and that is all: it has no token, no offset and no length value, so line 195 returns
before anything goes into the bitstream. The decoder knows the last sequence from the literals left,
see [the format chapter](#ch-format--end). Until 11th October 2026 this branch wrote the token
`seqlz_token(ll, 0, 0)`, an `ll` value if there were 15 literals or more, and a flush, about 9 bits per
page.

@listing src/seqlz_compress.c 196-215

**The class and the token.** Line 197 asks `seqlz_off_class()` ([chapter 6](#ch-header)) for the offset's
class and how many of its bits follow the token, without a branch. Line 198 builds the token with that
class and gets its code. For the first sequence of the page of `ab`: `off` 2, `last` 1, so the offset is
new and below 16: class 1, 4 bits. The token is $2 + 16 \times 31 + 512 \times 1 = 1010$
(the match length field is capped at 31), with the 11-bit code `11011100011`.

**Token and offset in one put.** Lines 203 to 206 shift the code left by the number of offset bits and
OR the offset's bits in. The offset is shifted right by `SEQLZ_OFF_SHIFT(cls)`, 3 for classes 4 and 5,
which send the offset divided by 8, else 0. Then it is ANDed with `0U - (cls != 0)`:

| `cls` | `cls != 0` | `0U - (cls != 0)` | effect |
| --- | --- | --- | --- |
| 0 | 0 | `0x00000000` | the offset vanishes |
| 1 to 5 | 1 | `0xffffffff`, unsigned wrap-around | the offset stays as it is |

Class 0 sends no offset bits, `raw_bits` is 0, and without the mask the offset would be ORed straight into
the low bits of the token's code. Classes 1 to 5 need no mask: `seqlz_off_class()` picks a class whose
bits hold the offset, e.g. an offset of 24 with a repeat offset of 8 is class 4, and `24 >> 3 = 3` fits
its 5 bits. The comment on lines 200 and 201 says this. Masking only class 0, instead of masking every
offset to `raw_bits` bits, came with the change that loaded the matcher's values one position ahead
(commit `996248b`); the repository gives no number for this part on its own. For the page of `ab`: `0x6e3 << 4 | 2 = 0x6e32`, 15 bits.

Putting token and offset together saves a shift and an OR per sequence against two puts. In the second
round of work on the compressor, "token and offset in one put" took 21 786 to 21 233 cycles per page on
x86-64
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).

**The length values, and when the flushes come.** With 15 literals or more, lines 207 to 210 put the `ll`
value and flush. With a match of 35 bytes or more, `ml - 4 >= 31`, lines 211 and 212 put the `ml` value.
Line 213 flushes. So a sequence flushes once, or twice when it has an `ll` value, and the bit budget of
the long comment holds at every put:

| put | `cnt` before at most | bits at most | `cnt` after at most |
| --- | --- | --- | --- |
| token and offset | 7 | 31 | 38 |
| `ll` value | 38 | 20 | 58, then a flush leaves 7 |
| `ml` value | 38 | 20 | 58 |
| flush | | | 7 |

`enc_put()` needs `n < 64 - cnt`: $31 < 57$, then $20 < 26$. Without the flush between them, an `ll` value
and an `ml` value after a long escaped token could reach $7 + 31 + 20 + 20 = 78$ bits, and the top bits
would be lost. On 16 KiB pages the values have 22 bits, and the budget is 60.

Line 214 remembers the offset for the next sequence's class 0. In the matcher, `last` changed at the
same moment, so both agree on what the repeat offset is.

## encoder_init() {#ch-encoder--init}

@listing src/seqlz_compress.c 217-227

One assignment of a compound literal: `(struct seqlz_encoder){ ... }` is a struct value written in
place, and every field it does not name is 0, so `acc` and `cnt` start at 0. `p` starts at the end of
`dst`, `lit` right after the 2 bytes of the u16, which is written last. `last` starts at 1, the
format's repeat offset at the start of a page, and the matcher's.

## encoder_finish(): the bitstream turned around {#ch-encoder--finish}

@listing src/seqlz_compress.c 229-253

Line 237: `end` is the end of `dst`, `out` where the bitstream goes, right after the literals. Line 239
counts the literals from where `lit` stands.

Line 242 is the failure check: `p == lit` only after a failed room check, because a successful one
leaves at least 16 bytes between them. Lines 249 and 250 take in the bits that are left, at most 7. They
are in memory already: every sequence ends with a flush, which wrote them as the top bits of the byte
below `p`, with zeros below them. So that byte is the bitstream's last, and `p` only moves down by one to
count it. The format wants exactly this: the bitstream ends in its last byte, and the bits after the
last code are 0, see [when a page is valid](#ch-format--validity). Until 11th October a store here wrote
that byte a second time; without it the bytes were the same on 6000 made-up pages and on the tests,
and only `seqlz_compress()`'s machine code changed (#180).

Line 251 is where the bitstream starts in memory, line 252 how many bytes it has. Line 253 writes the
u16 at the front, now that the count is known, little endian: for the page of pointers 557 literals,
`2d 02`. The top bit, `SEQLZ_LIT_CODED`, stays 0: the literals are raw. If they get coded,
`code_literals()` rewrites the u16 later.

@listing src/seqlz_compress.c 254-277

Byte `k` of the bitstream is at `end - 1 - k`. Line 259 checks whether the place it goes to,
`out .. out + bytes`, ends before the place it comes from begins, `in`. If so the copy can go 8 bytes at a
time (lines 260 to 262): load the 8 bytes at `end - 8 - k` little endian, store them big endian at `out +
k`. A little-endian load makes the byte at the highest address the top byte of the number; a big-endian
store writes the top byte to the lowest address. So the byte at `end - 1 - k` goes to `out + k`, the
byte at `end - 2 - k` to `out + k + 1`, and so on: the 8 bytes are reversed. Compiled with `gcc -O2` for
x86-64 the loop is a load, a `bswap` and a store; `clang -O2` for arm64 uses `rev` for the swap.

For the page of pointers, the last 8 bytes of `dst` are `22 22 22 22 22 22 b0 d8`.
`get_unaligned_le64()` reads them as `0xd8b0222222222222`, and `put_unaligned_be64()` writes `d8 b0 22 22
22 22 22 22` at `out`: the bitstream's first 8 bytes in order. Lines 263 and 264 copy the 0 to 7 bytes
left one at a time, the same reversal.

If the two places overlap, which happens only when the page nearly fills `dst` (the free room left is
less than the bitstream), lines 266 to 274 take the slow way: reverse the bitstream where it is, swapping
the bytes at the two ends and moving inward, then `memmove()` it down, which is defined for overlapping
memory. With the smallest `dst` each of the 22 500 made-up pages above would take, which leaves at most
31 bytes of room and so sends every page with a longer bitstream down this path, the bytes were the same
as with two pages.

Line 276 returns the length of the page: the u16, the literals, the bitstream.

The last steps of the [animation above](#ch-encoder--layout) do all of this for chapter 16's page; its
smallest `dst` takes the in-place path.

`encoder_finish()` is `static` but not inline. It runs once per page, so the call costs nothing that
matters, and it is called from both `compress_page()` and `encode_raw()`.

## encode_raw(): sequences from outside, checked first {#ch-encoder--raw}

`seqlz_encode()` writes a page from sequences the caller made, for tests and tools that try other
matchers or check the format. Its first half is `encode_raw()`.

@listing src/seqlz_compress.c 279-311

Line 289: `dst_cap` must be at least 34 bytes, the u16 and `ENC_ROOM`. That matters for more than
speed: with a smaller `dst`, `p` would start below `lit + 32`, or even below `lit`, and the room check's
`(size_t)` cast would turn a negative room into a huge one. The tables must have a code for every symbol
(`all_symbols`), there must be at least one sequence, at most `SEQLZ_MAX_SEQUENCES`, and at most a page
of literals.

Lines 298 to 311 check the sequences before a single byte is written, by the rules a decoder checks
(see [chapter 14](#ch-decode)): `pos` is the position in the page, `lits` the literals counted. Each
sequence's literals must stay in the page (line 301). The last sequence's match is ignored, it has none
(line 303). Every other one needs a match of at least 4 bytes and an offset from 1 to `pos`, the bytes
before the match (lines 305 to 307). At the end the sequences must fill the page exactly, and use every
literal (line 310).

::: warn Why a match below 4 bytes is not only invalid but dangerous
`encode_emit()` computes `ml - 4` in unsigned arithmetic. For `ml` 3 that wraps to 4 294 967 295, far above
31, so a length value follows, and `seqlz_len_symbol()` gives it 31 extra bits. That breaks the bit
budget of the accumulator and the 31 bits per 4 bytes of the room proof. Before commit `256768e` the
encoder checked only that the literals were there, and 1025 such sequences wrote past the two pages of
`dst`; AddressSanitizer, the compiler's check that stops a program at its first read or write outside
an allocation, reported it in `enc_flush()` ([#121](https://github.com/martinus/quetschn/issues/121)). None of the fuzzers had found it, because each
reached `seqlz_encode()` with valid sequences only. `fuzz/seqlz_encode_fuzz.c` gives it raw sequences now,
and failed within seconds against the old encoder. `seqlz_compress()` was never affected: its matcher
makes only valid sequences.
:::

@listing src/seqlz_compress.c 312-321

Then the encoding is a loop. `encoder_init()` gets `literals + n_literals` as `src_end`, so the 16-byte
copies never read past the caller's literals. Each sequence goes to `encode_emit()` with its literals,
which follow each other in `literals`; the last gets `ml` 0, whatever its `match` field says, and that is
what makes `encode_emit()` write it as the end of the page. `encoder_finish()` returns the length, or 0.

## The three entry points {#ch-encoder--entry}

The file ends with the public functions, after the code of [chapter 11](#ch-literals-enc). Here they are
out of order, because they tie this chapter's code together.

@listing src/seqlz_compress.c 597-607

`seqlz_encode()` is `encode_raw()` and then, if `coded` is set and the page was written,
`code_literals()`, which codes the literals with one of the 8 literal tables if that saves enough, and
returns the new length or `len` unchanged. The conditional operator reads `(len == 0 || !coded) ? len :
code_literals(...)`. `code_literals()` gets the caller's literals as `raw`, a copy it can read while it
writes the coded ones over the raw ones in `dst`.

@listing src/seqlz_compress.c 609-631

`compress_page()` is the compressor's version of `encode_raw()` without the checks: its sequences come
from the matcher and are valid by construction. Line 615 rejects a `dst` below 34 bytes and tables that
cannot write every symbol. Line 617 sets up the encoder with the end of the page as `src_end`, line 618
runs the matcher with `encode_emit()` as its receiver, and that one line is the whole compression loop
once the compiler has inlined it. Line 619 finishes the page.

`seqlz_compress()` is what zram calls, through the glue of [chapter 15](#ch-zram-glue). `coded` is the
level: `false` is `seqlz-fast`, raw literals, `true` is `seqlz-fast-lit`. For the coded literals it passes
no raw copy but `st->literals`, the matcher's 8 KiB table seen as bytes through the union in `struct
seqlz_state`. The matcher is done with it, so `code_literals()` can copy the raw literals there and needs
no memory of its own ([chapter 11](#ch-literals-enc)). Both functions write the same bytes for the same
sequences, and the round trip fuzzer checks that for `seqlz_find()`'s sequences on every input.

The kernel's copy keeps only `compress_page()` and `seqlz_compress()`, the latter renamed to
`seqlz_compress_page()`, because the kernel's interface function, which takes the level and the work
memory, is called `seqlz_compress()`. It also drops the `!t->all_symbols` test from line 615, because
the kernel's tables are checked when they are generated ([chapter 15](#ch-zram-glue--init)).

## The page of ab, bit by bit {#ch-encoder--example}

The format chapter takes the 8 bytes `02 00 61 62 dc 65 ef db` apart, a page of `ab` 2048 times; see
[the example](#ch-format--example). Here they come into being. The numbers are from running
`seqlz_compress()` with raw literals into two pages, and from a copy of `seqlz_compress.c` with a
`printf()` in `enc_put()` and `enc_flush()`.

The matcher finds two sequences. Position 1 misses. At position 2, the slot of the 5 bytes `ababa` was
never written and holds 0, the 4 bytes at 0 are `abab`, and they match: 2 literals, `ab`, then a match at
offset 2 that runs to the end of the page, 4094 bytes. The last sequence has 0 literals.

| step | what happens | `acc` after | `cnt` |
| --- | --- | --- | --- |
| room check | `p - lit` = 8190 ≥ 2 + 32 | | 0 |
| literals | 16 bytes `abab...` copied to `d + 2`, `lit` moves on by 2 | | 0 |
| token and offset | class 1, token 1010, code `11011100011`, offset `0010` | `0x6e32` | 15 |
| `ml` value | $4090 - 31 = 4059$: code `111101`, extra `11111011011` | `0xdc65efdb` | 32 |
| flush | `acc << 32` = `0xdc65efdb00000000` stored at `end - 8`: `00 00 00 00 db ef 65 dc`; `p` = `end - 4` | `0xdc65efdb` | 0 |
| last sequence | 0 literals, then the return of line 195: no token, nothing put | `0xdc65efdb` | 0 |
| finish | `cnt` is 0, no partial byte; 4 bytes of bitstream; u16 = 2 | | |

The flush is 32 bits, 4 whole bytes, so on this page no byte is ever partial. The old bits `dc65efdb`
are still in `acc` after it, which does not matter: the next put shifts them out, and here no put comes.
The last 4 bytes of `dst` are now `db ef 65 dc`. `encoder_finish()` writes `02 00` at the front, and
since 4 is less than 8, it copies byte by byte from `end - 1` down: `dc 65 ef db`, right after `61 62`.
8 bytes. The 14 more bytes of `abab` that the literal copy wrote past `d + 4` are overwritten by the
bitstream or lie beyond the 8 bytes returned.

The 32 bits, in the order the decoder reads them: `11011100011` (token 1010) `0010` (offset 2) `111101`
(symbol 23 of the `ml` table) `11111011011` (2011).

::: try Compress a page yourself
In the repository, save this as `ab.c` and build it with `gcc -O2 -Isrc -o ab ab.c src/seqlz_compress.c
src/seqlz_codes.c src/seqlz_default_tables.c src/seqlz_lit_sets.c`:

```c
#include "seqlz.h"
#include <stdio.h>
#include <stdlib.h>

static struct seqlz_state st;
static u8 page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE];

int main(void)
{
	struct seqlz_tables *t = malloc(seqlz_tables_size());
	unsigned int i, len;

	if (!t || seqlz_tables_init(t, &seqlz_default_own))
		return 1;
	for (i = 0; i < SEQLZ_PAGE; i++)
		page[i] = i & 1 ? 'b' : 'a';
	len = seqlz_compress(t, &st, page, dst, sizeof(dst), false);
	for (i = 0; i < len; i++)
		printf("%02x ", dst[i]);
	printf("\n");
	return 0;
}
```
@program ch10-ab

It prints:

@output ch10-ab

With `true` instead of `false` the result is the same: 2 literals are too few to save the 51 bytes
coded literals must save.
@assert ab.coded.len == ab.len
:::

What this design gives up is the buffer of one page. The page is written with raw literals first, so a
page whose raw form does not fit fails, also where its coded form would. With a `dst` of exactly one
page, as zswap gives, that costs 0.1% to 0.5% more memory on four dumps
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-from-the-matchers-table-a-buffer-of-one-page-costs-01-to-05-instead-of-16-to-29-kept)).
zram gives two pages, and there it costs nothing.
