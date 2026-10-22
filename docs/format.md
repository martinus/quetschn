# The seqlz format

<!-- not in the kernel's copy -->
seqlz compresses one memory page into one compressed page. This file describes the bytes, so that a
decoder can be written from it alone, and it says which compressed pages are valid. It describes the
format as `src/` writes and reads it. Sentences marked *Why:* explain a choice and are
not part of the format; the measurements behind them are in
[explored-designs.md](explored-designs.md). How seqlz works, for a reader new to compression, is
in the book [seqlz, line by line](https://martin.ankerl.com/quetschn/), the result and the numbers in
[seqlz.md](seqlz.md).

`seqlz-1`, `seqlz-2` and `seqlz-3`, the levels of `seqlz_compress()`, are the same format: level 1
always stores the literals as they are, level 2 codes them with one of 8 fixed tables where that pays,
level 3 also with a table of the page's own, and stores them as deltas to earlier bytes of the page
first where that pays. A decoder for one decodes all of them.

`tools/seqlz-ref/seqlz_ref.py` and `tools/seqlz-ref/seqlz_ref.c` are decoders written from this
file, slow on purpose, and checked against `seqlz_decode()`, see
[How this file was checked](#how-this-file-was-checked).

The text between the comments "not in the kernel's copy" and "end" is left out of the kernel's copy of
this file, which `tools/kernel-port/port.py` writes as reStructuredText; the comments "in the kernel's
copy" have text only for that copy.
<!-- end -->
<!-- in the kernel's copy:
seqlz compresses one memory page into one compressed page, for zram. This document describes the
bytes, so that a decoder can be written from it alone, and it says which compressed pages are valid.
Sentences marked *Why:* explain a choice and are not part of the format.

At level 1 the compressor stores the literals as they are, at level 2 it codes them with one of 8 fixed
tables where that pays, at level 3 also with a table of the page's own, and stores them as deltas to
earlier bytes of the page first where that pays. It is one format, and the
decoder decodes all of them.
-->

<!-- not in the kernel's copy -->
## Status

> [!IMPORTANT]
> This is the format of 21st October 2026 for 4 KiB pages, and nobody uses it yet. Until seqlz has users
> it may still change, the tables included, and such a change updates this file: its rules, the hashes
> of the tables and the example. Once seqlz is in a kernel, zram's pages still live only as long as the
> kernel that wrote them, so a change needs no new name in zram, see
> [No version in the page](#no-version-in-the-page).

The format for 16 KiB pages is not fixed yet. Its tables, the literal tables included, are its own,
trained on a zram dump of the Android 17 emulator with 16 KiB pages
([explored-designs.md](explored-designs.md#android-17-in-the-emulator-the-4-kib-tables-fit-the-16-kib-ones-trained-again-26-smaller)).
That is one emulator, no real phone with 16 KiB pages yet. And with 16 KiB pages `seqlz-2`
asks for 32 784 bytes of work memory per CPU, twice `lz4`'s
([seqlz.md](seqlz.md#what-is-not-known-yet)). So it is not in the first series for the kernel:
`tools/kernel-port/` builds seqlz for 4 KiB pages only.
<!-- end -->

## The idea

Like lz4 and zstd, seqlz describes a page as a list of **sequences**. Each sequence says: copy the next
`ll` **literals** (bytes stored in the compressed page as they are) to the output, then copy a **match**
of `ml` bytes that starts `off` bytes back in the output, its **offset**. The last sequence has literals
only, the ones left, and no bits: they fill the rest of the page, and that is how a decoder knows it is
the last. The numbers `ll`, `ml` and `off` are stored with **prefix codes** (Huffman
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
| `LEN_SYMBOLS` | 24 | 26 | symbols of a length value, `12 + P`, see [Length values](#length-values) |

The compressed page is a byte string of length `len`. Its length is not stored in it either: zram
stores it and gives it to the decoder.
<!-- in the kernel's copy:

Linux has seqlz for 4 KiB pages only. The format for 16 KiB pages is not fixed yet.
-->

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
| 2 | 1 | `t`: `code = t & 15` is how the literals are coded, `w = 5 + ((t >> 4) & 7)` the bits of each stream size, `e = t >> 7` whether they are deltas |
| 3 | `e` | with `e = 1` the byte `dt`, which delta the literals are, see [Literals as deltas](#literals-as-deltas), else nothing |
| `3 + e` | `w` | `s[0]` to `s[7]`, the size of each literal stream, `w` bits each |
| `3 + w + e` | `d` | with `code = 8` the page's own literal table, see [A page's own literal table](#a-pages-own-literal-table), else nothing, `d = 0` |
| `3 + w + e + d` | `s[0] + ... + s[7]` | the 8 literal streams, one after the other |
| `3 + w + e + d + s[0] + ... + s[7]` | up to `len` | `B`, the bitstream of the sequences |

`code` 0 to 7 is one of the 8 fixed literal tables, `code` 8 a table of the page's own, and 9 to 15
are invalid.

The `w` bytes from byte `3 + e` on are one number `S` of `8 * w` bits, little endian, and
`s[j] = (S >> (j * w)) & ((1 << w) - 1)`: 8 sizes of `w` bits fill exactly `w` bytes; `w` is 5 to 12.
*Why:* a stream holds at most every 8th literal, of at most 10 bits, so at most 640 bytes in a 4 KiB
page and 2560 in a 16 KiB page; the largest stream of most pages is below 128 bytes, so 7 bits each
do, and 2 bytes each made pages about 5 bytes larger on average. The codes 9 to 15 are kept for later.

The coded literals are split into 8 **streams**: literal 0 goes to stream 0, literal 1 to stream 1, ...,
literal 8 to stream 0 again. Stream `j` starts at byte `start[j] = 3 + w + e + d + s[0] + ... + s[j-1]`,
so `start[0] = 3 + w + e + d`. *Why:* in one stream each literal's code can only be found once the one before is
decoded, a chain of table lookups; 8 streams are 8 chains, which a CPU works on side by side.

A page is invalid if

- `len < 2`, or `n > PAGE`;
- with raw literals `2 + n > len`;
- with coded literals `len < 3 + w + e`, `dt` is not 1, 2 or 3, `code > 8`, the page's own table is
  invalid, or `3 + w + e + d + s[0] + ... + s[7] > len`.

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
set of tables would be a new format, see [No version in the page](#no-version-in-the-page). Each table
is a list of code lengths, one byte per symbol, symbol 0 first:

<!-- in the kernel's copy:
| table | symbols | longest code | in `lib/seqlz/seqlz_tables.c` |
| --- | --- | --- | --- |
| tokens and escape, `TOK` | 3073 | 11 bits | `seqlz_default_own.token` |
| literal length values, `LL` | `LEN_SYMBOLS` | 8 bits | `seqlz_default_own.ll` |
| match length values, `ML` | `LEN_SYMBOLS` | 8 bits | `seqlz_default_own.ml` |
| literal tables 0 to 7 | 256 each | 10 bits | `seqlz_lit_sets[0]` to `[7]` |
| a page's own literal table, `OWN` | 19 | 8 bits | `seqlz_own_lengths` |
-->
<!-- not in the kernel's copy -->
| table | symbols | longest code | 4 KiB pages | 16 KiB pages |
| --- | --- | --- | --- | --- |
| tokens and escape, `TOK` | 3073 | 11 bits | `src/seqlz_default_tables_4k.inc`, `.token` | `src/seqlz_default_tables_16k.inc`, `.token` |
| literal length values, `LL` | `LEN_SYMBOLS` | 8 bits | the same, `.ll` | the same, `.ll` |
| match length values, `ML` | `LEN_SYMBOLS` | 8 bits | the same, `.ml` | the same, `.ml` |
| literal tables 0 to 7 | 256 each | 10 bits | `src/seqlz_lit_sets_4k.inc`, `seqlz_lit_sets[0]` to `[7]` | `src/seqlz_lit_sets_16k.inc`, the same |
| a page's own literal table, `OWN` | 19 | 8 bits | the same, `seqlz_own_lengths` | the same, `seqlz_own_lengths` |
<!-- end -->

`LL` and `ML` are not the literal tables: they code the lengths `ll` and `ml` of a sequence when they
are too large for a token. The literal tables code the literals themselves. `OWN` codes the description
of a page's own literal table.

### Where the tables come from

<!-- in the kernel's copy:
Every table is built from counts that are in the kernel, `lib/seqlz/seqlz_counts.txt`, by
`scripts/gen-seqlz-tables.py`, which writes `lib/seqlz/seqlz_tables.c`. The counts say how often each
token, `ll` and `ml` symbol occurred on the training pages, for each literal table, how often each
byte occurred as a literal on the pages that table codes best, and how often each symbol of `OWN`
occurred in the pages' own tables. The pages are 1 091 983 memory pages of a desktop and a phone. They are not published, because memory pages hold keys and passwords. The counts
can't give those back: they are sums over all pages, each literal table's over 10 590 pages or more, and
they don't say which symbol came after which or on which page.
-->
<!-- not in the kernel's copy -->
Every table is built from counts that are in this repository:
[`bench/seqlz_counts_4k.txt`](../bench/seqlz_counts_4k.txt) and
[`bench/seqlz_counts_16k.txt`](../bench/seqlz_counts_16k.txt). They have how often each token, `ll` and
`ml` symbol occurred on the training pages, for each literal table, how often each byte occurred as
a literal on the pages that table codes best, and how often each symbol of `OWN` occurred in the
descriptions of the pages that level 3 gives a table of their own. `tools/kernel-port/gen-seqlz-tables.py` turns the counts
into the tables in `src/`, and `tools/seqlz-ref/check.sh` checks in CI that they are its output, byte for
byte. `quetschn-seqlz-train --from-counts <file>` does the same in C++, see `bench/seqlz_train.cpp`, and
the test `seqlz_train` checks that it gives the same tables, for both page sizes. The kernel gets the
generator as `scripts/gen-seqlz-tables.py` and the 4 KiB counts.

The 4 KiB tables are counted on 1 091 983 pages: three sessions of a Xiaomi Mi 9T under memory
pressure, the resident pages of a desktop and 200 000 pages of its zram dump of 28th September 2026. The
16 KiB tables are counted on 26 933 pages of a zram dump of the
Android 17 emulator with 16 KiB pages. Those pages are not published, because memory dumps hold keys
and passwords. The counts can't give those back: they are sums over all pages, each literal table's over
10 590 pages or more for 4 KiB and 453 or more for 16 KiB, and they don't say which symbol came after which
or on which page. `quetschn-seqlz-train --corpus <pages> --counts <file>` counts other pages the same
way.
<!-- end -->

The SHA-256 of each table's lengths, as bytes in symbol order (the literal tables one after the other),
so that a decoder can check that it has the right ones:

| table | SHA-256 |
| --- | --- |
| `TOK_4k` | `c5f97d70cd50ed0e3517c69231437fd534f5e772bb9ed70dc3769a3337b2d6e0` |
| `LL_4k` | `ce0b70bdefdad678f22ef1139cf8284c48b61f720adb007b0a28b0ad3777bf16` |
| `ML_4k` | `c39358b16a14c2462e092ee13b345e9b5002e59c5efc9521f0967063a7873289` |
| `LIT_4k` | `43f552dca80dcdffd6dc21231555f1c938580650a1b90b94b8c515120ad9ad9e` |
| `OWN_4k` | `96312d8701c3fa72a06db3dcdb982aab0075aaac91bc895cf1eed3ae88e00721` |
| `TOK_16k` | `e33e3a2353fbc5c9296abb79034a3d118ce34344e059099e1c85afda996b7219` |
| `LL_16k` | `5e9e225ea55c28a9b0f358101521c670e89cbd32cec5e7f02fe608648dabb8b2` |
| `ML_16k` | `fe6a87649f4b95830e663270eefe4289561269420cba951931f95c5fe38ccfce` |
| `LIT_16k` | `cc37c3ba7e7ffdb91ff873d7c9a19f04301efe79c4b2fd7202c137212a769db4` |
| `OWN_16k` | `53a7c56537de2052791c0d016ff3c92467ef61bfb7be0ef33ad2b81f3bc58906` |

<!-- not in the kernel's copy -->
`tools/seqlz-ref/seqlz_ref.py` checks them. How the tables were trained is in
[explored-designs.md](explored-designs.md#the-tables-trained-again-4-less-on-one-desktop-dump-the-phone-the-same).
<!-- end -->
<!-- in the kernel's copy:
The KUnit tests of seqlz check the 4 KiB ones.
-->

## The bitstream of the sequences

`B` is all bytes from its start up to `len`, and `|B|` is their number. It is read one bit at a time,
starting with bit 7, the most significant bit, of its first byte: bit `i` of `B` is bit `7 - (i mod 8)`
of byte `i div 8`, as in the literal streams. *Why:* a decoder looks up the next code by the top bits
of its register, and in a table indexed by them each code's entries are one range, so a page needs
fewer of the table's cache lines; read the other way round, the entries of a code are spread over the
whole table. A decoder reads three kinds of things from it:

- `read(n)`: the next `n` bits as a number, the first bit read is the most significant one. `read(0)`
  is 0.
- `symbol(table)`: the next bits, one by one, until they are a code of the table; the first bit read
  is the code's first bit. The result is the symbol of that code.
- `value(table)`: a length value, see below.

A valid page ends where its bits end: after the last bit it needs, fewer than 8 bits of `B` are left,
all in its last byte, and they are 0. *Why:* zram stores no checksum, so this is what tells a damaged
page from a valid one, at one compare per page, and the compressor writes nothing else. A decoder may
read zeros past the end instead of checking at every read, and check at the end that it did not use
them; `seqlz_decode()` does that, and the result is the same as checking at every read.

### Length values

A length value is a number from 0 up to `2^P - 1`. Small values are a symbol of their own, larger
ones a symbol for a range of values plus bits that say which one in the range:

```text
s = symbol(table)
if s < 16:  value = s
else:       b = s - 12;  value = 2^b + read(b)
```

The symbols 16 to `11 + P` stand for the ranges `[2^b, 2^(b+1))` with `b = s - 12`, i.e. 16 to 31, 32 to
63, and so on, and `read(b)` says how far into the range the value is. E.g. 100 is in `[64, 128)`, so
`b = 6`, the symbol is 18, and `read(6)` gives `100 - 64 = 36`. *Why `2^P - 1`:* the largest value a
page needs is `ll - 15` of `PAGE - 4` literals before a page's only match, `PAGE - 19`, so a symbol for `[2^P, 2^(P+1))`
would never be sent.

### Tokens

Each sequence but the last starts with a **token**, one symbol of `TOK` that holds three numbers at
once, so that the most frequent combinations take few bits:

```text
ll  = token & 15         the literal length, 0 to 15; 15 means 15 or more
mlf = (token >> 4) & 31  the match length minus 4, 0 to 31; 31 means 31 or more
c   = token >> 9         the class of the offset, 0 to 5
```

That is 16 * 32 * 6 = 3072 tokens. A match is at least 4 bytes, so the token stores `ml - 4`, the
**match length field** `mlf`. *Why:* one code for the three numbers lets frequent combinations share a
short code, and shorter matches cost more bits than the literals they replace.

Symbol 3072 of `TOK` is the **escape**: it is followed by the token's number in 12 bits. Any token can
be sent escaped, also one that has a code of its own; it means the same. *Why:* there are only 2048
codes of 11 bits, fewer than the 3072 tokens, and most tokens are rare, so only the frequent ones have
a code.

Reading a token:

```text
t = symbol(TOK)
if t == 3072:
    t = read(12)
    if t >= 3072: the page is invalid
```

### Offsets

The offset's class says how it is stored: each class reads its **offset bits** and turns them into
the offset. *Why:* small offsets are frequent, and so are multiples of 8, at which 8-byte values like
pointers repeat, so they get classes with fewer bits. Offset 2, at which 16-bit values repeat, is
the most frequent of all and gets a class of its own.

| class | offset bits | offset | offsets it can send |
| --- | --- | --- | --- |
| 0 | none | the last offset again | the offset of the sequence before |
| 1 | 1 | `read(1) * 2` | 2 |
| 2 | 8 | `read(8)` | 1 to 255 |
| 3 | `P`: 12 or 14 | `read(P)` | 1 to `PAGE - 1` |
| 4 | 5 | `read(5) * 8` | 8 to 248, multiples of 8 |
| 5 | 8 | `read(8) * 8` | 8 to 2040, multiples of 8 |

Class 1 stores the offset divided by 2, classes 4 and 5 divided by 8. The last offset starts as 1
for each page. For a sequence with a match, an offset of 0, or one larger than the bytes written so far,
makes the page invalid.

## Decoding

`lits` are the `n` literals from the layout (with coded literals: decoded as in
[Coded literals](#coded-literals)), `used` is the number of them already copied, `out` is the page
being written, `last` is 1. `copy(k)` appends the next `k` literals, `lits[used]` to
`lits[used + k - 1]`, to `out`, one after the other, with raw literals and with `e = 0` as they are,
with `e = 1` as [Literals as deltas](#literals-as-deltas) says, and adds `k` to `used`.

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

The match is copied one byte at a time, each byte after the one before is written, so it can repeat
bytes it writes itself: with `off = 1` and `ml = 10` it repeats the last byte 10 times. A block copy
like `memcpy` gives other bytes when `off < ml`.

There is no count of the sequences, and the last sequence has no token. Every match is at least 4
bytes, so once the literals left are as many as the bytes the page still needs, no match can follow:
the literals left are the last sequence. A page whose last match reaches the end of the page ends with
no literals left, a page of literals only has `n = PAGE` and no bits in `B`. *Why:* a decoder knows
`n` and `PAGE`, so a token for the last sequence would only say what the decoder knows already, in
about 1.2 bytes per page. Every sequence before the last writes at least 4 bytes, so a decoder stops
after at most `PAGE / 4 + 1` sequences, also on pages that are not valid.

Two things follow from the rules: a valid page has `n >= 1`, because the first match needs a byte
before it, and the first match's offset is at most that sequence's `ll`.

## Coded literals

Each literal stream is read most significant bit first: bit `i` of stream `j` is bit `7 - (i mod 8)` of
the page's byte `start[j] + i div 8`. Stream `j` holds literal `j`, `j + 8`, `j + 16`, ... below `n`,
each as one symbol of the literal table that `code` names, one after the other; a code's first bit is
the first one read.

The codes of stream `j`'s literals must end in its last byte: they take at most `8 * s[j]` bits and
fewer than 8 less, and the bits after the last code are 0. A stream without literals has `s[j] = 0`.
Else the page is invalid. A decoder may read past the end of a stream, as long as it checks this.

## Literals as deltas

With `e = 1` each literal was stored as its delta to an earlier byte of the page, the literal minus that
byte modulo 256, before it was coded, and `copy()` adds the byte back: the `i`-th literal of a sequence,
from 0, which goes to the page's byte `p = |out|`, is `(lits[used + i] + out[p - x]) mod 256` with

- `dt = 1`: `x = 4`, for `i >= 4`; the literals with `i < 4` are as they are;
- `dt = 2`: `x = 8`, for `i >= 8`; the literals with `i < 8` are as they are;
- `dt = 3`: `x = last`, the offset of the sequence before, 1 for the first; with `last` 1 or 2, `x = 4`,
  with `last = 3`, `x = 6`; for `p >= x`, the literals with `p < x` are as they are.

`out[p - x]` is a byte already written, a literal of the same sequence included, as it is in the page.
The last sequence is no different. *Why:* memory pages are full of 4- and 8-byte fields, whose bytes are
often those of the field before or differ from them by a little, as counters, sizes and addresses of
objects in a row do, and a literal after a match often differs only a little from the byte the match
would have copied. As deltas, such literals are many zeros and few other values, which the literal
tables code in fewer bits. With `last` below 4, a multiple of it from 4 on refers to the same repeated
bytes, and a decoder can add 4 bytes at a time.

## A page's own literal table

With `code = 8` the literals are coded with a table that the page brings. It starts at byte `3 + w + e`,
right after `dt` and the sizes, and is read most significant bit first, as a literal stream: bit `i`
of it is bit `7 - (i mod 8)` of byte `3 + w + e + i div 8`. It is a list of symbols of the table `OWN`, which say
the code length of each byte value with a code, in the order of the byte values, from 0 on:

- symbol `l - 1`, for `l` from 1 to 10: the next byte value has a code of `l` bits;
- symbol `9 + k`, for `k` from 1 to 9, then `x = read(k - 1)`: the next `2^(k-1) + x` byte values
  have no code. `k = 1` is one byte value, `read(0)` is 0.

The list ends as soon as the codes fill the code space: when `2^-l`, summed over the lengths so far,
is exactly 1. Byte values after the last one with a code have none. The table's last byte is the one
with its last bit, the bits after that one in it are 0, and `d` is its number of bytes. The codes
follow from the lengths as in [Prefix codes](#prefix-codes), the byte value being the symbol.

The page is invalid if the sum goes above 1, a byte value with a code is above 255, a bit after the
last symbol in its byte is not 0, or `3 + w + e + d > len`. *Why:* the description is in the order of the
byte values, so a byte value takes no bits of its own, and the long lengths, which most bytes of a
page's own table have, get the shortest codes in `OWN`. A table takes 0.4 to 0.5 bytes per byte value
with a code, 40 to 100 bytes on most pages that have one.

## Example

The 9 bytes `02 00 61 62 23 f2 ff fb 60` are a 4 KiB page of `ab` 2048 times, with the 4 KiB tables:

- `02 00`: `h = 2`, raw literals, `n = 2`; then the literals `61 62`, `a` and `b`.
- `B` is `23 f2 ff fb 60`, 40 bits, read most significant bit first:

| bits | read as | means |
| --- | --- | --- |
| `0010` | `symbol(TOK)` = 3072 | the escape: the token has no code of its own |
| `001111110010` | `read(12)` = 1010 | `ll = 2`, `mlf = 31`, class 1 |
| `1` | `read(1)` = 1 | `off = 1 * 2 = 2` |
| | | `a b` appended |
| `1111111` | `symbol(ML)` = 23, `b = 11` | |
| `11111011011` | `read(11)` = 2011 | `ml = 35 + 2^11 + 2011 = 4094`, `ab` repeated |
| | | 0 literals left for 0 bytes of the page: the last sequence, the end |
| `00000` | | the rest of the last byte |

## What the compressor writes

Not part of the format: any page that decodes is fine, and one page can be written in many ways, as
with lz4 or zstd. For reference, `seqlz_compress()` writes:

- at levels 1 and 2 the matches of a greedy matcher (`page_lz.h`); level 3 takes a match shorter than
  20 bytes only where the next position has no longer one, and extends each match backwards over
  the literals before it while their bytes are the same;
- each offset with class 0 when it is the last one, else the class with the fewest offset bits that
  holds it;
- the escape only for a token without a code;
- at levels 2 and 3 coded literals only when `51 + d + s[0] + ... + s[7] < n - n div 16`, with the
  literal table that codes them in the fewest bits, and the smallest `w` that holds every `s[j]`, at
  least 5. The 51 is not the size of the header; with it, a page has to save enough to be worth decoding
  its literals;
- at level 3, for pages with 512 literals or more, a table of the page's own where that and its
  description take at least 16 bytes less than the best fixed table. Its code lengths are close to a
  Huffman code's, at most 10 bits;
- at level 3, literals as deltas, with the `dt` whose deltas take the fewest bits in the best fixed
  table, 8 bits added for `dt`, counted for every 5th literal of the page, where that is not `e = 0`
  and the deltas get coded; else the literals as they are.

The compressed page fits the buffer the compressor is given, or the compressor fails. zram gives it two
pages, which every page fits, and stores a page that does not compress well enough as it is, so that
page never reaches the decoder.

## No version in the page

zram keeps its pages only while the kernel that wrote them runs: they are decompressed by the same
code, also the ones it wrote back to a backing device, and no page meets a newer decoder. So a change of
the format needs a new kernel, not a new algorithm name in zram, which would be visible to users in
`comp_algorithm` and in the Kconfig default. Two things would change that: another user of the codec
that keeps pages longer, or zram's pages kept across a kexec by live update (`kernel/liveupdate/`),
which zram does not do in 2026. The values of byte 2 with a code above 8, and the values of `dt` other
than 1 to 3, are invalid now and free for a format that has to tell itself apart.

<!-- not in the kernel's copy -->
## How this file was checked

`tools/seqlz-ref/seqlz_ref.py` decodes bit by bit, from this file alone, and agreed with `seqlz_decode()` on
every input it was given, valid and damaged pages of both page sizes; the numbers are in
[explored-designs.md](explored-designs.md#the-format-written-down-one-set-of-tables-and-stream-sizes-that-hold).

`tools/seqlz-ref/seqlz_ref.c` is the same in C, also from this file alone, fast enough to be fuzzed:
`fuzz/seqlz_diff_fuzz.c` gives any input to both decoders and fails when they disagree, whether a page
is valid or what it decodes to. `fuzz/smoke.sh` runs it for a minute in CI, `fuzz/afl.sh` for longer:
on 6 October, 10 AFL++ instances for 2 hours, 1.7 billion inputs, found no difference, and on 9 October,
with the encoder and decoder of `5342a9b`, 12 instances for 2 hours, 1.94 billion inputs on this target,
found none either.
`test/seqlz_ref_test.cpp` checks the example above, the compressor's pages at every level, damaged
pages, and pages made by hand for the rules that damage rarely reaches: the codes above 8 and the values of `dt`
other than 1 to 3, a literal code outside its stream, a page of literals only, a last sequence with a token, which
is invalid, and pages' own tables that overfill the code space, name a byte value above 255 or have
bits after their end.
<!-- end -->
