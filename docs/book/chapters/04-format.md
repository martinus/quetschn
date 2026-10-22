# The format on one page {#ch-format}

A page compressed by seqlz is a string of bytes, and zram stores it together with its length. This
chapter is about those bytes, as
[`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md) specifies them, and
it is the map for Part II: when `seqlz_decode()` in [chapter 14](#ch-decode) checks the top bit of the
first two bytes, or [chapter 13](#ch-literals-dec) reads 8 streams of literals, this is where the
bytes they read are described as a whole.

The spec is about {{ch04.format_lines}} lines, written so that a decoder can be built from it alone. Two such decoders
exist, `tools/seqlz-ref/seqlz_ref.py` and `tools/seqlz-ref/seqlz_ref.c`, both slow on purpose. CI
decodes 22 compressed pages with the Python one, and the unit tests and a fuzzer compare the C one
with `seqlz_decode()`. The format says which pages are valid and what they decode to, not how a
compressor finds them: one page of memory can be written in many ways, as with `lz4` or `zstd`. Where
this chapter says what `seqlz_compress()` writes, it says so.

## Two layouts {#ch-format--layout}

A compressed page starts with a 2-byte number `h`, little endian. The top bit, `h & 0x8000`, says
which of the two layouts follows; the other 15 bits, `n = h & 0x7fff`, are the number of literals of
the whole page, and `n > PAGE` makes the page invalid.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 238" role="img" aria-label="The two layouts of a compressed page: a 2-byte header, the literals, then the bitstream; with coded literals the header also has byte 2, with literals as deltas the byte that says which delta, the stream sizes, with code 8 the page's own table, and 8 streams" font-size="12">
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
  <rect x="140" y="134" width="26" height="34" class="fill-sheet" stroke="currentColor" stroke-dasharray="4 3"/>
  <text x="153.0" y="156" text-anchor="middle">dt</text>
  <rect x="166" y="134" width="100" height="34" class="fill-sheet" stroke="currentColor"/>
  <text x="216.0" y="156" text-anchor="middle">s[0] to s[7]</text>
  <rect x="266" y="134" width="50" height="34" class="fill-sheet" stroke="currentColor" stroke-dasharray="4 3"/>
  <text x="291.0" y="156" text-anchor="middle">own</text>
  <rect x="316" y="134" width="33" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="332.5" y="156" text-anchor="middle">0</text>
  <rect x="349" y="134" width="33" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="365.5" y="156" text-anchor="middle">1</text>
  <rect x="382" y="134" width="33" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="398.5" y="156" text-anchor="middle">2</text>
  <rect x="415" y="134" width="33" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="431.5" y="156" text-anchor="middle">3</text>
  <rect x="448" y="134" width="33" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="464.5" y="156" text-anchor="middle">4</text>
  <rect x="481" y="134" width="33" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="497.5" y="156" text-anchor="middle">5</text>
  <rect x="514" y="134" width="33" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="530.5" y="156" text-anchor="middle">6</text>
  <rect x="547" y="134" width="33" height="34" class="fill-mark" stroke="currentColor" fill-opacity="0.35"/>
  <text x="563.5" y="156" text-anchor="middle">7</text>
  <rect x="580" y="134" width="120" height="34" class="fill-acc" stroke="currentColor" fill-opacity="0.35"/>
  <text x="640.0" y="156" text-anchor="middle">B, the bitstream</text>
  <line x1="20" y1="168" x2="20" y2="176" stroke="currentColor"/>
  <text x="20" y="190" text-anchor="middle" class="mut">0</text>
  <line x1="110" y1="168" x2="110" y2="176" stroke="currentColor"/>
  <text x="110" y="190" text-anchor="middle" class="mut">2</text>
  <line x1="140" y1="168" x2="140" y2="176" stroke="currentColor"/>
  <text x="140" y="190" text-anchor="middle" class="mut">3</text>
  <line x1="166" y1="168" x2="166" y2="176" stroke="currentColor"/>
  <text x="170" y="190" text-anchor="middle" class="mut">+ e</text>
  <line x1="266" y1="168" x2="266" y2="176" stroke="currentColor"/>
  <text x="270" y="190" text-anchor="middle" class="mut">+ w</text>
  <line x1="316" y1="168" x2="316" y2="176" stroke="currentColor"/>
  <text x="320" y="190" text-anchor="middle" class="mut">+ d</text>
  <line x1="580" y1="168" x2="580" y2="176" stroke="currentColor"/>
  <text x="580" y="190" text-anchor="middle" class="mut">3 + w + e + d + Σs</text>
  <line x1="700" y1="168" x2="700" y2="176" stroke="currentColor"/>
  <text x="700" y="190" text-anchor="middle" class="mut">len</text>
  <text x="448" y="210" text-anchor="middle">stream 0 to 7, s[j] bytes each, literal k in stream k mod 8</text>
  <text x="20" y="228" text-anchor="start" class="mut">dt: e bytes, 1 if the literals are deltas, else 0; own: d bytes, with code 8 only</text>
</svg>
</div>
<figcaption>The two layouts. The offsets below each box are byte offsets in the compressed page of <code>len</code> bytes; the dashed boxes are there only with literals as deltas, <code>e</code> = 1, and with code 8, the page's own literal table, else <code>e</code> and <code>d</code> are 0.</figcaption>
</figure>

**Raw literals.** With the top bit clear, the `n` literals follow as they are, in page order, and
everything after them, up to the end of the compressed page, is `B`, the bitstream of the sequences.
`B` has no length of its own: it ends where the page ends, and zram knows where that is. This is the
only layout `seqlz-1` writes, and levels 2 and 3 write it for every page whose literals do not
code well enough.

**Coded literals.** With the top bit set, the literals are Huffman coded, with one of 8 fixed literal
tables or with a table that the page brings, and more fields come first. Byte 2, `t`:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 134" role="img" aria-label="Byte 2 of a page with coded literals, {{ch04.text.t}}: the delta bit, w minus 5 in bits 4 to 6, the literals' code in bits 0 to 3" font-size="12">
  <text x="222.0" y="30" text-anchor="middle" class="mut">bit 7</text>
  <rect x="200" y="40" width="44" height="30" class="fill-sheet" stroke="currentColor"/>
  <text x="222.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.7}}</text>
  <text x="266.0" y="30" text-anchor="middle" class="mut">bit 6</text>
  <rect x="244" y="40" width="44" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="266.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.6}}</text>
  <text x="310.0" y="30" text-anchor="middle" class="mut">bit 5</text>
  <rect x="288" y="40" width="44" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="310.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.5}}</text>
  <text x="354.0" y="30" text-anchor="middle" class="mut">bit 4</text>
  <rect x="332" y="40" width="44" height="30" class="fill-mark" stroke="currentColor" fill-opacity="0.3"/>
  <text x="354.0" y="60" text-anchor="middle" font-weight="bold">{{ch04.text.t_bit.4}}</text>
  <text x="398.0" y="30" text-anchor="middle" class="mut">bit 3</text>
  <rect x="376" y="40" width="44" height="30" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/>
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
  <path d="M203 78 V84 H241 V78" fill="none" stroke="currentColor"/>
  <text x="222.0" y="102" text-anchor="middle" font-weight="bold">e = {{ch04.text.t_bit.7}}</text>
  <text x="222.0" y="119" text-anchor="middle">t &gt;&gt; 7</text>
  <path d="M247 78 V84 H373 V78" fill="none" stroke="currentColor"/>
  <text x="310.0" y="102" text-anchor="middle" font-weight="bold">w - 5 = {{ch04.text.w5}}</text>
  <text x="310.0" y="119" text-anchor="middle">(t &gt;&gt; 4) &amp; 7</text>
  <path d="M379 78 V84 H549 V78" fill="none" stroke="currentColor"/>
  <text x="464.0" y="102" text-anchor="middle" font-weight="bold">code = {{ch04.text.set}}</text>
  <text x="464.0" y="119" text-anchor="middle">t &amp; 15</text>
  <text x="150" y="60" text-anchor="end">t = {{ch04.text.t}}</text>
</svg>
</div>
<figcaption>Byte 2 of the made-up text page below: <code>{{ch04.text.t}}</code> is literal table {{ch04.text.set}} and stream sizes of {{ch04.text.w}} bits.</figcaption>
</figure>

- `code = t & 15` says how the literals are coded: 0 to 7 is one of the 8 fixed literal tables, 8
  (`SEQLZ_LIT_OWN`) a table of the page's own ([below](#ch-format--own)), and 9 to 15 make the page
  invalid.
- `w = 5 + ((t >> 4) & 7)` is the number of bits of each stream size, 5 to 12.
- `e = t >> 7` says whether the literals are stored as deltas to earlier bytes of the page
  ([below](#ch-format--deltas)), which only `seqlz-3` writes. The codes 9 to 15 are free for a later format
  ([no version in the page](#ch-format--version)).

With `e = 1` byte 3 is `dt`, which says which earlier byte each delta is to. The next `w` bytes, from
byte $3 + e$ on, are one little-endian number `S` of `8 * w` bits with the 8 stream sizes, `w` bits
each, the lowest first: `s[j] = (S >> (j * w)) & ((1 << w) - 1)`. With code 8 the page's own table follows,
`d` bytes; with the codes 0 to 7, `d` is 0. Then come the 8 streams, `s[j]`
bytes each, and after the last one `B`. The literals are dealt out to the streams like cards: stream
$j$ holds literals $j$, $j + 8$, $j + 16$ and so on, each as a code of the table that `code` names,
read most significant bit first like `B`. A stream's codes must end in its last byte, and the bits
after the last code are 0; a stream without literals has `s[j] = 0`. All 8 streams use the same table:
that there are 8 streams and 8 fixed tables is a coincidence.

A page made up for this book: 4096 bytes of "words" of 2 to 9 letters, picked at random from the 12
letters `etaoinshrdlu` by the C library's classic random number generator from seed 1, each word
followed by a space. `seqlz-1` stores it in {{ch04.text.raw}} bytes, `seqlz-2` in
{{ch04.text.len}}. The first {{ch04.text.streams_at}} bytes of the coded page are

```text
{{ch04.text.hdr}}  {{ch04.text.t_hex}}  {{ch04.text.sizes_hex}}
```

- `h = {{ch04.text.h}}`: coded literals, and `n = {{ch04.text.n_hex}} = {{ch04.text.n}}`,
  {{ch04.text.per_stream}} per stream.
- `t = {{ch04.text.t}} = 0b{{ch04.text.t_bin}}`: code `{{ch04.text.t}} & 15 = {{ch04.text.set}}`,
  the fixed table that codes this page's literals in the fewest bits, and
  `w = 5 + (({{ch04.text.t}} >> 4) & 7) = 5 + {{ch04.text.w5}} = {{ch04.text.w}}`.
- The {{ch04.text.w}} bytes of sizes are `S = {{ch04.text.S}}`. Its lowest {{ch04.text.w}} bits,
  `{{ch04.text.s0_hex}} = {{ch04.text.s.0}}`, are the size of stream 0; the 8 sizes are
  {{ch04.text.s.0}}, {{ch04.text.s.1}}, {{ch04.text.s.2}}, {{ch04.text.s.3}}, {{ch04.text.s.4}},
  {{ch04.text.s.5}}, {{ch04.text.s.6}} and {{ch04.text.s.7}} bytes, {{ch04.text.sum}} together.
- No deltas and no own table, `e = 0` and `d = 0`: the streams start at byte
  $3 + w = {{ch04.text.streams_at}}$, `B` at
  ${{ch04.text.streams_at}} + {{ch04.text.sum}} = {{ch04.text.b_at}}$, and takes the last
  {{ch04.text.b_bytes}} bytes of the page.
@assert ch04.text.best == ch04.text.set and ch04.text.set < 8 and ch04.text.t_bit[7] == 0

{{ch04.text.per_stream}} literals in about {{ch04.text.avg}} bytes is {{ch04.text.bits_per_lit}} bits
per literal, where the raw page spends 8. `seqlz-3` stores the same page in {{own.text.len}} bytes, with a
table of its own, [below](#ch-format--own).

::: why Why the stream sizes take `w` bits and not 2 bytes each
A stream holds every 8th literal, and a literal code has at most 10 bits: on a 4 KiB page at most 640
bytes, 10 bits, on a 16 KiB page 2560 bytes, 12 bits. The largest stream of most pages is below 128
bytes, 7 bits. With 2 bytes per size, as until 5th October 2026, the header was 19 bytes, and pages
were 5.3 bytes larger on average
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#stream-sizes-in-as-many-bits-as-the-largest-needs-kept)).
Widths of 3 and 4 bits are left out: few pages would use them.
:::

Why the literals are in 8 streams is a question of the decoder's speed
([chains of dependent steps](#ch-cpu--chains)). How the encoder picks the table, and when it codes the
literals at all, is in [chapter 11](#ch-literals-enc).

## Prefix codes from code lengths {#ch-format--codes}

Everything in `B`, and every coded literal, is either a code from a table or plain bits. The tables
are part of the format, not of the page. There are {{ch17.tables}}:

| table | symbols | longest code allowed | codes |
| --- | --- | --- | --- |
| `TOK`, the tokens and the escape | {{tok.symbols}} | {{ch06.TOKEN_BITS}} bits | the token of each sequence |
| `LL`, literal length values | {{ll.symbols}} ({{k16.ll.symbols}} on 16 KiB pages) | {{ch06.MAX_BITS}} bits | the rest of a long `ll` |
| `ML`, match length values | {{ml.symbols}} ({{k16.ml.symbols}} on 16 KiB pages) | {{ch06.MAX_BITS}} bits | the rest of a long `ml` |
| literal tables 0 to 7 | 256 each | {{ch06.LIT_BITS}} bits | the coded literals |
| `OWN`, a page's own literal table | {{own.symbols}} | {{own.code_bits}} bits | the lengths of a page's own table, [below](#ch-format--own) |
@assert ch04.longest.tok == ch06.TOKEN_BITS and ch04.longest.ll <= ch06.MAX_BITS and ch04.longest.ml <= ch06.MAX_BITS and ch04.longest.lit == ch06.LIT_BITS

A table holds only the **code length** of each symbol, 0 for a symbol without a code, and the codes
follow by the rule of deflate, the [canonical code](#ch-compression--canonical-codes). Applied to the
real token table of 4 KiB pages:

- No symbol has a code of 1, 2 or 3 bits, so `first[4] = {{ch04.first.4}}`. The
  {{tok.codes_of.4}} symbols with a code of 4 bits are, in the order of their number, the tokens
  {{ch04.syms.4.0}} and {{ch04.syms.4.1}} and the escape {{ch04.syms.4.2}}. They get
  `{{tok.code.49}}`, `{{tok.code.2097}}` and `{{tok.code.3072}}`.
- `first[5] = ({{ch04.first.4}} + {{tok.codes_of.4}}) * 2 = {{ch04.first.5}}`, binary
  `{{ch04.first_bin.5}}`. The {{tok.codes_of.5}} symbols with 5 bits, tokens {{ch04.syms.5.0}},
  {{ch04.syms.5.1}} and {{ch04.syms.5.2}}, get `{{tok.code.34}}` to `{{tok.code.2112}}`.
- `first[6] = ({{ch04.first.5}} + {{tok.codes_of.5}}) * 2 = {{ch04.first.6}}`, binary
  `{{ch04.first_bin.6}}`, the code of token {{ch04.syms.6.0}}, the first of {{tok.codes_of.6}}
  symbols with 6 bits.
@assert tok.codes_of[1] + tok.codes_of[2] + tok.codes_of[3] == 0 and F("ch04.syms.4") == [49, 2097, 3072] and F("ch04.syms.5") == [34, 2082, 2112]

Every table is [complete](#ch-compression--complete). For the token table that is
{{tok.codes_of.4}} codes of 4 bits, {{tok.codes_of.5}} of 5, {{tok.codes_of.6}} of 6,
{{tok.codes_of.7}} of 7, {{tok.codes_of.8}} of 8, {{tok.codes_of.9}} of 9, {{tok.codes_of.10}} of 10
and {{tok.codes_of.11}} of 11 bits, {{tok.with_code}} codes, and in units of $\frac{1}{2048}$:
${{tok.codes_of.4}} \cdot 128 + {{tok.codes_of.5}} \cdot 64 + {{tok.codes_of.6}} \cdot 32 + {{tok.codes_of.7}} \cdot 16 + {{tok.codes_of.8}} \cdot 8 + {{tok.codes_of.9}} \cdot 4 + {{tok.codes_of.10}} \cdot 2 + {{tok.codes_of.11}} = 2048$.
So `seqlz_decode()` can look up 11 bits in a table of 2048 entries without checking the entry; the
table is built in [chapter 8](#ch-codes).
@assert tok.codes_of[4] * 128 + tok.codes_of[5] * 64 + tok.codes_of[6] * 32 + tok.codes_of[7] * 16 + tok.codes_of[8] * 8 + tok.codes_of[9] * 4 + tok.codes_of[10] * 2 + tok.codes_of[11] == 2048 and tok.with_code == tok.codes_of[4] + tok.codes_of[5] + tok.codes_of[6] + tok.codes_of[7] + tok.codes_of[8] + tok.codes_of[9] + tok.codes_of[10] + tok.codes_of[11]

## The bitstream {#ch-format--bitstream}

`B` is read most significant bit first: bit `i` of `B` is bit `7 - (i mod 8)` of byte `i div 8`, so
the byte `23` is read as `0`, `0`, `1`, `0`, `0`, `0`, `1`, `1`. The spec reads three kinds of things
from it: `read(n)`, the next `n` bits as a number, the first bit read the most significant, `read(0)`
being 0; `symbol(table)`, bits one by one until they are a code of the table; and `value(table)`, a
[length value](#ch-format--length-values). The bit order does not change the size of a page, only how
many of the decoder's cache lines a page needs ([chapter 3](#ch-cpu--msb-first)).

## A page's own literal table, in byte order {#ch-format--own}

With code 8 the page brings its own literal table, right after `dt` and the stream sizes, at
byte $3 + w + e$. It is
read most significant bit first, like `B`. It is not a list of 256 code lengths: it is a list of
symbols of one more fixed table, `OWN`, which goes through the byte values from 0 up and says for each
one whether it has a code, and how long it is:

- symbol `l - 1`, for `l` from 1 to 10: the next byte value has a code of `l` bits;
- symbol `9 + k`, for `k` from 1 to 9, then `x = read(k - 1)`: the next $2^{k-1} + x$ byte values have
  no code, a **run**. Symbol 10 is a run of 1 with no bits after it, symbol 15 with the 5 bits `00000`
  a run of 32.

A run is a count of byte values in a row that have no code. Small runs are common, a table skips
one or two byte values here and there, and long ones are rare, e.g. the 32 control characters below
the space in a text page. So the code for a run has to be short for small counts and still work for
large ones. `OWN` writes a run in two parts: first how many bits the count has in binary, its
**width** `k`, as the symbol `9 + k`; then the bits of the count below its first 1:

| run | in binary | width `k` | symbol `9 + k` | bits after the symbol |
| ---: | --- | ---: | ---: | --- |
| 1 | `1` | 1 | 10 | none |
| 2 | `10` | 2 | 11 | `0` |
| 3 | `11` | 2 | 11 | `1` |
| 4 | `100` | 3 | 12 | `00` |
| 5 | `101` | 3 | 12 | `01` |
| 7 | `111` | 3 | 12 | `11` |
| 32 | `100000` | 6 | 15 | `00000` |
| 100 | `1100100` | 7 | 16 | `100100` |

The first bit of a number in binary is always a 1, else the number would have one bit less. So once the
decoder knows the width, it knows that bit too, and only the `k - 1` bits below it are in the
bitstream. To decode, it puts the 1 back in front: symbol 12 means width 3, `read(2)` gives `01`, and a
1 in front of `01` is `101`, a run of 5. The rule above, $2^{k-1} + x$, says the same with numbers
instead of bits: $2^{k-1}$ is the 1 in front, 4 for `k = 3`, and `x` is the number the bits after the
symbol make, 1 for `01`.

Writing a number as its width and then its bits is an old idea. Its textbook form is the
[Elias gamma code](https://en.wikipedia.org/wiki/Elias_gamma_coding): `k - 1` zeros, then all `k`
bits of the number, the 1 in front included. The decoder counts zeros until it reads a 1; that 1 is
the number's first bit, and as many bits follow as it counted zeros. A run of 5 is `00` and `101`,
`00101`, and a run of 32 is `00000100000`, 11 bits. The
[Exp-Golomb code](https://en.wikipedia.org/wiki/Exponential-Golomb_coding) of order 0 is the same code
for numbers that start at 0: it writes `n` as Elias gamma writes `n + 1`, so a run as `run - 1`
gives the same bits. [Chapter 17](#ch-decisions--level3) has its other orders.

`OWN` does not write the zeros: the width is one of its symbols, in the same prefix code as the
lengths. This does two things. One symbol says that a run comes and how wide it is; with Elias gamma
each symbol would need one more bit in front, to tell a run from a length. And like every
[Huffman code](#ch-compression--huffman), `OWN` gives a symbol the shorter a code the more often it came
on the pages `OWN` was counted on, lengths and runs together. On those pages the lengths came more
often than the runs, so the lengths of 9 and 10 bits got codes of {{own.len.8}} bits, 7 and 8 bits
codes of {{own.len.6}} bits, and a run costs more than in Elias gamma (the whole code is in the table
below):

| run | bits in `OWN`: the symbol and the bits after it | bits in Elias gamma, and 1 for run or length |
| --- | ---: | ---: |
| 1 | {{own.len.10}} | 1 + 1 |
| 2 or 3 | {{own.len.11}} + 1 | 3 + 1 |
| 4 to 7 | {{own.len.12}} + 2 | 5 + 1 |
| 32 to 63 | {{own.len.15}} + 5 | 11 + 1 |
@assert own.len[10] > 2 and own.len[11] + 1 > 4 and own.len[12] + 2 > 6 and own.len[15] + 5 > 12 and own.len[8] == own.len[9] == 2 and own.len[6] == own.len[7] == 3

The bits go where a page's table spends them: every byte value with a code costs one length, and a
run comes only between them. In the example below, {{own.text.with_code}} of the {{own.text.rows}}
symbols are lengths.

The list ends as soon as the codes fill the code space, when $\sum 2^{-l}$ over the lengths so far
is exactly 1: the code is then [complete](#ch-compression--complete), and the byte values after the
last one have no code. The bits after the last symbol, up to the end of its byte, are 0, and `d` is
the number of bytes the table takes. The codes follow from the lengths as for every table, the
[canonical code](#ch-compression--canonical-codes), with the byte value as the symbol. `OWN` has
{{own.symbols}} symbols and codes of at most {{own.code_bits}} bits, by length:

| symbols | mean | codes |
| --- | --- | --- |
| 8, 9 | lengths 9 and 10 | `{{own.code.8}}`, `{{own.code.9}}` |
| 6, 7 | lengths 7 and 8 | `{{own.code.6}}`, `{{own.code.7}}` |
| 5, 10 | length 6, a run of 1 | `{{own.code.5}}`, `{{own.code.10}}` |
| 4, 11 | length 5, a run of 2 or 3 | `{{own.code.4}}`, `{{own.code.11}}` |
| 12 | a run of 4 to 7 | `{{own.code.12}}` |
| 2, 3 | lengths 3 and 4 | `{{own.code.2}}`, `{{own.code.3}}` |
| 0, 1, 13 to 18 | lengths 1 and 2, runs of 8 to 15, ..., 256 to 511 | `{{own.code.0}}` to `{{own.code.18}}` |
@assert own.len[8] == own.len[9] == 2 and own.len[6] == own.len[7] == 3 and own.len[5] == own.len[10] == 4 and own.len[4] == own.len[11] == 5 and own.len[12] == 6 and own.len[2] == own.len[3] == 7 and own.len[0] == own.len[1] == own.len[13] == own.len[18] == 8 and own.symbols == 19

The lengths that pages' own tables have most, 7 to 10 bits, get the shortest codes. The
description is in the order of the byte values, so a byte value costs no bits of its own, only its
length does; `docs/format.md` gives about 0.45 bytes per byte value with a code, 60 to 120 bytes on
most pages that have a table of their own
([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#a-pages-own-literal-table)).
A run of 256 to 511 can never be valid, as a table has only 256 byte values; it is there so that `OWN`
is complete too. Sent in byte order, with runs, the descriptions made pages 0.5% smaller than the ones
of 9th October, which listed the bytes by code length, each as its distance to the one before
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-pages-own-literal-table-in-the-order-of-the-bytes-decoded-two-lengths-at-a-time-05-smaller-than-by-code-length-reads-29-slower-on-the-a76-kept)). Where its counts come from is [below](#ch-format--tables).

The text page above, at level 3, is {{own.text.len}} bytes. Its byte 2 is `{{own.text.t}}`,
`0b{{own.text.t_bin}}`: code {{own.text.lcode}} and `w = 5 + {{own.text.w5}} = {{own.text.w}}`. The
{{own.text.dbytes}} bytes from byte {{own.text.desc_at}} on are its table:

```text
{{own.text.desc}}
```

| bits | symbol | means | $\sum 2^{-l}$, in 1/1024 |
| --- | --- | --- | --- |
| `{{own.text.d.0.bits}}` | {{own.text.d.0.sym}} | {{own.text.d.0.what}} | {{own.text.d.0.space}} |
| `{{own.text.d.1.bits}}` | {{own.text.d.1.sym}} | {{own.text.d.1.what}} | {{own.text.d.1.space}} |
| `{{own.text.d.2.bits}}` | {{own.text.d.2.sym}} | {{own.text.d.2.what}} | {{own.text.d.2.space}} |
| `{{own.text.d.3.bits}}` | {{own.text.d.3.sym}} | {{own.text.d.3.what}} | {{own.text.d.3.space}} |
| `{{own.text.d.4.bits}}` | {{own.text.d.4.sym}} | {{own.text.d.4.what}} | {{own.text.d.4.space}} |
| `{{own.text.d.5.bits}}` | {{own.text.d.5.sym}} | {{own.text.d.5.what}} | {{own.text.d.5.space}} |
| `{{own.text.d.6.bits}}` | {{own.text.d.6.sym}} | {{own.text.d.6.what}} | {{own.text.d.6.space}} |
| `{{own.text.d.7.bits}}` | {{own.text.d.7.sym}} | {{own.text.d.7.what}} | {{own.text.d.7.space}} |
| ... | | | |
| `{{own.text.d.18.bits}}` | {{own.text.d.18.sym}} | {{own.text.d.18.what}} | {{own.text.d.18.space}} |
| `{{own.text.d.19.bits}}` | {{own.text.d.19.sym}} | {{own.text.d.19.what}} | {{own.text.d.19.space}} |
@assert own.text.rows == 20 and own.text.last_row == 19 and own.text.d[19].space == 1024 and own.text.d[18].space < 1024

@anim owndesc wide The same {{own.text.dbytes}} bytes, one symbol of `OWN` per step: the byte values get their lengths or a run skips them, and the lengths fill the code space until the code is complete; then the canonical codes. From the real compressor at level 3.

The sum reaches 1 with `u`, after {{own.text.desc_bits}} bits, and the last byte has
{{own.text.desc_pad}} zero bits after them. {{own.text.with_code}} byte values have a code: the space,
`l` and `u` 3 bits, the other 10 letters 4 bits, $3 \cdot \frac{1}{8} + 10 \cdot \frac{1}{16} = 1$. As
canonical codes, by length and then by byte value, the space is `{{own.text.code.20}}`, `l`
`{{own.text.code.6c}}`, `u` `{{own.text.code.75}}`, then `a` `{{own.text.code.61}}`, `d`
`{{own.text.code.64}}` and so on up to `t` `{{own.text.code.74}}`. The streams start at
${{own.text.desc_at}} + {{own.text.dbytes}} = {{own.text.streams_at}}$ and take {{own.text.sum}} bytes,
{{own.enc.bits_per_lit}} bits per literal where table {{ch04.text.set}} took {{own.enc.set_bits_per_lit}}, and
the bitstream is the same {{own.text.b_bytes}} bytes as at level 2. The two runs before the space
and `a` take {{own.len.15}} + 5 and {{own.len.16}} + 6 bits: on a page of text most of the table's
bits are the lengths.
@assert own.text.clen["20"] == own.text.clen["6c"] == own.text.clen["75"] == 3 and own.text.with_code == 13 and own.text.b_bytes == ch04.text.b_bytes

A page is invalid if the sum goes above 1, if a byte value above 255 would get a code, if a bit after
the last symbol in its byte is not 0, or if $3 + w + e + d$ is beyond `len`. `seqlz_decode()` reads zeros
past the end of the page, and a code of zeros, `{{own.code.8}}`, is a length, so a table that never
fills the code space still ends there, and the last of those checks rejects it. How the encoder chooses the lengths and which pages get a
table is in [chapter 11](#ch-literals-enc--own); how the decoder builds a decode table from them
without any memory of its own, in [chapter 13](#ch-literals-dec--own).

## Literals as deltas {#ch-format--deltas}

With `e = 1` each literal was stored as its **delta** to an earlier byte of the page before it was
coded: the literal minus that byte, modulo 256. The decoder adds the same byte back as it appends the
literal, modulo 256 again, which gives the literal back. The byte `dt`, byte 3, right after `t` and
before the stream sizes, says which byte. For the `i`-th literal of a sequence, counted from 0, which goes to byte `p = |out|`
of the page:

| `dt` | the literal is `(lits[used + i] + out[p - x]) mod 256` with | literals as they are |
| --- | --- | --- |
| 1 | `x = 4` | `i < 4`, the first 4 of each sequence |
| 2 | `x = 8` | `i < 8` |
| 3 | `x = last`, the offset of the sequence before, 1 for the first; with `last` 1 or 2, `x = 4`, with `last` 3, `x = 6` | `p < x`, near the page's start |

`out[p - x]` is a byte the decoder has already written, a byte of a match or an earlier literal of
the same sequence, as it is in the page; the last sequence is no different. Any other `dt` makes the
page invalid. Memory pages are full of 4- and 8-byte fields, whose bytes are often those of the field
before or differ from them by a little, as counters, sizes and addresses of objects in a row do, and
the byte after a match often differs only a little from the byte the match would have copied next. As
deltas, such literals become a few small values, which a literal table codes in fewer bits
([`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md#literals-as-deltas)).
A delta, not a XOR: a field that grows by 1 from `0x0f` to `0x10` has a delta of 1, but `0x0f` XOR
`0x10` is `0x1f`, every bit the carry flipped. The first form of `e`, on 21st October 2026, XORed the
literals; the subtraction replaced it the same day, before it was merged ([chapter 17](#ch-decisions--deltas)).

The page of 512 pointers of [chapter 9](#ch-matcher--back) at level 3 begins with
`{{delta.ptr.l3.head}}`: `h` says {{delta.ptr.l3.n}} coded literals, `t = {{delta.ptr.l3.t}}` is code
{{delta.ptr.l3.code}} with `w = {{delta.ptr.l3.w}}` and `e = 1`, byte {{delta.ptr.l3.dt_at}} is
`dt = {{delta.ptr.l3.dt}}`, and 5 bytes of sizes follow. Its second sequence is 1 literal at byte
{{delta.ptr.rec.1.pos}} after a match at offset 8, so `x = 8`, and the decoded delta `0x10` is added
to `out[8] = 0x20`, the low byte of the pointer before, which gives `0x30`.
@assert delta.ptr.l3.w == 5 and delta.ptr.l3.dt == 3 and delta.ptr.rec[1].last == 8 and delta.ptr.rec[1].pos == 16 and delta.ptr.fast.before[:2] == "10" and delta.ptr.fast.ref[:2] == "20" and delta.ptr.fast.after[:2] == "30"

## One sequence: the token {#ch-format--token}

Each sequence starts with a **token**, one symbol of `TOK` that holds three numbers: the literal
length `ll`, the **match length field** `mlf`, which is `ml - 4`, and the offset class `c`:

```text
token = min(ll, 15) + 16 * min(ml - 4, 31) + 512 * class

ll  = token & 15          the literal length, 0 to 15; 15 means 15 or more
mlf = (token >> 4) & 31   ml - 4, 0 to 31; 31 means 31 or more, ml 35 or more
c   = token >> 9          the offset class, 0 to 5
```

16 values of `ll` times 32 of `mlf` times 6 classes are 3072 tokens, numbered 0 to 3071:

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

A match is at least 4 bytes ([chapter 2](#ch-compression--lz77)), so no token is spent on lengths 0
to 3. With the caps, `ll` up to 14 and `ml` up to 34 fit into the token alone; a 15 or a 31 means a
length value follows. One code for all three numbers lets the code learn how they go together. The
shortest codes in the real 4 KiB table, with the escape among them:

| token | `ll` | `ml` | class | code | what it is |
| --- | --- | --- | --- | --- | --- |
| 49 | 1 | 7 | 0 | {{tok.len.49}} bits | 1 literal, 7 bytes, same offset: the sequence of an array of pointers |
| 2097 | 1 | 7 | 4 | {{tok.len.2097}} bits | 1 literal, 7 bytes from a new offset that is a multiple of 8 below 256 |
| 3072 | | | | {{tok.len.3072}} bits | the escape |
| 34 | 2 | 6 | 0 | {{tok.len.34}} bits | 2 literals, then 6 bytes from the same offset as the match before |
@assert F("ch04.syms.4") == [49, 2097, 3072] and tok.len[34] == tok.len[49] + 1 and tok.codes_of[1] + tok.codes_of[2] + tok.codes_of[3] == 0

Token 49 is the sequence of an array of 8-byte pointers that differ from the one 8 bytes back only in
their lowest byte: 1 literal and 7 bytes from 8 back, the same offset as before, in {{tok.len.49}}
bits. Where the lowest byte carries into the next one, the sequence is 2 literals and 6 bytes, token
34, at {{tok.len.34}} bits. Of the {{ch04.ptr.tokens}} tokens of [chapter 9](#ch-matcher)'s page of
512 pointers, {{ch04.ptr.t49}} are token 49 and {{ch04.ptr.t34}} token 34.
@assert ch04.ptr.t49 + ch04.ptr.t34 + 1 == ch04.ptr.tokens

**The escape.** Codes of at most 11 bits have room for 2048 symbols, fewer than the 3072 tokens, so
only the frequent tokens have a code, {{ch04.tok_coded}} of them on 4 KiB pages. Any other token is
sent as the escape, symbol 3072, followed by the token's number in 12 plain bits: {{ch04.esc_bits}}
bits in all. Any token may be escaped, also one with a code, and it means the same;
`seqlz_compress()` escapes only tokens without a code. In the spec's words:

```text
t = symbol(TOK)
if t == 3072:
    t = read(12)
    if t >= 3072: the page is invalid
```

## Offset classes {#ch-format--offsets}

Right after the token come the **offset bits**, as many as the token's class says:

| class | offset bits, 4 KiB | offset bits, 16 KiB | offset | offsets it can send |
| --- | --- | --- | --- | --- |
| 0 | {{ch04.raw.0}} | {{k16.ch04.raw.0}} | the offset of the match before | the same again |
| 1 | {{ch04.raw.1}} | {{k16.ch04.raw.1}} | `read(1) * 2` | 2 |
| 2 | {{ch04.raw.2}} | {{k16.ch04.raw.2}} | `read(8)` | 1 to 255 |
| 3 | {{ch04.raw.3}} | {{k16.ch04.raw.3}} | `read(P)` | 1 to `PAGE - 1` |
| 4 | {{ch04.raw.4}} | {{k16.ch04.raw.4}} | `read(5) * 8` | 8 to 248, multiples of 8 |
| 5 | {{ch04.raw.5}} | {{k16.ch04.raw.5}} | `read(8) * 8` | 8 to 2040, multiples of 8 |
@assert ch04.raw[1] == k16.ch04.raw[1] == 1 and ch04.raw[5] == k16.ch04.raw[5] == 8

Class 0 is the **repeat offset**, the offset of the sequence before, in 0 bits; at the start of each
page it is 1, so class 0 also works for a first match that is a run of one byte. Class 1 is the
single offset 2, an array of 16-bit values: of the offsets that are not the repeat offset, about 8%
are 2 on phone and desktop pages, more than any other single offset
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-2-in-1-bit-multiples-of-8-below-2048-in-8-03-to-08-smaller-at-the-same-speed-kept)).
Classes 4 and 5 store the offset divided by 8, for the pointers and 8-byte fields of memory: class 4
in 3 bits less than class 2, class 5 in 4 bits less than class 3 on 4 KiB pages and 6 less on 16 KiB
pages. A multiple of 8 from 2048 on goes to class 3. An offset must be 1 to the number of bytes
written so far; class 1 with the bit `0` gives offset 0, and that page is invalid.

The format does not care which class a compressor uses, as long as the offset comes out: class 3 can
send offset 5, class 2 can send 2 and 8. `seqlz_compress()` uses class 0 for the offset before, else
the class with the fewest offset bits that holds it, chosen in `seqlz_off_class()` without a branch
([chapter 6](#ch-header--offsets)):

| offset | offset before | class | offset bits |
| --- | --- | --- | --- |
| 24 | 24 | {{ch04.oc.24_24.cls}} | none |
| 24 | 16 | {{ch04.oc.24_16.cls}} | {{ch04.oc.24_16.raw}} bits, `read({{ch04.oc.24_16.raw}}) = {{ch04.oc.24_16.read}}` |
| 2 | 1 | {{ch04.oc.2_1.cls}} | {{ch04.oc.2_1.raw}} bit, `read({{ch04.oc.2_1.raw}}) = {{ch04.oc.2_1.read}}` |
| 8 | 1 | {{ch04.oc.8_1.cls}} | {{ch04.oc.8_1.raw}} bits, `read({{ch04.oc.8_1.raw}}) = {{ch04.oc.8_1.read}}` |
| 100 | 1 | {{ch04.oc.100_1.cls}} | {{ch04.oc.100_1.raw}} bits |
| 256 | 1 | {{ch04.oc.256_1.cls}} | {{ch04.oc.256_1.raw}} bits, `read({{ch04.oc.256_1.raw}}) = {{ch04.oc.256_1.read}}` |
| 300 | 1 | {{ch04.oc.300_1.cls}} | {{ch04.oc.300_1.raw}} bits |
| 2048 | 1 | {{ch04.oc.2048_1.cls}} | {{ch04.oc.2048_1.raw}} bits: a multiple of 8, but too far for class 5 |
@assert ch04.oc["24_24"].raw == 0 and ch04.oc["2_1"].raw == 1 and ch04.oc["2048_1"].cls == 3

::: why Why offsets are plain bits, and why the class is in the token
Offsets were once a Huffman-coded symbol for their number of bits, followed by the bits. Moving the
class into the token, with plain bits after it, cost 0.4 points of memory and took the decoder from
8758 to 8168 cycles per page: one table lookup on the chain from one token to the next, not two
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
The classes for multiples of 8 came later and saved 13 bytes per page in the kernel, for about 0.2 µs
more per page written
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)).
On 14th October 2026 class 1 became offset 2 in 1 bit, instead of the offsets below 16 in 4 bits, and
class 5 the multiples of 8 below 2048 in 8 bits, instead of all of them from 256 on in 9. With the
token tables trained again, `seqlz-2` stores 0.6% less on the 4 KiB dumps, on average, and
1.4% less on the 16 KiB ones, at the same speed on the Mi 9T and in the kernel VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-2-in-1-bit-multiples-of-8-below-2048-in-8-03-to-08-smaller-at-the-same-speed-kept)).
:::

## Length values {#ch-format--length-values}

A length that does not fit into the token follows the offset bits as a **length value**: if `ll` is
15, the real `ll` is `15 + value(LL)`, and if `mlf` is 31, the real `ml` is `35 + value(ML)`, the `ll`
value first. Both use the same scheme, each with its own table:

```text
s = symbol(table)
if s < 16:  value = s
else:       b = s - 12;  value = 2^b + read(b)
```

A value of 16 or more is sent as the position of its highest bit, $b$, as the symbol $12 + b$, and the
$b$ bits below it as plain bits. That is {{ll.symbols}} symbols on 4 KiB pages, $12 + P$, and
{{k16.ll.symbols}} on 16 KiB pages. E.g. the value 100 is in the range 64 to 127, so $b = 6$, the
symbol is 18, and `read(6)` is $100 - 64 = 36$; symbol 18 has the code `{{ll.code.18}}` in the real
`LL` table, so `ll = 115` costs the token, {{ll.len.18}} bits of code and 6 plain bits.
@assert ll.symbols == ml.symbols == 24 and k16.ll.symbols == 26

A code of a length value has at most {{ch06.MAX_BITS}} bits, a decode table of 256 entries. A match
length value follows 8.5% of the matches, which is why `ml` has 5 bits in the token and not 4: with 4,
a value followed 18.7% of them
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).

::: why Why two trained tables and not 5 plain bits
The symbol of a length value could be 5 plain bits, which would take two tables out of the format.
Measured twice, the 5 bits made pages 3 bytes larger
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-instead-of-their-tables-3-bytes-per-page-more-not-kept)),
and on the decoder of 10th October 2026, timed, 0.2% or 2.2 bytes larger, with no read faster
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#length-values-in-5-plain-bits-again-02-to-03-more-memory-no-read-faster-not-kept)).
:::

