# Compression from zero: matches, literals and codes {#ch-compression}

Almost every fast compressor, `lz4`, `zstd` and seqlz included, is built from two ideas. The first
is to say "that again" instead of repeating bytes: a page becomes a list of **literals**, bytes stored
as they are, and **matches**, "copy so many bytes from so far back". The second is to give frequent
things short codes and rare things long ones, a **Huffman code** instead of a fixed number of bits.
`lz4` uses only the first idea, `zstd` both, with code tables it builds from the data and stores in
it. seqlz uses both with tables that are fixed, trained once on many pages and part of the format.
This chapter builds both ideas up with examples small enough to check by hand.

## Literals, matches and sequences {#ch-compression--lz77}

The first idea is Lempel and Ziv's of 1977, and compressors of this kind are called **LZ77** or just
LZ. Where the next bytes appeared before, the compressor writes a **match**: copy `ml` bytes, the
match length, from `offset` bytes back in what is already written. Where they did not, it keeps the
byte as a **literal**. The output is a list of **sequences**, each `ll` literals and then one match;
the last sequence has literals only. The decoder of one sequence:

```c
memcpy(out + pos, lit, ll);
pos += ll;
lit += ll;
for (i = 0; i < ml; i++)
	out[pos + i] = out[pos + i - off];
pos += ml;
```

A made-up page that starts with two records of the same layout, 24 bytes of text:

```text
position  0         1         2
          012345678901234567890123
          cat=1,dog=2;cat=3,dog=4;
```

A compressor that remembers where every 4 bytes last appeared finds 12 literals, `cat=1,dog=2;`; at
position 12 a match of 4 bytes, `cat=`, from 12 back; one literal, `3`; at position 17 a match of 5
bytes, `,dog=`, again from 12 back; and the last sequence, 2 literals, `4;`.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 178" role="img" aria-label="The 24 bytes cat=1,dog=2;cat=3,dog=4; as cells: 12 literals, a match of 4 bytes from 12 back, 1 literal, a match of 5 bytes from 12 back, 2 literals.">
  <g fill="none" stroke="currentColor" stroke-width="1">
    <rect x="24" y="44" width="336" height="32" class="fill-sheet"/>
    <rect x="472" y="44" width="28" height="32" class="fill-sheet"/>
    <rect x="640" y="44" width="56" height="32" class="fill-sheet"/>
  </g>
  <g class="acc" fill="none" stroke="currentColor" stroke-width="1">
    <rect x="360" y="44" width="112" height="32" class="fill-acc" fill-opacity="0.25"/>
    <rect x="500" y="44" width="140" height="32" class="fill-acc" fill-opacity="0.25"/>
  </g>
  <g fill="none" stroke="currentColor" stroke-width="0.6">
    <path d="M52,44 V76 M80,44 V76 M108,44 V76 M136,44 V76 M164,44 V76 M192,44 V76 M220,44 V76 M248,44 V76 M276,44 V76 M304,44 V76 M332,44 V76 M388,44 V76 M416,44 V76 M444,44 V76 M528,44 V76 M556,44 V76 M584,44 V76 M612,44 V76 M668,44 V76"/>
  </g>
  <g font-size="13" text-anchor="middle">
    <text x="38" y="65">c</text><text x="66" y="65">a</text><text x="94" y="65">t</text><text x="122" y="65">=</text>
    <text x="150" y="65">1</text><text x="178" y="65">,</text><text x="206" y="65">d</text><text x="234" y="65">o</text>
    <text x="262" y="65">g</text><text x="290" y="65">=</text><text x="318" y="65">2</text><text x="346" y="65">;</text>
    <text x="374" y="65">c</text><text x="402" y="65">a</text><text x="430" y="65">t</text><text x="458" y="65">=</text>
    <text x="486" y="65">3</text><text x="514" y="65">,</text><text x="542" y="65">d</text><text x="570" y="65">o</text>
    <text x="598" y="65">g</text><text x="626" y="65">=</text><text x="654" y="65">4</text><text x="682" y="65">;</text>
  </g>
  <g class="acc" fill="none" stroke="currentColor" stroke-width="1.4">
    <path d="M416,42 C416,10 80,10 80,42"/><path d="M76,36 L80,42 L84,36"/>
    <path d="M570,78 C570,104 234,104 234,78"/><path d="M230,84 L234,78 L238,84"/>
  </g>
  <g class="acc" font-size="11" text-anchor="middle">
    <text x="248" y="15" class="halo">match: 4 bytes from 12 back</text>
    <text x="402" y="116">match: 5 bytes from 12 back</text>
  </g>
  <g fill="none" stroke="currentColor" stroke-width="1">
    <path d="M26,138 V144 H470 V138"/><path d="M474,138 V144 H638 V138"/><path d="M642,138 V144 H694 V138"/>
  </g>
  <g font-size="11" text-anchor="middle">
    <text x="248" y="162">sequence 1: ll 12, ml 4, offset 12</text>
    <text x="556" y="162">2: ll 1, ml 5, off 12</text>
    <text x="668" y="162">3: ll 2</text>
  </g>
