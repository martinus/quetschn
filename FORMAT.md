# The seqlz format

seqlz compresses one memory page into one compressed page. This file describes the bytes, so that a
decoder can be written from it alone, and it says which compressed pages are valid. It describes the
format as `explore/seqlz.c` writes and reads it. Sentences marked *Why:* explain a choice and are
not part of the format; the measurements behind them are in
[docs/explored-designs.md](docs/explored-designs.md). How seqlz works, for a reader new to
compression, is in [docs/seqlz.md](docs/seqlz.md).

`seqlz-fast` and `seqlz-fast-lit` are the same format: `seqlz-fast` always stores the literals as they
are, `seqlz-fast-lit` codes them where that pays. A decoder for one decodes both.

`tools/seqlz_ref.py` is a decoder written from this file, slow on purpose, and checked against
`seqlz_decode()`, see [How this file was checked](#how-this-file-was-checked).

## The idea

Like lz4 and zstd, seqlz describes a page as a list of **sequences**. Each sequence says: copy the next
`ll` **literals** (bytes stored in the compressed page as they are) to the output, then copy a **match**
of `ml` bytes that starts `off` bytes back in the output, its **offset**. The last sequence has literals
only and ends the page. The numbers `ll`, `ml` and `off` are stored with **prefix codes** (Huffman
codes) from fixed tables that are part of the format and not in the compressed page. *Why:* a page of
4 KiB is too small to carry tables of its own.

## Notation

- Sizes are in bytes unless they say bits. A literal is one byte.
- Numbers of several bytes are little endian: the first byte is the low one.
- `|x|` is the length of `x`: the bytes of a byte string, the bytes written so far for `out`.
- `a << b` is `a * 2^b`, `a >> b` is `a` divided by `2^b`, rounded down, `a & b` is the bitwise and,
  `a div b` and `a mod b` are the quotient and remainder, `c ? a : b` is `a` if `c` holds, else `b`.
- A **bit string** like `110` is written in the order its bits are read, the first one leftmost. A
  number like 6 is a number. `read(3)` of the bits 0, 1, 1 is the number 6; the same bits as a code are
  the bit string `011`.

## Page size

seqlz is defined for pages of 4 KiB and of 16 KiB; the page size is not stored in the compressed page,
the decoder has to know it. Some numbers depend on it:

| name | 4 KiB pages | 16 KiB pages | what it is |
| --- | --- | --- | --- |
| `P` | 12 | 14 | log2 of the page size |
| `PAGE` | 4096 | 16384 | bytes of the uncompressed page |
| `LEN_SYMBOLS` | 25 | 27 | symbols of a length value, `13 + P`, see [Length values](#length-values) |

The compressed page is a byte string of length `len`. Its length is not stored in it either: zram
stores it and gives it to the decoder.

## Layout of a compressed page

There are two layouts: one with the literals stored as they are (**raw literals**), one with them coded
(**coded literals**). Bytes 0 and 1 are a 2-byte number `h`. Its highest bit, `h & 0x8000` (bit 7 of
byte 1), says which layout it is. The other 15 bits, `n = h & 0x7fff`, are the number of literals; 15
bits could count up to 32767, but a page has at most `PAGE` literals, and `n > PAGE` is invalid in both
layouts.

**Raw literals**, `h & 0x8000` is 0:

| offset | size | content |
| --- | --- | --- |
| 0 | 2 | `h`, which is `n` |
| 2 | `n` | the literals, in page order |
| `2 + n` | up to `len` | `B`, the bitstream of the sequences |

**Coded literals**, `h & 0x8000` is not 0:

| offset | size | content |
| --- | --- | --- |
| 0 | 2 | `h`, which is `0x8000 + n` |
| 2 | 1 | `set + 8 * w`: `set = t & 7` is which of the 8 literal tables codes them, `w = t >> 3` the bits of each stream size |
| 3 | `w` | `s[0]` to `s[7]`, the size of each literal stream, `w` bits each |
| `3 + w` | `s[0] + ... + s[7]` | the 8 literal streams, one after the other |
| `3 + w + s[0] + ... + s[7]` | up to `len` | `B`, the bitstream of the sequences |

Here `t` is byte 2. The `w` bytes from byte 3 on are one number `S` of `8 * w` bits, little endian, and
`s[j] = (S >> (j * w)) & ((1 << w) - 1)`: 8 sizes of `w` bits fill exactly `w` bytes. *Why:* the largest
stream of most pages is below 128 bytes, so 7 bits each do; 2 bytes each made pages about 5 bytes
larger on average.

The coded literals are split into 8 **streams**: literal 0 goes to stream 0, literal 1 to stream 1, ...,
literal 8 to stream 0 again. Stream `j` starts at byte `start[j] = 3 + w + s[0] + ... + s[j-1]`, so
`start[0] = 3 + w`. *Why:* in one stream each literal's code can only be found once the one before is
decoded, a chain of table lookups; 8 streams are 8 chains, which a CPU works on side by side.

A page is invalid if

- `len < 2`, or `n > PAGE`;
- with raw literals `2 + n > len`;
- with coded literals `len < 3`, `w < 1`, `w > 16`, `len < 3 + w`, or `3 + w + s[0] + ... + s[7] > len`.

## Prefix codes

A **prefix code** gives each symbol a bit string, its **code**, and no code is the start of another
one. That way a decoder can read bit by bit and knows when a code is complete. Frequent symbols get
short codes, rare ones long codes. E.g. with the three symbols A, B, C and the codes `0`, `10`, `11`,
the bits `0 11 10` are A C B.

A table here holds only the **length** of each symbol's code, 0 for a symbol without a code. The codes
themselves follow from the lengths, as in deflate (RFC 1951, section 3.2.2): shorter codes first, and
among codes of the same length the symbols in the order of their number. As a rule:

1. `count[l]` is the number of symbols with a code of length `l`; `count[0]` is taken as 0.
2. `first[1] = 0`, and for `l >= 2`: `first[l] = (first[l-1] + count[l-1]) * 2`.
3. The symbols with a code of length `l`, in the order of their number, get the numbers `first[l]`,
   `first[l] + 1`, ...
4. A symbol's code is its number written in binary with exactly `l` digits, leading zeros included;
   the most significant digit is the code's first bit.

E.g. the lengths A 1, B 2, C 2 give A the number 0, B 2 and C 3, and so the codes `0`, `10`, `11` from
above.

Every table is **complete**: its codes use up all bit strings, `2^-l` summed over all codes is exactly 1.
Then every long enough string of bits starts with exactly one code, and a decoder never meets bits that
are no code.

## The tables

The tables are part of the format: they are fixed, and a page cannot say that it uses others. A new
set of tables would be a new format, with a new name in zram. Each table is a list of code lengths, one
byte per symbol, symbol 0 first:

| table | symbols | longest code | 4 KiB pages | 16 KiB pages |
| --- | --- | --- | --- | --- |
| tokens and escape, `TOK` | 3073 | 11 bits | `explore/seqlz_default_tables_4k.inc`, `.token` | `explore/seqlz_default_tables_16k.inc`, `.token` |
| literal length values, `LL` | `LEN_SYMBOLS` | 8 bits | the same, `.ll` | the same, `.ll` |
| match length values, `ML` | `LEN_SYMBOLS` | 8 bits | the same, `.ml` | the same, `.ml` |
| literal tables 0 to 7 | 256 each | 10 bits | `explore/seqlz_lit_sets.c`, `seqlz_lit_sets[0]` to `[7]` | the same |

`LL` and `ML` are not the literal tables: they code the lengths `ll` and `ml` of a sequence when they
are too large for a token. The literal tables code the literals themselves.

The SHA-256 of each table's lengths, as bytes in symbol order (the literal tables one after the other),
so that a decoder can check that it has the right ones; `tools/seqlz_ref.py` checks them:

| table | SHA-256 |
| --- | --- |
| `TOK_4k` | `6d4fa91ee2e24196043146a2955e1214e2ce2e981385716c95519ec2fbce2909` |
| `LL_4k` | `43dcf4540e61456a6c085a877b0a681ae71d09cfd89fc1ef2763c18c06d30f58` |
| `ML_4k` | `c14ec602093ba8d570e30c5f98300f7046726a73e64a3fcdda0ba038a0c397fc` |
| `TOK_16k` | `c0456a5a34f29618709e4cd6efba0e6db0926cc4c4948f4ae8df8b2b44f07613` |
| `LL_16k` | `e0dde5e597bce7ad74677227800d41de02c1a3b167786564dbf7cf8296ecc4cc` |
| `ML_16k` | `e487aeb101066058f2794a507d4c7bc48962dedcbfcfda92e4e621a196cc8bcc` |
| `LIT` | `8fac7c712644c8d48f19141f8982306489e0d740bdf60e56a4d9351a7e66cc3b` |

How the tables were trained is in [docs/explored-designs.md](docs/explored-designs.md#the-format-written-down-one-set-of-tables-and-stream-sizes-that-hold).

## The bitstream of the sequences

`B` is all bytes from its start up to `len`, and `|B|` is their number. It is read one bit at a time,
starting with bit 0, the least significant bit, of its first byte: bit `i` of `B` is bit `i mod 8` of
byte `i div 8`. A decoder reads three kinds of things from it:

- `read(n)`: the next `n` bits as a number, the first bit read is the least significant one. `read(0)`
  is 0.
- `symbol(table)`: the next bits, one by one, until they are a code of the table; the first bit read
  is the code's first bit. The result is the symbol of that code.
- `value(table)`: a length value, see below.

A valid page needs at most `8 * |B|` bits of `B`. The bits and bytes after the last one it needs are
ignored and can have any value. A decoder may read zeros past the end instead of checking at every
read, and check at the end that it did not use them; `seqlz_decode()` does that, and the result is the
same as checking at every read.

### Length values

A length value is a number from 0 up to `2^(P+1) - 1`. Small values are a symbol of their own, larger
ones a symbol for a range of values plus bits that say which one in the range:

    s = symbol(table)
    if s < 16:  value = s
    else:       b = s - 12;  value = 2^b + read(b)

The symbols 16 to `12 + P` stand for the ranges `[2^b, 2^(b+1))` with `b = s - 12`, i.e. 16 to 31, 32 to
63, and so on, and `read(b)` says how far into the range the value is. E.g. 100 is in `[64, 128)`, so
`b = 6`, the symbol is 18, and `read(6)` gives `100 - 64 = 36`.

### Tokens

Each sequence starts with a **token**, one symbol of `TOK` that holds three numbers at once, so that
the most frequent combinations take few bits:

    ll  = token & 15         the literal length, 0 to 15; 15 means 15 or more
    mlf = (token >> 4) & 31  the match length minus 4, 0 to 31; 31 means 31 or more
    c   = token >> 9         the class of the offset, 0 to 5

That is 16 * 32 * 6 = 3072 tokens. A match is at least 4 bytes, so the token stores `ml - 4`. *Why:*
one code for the three numbers lets frequent combinations share a short code, and shorter matches
cost more bits than the literals they replace.

Symbol 3072 of `TOK` is the **escape**: it is followed by the token's number in 12 bits. Any token can
be sent escaped, also one that has a code of its own; it means the same. *Why:* there are only 2048
codes of 11 bits, fewer than the 3072 tokens, and most tokens are rare, so only the frequent ones have
a code.
Reading a token:

    t = symbol(TOK)
    if t == 3072:
        t = read(12)
        if t >= 3072: the page is invalid

### Offsets

The offset's class says how it is stored: each class reads its **offset bits** and turns them into
the offset. *Why:* small offsets are frequent, and so are multiples of 8, at which 8-byte values like
pointers repeat, so they get classes with fewer bits.

| class | offset bits | offset | offsets it can send |
| --- | --- | --- | --- |
| 0 | none | the last offset again | the offset of the sequence before |
| 1 | 4 | `read(4)` | 1 to 15 |
| 2 | 8 | `read(8)` | 1 to 255 |
| 3 | `P`: 12 or 14 | `read(P)` | 1 to `PAGE - 1` |
| 4 | 5 | `read(5) * 8` | 8 to 248, multiples of 8 |
| 5 | `P - 3`: 9 or 11 | `read(P - 3) * 8` | 8 to `PAGE - 8`, multiples of 8 |

Classes 4 and 5 store the offset divided by 8, which saves 3 bits. The last offset starts as 1 for
each page. For a sequence with a match, an offset of 0, or one larger than the bytes written so far,
makes the page invalid.

## Decoding

`lits` are the `n` literals from the layout (with coded literals: decoded as in
[Coded literals](#coded-literals)), `used` is the number of them already copied, `out` is the page
being written, `last` is 1.

    loop:
        token: ll, mlf, c
        x = read(offset bits of class c)
        off = (c == 0) ? last : x as the offset of class c
        if ll == 15: ll = 15 + value(LL)
        if ll > n - used or ll > PAGE - |out|: invalid
        append lits[used] to lits[used + ll - 1] to out; used = used + ll
        if |out| == PAGE: stop                      the last sequence
        ml = mlf + 4
        if mlf == 31: ml = 35 + value(ML)
        last = off
        if off == 0 or off > |out| or ml > PAGE - |out|: invalid
        ml times: append the byte off places before the end of out

    at the stop: used == n and at most 8 * |B| bits of B used, else invalid

The match is copied one byte at a time, each byte after the one before is written, so it can repeat
bytes it writes itself: with `off = 1` and `ml = 10` it repeats the last byte 10 times. A block copy
like `memcpy` gives other bytes when `off < ml`.

There is no count of the sequences. The page ends with the sequence whose literals fill it, and that is
the only way it ends: a page whose last match reaches the end of the page has one more sequence with
`ll = 0`. The last sequence's token and offset bits are read like any other, but its `mlf` and its
offset are not used and not checked. Every sequence before it writes at least 4 bytes, so a decoder
stops after at most `PAGE / 4 + 1` sequences, also on pages that are not valid.

Two things follow from the rules: a valid page has `n >= 1`, because the first match needs a byte
before it, and the first match's offset is at most that sequence's `ll`.

## Coded literals

Each literal stream is read most significant bit first: bit `i` of stream `j` is bit `7 - (i mod 8)` of
the page's byte `start[j] + i div 8`. Stream `j` holds literal `j`, `j + 8`, `j + 16`, ... below `n`,
each as one symbol of literal table `set`, one after the other; a code's first bit is the first one
read.

The codes of stream `j`'s literals must fit into its `s[j]` bytes, that is take at most `8 * s[j]` bits,
else the page is invalid. A stream may be longer than its codes: the bits and whole bytes after its
last code are ignored and can have any value, and a stream without literals can have any size. A
decoder may read past the end of a stream, as long as it checks this.

## Example

The 9 bytes `02 00 61 62 47 96 7b fb 19` are a 4 KiB page of `ab` 2048 times, with the 4 KiB tables:

- `02 00`: `h = 2`, raw literals, `n = 2`; then the literals `61 62`, `a` and `b`.
- `B` is `47 96 7b fb 19`, 40 bits, read least significant bit first:

| bits | read as | means |
| --- | --- | --- |
| `11100010011` | `symbol(TOK)` = 1010 | `ll = 2`, `mlf = 31`, class 1 |
| `0100` | `read(4)` = 2 | `off = 2` |
| | | `a b` appended |
| `111011` | `symbol(ML)` = 23, `b = 11` | |
| `11011011111` | `read(11)` = 2011 | `ml = 35 + 2^11 + 2011 = 4094`, `ab` repeated |
| `10011000` | `symbol(TOK)` = 0 | `ll = 0`: the page is full, the end |

## What the compressor writes

Not part of the format: any page that decodes is fine, and one page can be written in many ways, as
with lz4 or zstd. For reference, `seqlz_compress()` writes:

- the matches of a greedy matcher (`explore/page_lz.h`); each offset with class 0 when it is the last
  one, else class 4 or 5 for a multiple of 8 from 16 on, else the smallest of classes 1 to 3 that holds
  it;
- the escape only for a token without a code;
- for the last sequence `mlf = 0` and class 0, which needs no offset bits;
- zero bits to fill the last byte of the bitstream and of each literal stream, and nothing after the
  bitstream;
- coded literals only when `19 + s[0] + ... + s[7] < n - n div 16`, with the literal table that codes
  them in the fewest bits, and the smallest `w` that holds every `s[j]`. The 19 is not the size of the
  header; with it, a page has to save enough to be worth decoding its literals.

The compressed page is at most `2 * PAGE` bytes, which is the buffer zram gives the compressor. zram
stores a page that does not compress well enough as it is, so that page never reaches the decoder.

## Open points

1. **The last sequence's unused fields.** Its `mlf` and offset can be anything (see
   [Decoding](#decoding)). Requiring `mlf = 0` and class 0, as the compressor writes them, would cost one
   compare per page and make one more kind of damaged page invalid.
2. **No version in the page.** A new format would be a new algorithm name in zram, which stores the name
   per device.

## How this file was checked

`tools/seqlz_ref.py` decodes bit by bit, from this file alone, and agreed with `seqlz_decode()` on
every input it was given, valid and damaged pages of both page sizes; the numbers are in
[docs/explored-designs.md](docs/explored-designs.md#the-format-written-down-one-set-of-tables-and-stream-sizes-that-hold).
