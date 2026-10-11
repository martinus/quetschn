# Compression from zero: matches, literals and codes {#ch-compression}

Almost every fast compressor, `lz4`, `zstd` and seqlz included, is built from two ideas. The first
is to say "that again" instead of repeating bytes: a page becomes a list of **literals**, bytes stored
as they are, and **matches**, "copy so many bytes from so far back". The second is to give frequent
things short codes and rare things long ones: the numbers that describe the literals and matches are
written with a **Huffman code** instead of in a fixed number of bits. `lz4` uses only the first idea,
`zstd` both, with code tables it builds from the data and stores in it. seqlz uses both with tables
that are fixed, trained once on many pages and part of the format.

Both ideas are built up here from zero, with small examples that can be checked by hand, up to how
`lz4`, `zstd` and seqlz write the same sequence down. The words are the ones of the rest of the
book: page, literal, match, offset, `ll`, `ml`, sequence, symbol, code, code length, table,
bitstream.

## Literals, matches and sequences {#ch-compression--lz77}

The first idea is from Abraham Lempel and Jacob Ziv, 1977, and compressors of this kind are called
**LZ77** or just LZ. The compressor walks through its input. When the next bytes appeared before, it
writes a **match**: go back `offset` bytes in what is already written, and copy `ml` bytes from
there, the match length. When they did not, it keeps the byte as a **literal**. The output is a list
of **sequences**, each the same two steps: first `ll` literals, then one match. The last sequence has
literals only.

The decoder is short enough to write down completely:

```c
/* one sequence: ll literals, then ml bytes from off back */
memcpy(out + pos, lit, ll);
pos += ll;
lit += ll;
for (i = 0; i < ml; i++)
	out[pos + i] = out[pos + i - off];
pos += ml;
```

The match is copied one byte at a time, front to back, on purpose; the next section shows why.

Here is a made-up page that starts with two records of the same layout, 24 bytes of text:

```text
position  0         1         2
          012345678901234567890123
          cat=1,dog=2;cat=3,dog=4;
```

A compressor that remembers where every 4 bytes last appeared finds this, from left to right:

- Positions 0 to 11, `cat=1,dog=2;`, never appeared before: 12 literals.
- At position 12, `cat=` appeared at position 0, 12 bytes back. The match is 4 bytes long; position
  16 has `3` where position 4 has `1`. Sequence 1: `ll` 12, a match of `ml` 4 at offset 12.
- Position 16, `3`, starts nothing seen before: a literal.
- At position 17, `,dog=` appeared at position 5, again 12 bytes back, 5 bytes long until `4`
  meets `2`. Sequence 2: `ll` 1, `ml` 5, offset 12.
- Positions 22 and 23, `4;`, are the rest. `;` appeared before, but 1 byte is no match. The last
  sequence: `ll` 2 and no match.

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

24 bytes became 15 literals and 3 sequences. Whether that is smaller depends on how the sequences are
written down, which is the second half of this chapter. Two details already show. Both matches have the
same offset, 12, the length of a record: memory is full of arrays of records and of 8-byte values, so
the offset of the match before repeats often. seqlz has a **repeat offset** for that, an offset that
costs no bits because it is the one of the match before. And the matches are short: most matches in
memory pages are a few bytes to a few dozen.

### Why a match is at least 4 bytes