</svg>
</div>
<figcaption>Three sequences for 24 bytes: literals are plain cells, matches are shaded, and the arrows point to where each match copies from.</figcaption>
</figure>

Two details show already. Both matches have the same offset, 12, the length of a record: memory is
full of arrays of records and of 8-byte values, so the offset of the match before repeats often. seqlz
has a **repeat offset** for that, an offset that costs no bits because it is the one of the match
before. And the matches are short: most matches in memory pages are a few bytes to a few dozen.

### Why a match is at least 4 bytes

A match costs bits for its length and its offset. In `lz4`'s format that is at least 3 bytes, so a
match of 3 bytes would save nothing; `lz4` matches are 4 bytes or longer, and the format stores
`ml - 4`. seqlz does the same. In its format a match of 4 bytes at a new offset of up to 12 bits costs
about as many bits as its 4 bytes as coded literals, so shorter ones are not worth looking for
([chapter 4](#ch-format--token)), while a 5-byte match at a 12-bit offset, about 21 bits, is cheaper
than its 5 literals, about 35 bits
([explored-designs.md, "The matcher without its step"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept)).
4 bytes are also one 32-bit load and one compare, which is how the matcher checks a candidate.

## A match may copy what it is writing {#ch-compression--overlap}

The offset of a match can be smaller than its length. Then the match reads bytes that it wrote
itself a moment earlier: a page of `ab` 2048 times is 2 literals, `a` and `b`, and one match of 4094
bytes at offset 2.
@assert ab.lits == 2 and ab.tokens == 1
With offset 1 a match repeats the last byte, which is how runs of one byte compress in every LZ
format. This is why the decoder above copies one byte at a time, front to back: `memmove()` would copy
the source as it was before the copy, bytes that were not written yet.

@anim overlap A match of 10 bytes at offset 1, 2 or 3, one byte per step. The byte loop copies what it wrote itself a moment before; memmove() copies the source as it was before the copy, and gets bytes that were not written yet.

A fast decoder copies 8 or 16 bytes at a time, and then has to take care with offsets below 8 or 16;
[chapter 12](#ch-dec-basics--copies) shows how seqlz does that.

## Finding matches: a hash table of positions {#ch-compression--hash}

Fast compressors find matches with a hash table of positions. At every position, the compressor
hashes the next few bytes, reads the slot: a **candidate** position; compares the bytes there with the
bytes here, and if the first 4 are equal counts how far the match goes; and writes the current
position into the slot. The table is a **cache, not a map**: one position per slot, so two strings
with the same hash overwrite each other and a match is missed now and then. A wrong match is never
made, because every candidate is compared before it is used, and each position costs one read and
one write of the table, whatever the page holds.

@anim hash wide The three steps on a short text, as a model, not seqlz's matcher: the table has 16 slots instead of {{ch02.HASH_SLOTS}}, the hash is seqlz's hash of 5 bytes cut to 4 bits, and there is no repeat offset. Every number is computed from the text.

seqlz's table has {{ch02.HASH_SLOTS}} slots of 2 bytes, 8 KiB, all of its work memory for
compression, and it is cleared for every page. Its hash covers the next **5 bytes**, not 4, which
finds fewer and longer matches; a match of exactly 4 bytes is still found at the repeat offset, which
needs no table. E.g. `,dog=` goes to slot {{ch02.hash.dog}} at position 5 and again at position 17,
which is how the match of sequence 2 is found. [Chapter 9](#ch-matcher--hash) has the hash and the
measurements behind 5 bytes.
@assert size.state == 2 * ch02.HASH_SLOTS == 8192 and ch02.hash.dog == ch02.hash.dog17

::: try The real matcher on the example
On a page that starts with `cat=1,dog=2;cat=3,dog=4;` and is zeros after, seqlz's matcher finds the
first two sequences exactly as above.
@assert (ch02.cat[0].ll, ch02.cat[0].ml, ch02.cat[0].off, ch02.cat[1].ll, ch02.cat[1].ml, ch02.cat[1].off) == (12, 4, 12, 1, 5, 12)
Its hash of 5 bytes cannot find `cat=` at position 12, because `cat=3` never appeared before. It
finds it anyway: the cleared table holds 0 in every slot, so an empty slot is a candidate at position
0, and position 0 starts with `cat=`. Put a `#` in front, and that match is gone: the matcher then
writes {{ch02.hash.sharp.ll}} literals and finds only `,dog=`.
@assert ch02.hash.sharp.ml == 5 and ch02.hash.sharp.n == 3
On the page without the `#`, the third sequence takes `4`, `;` and one zero as literals, and covers
the other {{ch02.cat.2.ml}} zeros with one match at offset 1. That match ends at the end of the page,
and the matcher still passes a last sequence with no literals and no match, for which the format
writes no bits.
@assert ch02.cat.n == 4 and ch02.cat[2].ll == 3 and ch02.cat[2].off == 1 and ch02.cat[3].ll == 0
[Chapter 9](#ch-matcher--back) has a program that prints every sequence the matcher finds; give it
this page, and others.
:::

The compressor here is **greedy**: it takes the first match it finds and moves on behind it. A
deeper search pays in bytes, `zstd` 3 looks for a long match first and checks one byte later, `lz4hc`
searches chains of older positions, and costs time on every write. A seqlz that searched a hash
chain, picked the match that saves the most bits by the tables and looked one position ahead stored
pages 3.3% smaller for about twice the time to write; it was not kept, nor a deeper one
([`docs/plan.md` §1.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#11-the-score-memory-against-time-not-bars)).
seqlz's level 3 of today looks one position ahead only for a match shorter than 20 bytes
([chapter 9](#ch-matcher--lazy)).

## Fixed-width numbers waste bits {#ch-compression--entropy}

Every sequence has to say its `ll`, its `ml` and its offset. `lz4` writes them in whole bytes, e.g.
2 bytes for every offset, though a 4 KiB page has offsets of at most 12 bits and small offsets are
common. How many bits a number needs depends on how often each value occurs. A small example with real
numbers: seqlz sorts every offset into one of 6 **offset classes**. These are their shares of the
sequences on the {{ch02.train_pages|grp}} pages seqlz's tables were trained on
([chapter 4](#ch-format--tables) says which pages), summed from the token counts in
[`bench/seqlz_counts_4k.txt`](https://github.com/martinus/quetschn/blob/main/bench/seqlz_counts_4k.txt):
@assert ch02.OFF_CLASSES == 6

| class | offset | share of the sequences |
| --- | --- | --- |
| 0 | the repeat offset | {{ch02.cls.0.share}}% |
| 1 | exactly 2 | {{ch02.cls.1.share}}% |
| 2 | below 256 | {{ch02.cls.2.share}}% |
| 3 | below 4096 | {{ch02.cls.3.share}}% |
| 4 | multiple of 8, 8 to 248 | {{ch02.cls.4.share}}% |
| 5 | multiple of 8, 256 to 2040 | {{ch02.cls.5.share}}% |

6 values in a fixed width take 3 bits. Information theory says what they need at least: a value of
probability $p$ carries $-\log_2 p$ bits, and the average of that over all values, the **entropy**, is
the least number of bits per value any code can reach on average, here {{ch02.cls.entropy}} bits.
Class 0, at {{ch02.cls.0.share}}%, carries {{ch02.cls.0.info}} bits; class 1, at
{{ch02.cls.1.share}}%, carries {{ch02.cls.1.info}}. A good code gives class 0 fewer than 3 bits and
class 1 more. (On a desktop's zram pages of September 2026, 18% to 25% of the matches use the repeat
offset, see [chapter 9](#ch-matcher--plan).)
@assert ch02.cls[0].info < "3" < ch02.cls[1].info

## Prefix codes {#ch-compression--prefix-codes}

With codes of different lengths, the decoder must know where one ends: in a **prefix code** no code is
the start of another one. With four symbols A, B, C and D at a half, a quarter, an eighth and an
eighth, and the codes `0`, `10`, `110` and `111`, the bits `01001110` can only be A B A D A. That is
$0.5 \times 1 + 0.25 \times 2 + 0.125 \times 3 \times 2 = 1.75$ bits per symbol on average, where 4
symbols in a fixed width take 2. A **symbol** is whatever gets a code: an offset class here, a token,
a byte value, a length.

## Huffman codes, built by hand {#ch-compression--huffman}

David Huffman's method of 1952 finds the prefix code with the fewest bits on average for given
frequencies. It builds a binary tree from the leaves up: each symbol is a node weighted by its
frequency; join the two lightest nodes under a new node with the sum of their weights; repeat until
one node is left. A symbol's code is the path from the root to its leaf, and its **code length** is
its depth. For the offset classes:

| step | joins | weights | new weight |
| --- | --- | --- | --- |
| 1 | {{ch02.join.1.what}} | ${{ch02.join.1.a}} + {{ch02.join.1.b}}$ | {{ch02.join.1.sum}} |
| 2 | {{ch02.join.2.what}} | ${{ch02.join.2.a}} + {{ch02.join.2.b}}$ | {{ch02.join.2.sum}} |
| 3 | {{ch02.join.3.what}} | ${{ch02.join.3.a}} + {{ch02.join.3.b}}$ | {{ch02.join.3.sum}} |
| 4 | {{ch02.join.4.what}} | ${{ch02.join.4.a}} + {{ch02.join.4.b}}$ | {{ch02.join.4.sum}} |
| 5 | {{ch02.join.5.what}} | ${{ch02.join.5.a}} + {{ch02.join.5.b}}$ | {{ch02.join.5.sum}} |

@anim huffman facts=ch02.cls The table above, one join per two steps, then the codes read off the tree. The shares are the ones of the offset classes in the training counts; the average is computed from them.

Classes 0 and 4 get 2 bits, the other four get 3.
@assert [ch02.cls[k].len for k in "012345"] == [2, 3, 3, 3, 2, 3]
On average that is
$2 \times ({{ch02.cls.0.share}}\% + {{ch02.cls.4.share}}\%) + 3 \times ({{ch02.cls.1.share}}\% + {{ch02.cls.2.share}}\% + {{ch02.cls.3.share}}\% + {{ch02.cls.5.share}}\%) = {{ch02.cls.avg}}$ bits, against 3 for the
fixed width and {{ch02.cls.entropy}} for the entropy. The gap to the entropy is what a Huffman code
cannot get: every code is a whole number of bits, so a symbol with probability 0.9, which carries 0.15
bits, still costs 1. Coders with fractional bits exist, arithmetic coding and ANS, which `zstd` uses
for its sequences as FSE, but their decoders do more work per symbol. On desktop and phone pages,
Huffman codes fitted to the pages are 2.3 bytes per page above the entropy, 0.3%, so seqlz stays with
Huffman codes
([explored-designs.md, "How tight the bits are"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).

seqlz does not give the offset class a code of its own: it puts the class into one symbol together
with `ll` and `ml`, the **token**, which has {{const.TOKEN_SYMBOLS}} values, and the same method
builds its code. A Huffman code for that many symbols, some very rare, can have codes of 20 bits and
more, and a decoder that decodes with a table, as below, needs $2^L$ entries for a longest code of
$L$ bits. So the trainer that builds seqlz's tables ([chapter 8](#ch-codes--lengths)) limits the code
lengths, {{ch02.TOKEN_BITS}} bits for tokens, {{ch02.LIT_BITS}} for literals, {{ch02.MAX_BITS}} for
[length values](#ch-format--length-values), at a small cost in bits, and gives the rarest tokens no
code at all: they share one code, the **escape**, followed by the token in {{ch02.ESCAPE_BITS}} plain
bits ([chapter 4](#ch-format--token)).

## Canonical codes: only the lengths are stored {#ch-compression--canonical-codes}

The tree gave each class some code, but swapping the 0 and the 1 at any node gives another code with
the same lengths and the same average. Only the lengths matter. So formats store the lengths and
derive the codes from them by a fixed rule, a **canonical code**. seqlz's tables are a list of code
lengths, one byte per symbol, and the rule is the one of deflate, RFC 1951 §3.2.2, written in
[`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md#prefix-codes):

1. `count[l]` is the number of symbols whose code has `l` bits; `count[0]` is taken as 0.
2. `first[1] = 0`, and for `l >= 2`: `first[l] = (first[l-1] + count[l-1]) * 2`.
3. The symbols with a code of `l` bits, in the order of their number, get the numbers `first[l]`,
   `first[l] + 1`, and so on, written in binary with exactly `l` digits.

Shorter codes come first, the codes of one length are consecutive numbers, and each length goes on
where the length before ended, shifted one bit to the left. For the offset classes, with the lengths
2, 3, 3, 3, 2, 3:

| step | value |
| --- | --- |
| `count[2]`, `count[3]` | 2 (classes 0 and 4), 4 (classes 1, 2, 3 and 5) |
| `first[2]` | `(0 + count[1]) * 2 = 0` |
| `first[3]` | `(first[2] + count[2]) * 2 = (0 + 2) * 2 = 4` |
| codes of 2 bits | class 0: 0 = `00`, class 4: 1 = `01` |
| codes of 3 bits | class 1: 4 = `100`, class 2: 5 = `101`, class 3: 6 = `110`, class 5: 7 = `111` |

The encoder and every decoder get the same codes from 6 bytes of lengths. In `src/` this is
`first_codes()` in `src/seqlz_codes.c` ([chapter 8](#ch-codes--first-codes)).

## Complete codes: every bit pattern starts a code {#ch-compression--complete}

A code of $l$ bits uses up $\frac{1}{2^l}$ of all bit strings. The sum of $2^{-l}$ over all codes, the
**Kraft sum**, is at most 1 for any prefix code; for the offset classes it is
$\frac{2}{4} + \frac{4}{8} = 1$. A code with a Kraft sum of exactly 1 is **complete**: every long
enough string of bits starts with exactly one code. With only A = `0` and B = `10`, the sum is
$\frac{3}{4}$, and the bits `11` start no code, which a decoder has to check for. Huffman codes are
always complete, because every node of the tree has two children. seqlz requires it of every table:
`first_codes()` adds `2^(bits - l)` for each code, the number of decode table entries the code fills,
and the sum must be exactly `2^bits` ([chapter 8](#ch-codes--first-codes)).

That buys a decoder without the check: any bits decode to some symbol, so there is no "invalid code"
branch from one sequence to the next. When the project made all its codes complete, the decoder ran
6% fewer instructions
([`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-choices-with-their-numbers)).
A damaged page then decodes to wrong symbols, not to an error, and the checks on lengths, offsets and
the end of the page catch it ([chapter 14](#ch-decode)).

## Table-driven decoding {#ch-compression--table-decoding}

Decoders do not read a code bit by bit. If the longest code has $L$ bits, the decoder peeks at the
next $L$ bits, uses them as an index into a table of $2^L$ entries, and the entry says which symbol it
is and how many bits its code has. Then it drops that many bits and peeks again: one lookup per code,
whatever its length.

For the offset classes, $L$ is 3. A code of $l$ bits is the first $l$ bits of the index, and the bits
after it can be anything, so it fills $2^{3-l}$ entries in a row: class 0, code `00`, fills `000` and
`001`; class 4, code `01`, fills `010` and `011`; each 3-bit code fills one. Decoding the classes 4,
1, 0, 5, whose codes make the bits `01 100 00 111`:

| bits left | peek 3 | entry | drop |
| --- | --- | --- | --- |
| `0110000111` | `011` | class 4, 2 bits | 2 |
| `10000111` | `100` | class 1, 3 bits | 3 |
| `00111` | `001` | class 0, 2 bits | 2 |
| `111` | `111` | class 5, 3 bits | 3 |

@anim table From the code lengths to a decode table and back to symbols: the [canonical codes](#ch-compression--canonical-codes), the 8 entries filled code by code, then the four lookups of the table above. Every number is computed from the lengths 2, 3, 3, 3, 2, 3.

The last peek may need bits past the end, e.g. when the last code is `00`; a decoder reads zeros
there, or the format guarantees that bytes follow. For a token of code `c` with `l` bits, the entries
from `c << (11 - l)` up to `(c + 1) << (11 - l)` are the 11-bit numbers whose top `l` bits are `c`
([chapter 8](#ch-codes--build-token)). seqlz's token table has `SEQLZ_TOKEN_BITS` =
{{ch02.TOKEN_BITS}}, {{ch02.tok_entries}} entries of {{ch02.tok_entry_bytes}} bytes,
{{ch02.tok_table_kib}} KiB. Token 49, "1 literal, a match of 7 bytes, the repeat offset", the sequence
of an array of 8-byte pointers that differ only in their lowest byte, has a code of {{tok.len.49}} bits
([chapter 4](#ch-format--token) has how a token's number comes from `ll`, `ml` and the class), so it
fills $2^{ {{ch02.TOKEN_BITS}}-{{tok.len.49}} } = {{ch02.tok49_entries}}$ entries, a sixteenth of the
table. The longest codes, {{ch02.TOKEN_BITS}} bits, fill one entry each.
@assert ch02.tok49 == 49 and 16 * ch02.tok49_entries == ch02.tok_entries and tok.codes_of[ch02.TOKEN_BITS] > 0
A token's entry holds more than the symbol and the code length: the two lengths and what is needed to
cut the offset bits out of the bits that follow ([chapter 7](#ch-internal)).

The table's size is the reason for the length limits above: it has to stay in the CPU's fastest
cache, or every lookup waits for memory ([chapter 3](#ch-cpu--cache)).

## Bits are read most significant first {#ch-compression--msb}

A **bitstream** is codes and plain numbers written one after the other, not aligned to bytes. seqlz
takes the most significant bit of each byte first: bit `i` of the stream is bit `7 - (i mod 8)` of
byte `i / 8`, so the 4 classes above, `0110000111`, are the bytes `61 c0`. A decoder keeps the next
bits in a 64-bit **bit register**, the next bit on top: peeking `L` bits is `reg >> (64 - L)`,
dropping `l` bits is `reg <<= l`, and a refill is one 8-byte load in big-endian order. The code's first
bit is then the index's top bit, which is why a code's entries are one range of the table.

Deflate reads least significant bit first. Then a peek is `reg & (2^L - 1)`, a code fixes the low $l$
bits of the index, its entries are every $2^l$-th one, spread over the whole table, and the codes have
to be bit-reversed when the table is built.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 230" role="img" aria-label="The 8-entry decode table of the offset classes, filled for bits read most significant first, where each code is one range, and least significant first, where codes are spread.">
  <g font-size="11" text-anchor="middle">
    <text x="185" y="30">000</text><text x="255" y="30">001</text><text x="325" y="30">010</text><text x="395" y="30">011</text>
    <text x="465" y="30">100</text><text x="535" y="30">101</text><text x="605" y="30">110</text><text x="675" y="30">111</text>
    <text x="185" y="118">000</text><text x="255" y="118">001</text><text x="325" y="118">010</text><text x="395" y="118">011</text>
    <text x="465" y="118">100</text><text x="535" y="118">101</text><text x="605" y="118">110</text><text x="675" y="118">111</text>
  </g>
  <g font-size="12">
    <text x="10" y="58">MSB first</text><text x="10" y="74" font-size="11">seqlz</text>
    <text x="10" y="146">LSB first</text><text x="10" y="162" font-size="11">deflate</text>
  </g>
  <g fill="none" stroke="currentColor" stroke-width="1">
    <rect x="150" y="40" width="70" height="40" class="fill-sheet"/><rect x="220" y="40" width="70" height="40" class="fill-sheet"/>
    <rect x="430" y="40" width="70" height="40" class="fill-sheet"/><rect x="500" y="40" width="70" height="40" class="fill-sheet"/>
    <rect x="570" y="40" width="70" height="40" class="fill-sheet"/><rect x="640" y="40" width="70" height="40" class="fill-sheet"/>
    <rect x="150" y="128" width="70" height="40" class="fill-sheet"/><rect x="220" y="128" width="70" height="40" class="fill-sheet"/>
    <rect x="360" y="128" width="70" height="40" class="fill-sheet"/><rect x="430" y="128" width="70" height="40" class="fill-sheet"/>
    <rect x="500" y="128" width="70" height="40" class="fill-sheet"/><rect x="640" y="128" width="70" height="40" class="fill-sheet"/>
  </g>
  <g class="acc" fill="none" stroke="currentColor" stroke-width="1.6">
    <rect x="290" y="40" width="70" height="40" class="fill-acc" fill-opacity="0.3"/><rect x="360" y="40" width="70" height="40" class="fill-acc" fill-opacity="0.3"/>
    <rect x="290" y="128" width="70" height="40" class="fill-acc" fill-opacity="0.3"/><rect x="570" y="128" width="70" height="40" class="fill-acc" fill-opacity="0.3"/>
  </g>
  <g font-size="11" text-anchor="middle">
    <text x="185" y="58">class 0</text><text x="185" y="72">2 bits</text>
    <text x="255" y="58">class 0</text><text x="255" y="72">2 bits</text>
    <text x="325" y="58">class 4</text><text x="325" y="72">2 bits</text>
    <text x="395" y="58">class 4</text><text x="395" y="72">2 bits</text>
    <text x="465" y="58">class 1</text><text x="465" y="72">3 bits</text>
    <text x="535" y="58">class 2</text><text x="535" y="72">3 bits</text>
    <text x="605" y="58">class 3</text><text x="605" y="72">3 bits</text>
    <text x="675" y="58">class 5</text><text x="675" y="72">3 bits</text>
    <text x="185" y="146">class 0</text><text x="185" y="160">2 bits</text>
    <text x="255" y="146">class 1</text><text x="255" y="160">3 bits</text>
    <text x="325" y="146">class 4</text><text x="325" y="160">2 bits</text>
    <text x="395" y="146">class 3</text><text x="395" y="160">3 bits</text>
    <text x="465" y="146">class 0</text><text x="465" y="160">2 bits</text>
    <text x="535" y="146">class 2</text><text x="535" y="160">3 bits</text>
    <text x="605" y="146">class 4</text><text x="605" y="160">2 bits</text>
    <text x="675" y="146">class 5</text><text x="675" y="160">3 bits</text>
  </g>
  <text x="10" y="196" font-size="11">shaded: the entries of class 4, code 01. The index is the next 3 bits as a number:</text><text x="10" y="212" font-size="11">the first bit read is its left digit (MSB first) or its right digit (LSB first).</text>
</svg>
</div>
<figcaption>The same canonical code in a decode table: read most significant bit first, each code is one range; read least significant bit first, its entries are spread.</figcaption>
</figure>

With 8 entries it makes no difference. With the token table of {{ch02.tok_entries}} entries, most
significant bit first keeps the few short codes most sequences use in a few of the table's cache
lines, and a cold read loads fewer of them ([chapter 3](#ch-cpu--msb-first)). The compressed bytes are
the same size either way.

## lz4, zstd and seqlz write the same sequence differently {#ch-compression--formats}

**`lz4` writes bytes.** A sequence is a token byte, whose high 4 bits are `ll` and low 4 bits
`ml - 4`, more bytes if `ll` is 15 or more, the literals, 2 bytes of offset, and more bytes if
`ml - 4` is 15 or more; the extra bytes count in steps of 255. Every number is at a known byte, which
is why `lz4` decodes fast, and why it spends at least 3 bytes on every match.

**`zstd` writes bits, with tables in the data.** It Huffman-codes the literals with a table it builds
for the block and stores in front of it, and codes `ll`, `ml` and the offset of each sequence as three
symbols with FSE, from tables that are predefined or described in the block. For a large input the
tables pay for themselves many times. On a page of 4 KiB, the code lengths of a literal table for the
256 byte values, about 64 bytes packed
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept)),
often cost more than the code saves.

**seqlz writes bits, with fixed tables.** Its tables are trained once, compiled into the code and part
of the format; a page says only which of {{ch02.LIT_SETS}} literal tables it uses, in
{{ch02.lit_set_bits}} bits. A sequence is one token, which holds `ll`, `ml - 4` and the offset class,
then the offset in as many plain bits as the class says, then length values only for long runs. The
decoder does one table lookup per sequence, where `zstd` steps three FSE states. What seqlz gives up
is tables that fit the page: a literal table of each page's own made writes 14% slower, for 32 bytes
less per page, 2.4% of the stored size, and was not kept
([`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-choices-with-their-numbers)).

The example page, `cat=1,dog=2;cat=3,dog=4;` and zeros after, is {{cat.len}} bytes in seqlz:
{{ch02.HEADER}} bytes of header, the literal count {{cat.lits}}; the {{cat.lits}} literals as they
are; and {{cat.b.bytes}} bytes of bitstream for the four sequences of the box above, of which the last
takes no bits.
@assert cat.seqs == 4 and cat.coded.len == cat.len
The table below runs ahead of [chapter 4](#ch-format), which explains the token, the escape
([one sequence: the token](#ch-format--token)) and how a long `ml` is sent
([length values](#ch-format--length-values)). Read it now for the sizes, and again after chapter 4 for
the bits:

| bits | what they are |
| --- | --- |
| `{{ch02.cat.0.code}}` `{{ch02.cat.0.esc}}` | the escape, then token {{ch02.cat.0.tok}} in {{ch02.ESCAPE_BITS}} bits: `ll` {{ch02.cat.0.ll}}, `ml` {{ch02.cat.0.ml}}, class {{ch02.cat.0.cls}} |
| `{{ch02.cat.0.raw}}` | offset {{ch02.cat.0.off}} in the {{ch02.cat.0.rawbits}} bits of class {{ch02.cat.0.cls}} |
| `{{ch02.cat.1.code}}` | token {{ch02.cat.1.tok}}: `ll` {{ch02.cat.1.ll}}, `ml` {{ch02.cat.1.ml}}, class {{ch02.cat.1.cls}}, the repeat offset {{ch02.cat.1.off}} |
| `{{ch02.cat.2.code}}` | token {{ch02.cat.2.tok}}: `ll` {{ch02.cat.2.ll}}, `ml - 4` {{ch02.cat.2.tml}}, class {{ch02.cat.2.cls}} |
| `{{ch02.cat.2.raw}}` | offset {{ch02.cat.2.off}} |
| `{{ch02.cat.2.vcode}}` `{{ch02.cat.2.vextra}}` | the length value {{ch02.cat.2.value}}: `ml` = {{ch02.ml_base}} + {{ch02.cat.2.value}} = {{ch02.cat.2.ml}} |
| | 0 literals left, 0 bytes of the page left: the last sequence, no bits |
| `{{ch02.cat.fill}}` | fill to the end of the last byte |
@assert [ch02.cat[k].escaped for k in "012"] == [1, 0, 0] and ch02.cat[1].cls == 0 and ch02.cat[2].tml == 31
@assert ch02.cat[0].code + ch02.cat[0].esc + ch02.cat[0].raw + ch02.cat[1].code + ch02.cat[2].code + ch02.cat[2].esc + ch02.cat[2].raw + ch02.cat[2].vcode + ch02.cat[2].vextra == cat.b.bin and cat.b.pad == len(ch02.cat.fill)

That is {{cat.b.bits}} bits and {{cat.b.pad}} bits of fill, the bytes `{{cat.b.hex}}`, as
`seqlz_compress()` writes them. The first token is escaped: 12 literals and a match of 4 at a short
offset is rare on real pages and has no code of its own. The same four sequences in `lz4`'s format
take {{ch02.lz4.bytes}} bytes besides the literals, leaving aside `lz4`'s rules for the end of a
block: a token byte each, 2 bytes for each of the three offsets, and {{ch02.lz4.ml_bytes}} bytes to
say {{ch02.cat.2.ml}}, {{ch02.lz4.ml_255}} of them 255.
@assert cat.escapes == 1

::: measured Where the bytes come from
Which of the two ideas gives `zstd` its ratio was estimated on the zram pages of a desktop, with the
same matches from `lz4hc` level 9 costed in several formats: in `lz4`'s format they take 30.7% of the
page, with `ll`, `ml` and offsets entropy coded 25.8%, with repeat offsets as in `zstd` 25.1%, and with
entropy coded literals as well 23.6%. The model is trained on the pages it is measured on, so the
numbers are on the optimistic side. Coding the sequences in bits is the largest step, and that is what
seqlz is built on
([explored-designs.md, "Where the ratio of zstd comes from"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#where-the-ratio-of-zstd-comes-from)).
:::

Fixed tables cost the most on pages unlike the training pages. Against Huffman codes fitted to the
pages themselves, an upper bound, seqlz's tables lose 25.3 bytes per page on average over desktop and
phone pages, most of it on desktop pages whose literals and tokens fit the tables badly
([explored-designs.md, "How tight the bits are"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).
How well a fixed table fits pages from devices and programs nobody trained on is the open question of
this design.