## How a page ends {#ch-format--end}

A page says neither how many sequences it has nor how long its bitstream is:

- **The last sequence has no token.** Every match is at least 4 bytes, so once the literals left,
  `n - used`, are as many as the bytes the page still needs, `PAGE - |out|`, no match fits any more:
  the literals left are the last sequence, and decoding copies them and stops. A page whose last
  match reaches the end of the page ends with 0 literals left, and a page of literals only,
  `n = PAGE`, has an empty `B`. Until 11th October 2026 the last sequence had a token, which told the
  decoder only what it knew already, in 9.0 bits per page, about 1.2 bytes, on the zram pages of a
  desktop and a phone
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-last-sequence-without-a-token-12-bytes-per-page-less-kept)).
- **The bitstream ends where its bits end.** After the last bit the decoder needs, fewer than 8 bits
  of `B` are left, all in its last byte, all 0. The same holds for each literal stream. A page with a
  byte too many, or a bit 1 in the padding, is invalid. zram stores no checksum, so a page damaged in
  memory has to be caught by the decoder, and the strict end catches most damage with one compare per
  page, at no cost in bytes. It costs a warm read 0.07 to 0.10 µs on the phone's little core and
  nothing in the kernel VM on the PC
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-page-ends-where-its-bits-end-no-damaged-page-with-bytes-or-bits-too-many-decodes-01-µs-on-the-a55-kept)).

