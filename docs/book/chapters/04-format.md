# The format on one page {#ch-format}

A page compressed by seqlz is a string of bytes, and zram stores it together with its length. This
chapter is about those bytes, as
[`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md) specifies them. It
is the map for Part II: when `seqlz_decode()` in [chapter 14](#ch-decode) checks the top bit of the
first two bytes, or [chapter 13](#ch-literals-dec) reads 8 streams of literals, this chapter is
where the bytes they read are described as a whole.

The spec is short, about 450 lines, and it is written so that a decoder can be built from it alone.
Two such decoders exist, `tools/seqlz-ref/seqlz_ref.py` and `tools/seqlz-ref/seqlz_ref.c`. Both are
slow on purpose. CI decodes 22 compressed pages with the Python one, and the unit tests and a fuzzer
compare the C one with `seqlz_decode()`. Every number in this chapter
was computed with the Python one and the tables of `src/`, and the two pages at the end were written
by `seqlz_compress()` and are taken apart bit by bit.

One thing first: the format says which pages are valid and what they decode to. It does not say how
a compressor finds them. Any page that decodes is a valid page, and one page of memory can be
written in many ways, as with lz4 or zstd. Where this chapter says what `seqlz_compress()` writes, it
says so.

## Two layouts {#ch-format--layout}

A compressed page starts with a 2-byte number `h`, little endian: byte 0 is the low byte. The top
bit, `h & 0x8000`, says which of the two layouts follows. The other 15 bits, `n = h & 0x7fff`, are
the number of literals of the whole page. 15 bits could count up to 32767, but a page has at most
`PAGE` literals, 4096 on 4 KiB pages, and `n > PAGE` makes the page invalid.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 224" role="img" aria-label="The two layouts of a compressed page: a 2-byte header, the literals, then the bitstream; with coded literals the header also has byte 2, the stream sizes, and 8 streams" font-size="12">
  <text x="20" y="24" text-anchor="start" font-weight="bold">raw literals: h &amp; 0x8000 is 0</text>
  <rect x="20" y="34" width="90" height="34" class="fill-sheet" stroke="currentColor"/>
  <text x="65.0" y="56" text-anchor="middle">h = n</text>
  <rect x="110" y="34" width="360" height="34" class="fill-paper" stroke="currentColor"/>
  <text x="290.0" y="56" text-anchor="middle">the n literals, in page order</text>
  <rect x="470" y="34" width="230" height="34" class="fill-acc" stroke="currentColor" fill-opacity="0.35"/>
  <text x="585.0" y="56" text-anchor="middle">B, the bitstream</text>
  <line x1="20" y1="68" x2="20" y2="76" stroke="currentColor"/>
  <text x="20" y="90" text-anchor="middle" class="mut">0</text>
  <line x1="110" y1="68" x2="110" y2="76" stroke="currentColor"/>
  <text x="110" y="90" text-anchor="middle" class="mut">2</text>
  <line x1="470" y1="68" x2="470" y2="76" stroke="currentColor"/>
  <text x="470" y="90" text-anchor="middle" class="mut">2 + n</text>
  <line x1="700" y1="68" x2="700" y2="76" stroke="currentColor"/>
  <text x="700" y="90" text-anchor="middle" class="mut">len</text>
  <text x="20" y="124" text-anchor="start" font-weight="bold">coded literals: h &amp; 0x8000 is set</text>
  <rect x="20" y="134" width="90" height="34" class="fill-sheet" stroke="currentColor"/>
  <text x="65.0" y="156" text-anchor="middle" font-size="11">h = 0x8000+n</text>
  <rect x="110" y="134" width="30" height="34" class="fill-sheet" stroke="currentColor"/>
  <text x="125.0" y="156" text-anchor="middle">t</text>
  <rect x="140" y="134" width="110" height="34" class="fill-sheet" stroke="currentColor"/>
  <text x="195.0" y="156" text-anchor="middle">s[0] to s[7]</text>
  <rect x="250" y="134" width="40" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="270.0" y="156" text-anchor="middle">0</text>
  <rect x="290" y="134" width="40" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="310.0" y="156" text-anchor="middle">1</text>
  <rect x="330" y="134" width="40" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="350.0" y="156" text-anchor="middle">2</text>
  <rect x="370" y="134" width="40" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="390.0" y="156" text-anchor="middle">3</text>
  <rect x="410" y="134" width="40" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="430.0" y="156" text-anchor="middle">4</text>
  <rect x="450" y="134" width="40" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="470.0" y="156" text-anchor="middle">5</text>
  <rect x="490" y="134" width="40" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="510.0" y="156" text-anchor="middle">6</text>
  <rect x="530" y="134" width="40" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="550.0" y="156" text-anchor="middle">7</text>
  <rect x="570" y="134" width="130" height="34" class="fill-acc" stroke="currentColor" fill-opacity="0.35"/>
  <text x="635.0" y="156" text-anchor="middle">B, the bitstream</text>
  <line x1="20" y1="168" x2="20" y2="176" stroke="currentColor"/>
  <text x="20" y="190" text-anchor="middle" class="mut">0</text>
  <line x1="110" y1="168" x2="110" y2="176" stroke="currentColor"/>
  <text x="110" y="190" text-anchor="middle" class="mut">2</text>
  <line x1="140" y1="168" x2="140" y2="176" stroke="currentColor"/>
  <text x="140" y="190" text-anchor="middle" class="mut">3</text>
  <line x1="250" y1="168" x2="250" y2="176" stroke="currentColor"/>
  <text x="250" y="190" text-anchor="middle" class="mut">3 + w</text>
  <line x1="570" y1="168" x2="570" y2="176" stroke="currentColor"/>
  <text x="570" y="190" text-anchor="middle" class="mut">3 + w + Σs</text>
  <line x1="700" y1="168" x2="700" y2="176" stroke="currentColor"/>
  <text x="700" y="190" text-anchor="middle" class="mut">len</text>
  <text x="410" y="210" text-anchor="middle">stream 0 to 7, s[j] bytes each, literal k in stream k mod 8</text>
</svg>
</div>
<figcaption>The two layouts. The offsets below each box are byte offsets in the compressed page of <code>len</code> bytes.</figcaption>
</figure>

**Raw literals.** With the top bit clear, the `n` literals follow as they are, in the order they
appear in the page, from byte 2 to byte $2 + n - 1$. Everything after them, up to the end of the
compressed page, is `B`, the bitstream of the sequences. `B` has no length field of its own: it ends
where the page ends, and zram knows where that is because it stores the length. This is the only
layout `seqlz-fast` writes, and `seqlz-fast-lit` writes it for every page whose literals do not code
well enough.

**Coded literals.** With the top bit set, the literals are Huffman coded with one of 8 fixed
literal tables, and three more fields come before them. Byte 2, `t`, holds two numbers and two bits
that must be 0:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 134" role="img" aria-label="Byte 2 of a page with coded literals, {{ch04.text.t}}: two zero bits, w minus 5 in bits 3 to 5, the literal table in bits 0 to 2" font-size="12">
  <text x="222.0" y="30" text-anchor="middle" class="mut">bit 7</text>
  <rect x="200" y="40" width="44" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="222.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.7}}</text>
  <text x="266.0" y="30" text-anchor="middle" class="mut">bit 6</text>
  <rect x="244" y="40" width="44" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="266.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.6}}</text>
  <text x="310.0" y="30" text-anchor="middle" class="mut">bit 5</text>
  <rect x="288" y="40" width="44" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="310.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.5}}</text>
  <text x="354.0" y="30" text-anchor="middle" class="mut">bit 4</text>
  <rect x="332" y="40" width="44" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="354.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.4}}</text>
  <text x="398.0" y="30" text-anchor="middle" class="mut">bit 3</text>
  <rect x="376" y="40" width="44" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="398.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.3}}</text>
  <text x="442.0" y="30" text-anchor="middle" class="mut">bit 2</text>
  <rect x="420" y="40" width="44" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
  <text x="442.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.2}}</text>
  <text x="486.0" y="30" text-anchor="middle" class="mut">bit 1</text>
  <rect x="464" y="40" width="44" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
  <text x="486.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.1}}</text>
  <text x="530.0" y="30" text-anchor="middle" class="mut">bit 0</text>
  <rect x="508" y="40" width="44" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
  <text x="530.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.0}}</text>
  <path d="M203 78 V84 H285 V78" fill="none" stroke="currentColor"/>
  <text x="244.0" y="102" text-anchor="middle" font-weight="bold">must be 0</text>
  <text x="244.0" y="119" text-anchor="middle">t &gt;&gt; 6</text>
  <path d="M291 78 V84 H417 V78" fill="none" stroke="currentColor"/>
  <text x="354.0" y="102" text-anchor="middle" font-weight="bold">w - 5 = {{ch04.text.w5}}</text>
  <text x="354.0" y="119" text-anchor="middle">(t &gt;&gt; 3) &amp; 7</text>
  <path d="M423 78 V84 H549 V78" fill="none" stroke="currentColor"/>
  <text x="486.0" y="102" text-anchor="middle" font-weight="bold">set = {{ch04.text.set}}</text>
  <text x="486.0" y="119" text-anchor="middle">t &amp; 7</text>
  <text x="150" y="60" text-anchor="end">t = {{ch04.text.t}}</text>
</svg>
</div>
<figcaption>Byte 2 of the made-up text page below: <code>{{ch04.text.t}}</code> is literal table {{ch04.text.set}} and stream sizes of {{ch04.text.w}} bits.</figcaption>
</figure>

- `set = t & 7` is the literal table, 0 to 7.
- `w = 5 + ((t >> 3) & 7)` is the number of bits of each stream size, 5 to 12.
- `t >> 6` must be 0. These two bits are free for a later format, see
  [No version in the page](#ch-format--version).

The next `w` bytes, from byte 3 on, are one little-endian number `S` of `8 * w` bits that holds the
8 stream sizes, `w` bits each, the lowest first: `s[j] = (S >> (j * w)) & ((1 << w) - 1)`. 8 sizes
of `w` bits are exactly `w` bytes, so no bit is left over. Then come the 8 streams, stream 0 first,
`s[j]` bytes each, and after the last stream the bitstream `B`.

The coded literals are dealt out to the streams like cards: literal 0 goes to stream 0, literal 1 to
stream 1, literal 7 to stream 7, and literal 8 to stream 0 again. Stream $j$ holds literals $j$,
$j + 8$, $j + 16$ and so on, each as the code of literal table `set`, one after the other, read most
significant bit first, as the bitstream. Its codes must end in its last byte: they take at most
`8 * s[j]` bits and fewer than 8 bits less, and the bits after the last code are 0. A stream without
literals has `s[j] = 0`. All 8 streams use the same table; the 8 streams and the 8 tables have
nothing to do with each other, that both are 8 is a coincidence.

Here are real numbers. A page made up for this book: 4096 bytes of "words" of 2 to 9 letters, picked
at random from the 12 letters `etaoinshrdlu` by the C library's classic random number generator from
seed 1, each word followed by a space. `seqlz-fast` stores it in {{ch04.text.raw}} bytes,
`seqlz-fast-lit` in {{ch04.text.len}}. The first {{ch04.text.streams_at}} bytes of the coded page are

```text
{{ch04.text.hdr}}  {{ch04.text.t_hex}}  {{ch04.text.sizes_hex}}
```

- `h = {{ch04.text.h}}`. The top bit is set, so the literals are coded, and `n = {{ch04.text.n_hex}} = {{ch04.text.n}}`: the page has
  {{ch04.text.n}} literals, which are {{ch04.text.per_stream}} per stream.
- `t = {{ch04.text.t}} = 0b{{ch04.text.t_bin}}`. The table is `{{ch04.text.t}} & 7 = {{ch04.text.set}}`, the one that codes this page's literals in the fewest bits; `w = 5 +
  (({{ch04.text.t}} >> 3) & 7) = 5 + {{ch04.text.w5}} = {{ch04.text.w}}`; the top two bits are 0.
- The {{ch04.text.w}} bytes of sizes are the number `S = {{ch04.text.S}}`, the last byte the high one. Its
  lowest {{ch04.text.w}} bits are `{{ch04.text.s0_hex}} = {{ch04.text.s.0}}`, the size of stream 0. The 8 sizes are {{ch04.text.s.0}}, {{ch04.text.s.1}}, {{ch04.text.s.2}}, {{ch04.text.s.3}}, {{ch04.text.s.4}}, {{ch04.text.s.5}},
  {{ch04.text.s.6}} and {{ch04.text.s.7}} bytes, {{ch04.text.sum}} together.
- The streams start at byte $3 + w = {{ch04.text.streams_at}}$, the bitstream at ${{ch04.text.streams_at}} + {{ch04.text.sum}} = {{ch04.text.b_at}}$, and it takes the last
  {{ch04.text.b_bytes}} bytes of the page.
@assert ch04.text.best == ch04.text.set and ch04.text.t_bit[6] == 0 and ch04.text.t_bit[7] == 0

{{ch04.text.per_stream}} literals in about {{ch04.text.avg}} bytes is {{ch04.text.bits_per_lit}} bits per literal, where the raw page spends 8.

::: why Why the stream sizes take `w` bits and not 2 bytes each
A stream holds every 8th literal, and a literal code has at most 10 bits. On a 4 KiB page that is at
most 512 literals of 10 bits, 640 bytes, which needs 10 bits; on a 16 KiB page 2560 bytes, 12 bits.
So `w` from 5 to 12 holds every case. Most pages have streams below 128 bytes, 7 bits. With 2 bytes
per size, as until 5th October 2026, the header was 19 bytes, and pages were 5.3 bytes larger on
average
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#stream-sizes-in-as-many-bits-as-the-largest-needs-kept)).
Widths of 3 and 4 bits are left out: few pages would use them.
:::

Why the literals are in 8 streams at all is a question of the decoder's speed, see
[chains of dependent steps](#ch-cpu--chains). How the encoder picks the table, and when it codes the
literals at all, is in [chapter 11](#ch-literals-enc).

## Prefix codes from code lengths {#ch-format--codes}

Everything in `B`, and every coded literal, is either a **code** from a table or plain bits. The
tables are fixed: they are part of the format and are not in the page. There are 11 of them:

| table | symbols | longest code allowed | codes |
| --- | --- | --- | --- |
| `TOK`, the tokens and the escape | {{tok.symbols}} | {{ch06.TOKEN_BITS}} bits | the token of each sequence |
| `LL`, literal length values | {{ll.symbols}} ({{k16.ll.symbols}} on 16 KiB pages) | {{ch06.MAX_BITS}} bits | the rest of a long `ll` |
| `ML`, match length values | {{ml.symbols}} ({{k16.ml.symbols}} on 16 KiB pages) | {{ch06.MAX_BITS}} bits | the rest of a long `ml` |
| literal tables 0 to 7 | 256 each | {{ch06.LIT_BITS}} bits | the coded literals |
@assert ch04.longest.tok == ch06.TOKEN_BITS and ch04.longest.ll <= ch06.MAX_BITS and ch04.longest.ml <= ch06.MAX_BITS and ch04.longest.lit == ch06.LIT_BITS

A table holds only the **code length** of each symbol, 0 for a symbol without a code. The codes
follow from the lengths by the rule of deflate (RFC 1951, section 3.2.2), the **canonical** code of
[chapter 2](#ch-compression--canonical-codes): shorter codes first, and the codes of one length in
the order of the symbols' numbers. Here is the rule applied to the real token table of 4 KiB pages:

- No symbol has a code of 1, 2 or 3 bits, so `first[4] = {{ch04.first.4}}`. The {{tok.codes_of.4}} symbols with a code of 4 bits are,
  in the order of their number, the tokens {{ch04.syms.4.0}}, {{ch04.syms.4.1}}, {{ch04.syms.4.2}} and the escape {{ch04.syms.4.3}}. They get `{{tok.code.34}}`, `{{tok.code.49}}`,
  `{{tok.code.2097}}` and `{{tok.code.3072}}`.
- `first[5] = ({{ch04.first.4}} + {{tok.codes_of.4}}) * 2 = {{ch04.first.5}}`, binary `{{ch04.first_bin.5}}`. The {{tok.codes_of.5}} symbols with 5 bits, tokens {{ch04.syms.5.0}}, {{ch04.syms.5.1}}, {{ch04.syms.5.2}} and
  {{ch04.syms.5.3}}, get `{{tok.code.576}}` to `{{tok.code.2624}}`.
- `first[6] = ({{ch04.first.5}} + {{tok.codes_of.5}}) * 2 = {{ch04.first.6}}`, binary `{{ch04.first_bin.6}}`, the code of token {{ch04.syms.6.0}}, the first of {{tok.codes_of.6}} symbols with
  6 bits.
@assert tok.codes_of[1] + tok.codes_of[2] + tok.codes_of[3] == 0 and F("ch04.syms.4") == [34, 49, 2097, 3072] and F("ch04.syms.5") == [576, 2082, 2112, 2624]

Every table is **complete**: its codes use up all bit strings, the sum of $2^{-l}$ over all codes is
exactly 1 ([chapter 2](#ch-compression--complete)). For the token table that is {{tok.codes_of.4}} codes of 4 bits, {{tok.codes_of.5}} of 5, {{tok.codes_of.6}} of 6, {{tok.codes_of.7}} of 7, {{tok.codes_of.8}} of 8, {{tok.codes_of.9}} of 9,
{{tok.codes_of.10}} of 10 and {{tok.codes_of.11}} of 11 bits, {{tok.with_code}} codes. Counted in units of $\frac{1}{2048}$:
${{tok.codes_of.4}} \cdot 128 + {{tok.codes_of.5}} \cdot 64 + {{tok.codes_of.6}} \cdot 32 + {{tok.codes_of.7}} \cdot 16 + {{tok.codes_of.8}} \cdot 8 + {{tok.codes_of.9}} \cdot 4 + {{tok.codes_of.10}} \cdot 2 + {{tok.codes_of.11}} = 2048$. So every string of 11 bits starts with exactly one
code, and a decoder never meets bits that are no code. That is what lets `seqlz_decode()` look up 11
bits in a table of 2048 entries without checking the entry; the table is built in
[chapter 8](#ch-codes).
@assert tok.codes_of[4] * 128 + tok.codes_of[5] * 64 + tok.codes_of[6] * 32 + tok.codes_of[7] * 16 + tok.codes_of[8] * 8 + tok.codes_of[9] * 4 + tok.codes_of[10] * 2 + tok.codes_of[11] == 2048 and tok.with_code == tok.codes_of[4] + tok.codes_of[5] + tok.codes_of[6] + tok.codes_of[7] + tok.codes_of[8] + tok.codes_of[9] + tok.codes_of[10] + tok.codes_of[11]

## The bitstream {#ch-format--bitstream}

`B` is read one bit at a time, starting with bit 7, the most significant bit, of its first byte: bit
`i` of `B` is bit `7 - (i mod 8)` of byte `i div 8`. The byte `dc` is read as `1`, `1`, `0`, `1`, `1`,
`1`, `0`, `0`. A decoder reads three kinds of things from it:

- `read(n)`: the next `n` bits as a number, the first bit read the most significant. `read(4)` of the
  bits `0010` is 2. `read(0)` is 0.
- `symbol(table)`: bits one by one until they are a code of the table, and the result is that code's
  symbol. The first bit read is the code's first bit.
- `value(table)`: a [length value](#ch-format--length-values), a symbol and some plain bits.

The order matters for speed, not for size: read the other way round, the same page has the same
number of bits. Most significant bit first, the next code is the top bits of a 64-bit register, and
the entries of one code in the decoder's table are next to each other, so a page needs fewer of the
table's cache lines. [Chapter 3](#ch-cpu--msb-first) shows it with the numbers.

## One sequence: the token {#ch-format--token}

Each sequence starts with a **token**, one symbol of `TOK` that holds three numbers: the literal
length `ll`, the **match length field** `mlf`, which is `ml - 4`, and the offset class `c`:

```text
token = min(ll, 15) + 16 * min(ml - 4, 31) + 512 * class

ll  = token & 15          the literal length, 0 to 15; 15 means 15 or more
mlf = (token >> 4) & 31   ml - 4, 0 to 31; 31 means 31 or more, ml 35 or more
c   = token >> 9          the offset class, 0 to 5
```

16 values of `ll` times 32 of `mlf` times 6 classes are 3072 tokens, numbered 0 to 3071. Like hours,
minutes and seconds in one number of seconds, the three come apart with shifts and masks:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 134" role="img" aria-label="Token 2098 in binary, 100 00011 0010: offset class 4, ml minus 4 equal to 3, ll equal to 2" font-size="12">
  <text x="158.0" y="30" text-anchor="middle" class="mut">11</text>
  <text x="194.0" y="30" text-anchor="middle" class="mut">10</text>
  <text x="230.0" y="30" text-anchor="middle" class="mut">9</text>
  <text x="266.0" y="30" text-anchor="middle" class="mut">8</text>
  <text x="302.0" y="30" text-anchor="middle" class="mut">7</text>
  <text x="338.0" y="30" text-anchor="middle" class="mut">6</text>
  <text x="374.0" y="30" text-anchor="middle" class="mut">5</text>
  <text x="410.0" y="30" text-anchor="middle" class="mut">4</text>
  <text x="446.0" y="30" text-anchor="middle" class="mut">3</text>
  <text x="482.0" y="30" text-anchor="middle" class="mut">2</text>
  <text x="518.0" y="30" text-anchor="middle" class="mut">1</text>
  <text x="554.0" y="30" text-anchor="middle" class="mut">0</text>
  <rect x="140" y="40" width="36" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
  <text x="158.0" y="60" text-anchor="middle" font-weight="bold">1</text>
  <rect x="176" y="40" width="36" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
  <text x="194.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <rect x="212" y="40" width="36" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
  <text x="230.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <g class="acc"><path d="M143 78 V84 H245 V78" fill="none" stroke="currentColor" stroke-width="1.5"/>
  <text x="194.0" y="102" text-anchor="middle" font-weight="bold">class = 4</text></g>
  <text x="194.0" y="119" text-anchor="middle">token &gt;&gt; 9</text>
  <rect x="248" y="40" width="36" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="266.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <rect x="284" y="40" width="36" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="302.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <rect x="320" y="40" width="36" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="338.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <rect x="356" y="40" width="36" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="374.0" y="60" text-anchor="middle" font-weight="bold">1</text>
  <rect x="392" y="40" width="36" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="410.0" y="60" text-anchor="middle" font-weight="bold">1</text>
  <g class="mark"><path d="M251 78 V84 H425 V78" fill="none" stroke="currentColor" stroke-width="1.5"/>
  <text x="338.0" y="102" text-anchor="middle" font-weight="bold">ml - 4 = 3</text></g>
  <text x="338.0" y="119" text-anchor="middle">(token &gt;&gt; 4) &amp; 31</text>
  <rect x="428" y="40" width="36" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="446.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <rect x="464" y="40" width="36" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="482.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <rect x="500" y="40" width="36" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="518.0" y="60" text-anchor="middle" font-weight="bold">1</text>
  <rect x="536" y="40" width="36" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="554.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <g><path d="M431 78 V84 H569 V78" fill="none" stroke="currentColor" stroke-width="1.5"/>
  <text x="500.0" y="102" text-anchor="middle" font-weight="bold">ll = 2</text></g>
  <text x="500.0" y="119" text-anchor="middle">token &amp; 15</text>
  <text x="124" y="60" text-anchor="end">2098 =</text>
</svg>
</div>
<figcaption>Token 2098 as 12 bits: the class in the top 3, <code>ml - 4</code> in the middle 5, <code>ll</code> in the low 4.</figcaption>
</figure>

The token stores `ml - 4` because a match is at least 4 bytes: lengths 0 to 3 never occur, and no
token is spent on them. A shorter match would cost about as many bits as its bytes stored as
literals, so the matcher does not look for one, see [chapter 9](#ch-matcher). With the caps, `ll` up
to 14 and `ml` up to 34 fit into the token alone; a 15 or a 31 means a length value follows.

One code for all three numbers lets the code see how they go together. The shortest codes in the
real 4 KiB table, {{tok.len.34}} bits each, are:

| token | `ll` | `ml` | class | what it is |
| --- | --- | --- | --- | --- |
| {{ch04.syms.4.0}} | {{ch04.short.0.ll}} | {{ch04.short.0.ml}} | {{ch04.short.0.cls}} | 2 literals, then 6 bytes from the same offset as the match before |
| {{ch04.syms.4.1}} | {{ch04.short.1.ll}} | {{ch04.short.1.ml}} | {{ch04.short.1.cls}} | 1 literal, 7 bytes, same offset: the sequence of an array of pointers |
| {{ch04.syms.4.2}} | {{ch04.short.2.ll}} | {{ch04.short.2.ml}} | {{ch04.short.2.cls}} | 1 literal, 7 bytes from a new offset that is a multiple of 8 below 256 |
| {{ch04.syms.4.3}} | | | | the escape |
@assert F("ch04.syms.4") == [34, 49, 2097, 3072] and tok.codes_of[1] + tok.codes_of[2] + tok.codes_of[3] == 0

Token 49 is the sequence of an array of 8-byte pointers, each 16 bytes further than the one before.
Most pointers differ from the one 8 bytes back only in their lowest byte, so after the first pointer
the sequences are 1 literal and 7 bytes copied from 8 bytes back, the same offset as before. As token
49, such a sequence costs {{tok.len.49}} bits. Where the lowest byte carries into the next one, every
16th pointer, the sequence is 2 literals and 6 bytes, token 34, at {{tok.len.34}} bits too. Of the
{{ch04.ptr.tokens}} tokens of [chapter 9](#ch-matcher)'s page of 512 pointers, {{ch04.ptr.t49}} are token 49
and {{ch04.ptr.t34}} token 34.
@assert ch04.ptr.t49 + ch04.ptr.t34 + 1 == ch04.ptr.tokens

**The escape.** Codes of at most 11 bits have room for 2048 symbols, fewer than the 3072 tokens.
So only the frequent tokens have a code, {{ch04.tok_coded}} of them on 4 KiB pages. Any other token is sent as the
escape, symbol 3072, followed by the token's number in 12 plain bits, which hold every number up to
4095. The escape's code has {{tok.len.3072}} bits, so an escaped token takes {{ch04.esc_bits}} bits. Any token may be escaped, also
one with a code of its own, and it means the same; `seqlz_compress()` escapes only tokens without a
code. An escaped number of 3072 or more makes the page invalid. In the spec's words:

```text
t = symbol(TOK)
if t == 3072:
    t = read(12)
    if t >= 3072: the page is invalid
```

## Offset classes {#ch-format--offsets}

Right after the token come the **offset bits**, as many as the token's class says, and the class
says how they become the offset:

| class | offset bits, 4 KiB | offset bits, 16 KiB | offset | offsets it can send |
| --- | --- | --- | --- | --- |
| 0 | {{ch04.raw.0}} | {{k16.ch04.raw.0}} | the offset of the match before | the same again |
| 1 | {{ch04.raw.1}} | {{k16.ch04.raw.1}} | `read(4)` | 1 to 15 |
| 2 | {{ch04.raw.2}} | {{k16.ch04.raw.2}} | `read(8)` | 1 to 255 |
| 3 | {{ch04.raw.3}} | {{k16.ch04.raw.3}} | `read(P)` | 1 to `PAGE - 1` |
| 4 | {{ch04.raw.4}} | {{k16.ch04.raw.4}} | `read(5) * 8` | 8 to 248, multiples of 8 |
| 5 | {{ch04.raw.5}} | {{k16.ch04.raw.5}} | `read(P - 3) * 8` | 8 to `PAGE - 8`, multiples of 8 |

Class 0 is the **repeat offset**: the offset of the sequence before, in 0 bits. At the start of
each page it is 1, so class 0 also works for the first match, e.g. a run of a byte. Classes 4 and 5
store the offset divided by 8, 3 bits less than classes 2 and 3. Memory holds many 8-byte values,
pointers and 8-byte fields, so repeats at a distance that is a multiple of 8 are common.

The offset bits are plain binary, not a code. An offset is the distance back from the end of the
output: offset 16 copies from the byte 16 places before the next byte to be written. It must be at
least 1 and at most the number of bytes written so far; class 1 with the bits `0000` gives offset 0,
and that page is invalid.

The format does not care which class a compressor uses, as long as the offset comes out. Class 2 can
send offset 5 too, and class 4 can send 8 (`read(5) = 1`). `seqlz_compress()` uses class 0 when the
offset is the one before, else class 4 or 5 for a multiple of 8 from 16 on, else the smallest of
classes 1 to 3 that holds it. That choice is made in `seqlz_off_class()`, without a branch, see
[chapter 6](#ch-header--offsets). Some examples, computed with it:

| offset | offset before | class | offset bits |
| --- | --- | --- | --- |
| 24 | 24 | {{ch04.oc.24_24.cls}} | none |
| 24 | 16 | {{ch04.oc.24_16.cls}} | {{ch04.oc.24_16.raw}} bits, `read({{ch04.oc.24_16.raw}}) = {{ch04.oc.24_16.read}}` |
| 8 | 1 | {{ch04.oc.8_1.cls}} | {{ch04.oc.8_1.raw}} bits: 8 is a multiple of 8, but below 16 |
| 100 | 1 | {{ch04.oc.100_1.cls}} | {{ch04.oc.100_1.raw}} bits |
| 256 | 1 | {{ch04.oc.256_1.cls}} | {{ch04.oc.256_1.raw}} bits, `read({{ch04.oc.256_1.raw}}) = {{ch04.oc.256_1.read}}` |
| 300 | 1 | {{ch04.oc.300_1.cls}} | {{ch04.oc.300_1.raw}} bits |
@assert ch04.oc["24_24"].raw == 0

::: why Why offsets are plain bits, and why the class is in the token
Offsets were once a Huffman-coded symbol for their number of bits, followed by the bits. Moving the
class into the token, with plain bits after it, cost 0.4 points of memory (a point is 1% of a page,
41 bytes), and took the decoder from
8758 to 8168 cycles per page, because each sequence then needs one table lookup on the chain from one
token to the next, not two
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
The classes 4 and 5 for multiples of 8 came later and saved 13 bytes
per page in the kernel, for about 0.2 µs more per page written
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)).
:::

## Length values {#ch-format--length-values}

A length that does not fit into the token follows the offset bits as a **length value**: if `ll` is
15, the real `ll` is `15 + value(LL)`, and if `mlf` is 31, the real `ml` is `35 + value(ML)`. The `ll`
value comes first. Both use the same scheme, each with its own table:

```text
s = symbol(table)
if s < 16:  value = s
else:       b = s - 12;  value = 2^b + read(b)
```

Values below 16 are a symbol of their own. A larger value is sent as the position of its highest
bit, $b$, as the symbol $12 + b$, followed by the $b$ bits below the highest one as plain bits. The
symbols 16 to 23 stand for $b = 4$ to 11, the ranges 16 to 31, 32 to 63, and so on up to 2048 to 4095.
That is {{ll.symbols}} symbols on 4 KiB pages, $12 + P$, and {{k16.ll.symbols}} on 16 KiB pages: no value a page needs reaches
4096. E.g. the value 100 is in the
range 64 to 127, so $b = 6$, the symbol is 18, and `read(6)` is $100 - 64 = 36$. In the real `LL`
table symbol 18 has the code `{{ll.code.18}}`, so `ll = 115` costs the token, {{ll.len.18}} bits of code and 6 plain bits.
@assert ll.symbols == ml.symbols == 24 and k16.ll.symbols == 26

A code of a length value has at most {{ch06.MAX_BITS}} bits, so each decode table has 256 entries. A match
length value follows only 8.5% of the matches, which is why `ml` has 5 bits in the token and
not 4: with 4, a value followed 18.7% of them
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).

::: why Why two trained tables and not 5 plain bits
The symbol of a length value could be 5 plain bits, which would take two tables out of the format.
It was measured twice. The first time the verdict was "the tables cost the format two tables once,
the 5 bits cost every page 3 bytes"
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-instead-of-their-tables-3-bytes-per-page-more-not-kept)).
The second time, on the decoder of 10th October 2026 and timed, 5 plain bits made pages 0.2% larger,
2.2 bytes, and no read faster
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-again-02-to-03-more-memory-no-read-faster-not-kept)).
:::

## How a page ends {#ch-format--end}

A page does not say how many sequences it has, and its bitstream has no length field. Instead:

- **The last sequence has no token.** Every match is at least 4 bytes. So once the literals left,
  `n - used`, are as many as the bytes the page still needs, `PAGE - |out|`, no match fits any more:
  the literals left are the last sequence, and decoding copies them and stops. The decoder knows `n`
  from the header and `PAGE` from the page size, so the last sequence needs no bits at all. A page
  whose last match reaches the end of the page ends with 0 literals left, and a page of literals
  only, `n = PAGE`, has no sequence with a match and an empty `B`.
- **The bitstream ends where its bits end.** After the last bit the decoder needs, fewer than 8 bits
  of `B` are left, all of them in its last byte, and all of them 0. The same holds for each literal
  stream. A page with a byte too many, or a bit 1 in the padding, is invalid.

*Why:* zram stores no checksum. A page damaged in memory has to be caught by the decoder, and the
strict end catches most damage at one compare per page, at no cost in bytes: the compressor writes
nothing else anyway. Measured, the strict end costs a warm read 0.07 to 0.10 µs on the phone's little core
and nothing in the kernel VM on the PC
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-page-ends-where-its-bits-end-no-damaged-page-with-bytes-or-bits-too-many-decodes-01-µs-on-the-a55-kept)).

Until 11th October 2026 the last sequence had a token too: its `ll`, `mlf = 0` and class 0, so token
0 when the last match had filled the page. It told the decoder only what it knew already, and took
9.0 bits per page, about 1.2 bytes, on the zram pages of a desktop and a phone
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-last-sequence-without-a-token-12-bytes-per-page-less-kept)).

Every sequence but the last writes at least 4 bytes, so a decoder stops after at most `PAGE / 4 + 1`
sequences, {{const.MAX_SEQUENCES}} on a 4 KiB page, also on a page that is not valid. That bounds the work per page.

Two facts follow from the rules. A valid page has $n \ge 1$: the first match needs at least one byte
before it, and a page without matches is all literals. And the first match's offset is at most that
sequence's `ll`.

## The rules of a valid page {#ch-format--validity}

A page is valid for a page size and the tables of that page size if all of these hold. The first
group is the layout, the second the literal streams, the third the sequences.

1. `len >= 2`.
2. `n <= PAGE`.
3. Raw literals: `2 + n <= len`.
4. Coded literals: `len >= 3 + w`.
5. Coded literals: `t >> 6` is 0.
6. Coded literals: `3 + w + s[0] + ... + s[7] <= len`.
7. Every literal stream's codes take at most `8 * s[j]` bits and fewer than 8 bits less, and its bits
   after the last code are all 0. So a stream without literals has size 0.
8. Every escaped token is below 3072.
9. Every sequence's `ll` is at most the literals left, `n - used`, and at most the room left in the
   page, `PAGE - |out|`.
10. Every match's offset is 1 to `|out|`, the bytes written so far.
11. Every match fits into the room left in the page, `ml <= PAGE - |out|`.
12. At the end, 0 to 7 bits of `B` are left, and all of them are 0. A page that needs more bits than
    it has fails this: a decoder may read zeros past the end, but then it used bits that are not
    there, and fewer than 0 are left.

A header with more literals than the sequences use breaks rule 9 or 11: the literals left are then
more than the bytes left after every match, so the end is never reached, and the next token's `ll`
or match does not fit.

`seqlz_decode()` checks every one of them, in its own order and often in a cheaper form, see
[chapter 14](#ch-decode). The fuzz target `fuzz/seqlz_diff_fuzz.c` gives any input to it and to
`tools/seqlz-ref/seqlz_ref.c` and fails if they disagree on whether a page is valid or on what it
decodes to.

## Decoding, as the spec writes it {#ch-format--decode}

The whole decoder of the sequences fits in fewer than 20 lines. `lits` are the `n` literals (decoded first if
they are coded), `used` is how many of them are copied, `out` is the page being written, `last` is
1:

```text
loop:
    if n - used == PAGE - |out|:                the last sequence
        append lits[used] to lits[n - 1] to out
        stop
    token: ll, mlf, c
    x = read(offset bits of class c)
    off = (c == 0) ? last : x as the offset of class c
    if ll == 15: ll = 15 + value(LL)
    if ll > n - used or ll > PAGE - |out|: invalid
    append lits[used] to lits[used + ll - 1] to out; used = used + ll
    ml = mlf + 4
    if mlf == 31: ml = 35 + value(ML)
    last = off
    if off == 0 or off > |out| or ml > PAGE - |out|: invalid
    ml times: append the byte off places before the end of out

at the stop: fewer than 8 bits of B left, all 0, else invalid
```

The match is copied one byte at a time, each byte after the one before is written. So it can repeat
bytes it writes itself: with `off = 1` and `ml = 10` after an `a`, it writes `aaaaaaaaaa`. A block
copy like `memcpy()` gives other bytes when `off < ml`, see
[chapter 2](#ch-compression--overlap).

`seqlz_decode()` does the same work in another shape. It decodes all coded literals first, into a
buffer, so that the loop over the sequences reads them as if they were raw. It looks up 11 bits at a
time in a table instead of reading bit by bit. And it copies 16 bytes at a time where there is room,
see [copying more than needed](#ch-cpu--wild-copy). The result is the same for every input, valid or
not; that is what the fuzzers check.

## A page of `ab`, bit by bit {#ch-format--example}

The {{ab.len}} bytes `{{ab.hex}}` are a 4 KiB page of `ab` 2048 times. `seqlz-fast` and
`seqlz-fast-lit` both write exactly these bytes: 2 literals are far from saving 51 bytes, the
minimum for coding them.

- `{{ch04.ab.hdr}}`: `h = {{ab.lits}}`. The top bit is clear, so the literals are raw, and `n = {{ab.lits}}`.
- `{{ch04.ab.lithex}}`: the {{ab.lits}} literals, `a` and `b`.
- `{{ab.b.hex}}`: `B`, {{ab.b.bits}} bits.

@anim abbits The {{ab.len}} bytes read as the spec reads them, one field per step, with the codes of the real 4 KiB tables; the page fills up below.

Step by step, with the codes of the real 4 KiB tables:

| bits | read as | means |
| --- | --- | --- |
| `{{ch04.ab.s.0.code}}` | `symbol(TOK)` = {{ch04.ab.s.0.tok}} | ${{ch04.ab.s.0.tok}} = {{ch04.ab.s.0.llf}} + 16 \cdot {{ch04.ab.s.0.mlf}} + 512 \cdot {{ch04.ab.s.0.cls}}$: `ll = {{ch04.ab.s.0.llf}}`, `mlf = {{ch04.ab.s.0.mlf}}`, class {{ch04.ab.s.0.cls}} |
| `{{ch04.ab.s.0.offbits}}` | `read({{ch04.ab.s.0.raw}})` = {{ch04.ab.s.0.offread}} | class {{ch04.ab.s.0.cls}}: `off = {{ch04.ab.s.0.off}}` |
| | | `ll` is not 15, no `ll` value: append `a b`, the page has 2 bytes |
| `{{ch04.ab.s.0.mlcode}}` | `symbol(ML)` = {{ch04.ab.s.0.mlsym}} | `mlf` is 31, so a length value: $b = {{ch04.ab.s.0.mlsym}} - 12 = {{ch04.ab.s.0.mlb}}$ |
| `{{ch04.ab.s.0.mlbits}}` | `read({{ch04.ab.s.0.mlb}})` = {{ch04.ab.s.0.mlread}} | value $2^{{{ch04.ab.s.0.mlb}}} + {{ch04.ab.s.0.mlread}} = {{ch04.ab.s.0.mlval}}$, `ml = 35 + {{ch04.ab.s.0.mlval}} = {{ch04.ab.s.0.ml}}` |
| | | copy {{ch04.ab.s.0.ml}} bytes from {{ch04.ab.s.0.off}} back: `abab...`, the page has 4096 bytes |
| | | 0 literals left for 0 bytes of the page: the last sequence, no token |
@assert ab.seqs == 2 and ab.escapes == 0 and ch04.ab.s[0].ll == 2 and ch04.ab.s[0].mlf == 31

After the last code, {{ab.b.pad}} bits of `B` are left, fewer than 8 and all zero. The page is valid.

The match is the case where copying byte by byte matters: it starts 2 bytes back and is 4094 bytes
long, so after its first 2 bytes it copies bytes it wrote itself. A page of 4096 bytes takes {{ab.len}} bytes.
Until 11th October 2026 it took 9: the last sequence had a token, token 0 with the 8-bit code
`10001010`, which said "0 literals, the end" where the decoder could see that already.

::: try Decode it with the spec's decoder
In the repository, write the {{ab.len}} bytes to a file and give it to the reference decoder. It prints
whether the page is valid and the SHA-256 of the decoded page, which is the SHA-256 of `ab` 2048
times:

```text
printf '{{ch04.ab.printf}}' > /tmp/ab.seqlz
python3 tools/seqlz-ref/seqlz_ref.py /tmp/ab.seqlz
/tmp/ab.seqlz valid 2c888a3809c425e853a6ac08e1c1ffd9393fe3e49419e8f71ac23ab0362617a3
python3 -c "import hashlib; print(hashlib.sha256(b'ab' * 2048).hexdigest())"
2c888a3809c425e853a6ac08e1c1ffd9393fe3e49419e8f71ac23ab0362617a3
```

Flip any bit of the {{ab.len}} bytes and run it again: most flips make the page invalid. The same {{ab.len}} bytes go
through the real `seqlz_decode()` one statement at a time in [chapter 14](#ch-decode--example).
:::

## A second page: {{second.seqs}} sequences and {{second.b.pad}} bits of padding {#ch-format--example2}

The page of `ab` has one match. This one has three, with three kinds of offset, and a bitstream that
does not end on a byte boundary. It is made up for this book:

```text
bytes 0 to 11:   abcdefghijkl
bytes 12 to 15:  llll
bytes 16 to 17:  xy
bytes 18 to 24:  cdefghi       the same 7 bytes as at 2 to 8, 16 bytes back
bytes 25 to 4095: z, 4071 times
```

`seqlz_compress()` writes it in {{second.len}} bytes, with and without coded literals the same: {{second.lits}} literals
cannot save 51 bytes.
@assert second.coded.len == second.len

```text
{{ch04.second.hdr}}  {{ch04.second.lithex}}  {{second.b.hex}}
```

`h = {{second.lits}}`, raw literals, and the {{second.lits}} literals are `{{ch04.second.litstr}}`. `B` is {{second.b.bytes}} bytes, {{ch04.second.bbits}} bits:

| bits | read as | means |
| --- | --- | --- |
| `{{ch04.second.s.0.code}}` | `symbol(TOK)` = {{ch04.second.s.0.tok}} | `ll = {{ch04.second.s.0.llf}}`, `mlf = {{ch04.second.s.0.mlf}}`, class {{ch04.second.s.0.cls}} |
| | class 0: no bits | `off = last = {{ch04.second.s.0.off}}`, the start value |
| | | append `abcdefghijkl`, then {{ch04.second.s.0.ml}} bytes from {{ch04.second.s.0.off}} back: `llll`; 16 bytes |
| `{{ch04.second.s.1.code}}` | `symbol(TOK)` = {{ch04.second.s.1.tok}} | `ll = {{ch04.second.s.1.llf}}`, `mlf = {{ch04.second.s.1.mlf}}`, class {{ch04.second.s.1.cls}} |
| `{{ch04.second.s.1.offbits}}` | `read({{ch04.second.s.1.raw}})` = {{ch04.second.s.1.offread}} | `off = {{ch04.second.s.1.offread}} * 8 = {{ch04.second.s.1.off}}` |
| | | append `xy`, then {{ch04.second.s.1.ml}} bytes from {{ch04.second.s.1.off}} back: `cdefghi`; 25 bytes |
| `{{ch04.second.s.2.code}}` | `symbol(TOK)` = {{ch04.second.s.2.tok}} | `ll = {{ch04.second.s.2.llf}}`, `mlf = {{ch04.second.s.2.mlf}}`, class {{ch04.second.s.2.cls}} |
| `{{ch04.second.s.2.offbits}}` | `read({{ch04.second.s.2.raw}})` = {{ch04.second.s.2.offread}} | `off = {{ch04.second.s.2.off}}` |
| | | append `z`; 26 bytes |
| `{{ch04.second.s.2.mlcode}}` | `symbol(ML)` = {{ch04.second.s.2.mlsym}} | $b = {{ch04.second.s.2.mlb}}$ |
| `{{ch04.second.s.2.mlbits}}` | `read({{ch04.second.s.2.mlb}})` = {{ch04.second.s.2.mlread}} | `ml = 35 + 2^{{ch04.second.s.2.mlb}} + {{ch04.second.s.2.mlread}} = {{ch04.second.s.2.ml}}`: `z` {{ch04.second.s.2.ml}} more times; 4096 bytes |
| | | 0 literals left for 0 bytes of the page: the last sequence |
| `{{ch04.second.padbits}}` | | {{second.b.pad}} bits left, in the last byte, all 0: valid |
@assert second.seqs == 4 and second.escapes == 0 and ch04.second.s[0].cls == 0 and ch04.second.s[1].cls == 4 and ch04.second.s[2].cls == 1 and ch04.second.s[0].mlf < 31 and ch04.second.s[1].mlf < 31 and ch04.second.s[2].mlf == 31 and ch04.second.s[2].llf < 15

A few things to see here:

- **The first match uses the repeat offset.** At the start of a page `last` is 1, and the run `llll`
  is 4 bytes from 1 back, so it takes 0 offset bits. The matcher found it with that same candidate,
  see [chapter 9](#ch-matcher).
- **Token 12 has an {{tok.len.12}}-bit code.** 12 literals and a match of exactly 4 bytes at the repeat offset is
  rare on real pages, so its code is long. A token without any code would have taken {{ch04.esc_bits}} bits.
@assert tok.len[12] == ch06.TOKEN_BITS
- **Token 2098 is "2 literals, a match of 7 at offset 16".** 16 is a multiple of 8 from 16 on, so the
  class is 4 and the offset is sent as 2 in 5 bits. The sequence takes ${{tok.len.2098}} + 5 = {{ch04.second.s.1.bits}}$ bits plus its 2
  literals. In lz4's format the same sequence is a token byte and 2 bytes of offset, 24 bits plus
  the literals.
- **The third match changes the class but not the bits' meaning.** Offset 1 is not the repeat offset
  any more, `last` is 16 now, so it goes as class 1 with 4 bits.
- **The padding.** {{second.b.bits}} bits are used, {{second.b.pad}} are left in the last byte, and they are 0. With an eighth bit
  left, or one of them 1, the page would be invalid.

`python3 tools/seqlz-ref/seqlz_ref.py` agrees: it decodes the {{second.len}} bytes to the page above.

## No version in the page {#ch-format--version}

The page has no version number and no fixed bytes that mark it as seqlz. zram keeps its pages
only while the kernel that wrote them runs: they are decompressed by the same code that compressed them, also the ones it wrote
back to a backing device, and no page ever meets a newer decoder. So a change of the format needs a
new kernel, not a new algorithm name in zram, which users would see in `comp_algorithm` and in the
kernel's configuration.

Two things would change that: another user of the codec that keeps pages longer, or zram's pages kept
across a `kexec` by live update, which zram does not do in 2026. For that case the two top bits of
byte 2 are invalid today, so a later format can tell itself apart from this one. The format is not
frozen yet: nobody uses seqlz, and until it is in a kernel, the tables and the rules may still
change, together with `docs/format.md`'s hashes, its example and both reference decoders.

## Where the tables come from {#ch-format--tables}

The 11 tables are counts turned into code lengths. `bench/seqlz_counts_4k.txt` has, for 4 KiB
pages, how often each token, each `ll` symbol and each `ml` symbol occurred on a set of training
pages, and for each of the 8 literal tables, how often each byte value occurred as a literal on the
pages that table codes best. `tools/kernel-port/gen-seqlz-tables.py` turns the counts into length-limited
Huffman code lengths and writes `src/seqlz_default_tables_4k.inc` and `src/seqlz_lit_sets_4k.inc`;
their first lines name the command. CI checks that the files in `src/` are its output, byte for byte,
and `quetschn-seqlz-train --from-counts` does the same in C++. The kernel's copy gets the generator,
as `scripts/gen-seqlz-tables.py`, and the 4 KiB counts, and its `lib/seqlz/seqlz_tables.c` is that
script's output.

The training pages are not published, because memory pages hold keys and passwords. For 4 KiB pages
they are 524 912 pages: the resident pages of a desktop, 60 132 pages of its zram dump of 28th
September 2026, and a zram dump of a Xiaomi Mi 9T phone, counted five times, so that the
phone does not lose what the desktop's pages gain. The 8 literal tables come from k-means: pages whose
literals look alike go into one group, and each group gets the Huffman code of its literals. The
counts cannot give a page back: they are sums over hundreds of pages and say nothing about which
symbol came after which. The 16 KiB tables are counted on 26 933 pages of a zram dump of the Android
17 emulator with 16 KiB pages.

Since the tables are part of the format, a decoder has to have exactly the right ones.
`docs/format.md` lists the SHA-256 of each table's code lengths, as bytes in symbol order, and
`seqlz_ref.py` refuses to run when they do not match. For 4 KiB pages:

| table | SHA-256 of the code lengths |
| --- | --- |
| `TOK_4k` | `{{ch04.sha.TOK}}` |
| `LL_4k` | `{{ch04.sha.LL}}` |
| `ML_4k` | `{{ch04.sha.ML}}` |
| `LIT_4k` | `{{ch04.sha.LIT}}` |

Fixed tables cost bytes: a table fitted to each page would code it tighter. That was measured several
ways, and none paid for its time: a literal table of each page's own made writes 14% slower for 32
bytes less per page, 2.4% of the stored size
([Per page: its own literal table](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept)),
and tables trained on a phone's own pages saved 0.6% on pages of that phone they were not trained on
([The format written down](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-format-written-down-one-set-of-tables-and-stream-sizes-that-hold)).
How the code lengths become the encoder's codes and the decoder's lookup tables at run time is
[chapter 8](#ch-codes).