A match is not free: the format has to say how long it is and where it copies from. In `lz4`'s format
that is at least 3 bytes, a byte that holds the lengths and 2 bytes of offset. A match of 3 bytes would
save nothing, so `lz4` matches are 4 bytes or longer, and the format stores `ml - 4`, so that no number
is spent on lengths 0 to 3 that never occur. seqlz does the same. In its format a match of 4 bytes at a
new offset of up to 12 bits costs about as many bits as its 4 bytes as coded literals, often more, so
shorter ones are not worth looking for
([chapter 4](#ch-format--token)).
A 5-byte match at a 12-bit offset, about 21 bits, is cheaper than its 5 literals, about 35 bits
([explored-designs.md, "The matcher without its step"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept)).
There is a practical side too: 4 bytes are one 32-bit load and one compare, which is how the matcher
checks a candidate.

## A match may copy what it is writing {#ch-compression--overlap}

The offset of a match can be smaller than its length. Then the match reads bytes that it wrote itself
a moment earlier. Take a page that is `ab` 2048 times. Its compressed form is 2 literals, `a` and `b`,
and one match of 4094 bytes at offset 2: each byte copied is the byte 2 places before it, which the
copy has just written.

@anim overlap A match of 10 bytes at offset 1, 2 or 3, one byte per step. The byte loop copies what it wrote itself a moment before; memmove() copies the source as it was before the copy, and gets bytes that were not written yet.

With offset 1 a match repeats the last byte: a literal `00` and a match of 4095 at offset 1 is a page
of zeros, and that is how runs of one byte compress in every LZ format. This is why the decoder above
copies one byte at a time, front to back. A block copy gives other bytes: `memcpy()` with overlapping
source and destination is undefined behaviour in C, and `memmove()` is defined to copy as if through a
temporary buffer, so it copies the source as it was before the copy. For `ab` and a match of 6 at offset
2 it would copy `ab` and 4 bytes that were not written yet, where the byte loop gives `abababab`. A fast
decoder copies 8 or 16 bytes at a time, and then it has to take care with offsets below 8 or 16;
[chapter 14](#ch-decode) shows how seqlz does that.

## Finding matches: a hash table of positions {#ch-compression--hash}

To find a match, the compressor needs to know whether the next bytes appeared before, and where.
Searching all earlier positions is far too slow: 4096 positions, each against up to 4095 earlier
ones. Fast compressors keep a **hash table** of positions instead.

A **hash** is a number computed from some bytes, here from the next few bytes at a position, that
selects a **slot** of the table. The slot holds a position where bytes with that hash were seen last.
At every position the compressor:

1. computes the hash of the next bytes and reads the slot: a **candidate** position;
2. compares the bytes at the candidate with the bytes here; if the first 4 are equal, it is a match,
   and the compressor counts how far it goes on;
3. writes the current position into the slot, overwriting what was there.

The table is a **cache, not a map**. A map would keep every position for every byte string; the table
keeps one position per slot, and two different byte strings with the same hash, a **collision**,
overwrite each other. So the compressor misses a match now and then. It never makes a wrong one,
because every candidate is compared before it is used. In exchange, each position costs one read and
one write of the table, whatever the page holds.

@anim hash wide The three steps on a short text, as a model, not seqlz's matcher: the table has 16 slots instead of 4096, the hash is seqlz's hash of 5 bytes cut to 4 bits, and there is no repeat offset. Every number is computed from the text.

seqlz's table has 4096 slots of 2 bytes, 8 KiB, all of its work memory for compression, and it is cleared
for every page, so every position in it is a position of the current page. Its hash covers the next
**5 bytes**, not 4, which finds fewer and longer matches; a match of exactly 4 bytes is still found at
the repeat offset, which needs no table. E.g. the 5 bytes `,dog=` go to slot 2823 at position 5 and
again at position 17, which is how the match of sequence 2 is found. [Chapter 9](#ch-matcher--hash)
goes through the hash line by line and has the measurements behind 5 bytes.

::: try The real matcher on the example
On a page that starts with `cat=1,dog=2;cat=3,dog=4;` and is zeros after, seqlz's matcher finds the
first two sequences exactly as above. Its hash of 5 bytes cannot find `cat=` at position 12, because
`cat=3` never appeared before, but the matcher finds it anyway, for a reason that has nothing to do
with the hash: the cleared table holds 0 in every slot, an empty slot is a candidate at
position 0, and position 0 starts with `cat=`. Put a `#` in front, and the first match is gone: the
matcher then writes 18 literals and finds only `,dog=`. On the page without the `#`, the third
sequence takes `4`, `;` and one zero as literals, and covers the other 4071 zeros with one match at
offset 1. The match ends at the end of the page, and the matcher still passes a last sequence with no
literals and no match, which is how the format marks the end. [Chapter 9](#ch-matcher--back) has a
program that prints every sequence the matcher finds; give it this page, and others.
:::

The compressor here is **greedy**: it takes the first match it finds and moves on behind it, without
looking whether a match that starts one byte later would be longer. Searching more pays in bytes:
`zstd` 3 looks for a long match first and checks one byte later, `lz4hc` searches chains of older
positions. It costs time on every write. The project measured a seqlz that searches a hash chain, picks the
match that saves the most bits by the tables and looks one position ahead, level 3: 2.7% to 4.3%
smaller pages for about twice the time to write, and did not keep it, nor the deeper level 4
([`docs/plan.md` §1.1](https://github.com/martinus/quetschn/blob/main/docs/plan.md#11-the-score-memory-against-time-not-bars)).

## Fixed-width numbers waste bits {#ch-compression--entropy}

Now to the second idea. Every sequence has to say its `ll`, its `ml` and its offset. The simple way is a
fixed number of bits for each, as `lz4` does in whole bytes: 2 bytes for every offset, though a 4 KiB
page has offsets of at most 12 bits, and offsets below 16 are common.

How many bits does a number need, if not a fixed amount? It depends on how often each value occurs. A
small example with real numbers: seqlz sorts every offset into one of 6 **offset classes**, e.g. class 0
for the repeat offset, class 1 for offsets below 16, class 4 for multiples of 8 below 256. These are how
often each class occurred on the 524 912 pages seqlz's tables were trained on, summed from the token
counts in
[`bench/seqlz_counts_4k.txt`](https://github.com/martinus/quetschn/blob/main/bench/seqlz_counts_4k.txt):

| class | offset | share of the sequences |
| --- | --- | --- |
| 0 | the repeat offset | 30.55% |
| 1 | below 16 | 13.04% |
| 2 | below 256 | 8.82% |
| 3 | below 4096 | 8.12% |
| 4 | multiple of 8, 16 to 248 | 24.17% |
| 5 | multiple of 8, from 256 | 15.30% |

6 values in a fixed width take 3 bits, and 2 of the 8 patterns are never used. Information theory says
what they need at least: a value that occurs with probability $p$ carries $-\log_2 p$ bits of
information, and the average of that over all values, the **entropy**, is the least number of bits per
value any code can reach on average. Here it is 2.42 bits. (These are shares of all sequences in the
training counts, which count the pages of a phone's zram dump five times; on the desktop dumps, 18% to 25% of
the matches use the repeat offset, see [chapter 9](#ch-matcher--plan).) Class 0, at 30.55%, carries 1.71 bits;
class 3, at 8.12%, carries 3.62. A good code gives class 0 fewer than 3 bits and class 3 more.

## Prefix codes {#ch-compression--prefix-codes}

If codes have different lengths, the decoder must know where one ends and the next begins. A **prefix
code** makes that possible without any separator: no code is the start of another one. Say there are
four symbols, A in half the cases, B in a quarter, C and D in an eighth each, with the codes `0`, `10`,
`110` and `111`. Reading bit by bit, `0` is complete as A; `1` is not complete, `10` is B; `11` not,
`110` is C and `111` is D. So `0 10 0 111 0`, written without spaces as `01001110`, can only be
A B A D A. On average that is $0.5 \times 1 + 0.25 \times 2 + 0.125 \times 3 \times 2 = 1.75$ bits per symbol, where 4
symbols in a fixed width take 2. A **symbol** is whatever gets a code: an offset class here, a token,
a byte value, a length.

## Huffman codes, built by hand {#ch-compression--huffman}

David Huffman's method of 1952 finds the best prefix code for given frequencies, the one with the
fewest bits on average. It builds a binary tree from the leaves up:

1. Make each symbol a node, weighted by its frequency.
2. Take the two nodes with the smallest weights and join them under a new node whose weight is their
   sum.
3. Repeat until one node is left, the root.

Each symbol's code is the path from the root to its leaf, 0 for one branch and 1 for the other, and its
**code length** is its depth. For the offset classes:

| step | joins | weights | new weight |
| --- | --- | --- | --- |
| 1 | class 3 and class 2 | $8.12 + 8.82$ | 16.94 |
| 2 | class 1 and class 5 | $13.04 + 15.30$ | 28.34 |
| 3 | {3, 2} and class 4 | $16.94 + 24.17$ | 41.11 |
| 4 | {1, 5} and class 0 | $28.34 + 30.55$ | 58.89 |
| 5 | the two that are left | $41.11 + 58.89$ | 100 |

@anim huffman The table above, one join per two steps, then the codes read off the tree. The shares are the ones of the offset classes in the training counts; the average is computed from them.

Classes 0 and 4 get 2 bits, the other four get 3. On average that is
$2 \times (30.55\% + 24.17\%) + 3 \times (13.04\% + 8.82\% + 8.12\% + 15.30\%) = 2.45$ bits, against 3 for the
fixed width and 2.42 for the entropy. The gap to the entropy is what a Huffman code cannot get: every
code is a whole number of bits, so a symbol with probability 0.9, which carries 0.15 bits, still costs
1. Coders with fractional bits exist, arithmetic coding and ANS, which `zstd` uses for its sequences as
FSE, but their decoders do more work per symbol. On the project's dumps, Huffman codes fitted to the
pages are 1.4 to 3.5 bytes per page above the entropy, 0.2% to 0.3%, so seqlz stays with Huffman codes
([explored-designs.md, "How tight the bits are"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).

seqlz does not give the offset class a code of its own; it puts the class into one symbol together with
`ll` and `ml`, the **token**, and codes that. The class is a good example only because it has 6 values.
The token has 3072, and the same method builds its code.

One more limit matters in practice: a decoder that decodes with a table, as below, needs a table of
$2^L$ entries for a longest code of $L$ bits. A Huffman code for 3072 symbols, some of them very rare,
can have codes of 20 bits and more. So the trainer, the program that builds seqlz's tables from counts
([chapter 8](#ch-codes--lengths)), limits the code lengths, 11 bits for tokens, 10 for
literals, 8 for [length values](#ch-format--length-values), at a small cost in bits, and gives the rarest tokens no code at all; they
share one code, the **escape**, followed by the token in 12 plain bits ([chapter 4](#ch-format)).

## Canonical codes: only the lengths are stored {#ch-compression--canonical-codes}

The tree above gave class 0 the code `11`, but `00` would do as well: swapping the 0 and the 1 at any
node gives another code with the same lengths and the same average. What matters is only the length of
each code. So formats store the lengths and derive the codes from them by a fixed rule, a **canonical
code**. seqlz's tables are exactly that, a list of code lengths, one byte per symbol, and the rule is the
one of deflate, RFC 1951 §3.2.2, written in
[`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md#prefix-codes):

1. `count[l]` is the number of symbols whose code has `l` bits; `count[0]` is taken as 0.
2. `first[1] = 0`, and for `l >= 2`: `first[l] = (first[l-1] + count[l-1]) * 2`.
3. The symbols with a code of `l` bits, in the order of their number, get the numbers `first[l]`,
   `first[l] + 1`, and so on.
4. A symbol's code is that number in binary with exactly `l` digits, leading zeros included, the most
   significant digit first.

In words: shorter codes come first, the codes of one length are consecutive numbers, and each length
goes on where the length before ended, shifted one bit to the left. For the offset classes, with the
lengths 2, 3, 3, 3, 2, 3 for classes 0 to 5:

| step | value |
| --- | --- |
| `count[2]`, `count[3]` | 2 (classes 0 and 4), 4 (classes 1, 2, 3 and 5) |
| `first[1]` | 0 |
| `first[2]` | `(0 + count[1]) * 2 = (0 + 0) * 2 = 0` |
| `first[3]` | `(first[2] + count[2]) * 2 = (0 + 2) * 2 = 4` |
| codes of 2 bits | class 0: 0 = `00`, class 4: 1 = `01` |
| codes of 3 bits | class 1: 4 = `100`, class 2: 5 = `101`, class 3: 6 = `110`, class 5: 7 = `111` |

The codes differ from the tree's, the lengths are the same, and the encoder and every decoder get the
same codes from 6 bytes of lengths. In `src/` this is `first_codes()` in `src/seqlz_codes.c`;
[chapter 8](#ch-codes--first-codes) goes through it line by line and works the rule through for the
real table of `ll` length values.

## Complete codes: every bit pattern starts a code {#ch-compression--complete}

A code of $l$ bits uses up $\frac{1}{2^l}$ of all bit strings: of all strings of 3 bits, the code `01` starts 2,
`010` and `011`, a quarter. The sum of $2^{-l}$ over all codes, the **Kraft sum**, is at most 1 for any
prefix code. For the offset classes it is $\frac{2}{4} + \frac{4}{8} = 1$. A code whose Kraft sum is exactly 1 is
**complete**: it uses up every bit string, so every long enough string of bits starts with exactly one
code.

An incomplete code leaves gaps. With only A = `0` and B = `10`, the sum is $\frac{1}{2} + \frac{1}{4} = \frac{3}{4}$, and the
bits `11` start no code at all; a decoder has to check for that, and say what happens then. A sum above
1 cannot be a prefix code: three codes of lengths 1, 1 and 2 would need more than all bit strings.
Huffman codes are always complete, because every node of the tree has two children. seqlz requires it of
every table: `first_codes()` adds `2^(bits - l)` for each code, which is the Kraft sum times `2^bits`,
the number of decode table entries the codes fill, and it must be exactly `2^bits`
([chapter 8](#ch-codes--first-codes)).

What this buys is a decoder without the check. Any bits it reads decode to some symbol, so there is no
"invalid code" branch on the path from one sequence to the next. When the project made all its codes
complete, the decoder ran 6% fewer instructions
([`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-choices-with-their-numbers)).
A damaged page then decodes to wrong symbols, not to an error, and the decoder's other checks, on
lengths, offsets and the end of the page, are what catch it ([chapter 14](#ch-decode)).

## Table-driven decoding {#ch-compression--table-decoding}

Reading a code bit by bit is a loop with a branch per bit. Decoders do it with one table lookup
instead. If the longest code has $L$ bits, the decoder peeks at the next $L$ bits without consuming
them, uses them as an index into a table of $2^L$ entries, and the entry says which symbol it is and how
many bits its code has. The decoder then drops that many bits and peeks again. One lookup per code,
whatever its length.

For the offset classes, $L$ is 3 and the table has 8 entries. A code of $l$ bits is the first $l$ bits
of the index, and the bits after it can be anything, so it fills $2^{3-l}$ entries in a row: class 0,
code `00`, fills `000` and `001`; class 4, code `01`, fills `010` and `011`; each 3-bit code fills one.
Decoding the classes 4, 1, 0, 5, whose codes make the bits `01 100 00 111`:

| bits left | peek 3 | entry | drop |
| --- | --- | --- | --- |
| `0110000111` | `011` | class 4, 2 bits | 2 |
| `10000111` | `100` | class 1, 3 bits | 3 |
| `00111` | `001` | class 0, 2 bits | 2 |
| `111` | `111` | class 5, 3 bits | 3 |

@anim table From the code lengths to a decode table and back to symbols: the [canonical codes](#ch-compression--canonical-codes), the 8 entries filled code by code, then the four lookups of the table above. Every number is computed from the lengths 2, 3, 3, 3, 2, 3.

The last peek here needs no bits past the end, but in general it does, e.g. when the last code is
`00`; a decoder reads zeros past the end, or the format guarantees that bytes follow. Filling the table
is a loop at the end of each of seqlz's table builders ([chapter 8](#ch-codes--build-token)). For a
token of code `c` with `l` bits, the indices from `c << (11 - l)` up to, not including,
`(c + 1) << (11 - l)` are the 11-bit numbers whose top `l` bits are `c`, and each gets the same entry.
seqlz's token table has `SEQLZ_TOKEN_BITS` = 11, 2048 entries of 4 bytes, 8 KiB. Token 49, "1
literal, a match of 7 bytes, the repeat offset", the sequence of an array of 8-byte pointers that
differ only in their lowest byte, has a code of 4 bits (how a token's number comes from `ll`, `ml` and
the offset class is in [chapter 4](#ch-format--token)), so it fills $2^{11-4} = 128$ entries, one sixteenth of the table. The longest codes, 11
bits, fill one entry each. [Chapter 7](#ch-internal) shows what an entry holds: for a token not only the
symbol and the code length, but the two lengths and everything needed to cut the offset bits out of the
bits that follow.

The table's size is the reason for the length limits above. A table has to stay in the CPU's fastest
cache, the L1 of a few dozen KiB, or every lookup waits for memory; [chapter 3](#ch-cpu--cache) has
what one more bit of code length cost.

## Bits are read most significant first {#ch-compression--msb}

A **bitstream** is codes and plain numbers written one after the other, not aligned to bytes. There are
two ways to put such bits into bytes. seqlz takes the most significant bit of each byte first: bit `i` of
the stream is bit `7 - (i mod 8)` of byte `i / 8`. The 4 classes above, `0110000111`, are the bytes
`61 c0`: `01100001` and `11` followed by six 0 bits.

A decoder keeps the next bits in a 64-bit variable, its **bit register**, with the next bit in the most
significant position. Then peeking `L` bits is a shift, `reg >> (64 - L)`, dropping `l` bits is
`reg <<= l`, and loading more bytes is one 8-byte load in big-endian byte order. The peeked bits are the
table index with the code's first bit in the index's top bit, which is why a code's entries are one range
of the table.

The other way, least significant bit first, is the one of deflate. The
decoder then peeks with a mask, `reg & (2^L - 1)`, and the code's first bit is the lowest bit of the
index. The entries of one code are then not a range: a code of $l$ bits fixes the low $l$ bits of the
index, and its entries are every $2^l$-th one, spread over the whole table. And the codes, whose first
bit is their most significant digit by the canonical rule, have to be bit-reversed when the table is
built.

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

With 8 entries it makes no difference. With the token table of 2048 entries it does: read most
significant bit first, the few short codes most sequences use stay in a few of the table's cache
lines, and a cold read loads fewer of them from memory, see [chapter 3](#ch-cpu--msb-first). The
compressed bytes are the same size either way; only the order of the bits differs.

## lz4, zstd and seqlz write the same sequence differently {#ch-compression--formats}

All three formats describe a page as literals and sequences. They differ in how they write the
numbers.

**`lz4` writes bytes.** A sequence is a token byte, whose high 4 bits are `ll` and low 4 bits `ml - 4`,
more bytes if `ll` is 15 or more, the literals, 2 bytes of offset, and more bytes if `ml - 4` is 15 or
more; the extra bytes count in steps of 255.
No tables, no bits; every number is at a known byte. That is why `lz4` decodes fast, and why it spends
at least 3 bytes on every match.

**`zstd` writes bits, with tables in the data.** It Huffman-codes the literals with a table it builds
for the block and stores in front of it, and codes `ll`, `ml` and the offset of each sequence as three
symbols with FSE, an ANS coder, from tables it either takes predefined or builds for the block and
describes in it. For a large input the tables are paid for many times over. A page is not large: the
code lengths of a literal table for the 256 byte values take `zstd` about 64 bytes, packed, and on a
page of 4 KiB that often costs more than the code saves.

**seqlz writes bits, with fixed tables.** Its tables are trained once on many pages, compiled into the
code, and part of the format; a page says only which of 8 literal tables it uses, in 3 bits. A
sequence is one token, which holds `ll`, `ml - 4` and the offset class, with its Huffman code, then the
offset in as many plain bits as the class says, then length values only for long runs. The decoder does
one table lookup per sequence, where `zstd` steps three FSE states. What seqlz gives up is tables that
fit the page. A literal table of each page's own, where that saves 16 bytes, made writes 13% and 15%
slower on the project's two desktop dumps, for 2.7 and 60.6 bytes less per page, 0.3% and 4.6% of the
stored size, and was not kept
([`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-choices-with-their-numbers)).

The example page from the start of this chapter, `cat=1,dog=2;cat=3,dog=4;` and zeros after, is 28 bytes
in seqlz: 2 bytes of header, the literal count 16; the 16 literals as they are, the matcher's sequences
being the ones from the box above; and 10 bytes of bitstream for the four sequences. The table below
runs ahead of [chapter 4](#ch-format), which explains how a token's number packs `ll`, `ml` and the
offset class and what the escape is ([one sequence: the token](#ch-format--token)), and how a long
`ml` is sent ([length values](#ch-format--length-values)). Read it now for the sizes, and again after
chapter 4 for the bits:

| bits | what they are |
| --- | --- |
| `0011` `001000001100` | the escape, then token 524 in 12 bits: `ll` 12, `ml` 4, class 1 |
| `1100` | offset 12 in the 4 bits of class 1 |
| `101001011` | token 17: `ll` 1, `ml` 5, class 0, the repeat offset 12 |
| `0011` `001111110011` | the escape, then token 1011: `ll` 3, `ml - 4` 31, class 1 |
| `0001` | offset 1 |
| `111101` `11111000100` | the length value 4036: `ml` = 35 + 4036 = 4071 |
| `10001010` | token 0: `ll` 0, the page is full, the end |
| `000000` | fill to the end of the last byte |

That is 74 bits and 6 bits of fill, the bytes `32 0c ca 59 9f 98 fb f1 22 80`, as `seqlz_compress()`
writes them. Two of the tokens are escaped: a sequence of 12 literals and a match of 4 at a short offset
is rare on real pages and has no code of its own. The same four sequences in `lz4`'s format take 26
bytes besides the literals, leaving aside `lz4`'s rules for the end of a block: a token byte each, 2 bytes
for each of the three offsets, and 16 bytes to say 4071, 15 of them 255.

::: measured Where the bytes come from
Which of the two ideas gives `zstd` its ratio was estimated on the project's first desktop dump, with
the same good matches from `lz4hc` level 9 costed in several formats: in `lz4`'s format they take
30.7% of the page, with `ll`, `ml` and offsets entropy coded 25.8%, with repeat offsets as in `zstd`
25.1%, and with entropy coded literals as well 23.6%. The model is trained on the pages it is
measured on, so the numbers are on the optimistic side. Coding the sequences in bits is the largest
step, and that is what seqlz is built on
([explored-designs.md, "Where the ratio of zstd comes from"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#where-the-ratio-of-zstd-comes-from)).
:::

Fixed tables cost the most on pages that are unlike the training pages. Against Huffman codes fitted to
each dump itself, an upper bound, seqlz's tables lose 4.6 to 14.4 bytes per page on three of the
project's dumps, and 75.7 bytes on the second desktop dump, whose literals and tokens fit the tables
badly
([explored-designs.md, "How tight the bits are"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#how-tight-the-bits-are-ans-would-give-03-at-most-the-offsets-have-2-16-literal-tables-01-to-2)).
How well a fixed table fits pages from devices and programs nobody trained on is the open question of
this design.