Every sequence but the last writes at least 4 bytes, so a decoder stops after at most `PAGE / 4 + 1`
sequences, {{const.MAX_SEQUENCES}} on a 4 KiB page, also on a page that is not valid. Two facts follow
from the rules: a valid page has $n \ge 1$, because the first match needs a byte before it, and the
first match's offset is at most that sequence's `ll`.

## The rules of a valid page {#ch-format--validity}

A page is valid for a page size and its tables if all of these hold:

1. `len >= 2`.
2. `n <= PAGE`.
3. Raw literals: `2 + n <= len`.
4. Coded literals: `len >= 3 + w + e`.
5. Coded literals: `code = t & 15` is at most 8, and with `e = 1`, `dt` is 1, 2 or 3.
6. Code 8: the page's own table is valid ([above](#ch-format--own)), and `3 + w + e + d <= len`.
7. Coded literals: `3 + w + e + d + s[0] + ... + s[7] <= len`, with `d = 0` for the codes 0 to 7.
8. Every literal stream's codes take at most `8 * s[j]` bits and fewer than 8 bits less, and its bits
   after the last code are all 0. So a stream without literals has size 0.
9. Every escaped token is below 3072.
10. Every sequence's `ll` is at most the literals left, `n - used`, and at most the room left in the
    page, `PAGE - |out|`.
