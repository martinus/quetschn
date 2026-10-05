# The seqlz format

seqlz compresses one memory page into one compressed page. This file describes the bytes, so that a
decoder can be written from it alone, and it says which compressed pages are valid. It describes the
format as `explore/seqlz.c` writes and reads it. The why of each number is in
[docs/explored-designs.md](docs/explored-designs.md); here it gets one sentence at most.

`seqlz-fast` and `seqlz-fast-lit` are the same format: `seqlz-fast` always stores the literals as they
are, `seqlz-fast-lit` codes them where that pays. A decoder for one decodes both.

`tools/seqlz_ref.py` is a decoder written from this file, slow on purpose, and checked against
`seqlz_decode()`, see [How this file was checked](#how-this-file-was-checked).

## The idea

Like lz4 and zstd, seqlz describes a page as a list of **sequences**. Each sequence says: copy the next
`ll` **literals** (bytes stored in the compressed page as they are) to the output, then copy a **match**
of `ml` bytes that starts `off` bytes back in the output. The **offset** `off` points into what was
already written, so a match repeats bytes the page had before. The last sequence has literals only and
ends the page.

What makes seqlz smaller than lz4 is how the numbers `ll`, `ml` and `off` are stored: with **prefix
codes** (Huffman codes) from tables that are fixed and built into the decoder, so that frequent
combinations take few bits. The tables are not in the compressed page; a page of 4 KiB is too small to
carry its own.

All sizes in this file are in bytes unless they say bits. A literal is one byte. Numbers of more than
one byte are little endian.

## Page size

seqlz exists for 4 KiB pages and for 16 KiB pages. Some numbers depend on it:

| name | 4 KiB pages | 16 KiB pages | what it is |
| --- | --- | --- | --- |
| `P` | 12 | 14 | log2 of the page size |
| `PAGE` | 4096 | 16384 | bytes of the uncompressed page |
| `RAW_BITS` of class 3 and 5 | 12 and 9 | 14 and 11 | see [Offsets](#offsets) |
| `LEN_SYMBOLS` | 25 | 27 | symbols of a length value, `13 + P`, see [Length values](#length-values) |

The compressed page is a byte string of length `len`. Its length is not stored in it: zram stores it
and gives it to the decoder.

## Layout of a compressed page

There are two layouts: one with the literals stored as they are (**raw**), one with them coded
(**coded**). Bit 15 of the first two bytes says which one. The other 15 bits are `n`, the number of
literals; 15 bits could count up to 32767, but a page has at most `PAGE` literals.

**Raw literals**, bit 15 clear:

| offset | size | content |
| --- | --- | --- |
| 0 | 2 | `n` |
| 2 | `n` | the literals, in page order |
| `2 + n` | the rest | the bitstream of the sequences |

**Coded literals**, bit 15 set:

| offset | size | content |
| --- | --- | --- |
| 0 | 2 | `0x8000 + n` |
| 2 | 1 | `t`, which of the 8 literal tables codes them |
| 3 | 16 | `s[0]` to `s[7]`, 8 numbers of 2 bytes: the size of each literal stream |
| 19 | `s[0] + ... + s[7]` | the 8 literal streams, one after the other |
| `19 + s[0] + ... + s[7]` | the rest | the bitstream of the sequences |

The coded literals are split into 8 **streams**: literal 0 goes to stream 0, literal 1 to stream 1, ...,
literal 8 to stream 0 again. In one stream each literal's code can only be found once the one before is
decoded, a chain of table lookups; 8 streams are 8 chains, which a CPU works on side by side. Stream
`j` starts at `19 + s[0] + ... + s[j-1]`.

A page is invalid if

- `len < 2`, or with raw literals `2 + n > len`;
- with coded literals `len < 19`, `n > PAGE`, `t > 7`, or `19 + s[0] + ... + s[7] > len`.

Bytes after the bitstream of the sequences are allowed and ignored.

## Prefix codes

A **prefix code** gives each symbol a string of bits, its code, and no code is the start of another
one. That way a decoder can read bit by bit and knows when a code is complete. Frequent symbols get
short codes, rare ones long codes. E.g. with the three symbols A, B, C and the codes `0`, `10`, `11`,
the bits `0 11 10` are A C B.

A table here holds only the **length** of each symbol's code, 0 for a symbol without a code. The codes
themselves follow from the lengths, the same way as in deflate (RFC 1951, section 3.2.2): shorter codes
first, and among codes of the same length the symbols in the order of their number. As a rule:
count the codes of each length `l`; the first code of length `l` is the number
`first[l] = (first[l-1] + count[l-1]) * 2`, with `first[1] = 0` and `count[0] = 0`; the symbols of
length `l` get `first[l]`, `first[l] + 1`, ... in the order of their number. E.g. the lengths A 1,
B 2, C 2 give the codes `0`, `10`, `11` from above.

Every table is **complete**: its codes use up all bit strings, `2^-l` summed over all codes is exactly 1.
Then every long enough string of bits starts with exactly one code, and a decoder never meets bits that
are no code.

A code is always sent with its first bit, the leftmost in the examples, first. What "first" means is
different in the bitstream and in the literal streams, see there.

## The tables

The tables are part of the format: they are fixed, and a page cannot say that it uses others. A new
set of tables would be a new format, with a new name in zram.

| table | symbols | longest code | 4 KiB pages | 16 KiB pages |
| --- | --- | --- | --- | --- |
| tokens and escape, `TOK` | 3073 | 11 bits | `explore/seqlz_default_tables.c`, `.token` | `explore/seqlz_default_tables_16k.inc`, `.token` |
| literal length values, `LL` | `LEN_SYMBOLS` | 8 bits | the same, `.ll` | the same, `.ll` |
| match length values, `ML` | `LEN_SYMBOLS` | 8 bits | the same, `.ml` | the same, `.ml` |
| literal tables 0 to 7 | 256 each | 10 bits | `explore/seqlz_lit_sets.c` | the same |

The 4 KiB tables are trained on pages of a desktop and of an Android phone; tables trained on one
device's pages saved at most 1% on that device. The 16 KiB tables are trained on desktop pages only,
16 KiB pages made of 4 adjacent 4 KiB pages: there is no zram dump with 16 KiB pages yet.

`LL` and `ML` are not the literal tables: they code the lengths `ll` and `ml` of a sequence when they
are too large for a token. The literal tables code the literals themselves.

## The bitstream of the sequences

The bitstream `B` is read one bit at a time, starting with bit 0, the least significant bit, of its
first byte: bit `i` of `B` is bit `i mod 8` of byte `i div 8`. A decoder reads three kinds of things
from it:

- `read(n)`: the next `n` bits as a number, the first bit read is the least significant one. `read(0)`
  is 0.
- `symbol(table)`: the next bits, one by one, until they are a code of the table; the code's first bit
  is the first bit read. The result is the symbol of that code.
- `value(table)`: a length value, see below.

A valid page never needs more bits than `B` has: decoding uses at most `8 * |B|` bits. A decoder may
read zeros past the end instead of checking at every read, and check at the end that it did not use
them; `seqlz_decode()` does that.

### Length values

A length value is a number from 0 up to `2^(P+1) - 1`. Small values are a symbol of their own, larger
ones a symbol for a range of values plus bits that say which one in the range:

    s = symbol(table)
    if s < 16:  value = s
    else:       b = s - 12;  value = 2^b + read(b)

The symbols 16 to `12 + P` stand for the ranges `[2^b, 2^(b+1))` with `b = s - 12`, i.e. 16 to 31, 32 to
63, and so on. E.g. 100 is in `[64, 128)`, so `b = 6`, the symbol is 18, and `read(6)` gives
`100 - 64 = 36`. Symbol 16 starts at 16, right after the 16 small values, so no value is missing or
counted twice.

### Tokens

Each sequence starts with a **token**, one symbol of `TOK` that holds three numbers at once, so that
the most frequent combinations take few bits:

    ll  = token & 15         the literal length, 0 to 15; 15 means 15 or more
    mlf = (token >> 4) & 31  the match length minus 4, 0 to 31; 31 means 31 or more
    c   = token >> 9         the class of the offset, 0 to 5

That is 16 * 32 * 6 = 3072 tokens. `ml` is at least 4 because shorter matches are not worth it, so the
token stores `ml - 4`.

There are only 2048 codes of 11 bits, fewer than the 3072 tokens, and most tokens are rare, so only the
frequent ones have a code. Symbol 3072 of `TOK` is the **escape**: it is followed by the token's number
in 12 bits. Reading a token:

    t = symbol(TOK)
    if t == 3072:
        t = read(12)
        if t >= 3072: the page is invalid

### Offsets

The offset's class says how it is stored. Small offsets are frequent and offsets that are multiples of
8 too (pointers and other 8-byte values repeat at such distances), so they get classes with fewer bits:

| class | offset | bits read | shift | offsets it holds |
| --- | --- | --- | --- | --- |
| 0 | the last offset again | 0 | | the offset of the sequence before |
| 1 | `read(4)` | 4 | 0 | 0 to 15 |
| 2 | `read(8)` | 8 | 0 | 0 to 255 |
| 3 | `read(P)` | 12 or 14 | 0 | 0 to `PAGE - 1` |
| 4 | `read(5) * 8` | 5 | 3 | 0 to 248, multiples of 8 |
| 5 | `read(P - 3) * 8` | 9 or 11 | 3 | 0 to `PAGE - 8`, multiples of 8 |

The number read is the **raw** value, and **shift** is how far it is shifted left: a shift of 3
multiplies by 8, so a multiple of 8 needs 3 bits less. The last offset starts as 1 for each page. An
offset of 0, or one that points before the start of the page, makes the page invalid.

## Decoding

`lits` are the `n` literals from the layout (with coded literals: decoded as in
[Coded literals](#coded-literals)), `out` is the page being written, `last` is 1.

    loop:
        token: ll, mlf, c
        raw = read(bits of class c)
        off = (c == 0) ? last : raw << shift of class c
        if ll == 15: ll = 15 + value(LL)
        if ll > literals left or ll > PAGE - |out|: invalid
        append the next ll literals to out
        if |out| == PAGE: stop                      the last sequence
        ml = mlf + 4
        if mlf == 31: ml = 35 + value(ML)
        last = off
        if off == 0 or off > |out| or ml > PAGE - |out|: invalid
        append ml bytes to out, each one the byte off places before it

    at the stop: all n literals used and at most 8 * |B| bits of B used, else invalid

The bits of one sequence are therefore, in this order: the token (the escape and 12 bits if it is
escaped), the raw bits of the offset, the literal length value if `ll` was 15, the match length value
if `mlf` was 31. The match can overlap the bytes it writes: with `off = 1` and `ml = 10` it repeats the
last byte 10 times.

There is no count of the sequences. The page ends with the sequence whose literals fill it, and that is
the only way it ends: a page whose last match reaches the end of the page has one more sequence with
`ll = 0`. The last sequence's token and offset bits are read like any other, but its `mlf` and its
offset are not used, and they can be anything. The compressor writes `mlf = 0` and class 0, which needs
no offset bits.

## Coded literals

Each literal stream is read most significant bit first: bit `i` of stream `j` is bit `7 - (i mod 8)` of
the page's byte `start[j] + i div 8`. Stream `j` holds literal `j`, `j + 8`, `j + 16`, ... below `n`,
each as one symbol of literal table `t`, one after the other. A code's first bit is the first one read,
as in the bitstream of the sequences.

The codes of stream `j`'s literals must fit into its `s[j]` bytes, that is take at most `8 * s[j]` bits,
else the page is invalid. Bits that are left over in the last byte of a stream are ignored. A decoder
may read past the end of a stream, as long as it checks this.

## What the compressor writes

Not part of the format: any page that decodes is fine, and one page can be written in many ways, as
with lz4 or zstd. For reference, `seqlz_compress()` writes:

- the matches of a greedy matcher (`explore/page_lz.h`); each offset with class 0 when it is the last
  one, else class 4 or 5 for a multiple of 8 from 16 on, else the smallest of classes 1 to 3 that holds
  it;
- the escape only for a token without a code;
- for the last sequence `mlf = 0` and class 0;
- zero bits to fill the last byte of the bitstream and of each literal stream, and nothing after the
  bitstream;
- coded literals only when they save at least `n / 16` bytes against raw ones, with the literal table
  that codes them in the fewest bits.

The compressed page is at most `2 * PAGE` bytes, which is the buffer zram gives the compressor. zram
stores a page that does not compress well enough as it is, so that page never reaches the decoder.

## Open points

1. **The last sequence's unused fields.** Its `mlf` and offset class can be anything (see
   [Decoding](#decoding)). Requiring `mlf = 0` and class 0, as the compressor writes them, would cost one
   compare per page and make one more kind of damaged page invalid.
2. **No version in the page.** A new format would be a new algorithm name in zram, which stores the name
   per device.

## How this file was checked

`tools/seqlz_ref.py` decodes bit by bit, from this file alone. It was compared with `seqlz_decode()` on
13279 inputs: the inputs AFL++ kept for the decode target (`fuzz/`), the pages of the roundtrip target
compressed with raw and with coded literals, and those pages with one literal stream one byte shorter.
On every input both said valid with the same page, 5216 times, or both said invalid. With the rule
of the stream sizes as it was before, with 50 bits more, the reference accepted all 5704 shortened
pages, so the comparison sees a difference when there is one.
