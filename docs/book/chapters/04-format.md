# The format on one page {#ch-format}

A page compressed by seqlz is a string of bytes, and zram stores it together with its length. This
chapter is about those bytes, as
[`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md) specifies them. It
is the map for Part II: when `seqlz_decode()` in [chapter 14](#ch-decode) checks the top bit of the
first two bytes, or [chapter 13](#ch-literals-dec) reads 8 streams of literals, this chapter is
where the bytes they read are described as a whole.

The spec is short, about 440 lines, and it is written so that a decoder can be built from it alone.
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
<svg viewBox="0 0 720 134" role="img" aria-label="Byte 2 of a page with coded literals, 0x20: two zero bits, w minus 5 in bits 3 to 5, the literal table in bits 0 to 2" font-size="12">
  <text x="222.0" y="30" text-anchor="middle" class="mut">bit 7</text>
  <rect x="200" y="40" width="44" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="222.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <text x="266.0" y="30" text-anchor="middle" class="mut">bit 6</text>
  <rect x="244" y="40" width="44" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="266.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <text x="310.0" y="30" text-anchor="middle" class="mut">bit 5</text>
  <rect x="288" y="40" width="44" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="310.0" y="60" text-anchor="middle" font-weight="bold">1</text>
  <text x="354.0" y="30" text-anchor="middle" class="mut">bit 4</text>
  <rect x="332" y="40" width="44" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="354.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <text x="398.0" y="30" text-anchor="middle" class="mut">bit 3</text>
  <rect x="376" y="40" width="44" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="398.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <text x="442.0" y="30" text-anchor="middle" class="mut">bit 2</text>
  <rect x="420" y="40" width="44" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
  <text x="442.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <text x="486.0" y="30" text-anchor="middle" class="mut">bit 1</text>
  <rect x="464" y="40" width="44" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
  <text x="486.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <text x="530.0" y="30" text-anchor="middle" class="mut">bit 0</text>
  <rect x="508" y="40" width="44" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
  <text x="530.0" y="60" text-anchor="middle" font-weight="bold">0</text>
  <path d="M203 78 V84 H285 V78" fill="none" stroke="currentColor"/>
  <text x="244.0" y="102" text-anchor="middle" font-weight="bold">must be 0</text>
  <text x="244.0" y="119" text-anchor="middle">t &gt;&gt; 6</text>
  <path d="M291 78 V84 H417 V78" fill="none" stroke="currentColor"/>
  <text x="354.0" y="102" text-anchor="middle" font-weight="bold">w - 5 = 4</text>
  <text x="354.0" y="119" text-anchor="middle">(t &gt;&gt; 3) &amp; 7</text>
  <path d="M423 78 V84 H549 V78" fill="none" stroke="currentColor"/>
  <text x="486.0" y="102" text-anchor="middle" font-weight="bold">set = 0</text>
  <text x="486.0" y="119" text-anchor="middle">t &amp; 7</text>
  <text x="150" y="60" text-anchor="end">t = 0x20</text>
</svg>
</div>
<figcaption>Byte 2 of the made-up text page below: <code>0x20</code> is literal table 0 and stream sizes of 9 bits.</figcaption>
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
at random from the 12 letters `etaoinshrdlu`, each word followed by a space. `seqlz-fast` stores it
in 4054 bytes, `seqlz-fast-lit` in 2651. The first 12 bytes of the coded page are

```text
60 8f  20  3e 77 f2 f4 89 93 a7 4e 9d
```

- `h = 0x8f60`. The top bit is set, so the literals are coded, and `n = 0x0f60 = 3936`: the page has
  3936 literals, which are 492 per stream.
- `t = 0x20 = 0b00100000`. The table is `0x20 & 7 = 0`, the one that codes this page's literals in the fewest bits; `w = 5 +
  ((0x20 >> 3) & 7) = 5 + 4 = 9`; the top two bits are 0.
- The 9 bytes of sizes are the number `S = 0x9d4ea79389f4f2773e`, the last byte the high one. Its
  lowest 9 bits are `0x13e = 318`, the size of stream 0. The 8 sizes are 318, 315, 316, 318, 312, 316,
  314 and 314 bytes, 2523 together.
- The streams start at byte $3 + w = 12$, the bitstream at $12 + 2523 = 2535$, and it takes the last
  116 bytes of the page.

492 literals in about 315 bytes is 5.1 bits per literal, where the raw page spends 8.

::: why Why the stream sizes take `w` bits and not 2 bytes each
A stream holds every 8th literal, and a literal code has at most 10 bits. On a 4 KiB page that is at
most 512 literals of 10 bits, 640 bytes, which needs 10 bits; on a 16 KiB page 2560 bytes, 12 bits.
So `w` from 5 to 12 holds every case. Most pages have streams below 128 bytes, 7 bits. With 2 bytes
per size, as until 5th October 2026, the header was 19 bytes, and pages were 5.2 and 5.5 bytes
larger on average on the two desktop dumps
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#stream-sizes-in-as-many-bits-as-the-largest-needs-kept)).
Widths of 3 and 4 bits are left out: few pages would use them.
:::

Why the literals are in 8 streams at all is a question of the decoder's speed, see
[chains of dependent steps](#ch-cpu--chains). How the encoder picks the table, and when it codes the
literals at all, is in [chapter 11](#ch-literals-enc).

## Prefix codes from code lengths {#ch-format--codes}

Everything in `B`, and every coded literal, is either a **code** from a table or plain bits. The
tables are fixed: they are part of the format and are not in the page. There are 11 of them:

| table | symbols | longest code | codes |
| --- | --- | --- | --- |
| `TOK`, the tokens and the escape | 3073 | 11 bits | the token of each sequence |
| `LL`, literal length values | 25 (27 on 16 KiB pages) | 8 bits | the rest of a long `ll` |
| `ML`, match length values | 25 (27 on 16 KiB pages) | 8 bits | the rest of a long `ml` |
| literal tables 0 to 7 | 256 each | 10 bits | the coded literals |

A table holds only the **code length** of each symbol, 0 for a symbol without a code. The codes
follow from the lengths by the rule of deflate (RFC 1951, section 3.2.2), the **canonical** code of
[chapter 2](#ch-compression--canonical-codes): shorter codes first, and the codes of one length in
the order of the symbols' numbers. Here is the rule applied to the real token table of 4 KiB pages:

- No symbol has a code of 1, 2 or 3 bits, so `first[4] = 0`. The 4 symbols with a code of 4 bits are,
  in the order of their number, the tokens 34, 49, 2097 and the escape 3072. They get `0000`, `0001`,
  `0010` and `0011`.
- `first[5] = (0 + 4) * 2 = 8`, binary `01000`. The 4 symbols with 5 bits, tokens 576, 2082, 2112 and
  2624, get `01000` to `01011`.
- `first[6] = (8 + 4) * 2 = 24`, binary `011000`, the code of token 177, the first of 5 symbols with
  6 bits.

Every table is **complete**: its codes use up all bit strings, the sum of $2^{-l}$ over all codes is
exactly 1 ([chapter 2](#ch-compression--complete)). For the token table that is 4 codes of 4 bits, 4 of 5, 5 of 6, 11 of 7, 27 of 8, 42 of 9,
76 of 10 and 408 of 11 bits, 577 codes. Counted in units of $\frac{1}{2048}$:
$4 \cdot 128 + 4 \cdot 64 + 5 \cdot 32 + 11 \cdot 16 + 27 \cdot 8 + 42 \cdot 4 + 76 \cdot 2 + 408 = 2048$. So every string of 11 bits starts with exactly one
code, and a decoder never meets bits that are no code. That is what lets `seqlz_decode()` look up 11
bits in a table of 2048 entries without checking the entry; the table is built in
[chapter 8](#ch-codes).

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

Each sequence starts with a **token**, one symbol of `TOK` that holds three numbers:

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
real 4 KiB table, 4 bits each, are:

| token | `ll` | `ml` | class | what it is |
| --- | --- | --- | --- | --- |
| 34 | 2 | 6 | 0 | 2 literals, then 6 bytes from the same offset as the match before |
| 49 | 1 | 7 | 0 | 1 literal, 7 bytes, same offset: the sequence of an array of pointers |
| 2097 | 1 | 7 | 4 | 1 literal, 7 bytes from a new offset that is a multiple of 8 below 256 |
| 3072 | | | | the escape |

Token 49 is the sequence of an array of 8-byte pointers, each 16 bytes further than the one before.
Each pointer differs from the one 8 bytes back only in its lowest byte, so after the first two every
sequence is 1 literal and 7 bytes copied from 8 bytes back, the same offset as before. As token 49,
such a sequence costs 4 bits.

**The escape.** Codes of at most 11 bits have room for 2048 symbols, fewer than the 3072 tokens.
So only the frequent tokens have a code, 576 of them on 4 KiB pages. Any other token is sent as the
escape, symbol 3072, followed by the token's number in 12 plain bits, which hold every number up to
4095. The escape's code has 4 bits, so an escaped token takes 16 bits. Any token may be escaped, also
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
| 0 | 0 | 0 | the offset of the match before | the same again |
| 1 | 4 | 4 | `read(4)` | 1 to 15 |
| 2 | 8 | 8 | `read(8)` | 1 to 255 |
| 3 | 12 | 14 | `read(P)` | 1 to `PAGE - 1` |
| 4 | 5 | 5 | `read(5) * 8` | 8 to 248, multiples of 8 |
| 5 | 9 | 11 | `read(P - 3) * 8` | 8 to `PAGE - 8`, multiples of 8 |

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
| 24 | 24 | 0 | none |
| 24 | 16 | 4 | 5 bits, `read(5) = 3` |
| 8 | 1 | 1 | 4 bits: 8 is a multiple of 8, but below 16 |
| 100 | 1 | 2 | 8 bits |
| 256 | 1 | 5 | 9 bits, `read(9) = 32` |
| 300 | 1 | 3 | 12 bits |

::: why Why offsets are plain bits, and why the class is in the token
Offsets were once a Huffman-coded symbol for their number of bits, followed by the bits. Moving the
class into the token, with plain bits after it, cost 0.4 points of memory (a point is 1% of a page,
41 bytes), and took the decoder from
8758 to 8168 cycles per page, because each sequence then needs one table lookup on the chain from one
token to the next, not two
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
The classes 4 and 5 for multiples of 8 came later and saved 24 and
2 bytes per page in the kernel, for about 0.2 µs more per page written
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
symbols 16 to 24 stand for $b = 4$ to 12, the ranges 16 to 31, 32 to 63, and so on up to 4096 to 8191.
That is 25 symbols on 4 KiB pages, $13 + P$, and 27 on 16 KiB pages. E.g. the value 100 is in the
range 64 to 127, so $b = 6$, the symbol is 18, and `read(6)` is $100 - 64 = 36$. In the real `LL`
table symbol 18 has the code `1010`, so `ll = 115` costs the token, 4 bits of code and 6 plain bits.

The longest code of a length value has 8 bits, so each decode table has 256 entries. A match
length value follows only 8.5% of the matches, which is why `ml` has 5 bits in the token and
not 4: with 4, a value followed 18.7% of them
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).

::: why Why two trained tables and not 5 plain bits
The symbol of a length value could be 5 plain bits, which would take two tables out of the format.
It was measured twice. The first time the verdict was "the tables cost the format two tables once,
the 5 bits cost every page 3 bytes"
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-instead-of-their-tables-3-bytes-per-page-more-not-kept)).
The second time, on the decoder of 10th October 2026 and timed, 5 plain bits made pages 0.2% to 0.3%
larger, 1.6 to 2.8 bytes, and no read faster
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-again-02-to-03-more-memory-no-read-faster-not-kept)).
:::

## How a page ends {#ch-format--end}

A page does not say how many sequences it has, and its bitstream has no length field. Instead:

- **The last sequence is the one whose literals fill the page.** After its literals are copied, the
  page has all of its 4096 bytes, and decoding stops. It has no match, so its token must have `mlf =
  0` and class 0, which means no offset bits either. Any other token there makes the page invalid. A
  page whose last match reaches the end of the page gets one more sequence with `ll = 0`: token 0.
- **The bitstream ends where its bits end.** After the last bit the decoder needs, fewer than 8 bits
  of `B` are left, all of them in its last byte, and all of them 0. The same holds for each literal
  stream. A page with a byte too many, or a bit 1 in the padding, is invalid.

*Why:* zram stores no checksum. A page damaged in memory has to be caught by the decoder, and these
rules catch most damage at one compare per page, at no cost in bytes: the compressor writes nothing
else anyway. Measured, the strict end costs a warm read 0.07 to 0.10 µs on the phone's little core
and nothing in the kernel VM on the PC
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-page-ends-where-its-bits-end-no-damaged-page-with-bytes-or-bits-too-many-decodes-01-µs-on-the-a55-kept)).

Every sequence but the last writes at least 4 bytes, so a decoder stops after at most `PAGE / 4 + 1`
sequences, 1025 on a 4 KiB page, also on a page that is not valid. That bounds the work per page.

Two facts follow from the rules. A valid page has $n \ge 1$: the first match needs at least one byte
before it, and a page without matches is all literals. And the first match's offset is at most that
sequence's `ll`.

## The rules of a valid page {#ch-format--validity}

A page is valid for a page size and the tables of that page size if none of these holds. The first
group is the layout, the second the literal streams, the third the sequences.

1. `len < 2`.
2. `n > PAGE`.
3. Raw literals: `2 + n > len`.
4. Coded literals: `len < 3 + w`.
5. Coded literals: `t >> 6` is not 0.
6. Coded literals: `3 + w + s[0] + ... + s[7] > len`.
7. A literal stream whose codes take more than `8 * s[j]` bits, or 8 or more bits less, or whose bits
   after the last code are not all 0. A stream without literals must have size 0.
8. An escaped token of 3072 or more.
9. A sequence's `ll` larger than the literals left, `n - used`, or than the room left in the page,
   `PAGE - |out|`.
10. The page is full after a sequence's literals, and its token does not have `mlf = 0` and class 0.
11. A match with offset 0, or an offset larger than the bytes written so far, `|out|`.
12. A match longer than the room left in the page, `ml > PAGE - |out|`.
13. At the end, not all `n` literals used.
14. At the end, 8 or more bits of `B` left, or a bit 1 among the fewer than 8 left. This includes a
    page that needs more bits than it has: a decoder may read zeros past the end, and then fails
    this check, because it used bits that are not there.

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
    token: ll, mlf, c
    x = read(offset bits of class c)
    off = (c == 0) ? last : x as the offset of class c
    if ll == 15: ll = 15 + value(LL)
    if ll > n - used or ll > PAGE - |out|: invalid
    append lits[used] to lits[used + ll - 1] to out; used = used + ll
    if |out| == PAGE:                           the last sequence
        if mlf != 0 or c != 0: invalid
        stop
    ml = mlf + 4
    if mlf == 31: ml = 35 + value(ML)
    last = off
    if off == 0 or off > |out| or ml > PAGE - |out|: invalid
    ml times: append the byte off places before the end of out

at the stop: used == n, and fewer than 8 bits of B left, all 0, else invalid
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

The 9 bytes `02 00 61 62 dc 65 ef db 8a` are a 4 KiB page of `ab` 2048 times. `seqlz-fast` and
`seqlz-fast-lit` both write exactly these bytes: 2 literals are far from saving 51 bytes, the
minimum for coding them.

- `02 00`: `h = 2`. The top bit is clear, so the literals are raw, and `n = 2`.
- `61 62`: the 2 literals, `a` and `b`.
- `dc 65 ef db 8a`: `B`, 40 bits.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 196" role="img" aria-label="The 40 bits of the bitstream of the page of ab, grouped into the token 1010, 4 offset bits, the length value symbol 23 with 11 extra bits, and token 0" font-size="12">
  <text x="104" y="36" text-anchor="middle" class="mut">byte 4</text>
  <text x="104" y="54" text-anchor="middle" font-weight="bold">dc</text>
  <text x="232" y="36" text-anchor="middle" class="mut">byte 5</text>
  <text x="232" y="54" text-anchor="middle" font-weight="bold">65</text>
  <text x="360" y="36" text-anchor="middle" class="mut">byte 6</text>
  <text x="360" y="54" text-anchor="middle" font-weight="bold">ef</text>
  <text x="488" y="36" text-anchor="middle" class="mut">byte 7</text>
  <text x="488" y="54" text-anchor="middle" font-weight="bold">db</text>
  <text x="616" y="36" text-anchor="middle" class="mut">byte 8</text>
  <text x="616" y="54" text-anchor="middle" font-weight="bold">8a</text>
  <rect x="40" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="48" y="87" text-anchor="middle">1</text>
  <rect x="56" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="64" y="87" text-anchor="middle">1</text>
  <rect x="72" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="80" y="87" text-anchor="middle">0</text>
  <rect x="88" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="96" y="87" text-anchor="middle">1</text>
  <rect x="104" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="112" y="87" text-anchor="middle">1</text>
  <rect x="120" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="128" y="87" text-anchor="middle">1</text>
  <rect x="136" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="144" y="87" text-anchor="middle">0</text>
  <rect x="152" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="160" y="87" text-anchor="middle">0</text>
  <rect x="168" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="176" y="87" text-anchor="middle">0</text>
  <rect x="184" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="192" y="87" text-anchor="middle">1</text>
  <rect x="200" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="208" y="87" text-anchor="middle">1</text>
  <rect x="216" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="224" y="87" text-anchor="middle">0</text>
  <rect x="232" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="240" y="87" text-anchor="middle">0</text>
  <rect x="248" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="256" y="87" text-anchor="middle">1</text>
  <rect x="264" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="272" y="87" text-anchor="middle">0</text>
  <rect x="280" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="288" y="87" text-anchor="middle">1</text>
  <rect x="296" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="304" y="87" text-anchor="middle">1</text>
  <rect x="312" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="320" y="87" text-anchor="middle">1</text>
  <rect x="328" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="336" y="87" text-anchor="middle">1</text>
  <rect x="344" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="352" y="87" text-anchor="middle">0</text>
  <rect x="360" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="368" y="87" text-anchor="middle">1</text>
  <rect x="376" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="384" y="87" text-anchor="middle">1</text>
  <rect x="392" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="400" y="87" text-anchor="middle">1</text>
  <rect x="408" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="416" y="87" text-anchor="middle">1</text>
  <rect x="424" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="432" y="87" text-anchor="middle">1</text>
  <rect x="440" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="448" y="87" text-anchor="middle">1</text>
  <rect x="456" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="464" y="87" text-anchor="middle">0</text>
  <rect x="472" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="480" y="87" text-anchor="middle">1</text>
  <rect x="488" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="496" y="87" text-anchor="middle">1</text>
  <rect x="504" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="512" y="87" text-anchor="middle">0</text>
  <rect x="520" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="528" y="87" text-anchor="middle">1</text>
  <rect x="536" y="70" width="16" height="24" class="fill-mark" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="544" y="87" text-anchor="middle">1</text>
  <rect x="552" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="560" y="87" text-anchor="middle">1</text>
  <rect x="568" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="576" y="87" text-anchor="middle">0</text>
  <rect x="584" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="592" y="87" text-anchor="middle">0</text>
  <rect x="600" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="608" y="87" text-anchor="middle">0</text>
  <rect x="616" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="624" y="87" text-anchor="middle">1</text>
  <rect x="632" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="640" y="87" text-anchor="middle">0</text>
  <rect x="648" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="656" y="87" text-anchor="middle">1</text>
  <rect x="664" y="70" width="16" height="24" class="fill-acc" fill-opacity="0.22" stroke="currentColor" stroke-width="0.5"/>
  <text x="672" y="87" text-anchor="middle">0</text>
  <line x1="40" y1="64" x2="40" y2="100" stroke="currentColor" stroke-width="2"/>
  <line x1="168" y1="64" x2="168" y2="100" stroke="currentColor" stroke-width="2"/>
  <line x1="296" y1="64" x2="296" y2="100" stroke="currentColor" stroke-width="2"/>
  <line x1="424" y1="64" x2="424" y2="100" stroke="currentColor" stroke-width="2"/>
  <line x1="552" y1="64" x2="552" y2="100" stroke="currentColor" stroke-width="2"/>
  <line x1="680" y1="64" x2="680" y2="100" stroke="currentColor" stroke-width="2"/>
  <g class="acc"><path d="M42 104 V110 H214 V104" fill="none" stroke="currentColor" stroke-width="1.5"/>
  <text x="128.0" y="128" text-anchor="middle" font-weight="bold">TOK 1010</text></g>
  <text x="128.0" y="145" text-anchor="middle">ll 2, ml-4 31, class 1</text>
  <g class="mark"><path d="M218 104 V110 H278 V104" fill="none" stroke="currentColor" stroke-width="1.5"/>
  <text x="248.0" y="128" text-anchor="middle" font-weight="bold">off 2</text></g>
  <text x="248.0" y="145" text-anchor="middle">read(4)</text>
  <g class="acc"><path d="M282 104 V110 H374 V104" fill="none" stroke="currentColor" stroke-width="1.5"/>
  <text x="328.0" y="128" text-anchor="middle" font-weight="bold">ML 23</text></g>
  <text x="328.0" y="145" text-anchor="middle">b = 11</text>
  <g class="mark"><path d="M378 104 V110 H550 V104" fill="none" stroke="currentColor" stroke-width="1.5"/>
  <text x="464.0" y="128" text-anchor="middle" font-weight="bold">2011</text></g>
  <text x="464.0" y="145" text-anchor="middle">ml = 4094</text>
  <g class="acc"><path d="M554 104 V110 H678 V104" fill="none" stroke="currentColor" stroke-width="1.5"/>
  <text x="616.0" y="128" text-anchor="middle" font-weight="bold">TOK 0</text></g>
  <text x="616.0" y="145" text-anchor="middle">ll 0, the end</text>
  <g class="acc"><rect x="40" y="168" width="14" height="14" class="fill-acc" fill-opacity="0.22" stroke="currentColor"/></g>
  <text x="62" y="180" text-anchor="start">a code of a table: TOK or ML</text>
  <g class="mark"><rect x="330" y="168" width="14" height="14" class="fill-mark" fill-opacity="0.22" stroke="currentColor"/></g>
  <text x="352" y="180" text-anchor="start">plain bits, read(n): offset bits and extra bits</text>
</svg>
</div>
<figcaption>The bitstream <code>dc 65 ef db 8a</code>, bit 7 of each byte first: codes from the tables in one colour, plain bits in the other.</figcaption>
</figure>

Step by step, with the codes of the real 4 KiB tables:

| bits | read as | means |
| --- | --- | --- |
| `11011100011` | `symbol(TOK)` = 1010 | $1010 = 2 + 16 \cdot 31 + 512 \cdot 1$: `ll = 2`, `mlf = 31`, class 1 |
| `0010` | `read(4)` = 2 | class 1: `off = 2` |
| | | `ll` is not 15, no `ll` value: append `a b`, the page has 2 bytes |
| `111101` | `symbol(ML)` = 23 | `mlf` is 31, so a length value: $b = 23 - 12 = 11$ |
| `11111011011` | `read(11)` = 2011 | value $2^{11} + 2011 = 4059$, `ml = 35 + 4059 = 4094` |
| | | copy 4094 bytes from 2 back: `abab...`, the page has 4096 bytes |
| `10001010` | `symbol(TOK)` = 0 | `ll = 0`, `mlf = 0`, class 0, no offset bits |
| | | no literals to copy, the page is full: the last sequence, valid |

After the last code, 0 bits of `B` are left, fewer than 8 and all zero. All 2 literals are used. The
page is valid.

The match is the case where copying byte by byte matters: it starts 2 bytes back and is 4094 bytes
long, so after its first 2 bytes it copies bytes it wrote itself. A page of 4096 bytes takes 9 bytes.

::: try Decode it with the spec's decoder
In the repository, write the 9 bytes to a file and give it to the reference decoder. It prints
whether the page is valid and the SHA-256 of the decoded page, which is the SHA-256 of `ab` 2048
times:

```text
printf '\x02\x00\x61\x62\xdc\x65\xef\xdb\x8a' > /tmp/ab.seqlz
python3 tools/seqlz-ref/seqlz_ref.py /tmp/ab.seqlz
/tmp/ab.seqlz valid 2c888a3809c425e853a6ac08e1c1ffd9393fe3e49419e8f71ac23ab0362617a3
python3 -c "import hashlib; print(hashlib.sha256(b'ab' * 2048).hexdigest())"
2c888a3809c425e853a6ac08e1c1ffd9393fe3e49419e8f71ac23ab0362617a3
```

Flip any bit of the 9 bytes and run it again: most flips make the page invalid. The repository also
has an animated version of this, `docs/seqlz-bit-by-bit.html`, which decodes three pages step by
step; download it and open it in a browser.
:::

## A second page: four sequences and 7 bits of padding {#ch-format--example2}

The page of `ab` has one match. This one has three, with three kinds of offset, and a bitstream that
does not end on a byte boundary. It is made up for this book:

```text
bytes 0 to 11:   abcdefghijkl
bytes 12 to 15:  llll
bytes 16 to 17:  xy
bytes 18 to 24:  cdefghi       the same 7 bytes as at 2 to 8, 16 bytes back
bytes 25 to 4095: z, 4071 times
```

`seqlz_compress()` writes it in 26 bytes, with and without coded literals the same: 15 literals
cannot save 51 bytes.

```text
0f 00  61 62 63 64 65 66 67 68 69 6a 6b 6c 78 79 7a  cd 96 71 6e 21 f7 e1 c5 00
```

`h = 15`, raw literals, and the 15 literals are `abcdefghijklxyz`. `B` is 9 bytes, 72 bits:

| bits | read as | means |
| --- | --- | --- |
| `11001101100` | `symbol(TOK)` = 12 | `ll = 12`, `mlf = 0`, class 0 |
| | class 0: no bits | `off = last = 1`, the start value |
| | | append `abcdefghijkl`, then 4 bytes from 1 back: `llll`; 16 bytes |
| `101100111` | `symbol(TOK)` = 2098 | `ll = 2`, `mlf = 3`, class 4 |
| `00010` | `read(5)` = 2 | `off = 2 * 8 = 16` |
| | | append `xy`, then 7 bytes from 16 back: `cdefghi`; 25 bytes |
| `11011100010` | `symbol(TOK)` = 1009 | `ll = 1`, `mlf = 31`, class 1 |
| `0001` | `read(4)` = 1 | `off = 1` |
| | | append `z`; 26 bytes |
| `111101` | `symbol(ML)` = 23 | $b = 11$ |
| `11111000011` | `read(11)` = 1987 | `ml = 35 + 2^11 + 1987 = 4070`: `z` 4070 more times; 4096 bytes |
| `10001010` | `symbol(TOK)` = 0 | `ll = 0`, the page is full: the last sequence |
| `0000000` | | 7 bits left, in the last byte, all 0: valid |

A few things to see here:

- **The first match uses the repeat offset.** At the start of a page `last` is 1, and the run `llll`
  is 4 bytes from 1 back, so it takes 0 offset bits. The matcher found it with that same candidate,
  see [chapter 9](#ch-matcher).
- **Token 12 has an 11-bit code.** 12 literals and a match of exactly 4 bytes at the repeat offset is
  rare on real pages, so its code is long. A token without any code would have taken 16 bits.
- **Token 2098 is "2 literals, a match of 7 at offset 16".** 16 is a multiple of 8 from 16 on, so the
  class is 4 and the offset is sent as 2 in 5 bits. The sequence takes $9 + 5 = 14$ bits plus its 2
  literals. In lz4's format the same sequence is a token byte and 2 bytes of offset, 24 bits plus
  the literals.
- **The third match changes the class but not the bits' meaning.** Offset 1 is not the repeat offset
  any more, `last` is 16 now, so it goes as class 1 with 4 bits.
- **The padding.** 65 bits are used, 7 are left in the last byte, and they are 0. With an eighth bit
  left, or one of them 1, the page would be invalid.

`python3 tools/seqlz-ref/seqlz_ref.py` agrees: it decodes the 26 bytes to the page above.

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
September 2026, and the first zram dump of a Xiaomi Mi 9T phone, counted five times, so that the
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
| `TOK_4k` | `8348c354f914ba1510728521a7ee3d1469e998ec8ff71dcdd35140e6c9d6e997` |
| `LL_4k` | `f019bfe73f61b5452473b27764ae587244bb5e31ddfca19c4a20288f2cce84d5` |
| `ML_4k` | `25e6a220de355e0508ac0478b9ba4df4511bb4477600f2741e7feed9206b375b` |
| `LIT_4k` | `efa140477f179ce5a9c5cc27b462fe5da459839b2ff71e0ee94fef939d63f4c2` |

Fixed tables cost bytes: a table fitted to each page would code it tighter. That was measured several
ways, and none paid for its time: a literal table of each page's own made writes 13% and 15% slower
on the two desktop dumps for 2.7 and 60.6 bytes less per page, 0.3% and 4.6% of the stored size, and tables trained on a phone's own pages saved at most 1% on another
dump of that phone ([docs/seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-bytes-of-a-page)).
How the code lengths become the encoder's codes and the decoder's lookup tables at run time is
[chapter 8](#ch-codes).