11. Every match's offset is 1 to `|out|`, the bytes written so far.
12. Every match fits into the room left in the page, `ml <= PAGE - |out|`.
13. At the end, 0 to 7 bits of `B` are left, and all of them are 0. A decoder may read zeros past the
    end, but a page that needs more bits than it has then has fewer than 0 left, and fails this.

A header with more literals than the sequences use breaks rule 10 or 12: the end is never reached,
and the next token's `ll` or match does not fit. `seqlz_decode()` checks every rule, in its own order
and often in a cheaper form ([chapter 14](#ch-decode)), and the fuzz target `fuzz/seqlz_diff_fuzz.c`
fails if it and `tools/seqlz-ref/seqlz_ref.c` disagree on whether any input is valid or on what it
decodes to.

## Decoding, as the spec writes it {#ch-format--decode}

`lits` are the `n` literals, decoded first if they are coded, `used` is how many of them are copied,
`out` is the page being written, `last` is 1. `copy(k)` appends the next `k` literals to `out` and adds
`k` to `used`: as they are, or with `e = 1` each plus the byte of `out` that
[literals as deltas](#ch-format--deltas) names, modulo 256.

```text
loop:
    if n - used == PAGE - |out|:                the last sequence
        copy(n - used)
        stop
    token: ll, mlf, c
    x = read(offset bits of class c)
    off = (c == 0) ? last : x as the offset of class c
    if ll == 15: ll = 15 + value(LL)
    if ll > n - used or ll > PAGE - |out|: invalid
    copy(ll)
    ml = mlf + 4
    if mlf == 31: ml = 35 + value(ML)
    last = off
    if off == 0 or off > |out| or ml > PAGE - |out|: invalid
    ml times: append the byte off places before the end of out

at the stop: fewer than 8 bits of B left, all 0, else invalid
```

The match is copied byte by byte, so it can repeat what it writes itself
([chapter 2](#ch-compression--overlap)). `seqlz_decode()` does the same work in another shape: it
decodes all coded literals into a buffer first, so that the loop over the sequences reads them as if
they were raw, it looks up 11 bits at a time in a table, and it copies 16 bytes at a time where there
is room ([copying more than needed](#ch-cpu--wild-copy)). The result is the same for every input,
valid or not; that is what the fuzzers check.

## A page of `ab`, bit by bit {#ch-format--example}

The {{ab.len}} bytes `{{ab.hex}}` are a 4 KiB page of `ab` 2048 times. `seqlz-1` and
`seqlz-2` both write exactly these bytes: 2 literals are far from saving the {{ch01.LIT_CODED_MIN}} bytes
that coding them needs ([chapter 11](#ch-literals-enc)).

- `{{ch04.ab.hdr}}`: `h = {{ab.lits}}`, raw literals.
- `{{ch04.ab.lithex}}`: the {{ab.lits}} literals, `a` and `b`.
- `{{ab.b.hex}}`: `B`, {{ab.b.bits}} bits.

@anim abbits The {{ab.len}} bytes read as the spec reads them, one field per step, with the codes of the real 4 KiB tables; the page fills up below.

| bits | read as | means |
| --- | --- | --- |
| `{{ch04.ab.s.0.esccode}}` | `symbol(TOK)` = 3072 | the escape: the token has no code of its own |
| `{{ch04.ab.s.0.tokbits}}` | `read(12)` = {{ch04.ab.s.0.tok}} | ${{ch04.ab.s.0.tok}} = {{ch04.ab.s.0.llf}} + 16 \cdot {{ch04.ab.s.0.mlf}} + 512 \cdot {{ch04.ab.s.0.cls}}$: `ll = {{ch04.ab.s.0.llf}}`, `mlf = {{ch04.ab.s.0.mlf}}`, class {{ch04.ab.s.0.cls}} |
| `{{ch04.ab.s.0.offbits}}` | `read({{ch04.ab.s.0.raw}})` = {{ch04.ab.s.0.offread}} | class {{ch04.ab.s.0.cls}}: `off = {{ch04.ab.s.0.off}}` |
| | | `ll` is not 15, no `ll` value: append `a b`, the page has 2 bytes |
| `{{ch04.ab.s.0.mlcode}}` | `symbol(ML)` = {{ch04.ab.s.0.mlsym}} | `mlf` is 31, so a length value: $b = {{ch04.ab.s.0.mlsym}} - 12 = {{ch04.ab.s.0.mlb}}$ |
| `{{ch04.ab.s.0.mlbits}}` | `read({{ch04.ab.s.0.mlb}})` = {{ch04.ab.s.0.mlread}} | value $2^{{{ch04.ab.s.0.mlb}}} + {{ch04.ab.s.0.mlread}} = {{ch04.ab.s.0.mlval}}$, `ml = 35 + {{ch04.ab.s.0.mlval}} = {{ch04.ab.s.0.ml}}` |
| | | copy {{ch04.ab.s.0.ml}} bytes from {{ch04.ab.s.0.off}} back: `abab...`, the page has 4096 bytes |
| | | 0 literals left for 0 bytes of the page: the last sequence, no token |
@assert ab.seqs == 2 and ab.escapes == 1 and ch04.ab.s[0].escaped == 1 and ch04.ab.s[0].ll == 2 and ch04.ab.s[0].mlf == 31

After the last code, {{ab.b.pad}} bits of `B` are left, fewer than 8 and all zero: the page is valid.
After its first 2 bytes the match copies bytes it wrote itself. Real pages almost never have a token
like this one, 2 literals and a match of the rest of the page, so it has no code of its own and costs
the escape's {{tok.len.3072}} bits and 12 more.

::: try Decode it with the spec's decoder
Write the {{ab.len}} bytes to a file and give it to the reference decoder. It prints whether the page
is valid and the SHA-256 of the decoded page, which is the SHA-256 of `ab` 2048 times:

```text
printf '{{ch04.ab.printf}}' > /tmp/ab.seqlz
python3 tools/seqlz-ref/seqlz_ref.py /tmp/ab.seqlz
/tmp/ab.seqlz valid 2c888a3809c425e853a6ac08e1c1ffd9393fe3e49419e8f71ac23ab0362617a3
python3 -c "import hashlib; print(hashlib.sha256(b'ab' * 2048).hexdigest())"
2c888a3809c425e853a6ac08e1c1ffd9393fe3e49419e8f71ac23ab0362617a3
```

Flip any bit of the {{ab.len}} bytes and run it again: most flips make the page invalid. The same
bytes go through the real `seqlz_decode()` one statement at a time in
[chapter 14](#ch-decode--example).
:::

## A second page: {{second.seqs}} sequences and {{second.b.pad}} bits of padding {#ch-format--example2}

This page has three matches with three kinds of offset, and a bitstream that does not end on a byte
boundary:

```text
bytes 0 to 11:   abcdefghijkl
bytes 12 to 15:  llll
bytes 16 to 17:  xy
bytes 18 to 24:  cdefghi       the same 7 bytes as at 2 to 8, 16 bytes back
bytes 25 to 4095: z, 4071 times
```

`seqlz_compress()` writes it in {{second.len}} bytes, with and without coded literals the same:
@assert second.coded.len == second.len

```text
{{ch04.second.hdr}}  {{ch04.second.lithex}}  {{second.b.hex}}
```

`h = {{second.lits}}`, raw literals, and the {{second.lits}} literals are `{{ch04.second.litstr}}`.
`B` is {{second.b.bytes}} bytes, {{ch04.second.bbits}} bits:

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
@assert second.seqs == 4 and second.escapes == 0 and ch04.second.s[0].cls == 0 and ch04.second.s[1].cls == 4 and ch04.second.s[2].cls == 2 and ch04.second.s[0].mlf < 31 and ch04.second.s[1].mlf < 31 and ch04.second.s[2].mlf == 31 and ch04.second.s[2].llf < 15

- **The first match uses the repeat offset.** `last` starts at 1, and `llll` is 4 bytes from 1 back,
  in 0 offset bits; the matcher finds it with that same candidate ([chapter 9](#ch-matcher)).
- **Token 12 has an {{tok.len.12}}-bit code.** 12 literals and a match of exactly 4 bytes at the
  repeat offset is rare on real pages, so its code is long; escaped it would have taken
  {{ch04.esc_bits}} bits.
@assert tok.len[12] == ch06.TOKEN_BITS
- **Token 2098 is "2 literals, a match of 7 at offset 16".** 16 is a multiple of 8 below 256, so the
  class is 4 and the offset is sent as 2 in 5 bits: ${{tok.len.2098}} + 5 = {{ch04.second.s.1.bits}}$
  bits plus the 2 literals, where `lz4`'s format takes 24 bits plus the literals.
- **The third match** has offset 1, which is not the repeat offset any more, `last` is 16 now, so it
  goes as class 2 with {{ch04.second.s.2.raw}} bits.
- **The padding.** {{second.b.bits}} bits are used and {{second.b.pad}} are left in the last byte, all
  0. With an eighth bit left, or one of them 1, the page would be invalid.

`python3 tools/seqlz-ref/seqlz_ref.py` decodes the {{second.len}} bytes to the page above.

## No version in the page {#ch-format--version}

The page has no version number and no fixed bytes that mark it as seqlz. zram keeps its pages only
while the kernel that wrote them runs, also the ones it wrote back to a backing device, so no page
ever meets a newer decoder, and a change of the format needs a new kernel, not a new algorithm name in zram. Two things
would change that: another user of the codec that keeps pages longer, or zram's pages kept across a
`kexec` by live update, which zram does not do in 2026. For that case the codes 9 to 15 and the values
of `dt` other than 1 to 3 are invalid today, so a later format can tell itself apart. The top bit of
byte 2 was such a free bit too, until the deltas took it on 21st October 2026. And the format is not frozen yet: until seqlz
is in a kernel, the tables and the rules may change, together with `docs/format.md`'s hashes, its
example and both reference decoders.

## Where the tables come from {#ch-format--tables}

The {{ch17.tables}} tables are counts turned into code lengths. `bench/seqlz_counts_4k.txt` has, for 4
KiB pages, how often each token, each `ll` symbol and each `ml` symbol occurred on the training pages,
and for each of the 8 literal tables, how often each byte value occurred as a literal on the pages that
table codes best; k-means put pages whose literals look alike into one group for each table. Its line
`own` has how often each symbol of `OWN` occurred in the tables of the {{own.counts.pages|grp}} training
pages that level 3 gave a table of their own; the lengths 7 to 10 are {{own.counts.share_7_10}}% of them.
`tools/kernel-port/gen-seqlz-tables.py` turns the counts into length-limited Huffman code lengths and
writes `src/seqlz_default_tables_4k.inc` and `src/seqlz_lit_sets_4k.inc`; CI checks that the files in
`src/` are its output, byte for byte, and `quetschn-seqlz-train --from-counts` does the same in C++.
The kernel's copy gets the generator, as `scripts/gen-seqlz-tables.py`, and the 4 KiB counts.

The 4 KiB tables of 13th October 2026 are counted on {{ch02.train_pages|grp}} pages: three sessions
of a Xiaomi Mi 9T under memory pressure, the resident pages of a desktop and 200 000 pages of its zram
dump of 28th September 2026. The 16 KiB tables are counted on 26 933 pages of a zram dump of the
Android 17 emulator with 16 KiB pages. The pages are not published, because memory pages hold keys
and passwords. The counts cannot give a page back: they are sums over many pages, each literal
table's over 10 590 pages or more for 4 KiB, and say nothing about which symbol came after which
([`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md#where-the-tables-come-from)).

A decoder has to have exactly the right tables. `docs/format.md` lists the SHA-256 of each table's
code lengths, as bytes in symbol order, and `seqlz_ref.py` refuses to run when they do not match. For
4 KiB pages:

| table | SHA-256 of the code lengths |
| --- | --- |
| `TOK_4k` | `{{ch04.sha.TOK}}` |
| `LL_4k` | `{{ch04.sha.LL}}` |
| `ML_4k` | `{{ch04.sha.ML}}` |
| `LIT_4k` | `{{ch04.sha.LIT}}` |
| `OWN_4k` | `{{ch04.sha.OWN}}` |

Fixed tables cost bytes, since a table fitted to each page codes it tighter. For the tokens and the
lengths no way of fitting them that was measured paid for its time, and tables trained on a phone's own
pages saved 0.6% on pages of that phone they were not trained on
([The format written down](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-format-written-down-one-set-of-tables-and-stream-sizes-that-hold)).
For the literals the first try was no better: a literal table of each page's own made writes 14% slower
for 32 bytes less per page, 2.4% of the stored size
([Per page: its own literal table](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept)).
The one of level 3, from 15th October 2026, is built only where the page's literals promise to save
enough, and on the Mi 9T's Cortex-A76 it got zram's pages to the size of `zstd` 3's in less than half
its time to write a page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-a-lazy-matcher-and-a-literal-table-of-the-pages-own-zstd-3s-memory-in-less-than-half-its-time-on-the-phone-kept)),
[chapter 17](#ch-decisions--level3) has the numbers.
How the code lengths become the encoder's codes and the decoder's tables at run time is
[chapter 8](#ch-codes).
