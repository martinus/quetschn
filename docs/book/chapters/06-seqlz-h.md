# seqlz.h: the format in constants {#ch-header}

`src/seqlz.h` is the header every file of the codec includes, and the one the tests, the benchmarks
and the zram glue include to call it. It holds three kinds of things. First, the format as numbers:
how many bits a token code may have, how many offset classes there are, where the fields of byte 2
sit. [Chapter 4](#ch-format) describes the format in words; this header is the same format as
`#define`s, and most of this chapter is working out what each one is and why it has its value.
Second, three small functions that both the encoder and other code need, which turn a sequence into
the numbers the format stores: `seqlz_len_symbol()`, `seqlz_off_class()` and `seqlz_token()`. Third,
the interface: the structs and the functions that build the tables, compress a page and decompress
one, each with its contract.

Nothing in this header depends on how the tables are laid out in memory; that is
`seqlz_internal.h`, [chapter 7](#ch-internal). A file that only calls the codec never needs to know.

## The top of the file: what a page is {#ch-header--summary}

@listing src/seqlz.h 1-5

The licence line is the same as in every file of `src/`, see [chapter 5](#ch-compat). The include
guard is `_LINUX_SEQLZ_H`, the kernel's convention for a header in `include/linux/`. The kernel's copy
has two headers: `include/linux/seqlz.h`, the interface zram calls, which `port.py` writes from
`tools/kernel-port/seqlz.h`, and `lib/seqlz/seqlz.h`, made from this file, which gets the guard
`_LIB_SEQLZ_SEQLZ_H`. Line 5 brings in the kernel's names, `u8`, `min()`, `static_assert()` and the
rest, through `seqlz_compat.h`; `port.py` replaces it with `asm/page.h`, `linux/minmax.h` and
`linux/types.h`.

The long comment that follows is a summary of the format, short enough to keep in your head while
reading the code. We go through it in three parts.

@listing src/seqlz.h 7-27

Lines 8 to 14 are LZ compression in one paragraph, see [chapter 2](#ch-compression): a page becomes a
list of sequences, each with some **literals**, bytes copied as they are, and a **match**, bytes copied
from earlier in the same page. The difference to `lz4` is in how the numbers of a sequence are stored:
`lz4` writes them in whole bytes, seqlz with Huffman codes from **fixed tables**. The tables are
trained once on real memory pages and compiled in. Lines 13 and 14 state the trade: pages get smaller, and
decoding the codes costs time.

Lines 16 to 19 name the three numbers of a sequence, `ll`, `ml` and the offset. `ml` is at least 4,
because a shorter match costs about as many bits as its bytes as literals, so the matcher does not
look for one; the last sequence of a page has no match, which the code writes as `ml` 0. "1 to the
page size" for the offset is a little generous: a match needs at least one byte before it, so the
largest offset is 4095 on a 4 KiB page, which is what class 3's 12 bits hold.

Lines 21 to 26 introduce the **token**: `ll`, `ml - 4` and the offset class in one number, so that the
decoder gets all three with one table lookup. The formula in line 24 is the one `seqlz_token()` below
computes. 16 values of `ll` times 32 of `ml - 4` times 6 classes are 3072 tokens. Line 25 announces the
**escape**: only the frequent tokens get a code of their own, a rare one is sent as the escape's code
followed by its number in plain bits.

@listing src/seqlz.h 28-58

Lines 28 to 36 list the six offset classes, the same table as in
[the format chapter](#ch-format--offsets). Class 0 is the **repeat offset**, the offset of the
sequence before, and costs no bits at all. Class 1 is one offset only, 2, sent divided by 2 in 1 bit.
Classes 2 and 3 are offsets by size, in 8 and 12 bits. Classes 4 and 5 are for multiples of 8: they
send the offset divided by 8, below 256 in 5 bits, 3 less than class 2, and below 2048 in 8 bits, 4
less than class 3. Lines 35 and 36 give the reasons. Memory pages are full of 8-byte aligned data,
pointers and 8-byte fields, so the same value often repeats 8, 16 or 24 bytes later. On the zram pages
of a desktop, 64% of the offsets from 16 to 255 that are not the repeat offset are multiples of 8, and
52% of those from 256 on
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)).
And an array of 16-bit values repeats 2 bytes later: of the offsets that are not the repeat offset,
about 8% are 2 on phone and desktop pages, more than any other single offset
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-2-in-1-bit-multiples-of-8-below-2048-in-8-03-to-08-smaller-at-the-same-speed-kept)).

Lines 38 to 41 describe the **length values**. A token holds `ll` up to 14 and `ml - 4` up to 30 as
they are; 15 and 31 mean "this or more, the rest follows". The rest, `ll - 15` or `ml - 4 - 31`, is a
length value: below 16 it is a symbol of its own, above it is sent as the position of its highest bit
plus the bits below. `bit_width(v)` is the number of bits `v` needs, 7 for 100, so 100 becomes symbol
$12 + 7 - 1 = 18$ and 6 plain bits. `seqlz_len_symbol()` below does exactly this.

Lines 43 to 48: everything except the literals goes into one **bitstream**, most significant bit
first, in the order token, offset bits, `ll` value, `ml` value. There is no count of the sequences, and
the last one has no bits: its literals are the ones left, and a decoder knows it has come when they are
as many as the bytes the page still needs. Every match has at least 4 bytes, so none can follow then.

Lines 50 to 54 are the bytes of a page: a `u16`, little endian, with the number of literals, then all
literals, then the bitstream. If the top bit of the `u16` is set, the literals are Huffman coded with
one of 8 fixed literal tables, and a few more header bytes follow; we get to those at
`SEQLZ_LIT_HEADER`.

Lines 56 and 57 point to the specification, `docs/format.md`, and to the log of measurements,
`docs/explored-designs.md`. Several comments below quote headings of that log in double quotes, and
this chapter links each. In the kernel's copy this sentence becomes "Documentation/staging/seqlz.rst
specifies the format.": `port.py` turns `docs/format.md` into that file, and references to the
repository are not allowed in the kernel.

@listing src/seqlz.h 60-69

This comment says why the code uses four of the compiler's builtins where the kernel has helpers of
its own. `__builtin_ctzll()` counts the zero bits below the lowest set bit of a 64-bit number, and
`__builtin_clz()` the zero bits above the highest set bit of a 32-bit one. The kernel has `__ffs64()`,
`__fls()` and `fls()` for the same, but on x86-64 they are inline assembly, and the compiler cannot
look into assembly: it cannot fold a constant through it or merge it with the code around it. With the
kernel's `__ffs()` and `fls()`, gcc wrote `decode_literals()` and `code_literals()` "in another shape"
in a kernel build, as commit `e814b1c` puts it; commit `61299db` wrote this comment to say so once.

`__builtin_memcpy()` is used for the fixed copies of 8 and 16 bytes, as `lib/lz4` and `lib/zstd` do
with `LZ4_memcpy()` and `ZSTD_memcpy()`. A kernel built with `CONFIG_FORTIFY_SOURCE` replaces
`memcpy()` with a version that checks sizes, and not every compiler inlines that version. On the Mi
9T's 4.14 kernel, built with clang, a module of the codec had 63 calls to `memcpy()`, one per copy of 8
or 16 bytes in the matcher, the encoder and the decoder, where the module before had 1 (commit
`48a7043`). `__builtin_memcpy()` is always inlined.

`__builtin_bswap64()` reverses the 8 bytes of a number. The literal coder uses it once per flush of a
stream, in `lit_flush()` ([chapter 11](#ch-literals-enc)), to turn its bits around in a register
before a little-endian store.

## The page size and the token {#ch-header--token}

@listing src/seqlz.h 71-74

`QUETSCHN_PAGE_BITS` is the one setting of the codec: 12 for 4 KiB pages, 14 for 16 KiB. CMake passes
`-DQUETSCHN_PAGE_BITS=14` for the 16 KiB build; without it, it is 12. `SEQLZ_PAGE` is the page size in
bytes, `1U << 12 = 4096`. The `U` makes it `unsigned int`, so that every expression with it is unsigned
and comparisons with other unsigned values do not mix signs.

In the kernel's copy `port.py` uses the kernel's `PAGE_SHIFT` instead, and adds an `#error` if it is
not 12: Linux on x86-64 and on most phones has 4 KiB pages, and the kernel's copy is built for those
only. The format for 16 KiB pages is not fixed yet
([`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md#status)).

@listing src/seqlz.h 75-86

These two numbers set the size of the decode tables, and the size of a decode table is what the
decoder pays for on a cold read.

A decoder of a prefix code does not read bit by bit. It takes the next $L$ bits, where $L$ is the length
of the longest code, and looks them up in a table of $2^L$ entries; a code of $l$ bits fills all
$2^{L-l}$ entries that start with its bits ([chapter 2](#ch-compression--table-decoding)). So the
longest code decides the table's size: 8 bits for length values give 256 entries, 4 bytes each, 1 KiB;
11 bits for tokens give 2048 entries, 4 bytes each, 8 KiB.
@assert ch06.MAX_BITS == 8 and ch06.TOKEN_BITS == 11

"A table twice as large made cold reads slower": the codes of length values were once allowed 9 bits,
and the comment that preceded this one said "With 9 bits a cold read took 80 ns longer"
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow),
the row "length values with codes of at most 8 bits"). A **cold** read is one where the compressed page
and the tables are not in the CPU's caches, as for a page swapped out a while ago; every cache line of
a table the page touches is then a wait for memory.

The token table is the larger trade. With 10 bits instead of 11 it would be 4 KiB, but fewer tokens fit
a code of their own and more take the longer escape, so pages get larger. On the Mi 9T this was measured
twice ([item 1 and item 7](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#six-choices-made-on-the-pc-measured-on-the-phone-the-token-table-and-the-prefetches-matter-the-rest-does-not)
of "Six choices made on the PC, measured on the phone"). Pages were 4.8 bytes larger on the phone
pages it was timed on, and 3.5 bytes on average over desktop and phone pages; 320 tokens got a code
of their own instead of 512.
In item 1, 10 bits read cold 2.0 to 2.7 µs faster on the A76, the big out-of-order core, and 2.3 to 3.2
µs on the A55, the little in-order core: the decoder prefetched 64 lines of the token table less. Item 7
measured again after the decoder had stopped prefetching the token table on the A76. Then 10 bits gave
the A76 nothing, its warm reads were 0.1 to 0.3 µs slower, and the A55 read cold 2.0 to 2.6 µs faster,
0.8 to 1.1 µs less time per page written. On the PC's hot loop, 10 bits decoded 2.4% slower, 7595
against 7418 cycles per page. 11 bits stayed.

Since then today's `seqlz_decode()` prefetches the token table on every core again
([chapter 14](#ch-decode)), so 10 bits would mean 64 lines less to load on every core. That is why it
was measured once more, on 11th October 2026, and why the comment changed
([`docs/explored-designs.md`, "The token table in 10 bits again"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-token-table-in-10-bits-again-the-a55s-cold-reads-13-µs-faster-x86-64-2-to-4-slower-not-kept)).
With 10 bits the pages were 3.5 bytes larger on average. On the Mi 9T's A55 the cold
reads of coded pages were 1.3 µs faster, about 0.4 µs less per page written; on the A76 the cold reads
were between two copies of the same code, and the warm reads 0.16 µs slower, as more tokens take the
escape and a second lookup. In the kernel VM on x86-64 the swap-ins were 2% to 4% slower. The gain of
the earlier items on the A76 is gone: since the bit order changed, a code's entries are next to each
other and a page touches fewer lines of the table anyway ([chapter 3](#ch-cpu--msb-first)). So 10 bits
pays only on the little in-order core, costs bytes on every page and time on the others, and the
comment says that in a line.

@listing src/seqlz.h 87-96

The token holds `ll` in 4 bits and `ml - 4` in 5 bits. Larger values are capped, and a length value
follows; a length value costs a second table lookup, in another table, so the fewer sequences need one,
the better.

With 5 bits for `ml - 4`, a match length value follows 8.5% of the matches; with 4 bits it was 18.7%.
The memory stayed the same, and decoding went from 9400 to 9180 cycles per page on the PC
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes),
"Rarer escapes: a token of 512 symbols"; then the token had no offset class yet).

Lines 91 to 93 say why the two numbers cannot grow: the decoder's table entry for a token has 4 bits
for `ll` and 6 bits for `ml`, see `token_entry()` in [chapter 7](#ch-internal). 4 bits hold `ll` up to
15, so `SEQLZ_LL_BITS` can be at most 4. The entry stores `ml` itself, the cap plus 4: with 5 bits that
is at most $31 + 4 = 35$, which fits 6 bits; with 6 bits it would be $63 + 4 = 67$, which does not.
`seqlz_internal.h` checks both with a `static_assert`.

@listing src/seqlz.h 97-103

The six classes take the top of the token, above the 9 bits of the two lengths.
`SEQLZ_TOKEN_SYMBOLS` is `6 << (4 + 5)`, $6 \cdot 512 = {{const.TOKEN_SYMBOLS}}$. A shift, not a multiplication, because the
class is a bit field: token `t` has `ll` in bits 0 to 3, `ml - 4` in bits 4 to 8 and the class in bits 9
to 11. [Chapter 4](#ch-format--token) draws the 12 bits of token 2098, the sequence that
[chapter 7](#ch-internal--examples)
takes apart: 2 literals, a match of 7 bytes, 16 bytes back. 16 is a multiple of 8 below 256, class 4,
sent as $16 / 8 = 2$ in 5 bits.

Why these 6 classes and not more: the comment names "Offset 2 in 1 bit", the search that found
them. `tools/seqlz-bound/space.py` priced layouts of 6 to 8 classes, each a number of bits and a
divisor of 1, 2, 4 or 8, on 20 000 pages of each of seven dumps, with the token codes of each dump
trained on the other six. Against the classes before 14th October 2026, today's 6 save 5.15 bytes per
page of tokens and offset bits, the mean of the seven dumps, and the best 7 save 6.24, 1.1 bytes more.
More comes from offsets that need no bits at all, e.g. 2 and 8 as classes of their own: with them, 8
classes save 8.46 bytes. But then the decoder chooses from three for each offset, its bits, a fixed one or
the repeat offset, and with one more select the Mi 9T's A55 wrote 1.0% and read warm pages 1.7%
slower, its A76 read warm pages 2.2% slower. The search before,
["Offset classes from a histogram"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-classes-from-a-histogram-todays-are-close-two-more-give-01-points-at-most),
had found 0.9 bytes per page for 6 classes; this one finds 4.65 with classes from 2 bits on, and 5.15
with the class of 1 bit for offset 2
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-2-in-1-bit-multiples-of-8-below-2048-in-8-03-to-08-smaller-at-the-same-speed-kept)).

## The escape and the caps {#ch-header--escape}

@listing src/seqlz.h 104-123

`SEQLZ_ESCAPE` is symbol {{ch06.ESCAPE}}, the first number after the last token. The token table therefore has
{{tok.symbols}} symbols, and `struct seqlz_lengths` below has `SEQLZ_TOKEN_SYMBOLS + 1` code lengths.

The reason for the escape is arithmetic. A prefix code with codes of at most 11 bits has room for at most
2048 codes: each code of $l$ bits uses $2^{-l}$ of the code space, and the sum may not exceed 1. 3072
tokens would need at least $\frac{3072}{2048} = 1.5$ times the space. So some tokens get no code. With the 4
KiB tables, {{ch04.tok_coded}} of the 3072 tokens have a code of their own, with the 16 KiB tables {{k16.ch04.tok_coded}}; all others are
sent as the escape's code plus the token's number in 12 plain bits. The escape is itself frequent, as a
whole, and has a {{ch06.esc_len}}-bit code in both tables.
@assert ch06.esc_len == k16.ch06.esc_len

Even when it was possible, giving every token a code was worse. In "seqlz, third decoder round" the
token had 1536 symbols, and with 11 bits each of them needs at least $\frac{1}{2048}$ of the code space, 75% of
it together. Pages took 27.5% of their size instead of the 26.6% the frequencies promised; with the
escape and the same 11-bit table it was 26.8%
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).

`SEQLZ_ESCAPE_BITS` is 12, the fewest bits that hold 3071, the largest token: $2^{11} = 2048$ is too
few, $2^{12} = 4096$ enough. The other 1024 values of 12 bits, 3072 to 4095, are not tokens, and the
decoder rejects a page that sends one.

`SEQLZ_MAX_ESCAPE_LEN` is the longest code the escape may have, and it comes from the encoder, not the
decoder. The encoder checks that a compressed page fits into zram's two pages with an argument that
needs at most 31 bits for a token and its offset ([chapter 10](#ch-encoder)). The longest such pair is
an escaped token with a class 3 offset: the escape's code, 12 bits of token, and `QUETSCHN_PAGE_BITS`
bits of offset. So the escape may have at most $31 - 12 - 12 = {{ch06.MAX_ESCAPE_LEN}}$ bits on 4 KiB pages and
$31 - 14 - 12 = {{k16.ch06.MAX_ESCAPE_LEN}}$ bits on 16 KiB pages. The trained tables give it {{ch06.esc_len}} bits, so the longest token and
offset take ${{ch06.esc_len}} + 12 + 12 = {{ch06.esc_pair}}$ bits on 4 KiB pages. `has_all_symbols()` in `seqlz_codes.c` checks
the limit when the tables are built ([chapter 8](#ch-codes)). Every other token code is at most 11
bits, and $11 + 12 = 23$ on 4 KiB pages, 25 on 16 KiB pages, is far below 31.
@assert ch06.esc_len <= ch06.MAX_ESCAPE_LEN and k16.ch06.esc_len <= k16.ch06.MAX_ESCAPE_LEN and ch04.longest.tok == 11

`SEQLZ_LL_CAP` and `SEQLZ_ML_CAP` are the largest values the two fields hold, $2^4 - 1 = 15$ and
$2^5 - 1 = 31$. `(1U << n) - 1U` is the usual way to write "n ones", which is also the mask for the
field. They have two meanings at once: the cap and the flag. A token with `ll` field 15 says "15 or
more, a length value follows", and the length value is `ll - 15`.

## Length values {#ch-header--length-values}

@listing src/seqlz.h 124-136

A length value is the rest of `ll` or `ml` beyond the cap. Most are small, a few are large: a page
can have a run of 2000 literals, or a match of 4000 zero bytes. The code has to handle both without a
table of 4096 symbols.

`SEQLZ_LEN_DIRECT` says that values 0 to 15 are symbols 0 to 15. A value of 16 or more is sent as
the position of its highest set bit, $b$, plus the $b$ bits below it. The symbol for that is
`SEQLZ_LEN_LOG_BASE + b`. $b$ is at least 4 for a value of at least 16 ($16 = 2^4$), so the first such
symbol is $12 + 4 = 16$, right after the direct ones. That is why the base is
`SEQLZ_LEN_DIRECT - 4U = 12`: the two kinds of symbols meet without a gap and without overlap.

`SEQLZ_LEN_SYMBOLS` is how many symbols there are: 16 direct ones, then one for each $b$ from 4 up to
`QUETSCHN_PAGE_BITS - 1`, which is $12 + 12 = {{ch06.LEN_SYMBOLS}}$ on 4 KiB pages and $12 + 14 = {{k16.ch06.LEN_SYMBOLS}}$ on 16 KiB pages.
Each symbol covers a range of values twice as wide as the one before:

| symbol | `b` | values | plain bits after the code |
| --- | --- | --- | --- |
| 0 to 15 | | 0 to 15 | 0 |
| 16 | 4 | 16 to 31 | 4 |
| 17 | 5 | 32 to 63 | 5 |
| 18 | 6 | 64 to 127 | 6 |
| ... | | | |
| {{ch06.last_sym}} | 11 | 2048 to 4095 | 11 |
@assert ch06.last_sym == 23

The largest length value a 4 KiB page can produce is `ll - 15` for 4092 literals before a match of 4
bytes, {{ch06.max_ll_value}}, or `ml - 35` for a match of 4095 bytes after 1 literal, {{ch06.max_ml_value}}. Both are symbol {{ch06.max_ll_sym}}, the last one. The comment says why no
symbol for a higher bit is needed: the largest length value is below the page size, so its highest bit
is at most `QUETSCHN_PAGE_BITS - 1`, 11. On 16 KiB pages values reach {{k16.ch06.max_ll_value|grp}}, symbol {{k16.ch06.max_ll_sym}}, the last.
@assert ch06.max_ll_sym == ch06.max_ml_sym == ch06.last_sym and k16.ch06.max_ll_sym == k16.ch06.max_ml_sym == k16.ch06.last_sym

Until 11th October 2026 the symbols went up to the page's bits, one more: symbol 24 on 4 KiB pages,
4096 to 8191, which no valid page sent. The trained tables still gave it a code of 8 bits in both
tables, because `seqlz_all_symbols()` below asks for a code for every length value symbol, 1/256 of
each table's code space. It went when the tables were trained again (#182).

Sending a length value's symbol in 5 plain bits instead, with no table, was measured twice and made
pages larger without a faster read, the first time by 3.0 bytes per page; the second time
is in [chapter 4](#ch-format--length-values).

@listing src/seqlz.h 137-143

`SEQLZ_HEADER` is the 2 bytes of the `u16` at the start of every page. `SEQLZ_LIT_CODED` is its top
bit, bit 15. A page has at most 4096 literals on 4 KiB pages, `0x1000`, which needs 13 bits, and at most
16384 on 16 KiB pages, `0x4000`, which needs 15. Bit 15 is never part of the count, so it can be the
flag. The decoder reads the `u16`, tests `n & SEQLZ_LIT_CODED`, and takes the count as
`n & (SEQLZ_LIT_CODED - 1U)`, the low 15 bits ([chapter 14](#ch-decode)). In the kernel's copy `port.py`
rewrites the comment to "at most 4 KiB of literals".

## The tables as code lengths {#ch-header--lengths}

@listing src/seqlz.h 145-155

A table, in this format, is nothing but a list of code lengths. From the lengths alone the canonical
Huffman code follows: codes of the same length are consecutive numbers in the order of the symbols,
and each length starts where the one before ended, shifted one bit left
([chapter 2](#ch-compression--canonical-codes)). So `struct seqlz_lengths` is all the format needs to
fix for the sequences: one byte per symbol, 0 for a symbol without a code.

`token` has {{tok.symbols}} entries, the 3072 tokens and the escape last. `ll` and `ml` have {{ll.symbols}} each, so the struct
is ${{tok.symbols}} + {{ll.symbols}} + {{ml.symbols}} = {{size.lengths}}$ bytes on 4 KiB pages and {{k16.size.lengths}} on 16 KiB pages. It has only `u8` members, so
there is no padding.

`seqlz_default_own`, declared at the end of this file, is the one set the code compiles in
(`seqlz_default_tables.c`, which includes `seqlz_default_tables_4k.inc` or `_16k.inc`). Those numbers
are part of the format: a decoder with other lengths reads other codes. `docs/format.md` lists their
SHA-256, so that a decoder written from the spec can check that it has the right ones. The 8 literal
tables are not in this struct; they are `seqlz_lit_sets[][]`, further down.

@listing src/seqlz.h 157-173

`seqlz_len_symbol()` turns a length value `v` into its symbol, and stores in `*extra_bits` how many
plain bits follow the symbol's code. The encoder calls it in `put_len_value()` ([chapter 10](#ch-encoder))
and then writes the low `*extra_bits` bits of `v`.

For `v` below 16, line 166, the symbol is `v` and nothing follows. Otherwise line 170 finds the
position of the highest set bit. `__builtin_clz(v)` counts the zero bits above it in the 32-bit `v`,
and `31 - clz` is the position. The builtin is undefined for 0, but here `v` is at least 16. The cast to
`unsigned int` is there because the builtin returns `int`, and `31U - int` would otherwise mix signed
and unsigned in a way compilers warn about.

Worked examples, computed with this function:

| `v` | binary | `__builtin_clz(v)` | `b` | symbol | plain bits, value |
| --- | --- | --- | --- | --- | --- |
| 5 | `{{ch06.lensym.5.bin}}` | | | {{ch06.lensym.5.sym}} | none |
| 16 | `{{ch06.lensym.16.bin}}` | {{ch06.lensym.16.clz}} | {{ch06.lensym.16.extra}} | {{ch06.lensym.16.sym}} | {{ch06.lensym.16.extra}} bits, {{ch06.lensym.16.low}} |
| 31 | `{{ch06.lensym.31.bin}}` | {{ch06.lensym.31.clz}} | {{ch06.lensym.31.extra}} | {{ch06.lensym.31.sym}} | {{ch06.lensym.31.extra}} bits, {{ch06.lensym.31.low}} |
| 100 | `{{ch06.lensym.100.bin}}` | {{ch06.lensym.100.clz}} | {{ch06.lensym.100.extra}} | {{ch06.lensym.100.sym}} | {{ch06.lensym.100.extra}} bits, {{ch06.lensym.100.low}} |
| 4059 | `{{ch06.lensym.4059.bin}}` | {{ch06.lensym.4059.clz}} | {{ch06.lensym.4059.extra}} | {{ch06.lensym.4059.sym}} | {{ch06.lensym.4059.extra}} bits, {{ch06.lensym.4059.low}} |
@assert ch06.lensym[5].extra == 0 and ch04.ab.s[0].mlval == 4059 and ch04.ab.s[0].ml == 4094

For `v = 100`: the highest bit is bit 6, so the symbol is $12 + 6 = 18$, and the 6 plain bits are the
bits below it, $100 - 64 = 36$, `100100`. The decoder reverses it: symbol 18 means base $2^6 = 64$, and
`64 + read(6) = 64 + 36 = 100`. The highest bit is not sent, because the symbol already says where it
is. The last row is from the format's worked example ([chapter 4](#ch-format--example)): a match of 4094
bytes has `ml - 4 - 31 = 4059`.

gcc compiles the function with one branch, for `v < 16`, and `bsr` for the bit position. The encoder
calls it only for the few sequences with a length value, so the branch costs little.

## Offset classes without a branch {#ch-header--offsets}

@listing src/seqlz.h 175-192

Lines 175 and 176 name the width of class 5: `SEQLZ_CLASS5_BITS` is {{ch06.CLASS5_BITS}}, so class 5 holds
the multiples of 8 below `8 << {{ch06.CLASS5_BITS}}` = {{ch06.class5_end}}, sent divided by 8. It is {{k16.ch06.CLASS5_BITS}} on 16 KiB pages too.
There class 5 could reach further with more bits, but 10 bits, up to 8 KiB back, made the 16 KiB
pages no smaller on average
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-2-in-1-bit-multiples-of-8-below-2048-in-8-03-to-08-smaller-at-the-same-speed-kept)).
@assert ch06.CLASS5_BITS == k16.ch06.CLASS5_BITS == 8 and ch06.class5_end == 2048

The decoder and the encoder need, for each class, the number of offset bits that follow the token:
{{ch06.RAW_BITS}} on 4 KiB pages. The obvious code is a `switch` or a small array. This is a constant
with one nibble, 4 bits, per class, and a shift picks the nibble:

```text
class:        5     4     3     2     1     0
nibble:       8     5     C     8     1     0      SEQLZ_RAW_NIBBLES = 0x85C810 (4 KiB)
nibble:       8     5     E     8     1     0      SEQLZ_RAW_NIBBLES = 0x85E810 (16 KiB)
```
@assert ch06.RAW_NIBBLES == "0x85C810" and k16.ch06.RAW_NIBBLES == "0x85E810"

Lines 184 and 185 build it from its parts, so that the page size goes in where it belongs: class 1
gets `1U << 4`, class 3 gets `QUETSCHN_PAGE_BITS << 12`, 12 or 14 in the fourth nibble, and class 5
gets `SEQLZ_CLASS5_BITS << 20`, 8 in the sixth. `<<` binds more tightly than `|` in C, so the
expression needs no parentheses around each term. The result is computed by the compiler; gcc's code
for `seqlz_off_class()` loads `${{ch06.RAW_NIBBLES_dec}}`, which is `{{ch06.RAW_NIBBLES}}`.

`SEQLZ_RAW_BITS(cls)` shifts the constant right by `4 * cls` and keeps the low 4 bits. For class 3:
`0x85C810 >> 12 = 0x85C`, and `0x85C & 15 = 0xC = 12`. For class 1: `0x85C810 >> 4 = 0x85C81`, and
`0x85C81 & 15 = 1`. For class 5: `0x85C810 >> 20 = 0x8`, 8 bits.
The comment explains why a constant: when class 3's bits were computed from `QUETSCHN_PAGE_BITS` in
code, gcc emitted a branch on the class. A shift and a mask cannot become a branch. The kernel's copy
has the same expression, with `PAGE_SHIFT` for `QUETSCHN_PAGE_BITS`.

`SEQLZ_OFF_SHIFT(cls)` is how far the offset is shifted right before it is sent: 1 for class 1, which
sends offset 2 as 1, 3 for classes 4 and 5, which send it divided by 8, else 0. It is the same trick
as `SEQLZ_RAW_BITS()`: `SEQLZ_SHIFT_NIBBLES` puts 1 into the nibble of class 1 and 3 into those of
classes 4 and 5, which makes `0x330010`, read from the right: 0, 1, 0, 0, 3, 3. For class 1,
`0x330010 >> 4 = 0x33001`, and `0x33001 & 15 = 1`; for class 4, `0x330010 >> 16 = 0x33`, and
`0x33 & 15 = 3`. The token's entry has 2 bits for the shift, and `seqlz_internal.h` asserts that
every shift fits them. The encoder sends `off >> SEQLZ_OFF_SHIFT(cls)`, and the token table's entry has the shift for
the decoder ([chapter 7](#ch-internal)).
@assert ch06.OFF_SHIFT == k16.ch06.OFF_SHIFT == "0, 1, 0, 0, 3, 3"

@listing src/seqlz.h 194-215

`seqlz_off_class()` picks the class for an offset `off`, given `last`, the offset of the sequence
before, and stores the class's number of offset bits in `*raw_bits`. The encoder calls it once per
match, in `encode_emit()` ([chapter 10](#ch-encoder)). The classes of consecutive matches follow no
pattern that a branch predictor could learn, so a chain of `if`s would mispredict often; the code
computes the class with arithmetic instead. [Chapter 3](#ch-cpu--branches) has what a wrong guess
costs, and when code without a branch is slower.

Line 203, `is_new = (off == last) - 1U`. A comparison in C gives the `int` 0 or 1. Subtracting `1U`
converts it to `unsigned int`: `1 - 1 = 0` when the offset repeats, and `0 - 1` wraps around to
`0xFFFFFFFF` when it is new. Unsigned arithmetic wraps by definition in C, so this is well defined. The
result is a mask: all zero bits for the repeat offset, all one bits otherwise.

Lines 204 and 205, `aligned = ((off & 7U) == 0) & (off < 8U << SEQLZ_CLASS5_BITS)`. `off & 7U` is the
offset's low 3 bits, 0 for a multiple of 8. `<<` binds more tightly than `<`, so the right side is
`off < 2048`, the offsets class 5 holds. The two comparisons are joined with `&`, not `&&`: `&&` must
not evaluate its right side when the left is false, and compilers often keep that as a branch; `&` on
two values that are 0 or 1 is a plain AND. So `aligned` is 1 for 8, 16, ..., 2040, and 0 for 2048 and
every offset that is no multiple of 8.

Line 210 computes the class: start at 3, subtract 1 if the offset is below 256 (class 2), another 1 if
it is 2 (class 1), and add 2 for a multiple of 8 below 2048, which moves class 2 to 4 and class 3 to 5.
2 is no multiple of 8, so class 1 never gets the 2 added, and the smallest result is $3 - 1 - 1 = 1$:
the subtractions never wrap. Line 211 ANDs it with `is_new`, which leaves it unchanged for a new
offset and makes it 0 for the repeat offset.

The whole thing, for several offsets, with `last` different from `off`:

| `off` | `off < 256` | `off == 2` | `aligned` | class, `3 - a - b + 2 * aligned` | offset bits | value sent |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | {{ch06.off.1.lt256}} | {{ch06.off.1.eq2}} | {{ch06.off.1.aligned}} | {{ch06.off.1.sum}} | {{ch06.off.1.raw}} | {{ch06.off.1.sent}} |
| 2 | {{ch06.off.2.lt256}} | {{ch06.off.2.eq2}} | {{ch06.off.2.aligned}} | {{ch06.off.2.sum}} | {{ch06.off.2.raw}} | {{ch06.off.2.sent}} |
| 8 | {{ch06.off.8.lt256}} | {{ch06.off.8.eq2}} | {{ch06.off.8.aligned}} | {{ch06.off.8.sum}} | {{ch06.off.8.raw}} | {{ch06.off.8.sent}} |
| 15 | {{ch06.off.15.lt256}} | {{ch06.off.15.eq2}} | {{ch06.off.15.aligned}} | {{ch06.off.15.sum}} | {{ch06.off.15.raw}} | {{ch06.off.15.sent}} |
| 24 | {{ch06.off.24.lt256}} | {{ch06.off.24.eq2}} | {{ch06.off.24.aligned}} | {{ch06.off.24.sum}} | {{ch06.off.24.raw}} | {{ch06.off.24.sent}} |
| 255 | {{ch06.off.255.lt256}} | {{ch06.off.255.eq2}} | {{ch06.off.255.aligned}} | {{ch06.off.255.sum}} | {{ch06.off.255.raw}} | {{ch06.off.255.sent}} |
| 256 | {{ch06.off.256.lt256}} | {{ch06.off.256.eq2}} | {{ch06.off.256.aligned}} | {{ch06.off.256.sum}} | {{ch06.off.256.raw}} | {{ch06.off.256.sent}} |
| 264 | {{ch06.off.264.lt256}} | {{ch06.off.264.eq2}} | {{ch06.off.264.aligned}} | {{ch06.off.264.sum}} | {{ch06.off.264.raw}} | {{ch06.off.264.sent}} |
| 2040 | {{ch06.off.2040.lt256}} | {{ch06.off.2040.eq2}} | {{ch06.off.2040.aligned}} | {{ch06.off.2040.sum}} | {{ch06.off.2040.raw}} | {{ch06.off.2040.sent}} |
| 2048 | {{ch06.off.2048.lt256}} | {{ch06.off.2048.eq2}} | {{ch06.off.2048.aligned}} | {{ch06.off.2048.sum}} | {{ch06.off.2048.raw}} | {{ch06.off.2048.sent}} |
| 4095 | {{ch06.off.4095.lt256}} | {{ch06.off.4095.eq2}} | {{ch06.off.4095.aligned}} | {{ch06.off.4095.sum}} | {{ch06.off.4095.raw}} | {{ch06.off.4095.sent}} |

And with `off == last`, e.g. `off = last = 24`: `is_new` is 0, so the class is `4 & 0 = 0` and
`SEQLZ_RAW_BITS(0)` is 0: no offset bits at all. At the start of a page `last` is 1, so a first match
at offset 1, a run of one repeated byte, is class 0 too.

Offset 8 is class 4, in 5 bits; class 2 could send it too, in 8. Offset 2048 is a multiple of 8, but
class 5's 8 bits end at 2040, so it is class 3, in 12 bits. The value sent is
`off >> SEQLZ_OFF_SHIFT(cls)`, and the bits above `raw_bits` are always 0: 4095 fits 12 bits,
$2040 / 8 = 255$ fits 8, and $2 / 2 = 1$ fits 1.
@assert ch06.off[8].sum == 4 and ch06.off[2048].sum == 3 and ch06.off[2].sum == 1 and ch06.off[2040].sent == 255

gcc 16.2.1 at `-O2` for x86-64 compiles the function without a jump: the comparisons become `setcc`
instructions, the sum an `add`, a `sub` and a `lea`, the AND a conditional move, and the bits a shift
of the constant. gcc turns `3U - (off < 256U)` into `2 + (off > 255)`, which is the same number. Line
212 is `SEQLZ_RAW_BITS` from above.

@listing src/seqlz.h 217-222

`struct seqlz_sequence` is a sequence as numbers: how many literals, how long the match, how far back.
It is the interface between a matcher and an encoder for code outside the compressor: `seqlz_find()`
fills an array of them, `seqlz_encode()` writes a page from one. Each field is a `u16`, enough for
lengths and offsets up to 16384 on 16 KiB pages, so a sequence takes 6 bytes. The compressor itself
does not use the struct: its matcher hands each sequence to the encoder as three arguments, as soon as
it is found ([chapter 9](#ch-matcher)). The kernel's copy leaves the struct out, along with every
function that uses it.

## The tables, behind a pointer {#ch-header--tables}

@listing src/seqlz.h 224-228

`struct seqlz_tables;` declares the struct without defining it. Code that includes `seqlz.h` can hold a
pointer to it and pass it on, but cannot look inside or know its size: an **opaque** type. The
definition is in `seqlz_internal.h`, which only the codec's own files include ([chapter 7](#ch-internal)).
It holds the codes for the encoder and the decode tables for the decoder, {{size.tables|grp}} bytes, the same on both
page sizes.
@assert size.tables == k16.size.tables

"Read only once built, one for all CPUs": the tables are built once from the code lengths and then
only read. Every CPU that compresses or decompresses uses the same copy. In the kernel's copy they
are one global struct, built once at boot or when the module loads, and marked `__ro_after_init`, so the
kernel makes that memory read-only after initialization ([chapter 15](#ch-zram-glue--init)). They are not part of
the per-CPU work memory.

The comments that start with `/**` below are **kernel-doc**, the kernel's format for documenting a
function, which a script checks against the prototype; [chapter 15](#ch-zram-glue--interface) has
the format.

@listing src/seqlz.h 230-235

`seqlz_tables_size()` is how a caller that cannot see the struct allocates one: it asks for the size.
The benchmark's zram glue does exactly that, `quetschn_zalloc(seqlz_tables_size(), ...)`
([chapter 15](#ch-zram-glue)). The function is one line, `return sizeof(struct seqlz_tables);`, in
`seqlz_codes.c`. The kernel's copy does not have it; there the tables are a global variable, and
nobody has to allocate them.

@listing src/seqlz.h 237-247

The encoder and the decoder need different things from a table. The decoder only needs the table to
be a complete prefix code: then every pattern of bits starts with exactly one code, and a lookup always
finds something ([chapter 2](#ch-compression--table-decoding)). A complete code can leave symbols out,
with length 0; a page that would need them simply cannot be written with these tables, and a page that
claims to use them does not decode.

The encoder needs more: a code for every symbol it may have to write, since it cannot refuse a page
because one of its tokens is rare. For tokens, either a code or the escape is enough. For length values
and literals, every symbol needs a code. `seqlz_all_symbols()` returns what `seqlz_tables_init()`
found when it checked that, stored in a field of the struct. `seqlz_encode()` and
`seqlz_compress()` return 0 when it is false.

In the kernel's copy this function, the field and the check do not exist. Since commit `4c42cc5`, the
script that writes the kernel's tables checks the code lengths before it writes them, so tables that
lack a code never get into the kernel, and building them at boot cannot fail.

@listing src/seqlz.h 249-258

`seqlz_tables_init()` builds the codes and the decode tables from a `struct seqlz_lengths`, into
memory of `seqlz_tables_size()` bytes. "e.g. seqlz_default_own": the codec itself always passes the
compiled-in set; the tests also pass made-up lengths, to check that bad ones are refused.

It returns `-EINVAL` when the lengths are not a complete prefix code, and the comment lists the four
ways that happens:

- a code longer than its table allows, e.g. a token code of 12 bits, which an 11-bit table cannot hold;
- more codes than fit, the sum of $2^{-l}$ over all codes above 1, so that two codes would share entries;
- gaps, the sum below 1, so that some patterns of bits are no code;
- no code at all.

`-EINVAL` is the kernel's way to report an error: a negative error number, 0 for success. The decoder
returns the same for a page that is not valid. `seqlz_codes.c` has the code ([chapter 8](#ch-codes)).
The kernel's copy has the same function, but `static` and `__init`, code the kernel frees after boot
([chapter 15](#ch-zram-glue--init)), and its kernel-doc moves to the
definition: `port.py` does that for the three functions of this header that are `static` in the copy,
this one, `seqlz_decode()` and `seqlz_compress()`.

## The literal tables and their header {#ch-header--literals}

@listing src/seqlz.h 260-269

The literals of a page are either stored as they are, or Huffman coded with one of 8 fixed **literal
tables**, each a code for the 256 byte values. Both numbers here were measured.

`SEQLZ_LIT_BITS` is 10: no literal code is longer than 10 bits, so a literal table's decode table has
1024 entries of 2 bytes, 2 KiB. With 9 bits it would be 1 KiB, half the cache lines on a cold read. On
the Mi 9T that was no faster on either core, and pages got 20 bytes larger on average, most of it on
desktop pages whose literals most likely need codes longer than 9 bits
([item 2 of "Six choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#2-the-literal-tables-in-9-bits-no-faster-up-to-64-bytes-larger-not-kept)).

`SEQLZ_LIT_SETS` is 8: the encoder picks, for each page, the table that codes its literals in the fewest
bits, and says which in 3 bits of the header. The tables were trained by clustering pages whose literals
look alike. 16 tables saved a few bytes per page for slower cold reads and were not taken; the numbers
are in [chapter 8](#ch-codes--lit-sets-file).

@listing src/seqlz.h 270-283

The coded literals are dealt out to 8 **streams** like cards: literal 0 to stream 0, literal 1 to
stream 1, ..., literal 8 to stream 0 again. The reason is the decoder. In one stream, the decoder only
knows where literal $k + 1$ starts after it has looked up literal $k$ and knows its code length: each
lookup waits for the one before, a chain. 8 streams are 8 independent chains, and a CPU that runs
several instructions at once works on them side by side ([chapter 13](#ch-literals-dec)).

4 streams would make pages a few bytes smaller, with 4 sizes fewer in the header, and were measured
and not taken, see [chapter 13](#ch-literals-dec--cost). That there are 8 tables and 8 streams is a coincidence; all 8 streams of a page use the page's one table.

Line 278 declares the code lengths of the 8 literal tables, 256 bytes each, defined in
`seqlz_lit_sets.c` from `seqlz_lit_sets_4k.inc` or `_16k.inc`. They are part of the format, like
`seqlz_default_own`.

`SEQLZ_LIT_ROUNDS` is `56 / 10 = 5`. The decoder refills a stream by loading 8 bytes at once and then
decodes several literals from it before the next load. 8 bytes are 64 bits. Up to 7 bits of the first
byte may already be used, and the decoder keeps one bit as a marker of where the valid bits end
([chapter 13](#ch-literals-dec)), so at least $64 - 7 - 1 = 56$ bits are left. 5 codes of at most 10
bits need at most 50 of them; 6 could need 60. So each refill is good for 5 literals per stream, 40 for
all 8.

@listing src/seqlz.h 284-304

A page with coded literals needs a little more header: which table, and where each stream starts. The
layout, after the `u16` with `SEQLZ_LIT_CODED` set:

```text
byte 2:      bit 7  bit 6 | bit 5  bit 4  bit 3 | bit 2  bit 1  bit 0
               0      0   |      w - 5          |    literal table
bytes 3 ...: the 8 stream sizes in bytes, w bits each, lowest bit first, w bytes together
```

`SEQLZ_LIT_HEADER(w)` is the header's length: 2 bytes of `u16`, 1 byte for byte 2, and `w` bytes for
the sizes, since 8 sizes of `w` bits are exactly `8w` bits, `w` bytes. With `w` from 5 to 12 the header
is 8 to 15 bytes. `SEQLZ_LIT_HEADER(0)` is 3, the offset of the first size byte; the decoder and the
encoder use it that way.

`w` is chosen per page: the fewest bits that hold the largest of the 8 sizes, at least 5. A page whose
largest stream has 100 bytes takes 7 bits per size, $100 < 128 = 2^7$, and a header of $3 + 7 = 10$
bytes. A stream holds every 8th literal, at most 10 bits each, so at most $4096 / 8 \cdot 10 / 8 = 640$
bytes on 4 KiB pages, which needs 10 bits, and 2560 on 16 KiB pages, which needs 12. That is where
`SEQLZ_SIZE_BITS_MAX` comes from.

The width is stored as `w - 5` in 3 bits, which holds exactly the 8 widths 5 to 12. Widths of 3 and 4
bits are left out, because only 1.8% of the pages with coded literals would use them, and they would
need a fourth bit. Until 5th October 2026 each size took 2 bytes, a header of 19 bytes; what `w` bits
saved is in [chapter 4](#ch-format--layout).

`SEQLZ_LIT_WIDTH_AT` and `SEQLZ_LIT_ZERO_AT` are where the fields of byte 2 start, bit 3 and bit 6. The
encoder writes `set | (w - 5) << 3`. The decoder reads the width as `(byte >> 3) & 7` and rejects the
page if `byte >> 6` is not 0: the two top bits are reserved, so that a future format could tell itself
apart ([format, "No version in the page"](https://github.com/martinus/quetschn/blob/main/docs/format.md#no-version-in-the-page)).
These two are written without `U`; they are small shift counts, and `seqlz_internal.h` checks with
`static_assert`s that the fields of byte 2 add up to $3 + 3$ bits.

@listing src/seqlz.h 305-314

The rule for coding the literals of a page: only where the 8 streams plus `SEQLZ_LIT_CODED_MIN` bytes
are smaller than $\frac{15}{16}$ of the raw literals,

```c
coded + 51 < n - n / 16      /* coded: the bytes of the 8 streams; n: the literals */
```

E.g. a page with 1000 literals: `1000 - 1000 / 16 = 1000 - 62 = 938`, so the streams must take at most
886 bytes, 11.4% less than the literals, for the page to get coded literals. With 100 literals the
streams must stay below $94 - 51 = 43$ bytes, less than half; few pages with so few literals qualify.
<!-- undated: arithmetic on the rule of SEQLZ_LIT_CODED_MIN, no measurement -->
@assert ch06.LIT_CODED_MIN == 51

Why $\frac{1}{16}$ and why 51 bytes, with the measurements on the phone, is in
[chapter 11](#ch-literals-enc--rule). In short: zsmalloc's size classes make a saving of a few bytes
worth little, and a page with coded literals costs a fixed time on every read.

The 51 bytes also cover the header: `SEQLZ_LIT_HEADER(12)` is 15 bytes, at most 13 more than the raw
page's 2. A `static_assert` in `seqlz_compress.c` checks that, because the encoder writes the coded
literals over the raw ones in place and relies on them ending before the bitstream starts
([chapter 11](#ch-literals-enc)).

@listing src/seqlz.h 315-321

The decoder decodes coded literals into a buffer first, and the loop over the sequences then reads them
from there as if they had been stored raw ([chapter 13](#ch-literals-dec)). That buffer is the
**scratch**, `SEQLZ_SCRATCH` bytes, one per CPU, part of the work memory. It is a page plus 16 bytes,
{{const.SCRATCH}} bytes on 4 KiB pages and {{k16.const.SCRATCH|grp}} on 16 KiB pages. The 16 bytes are for the decoder's fast path, which
copies literals 16 at a time and moves on by the real count ([chapter 14](#ch-decode)): when the last
literals are copied, those copies may read up to 16 bytes past the last literal. In the scratch those
bytes are there; for raw literals, the bytes after them in the compressed page are. The comment names
a second use too: in its last step `decode_literals()` lets every stream decode one more literal, also
the streams that have none left, and those land in up to 8 bytes after the last literal
([chapter 13](#ch-literals-dec)).

## Encoding and decoding a page {#ch-header--encode-decode}

@listing src/seqlz.h 323-348

`seqlz_encode()` writes a compressed page from sequences somebody else found. It exists "for tests and
tools, which make their own sequences": the tests, the fuzzer that feeds the encoder raw sequences, and
`tools/seqlz-worst/`, which writes valid pages from chosen sequences that the matcher would never find,
to search for the pages that are slowest to decode. zram uses `seqlz_compress()`, which
finds and writes the sequences in one pass.

The arguments: the tables, `n` sequences with the last one's `match` 0, the literals of all of them in
one array, the output buffer `dst` of `dst_cap` bytes, and `coded`, whether the literals may be Huffman
coded where the rule above says it pays.

Lines 337 to 340 say which sequences describe a page. Every sequence but the last has a match of at
least 4 bytes; its offset is between 1 and the number of bytes of the page before the match; and the
literal and match lengths add up to the page, using all `n_literals` literals. These are the rules the
decoder checks. The function checks them all before it writes anything, and returns 0 for sequences
that break one. It did not always: a match shorter than 4 bytes made `ml - 4` wrap around to a number
near $2^{32}$, its length value took more than 31 bits, and 1025 such sequences wrote past the two pages
of `dst`. The fuzzer `fuzz/seqlz_encode_fuzz.c`, which gives the encoder raw sequences, was added with
the fix (commit `256768e`).

The return value is the length of the compressed page, or 0. 0 is never a valid length, since every page
has at least its 2-byte header, so it can mean "failed". The kernel's copy leaves the function out;
only the harness and the tests use it.

@listing src/seqlz.h 350-365

`seqlz_decode()` is the decoder: the compressed page `src` of `src_len` bytes in, the page in `dst`
out. The tables must be the ones the page was written with; the page does not say which.

Lines 358 to 360 are its safety contract, and the most important lines of this header. Whatever
`src` holds, the decoder reads only inside `src_len` bytes from `src` and writes only inside `dst` and
`scratch`. zram stores no checksum, so a page damaged in memory, or a bug elsewhere, reaches the
decoder as it is, and a decoder that then wrote outside its buffer would turn a damaged page into a
damaged kernel. Every length and offset is checked before it is used ([chapter 14](#ch-decode)), and
fuzz targets feed the decoder random and damaged pages to keep it that way: 1.42 billion inputs, and
1.94 billion against a second decoder, in one run of 12 instances for 2 hours on 9th October 2026
([chapter 14](#ch-decode--safety)), 60 seconds per
target in CI for every change, and an hour a day with each of three sanitizers in ClusterFuzzLite, a
service that runs the fuzz targets for the repository ([chapter 15](#ch-zram-glue--fuzzing)).

`scratch` may be `NULL`. Then pages with coded literals are invalid, which is right for a zram device
at level 1, `seqlz-fast`: its compressor never codes literals, so its decoder needs no scratch, and the
device saves that memory per CPU ([chapter 15](#ch-zram-glue)).

It returns 0 for a page decoded completely, or `-EINVAL`. There is no partial result: a page is valid,
and `dst` holds it, or it is not.

@listing src/seqlz.h 367-376

`seqlz_token()` computes the token of a sequence, the formula of line 24. Line 371 caps `ll` at 15.
Line 372 caps `ml - 4` at 31. Only a sequence with a match has a token, so `ml` is at least 4 here;
for the last sequence's `ml` of 0, `0 - 4` in unsigned arithmetic would be `0xFFFFFFFC`, and `min()`
would make that 31. Until 11th October 2026 the last sequence had a token, and the line was `ml ?
min(ml - 4, SEQLZ_ML_CAP) : 0`. Line 374 packs the three fields with shifts: `a` in bits 0 to 3,
`b << 4` in bits 4 to 8, `cls << 9` in bits 9 to 11.

Worked examples, computed with this function:

| `ll` | `ml` | class | `a` | `b` | token |
| --- | --- | --- | --- | --- | --- |
| 2 | 7 | 4 | {{ch06.tok.0.a}} | {{ch06.tok.0.b}} | ${{ch06.tok.0.a}} + {{ch06.tok.0.b16}} + {{ch06.tok.0.c512}} = {{ch06.tok.0.tok}}$ |
| 1 | 7 | 0 | {{ch06.tok.1.a}} | {{ch06.tok.1.b}} | ${{ch06.tok.1.a}} + {{ch06.tok.1.b16}} + {{ch06.tok.1.c512}} = {{ch06.tok.1.tok}}$ |
| 2 | 4094 | 1 | {{ch06.tok.2.a}} | {{ch06.tok.2.b}} | ${{ch06.tok.2.a}} + {{ch06.tok.2.b16}} + {{ch06.tok.2.c512}} = {{ch06.tok.2.tok}}$ |
| 20 | 100 | 3 | {{ch06.tok.3.a}} | {{ch06.tok.3.b}} | ${{ch06.tok.3.a}} + {{ch06.tok.3.b16}} + {{ch06.tok.3.c512}} = {{ch06.tok.3.tok}}$ |
| 0 | 4 | 0 | {{ch06.tok.4.a}} | {{ch06.tok.4.b}} | {{ch06.tok.4.tok}} |

Token 49 is the sequence an array of pointers repeats in [chapter 9](#ch-matcher--back)'s example: one
literal, a match of 7 bytes, the repeat offset; it has a {{tok.len.49}}-bit code. Token 1010 is the first sequence of
the format's worked example, a page of `ab` repeated. Token 2047 has both fields at their caps, so both
length values follow it: `ll - 15 = 5` and `ml - 35 = 65`. Token 0 is no literals and a match of 4
bytes at the repeat offset.

gcc compiles the function without a branch, with a conditional move for each cap. The encoder calls it once per sequence, in `encode_emit()`.

## The compressor's state {#ch-header--compressor}

@listing src/seqlz.h 378-391

The comment in lines 378 to 381 is the compressor's design in one sentence: the matcher finds the
matches, and each sequence is written as soon as it is found, in one pass over the page. Matcher and
encoder are one loop ([chapter 9](#ch-matcher), [chapter 10](#ch-encoder)).

`SEQLZ_HASH_BITS` sets the matcher's hash table: `1 << 12 = 4096` entries of 2 bytes on 4 KiB pages,
8 KiB; `1 << 13 = 8192` entries on 16 KiB pages, 16 KiB. Each entry is the position of the last place
in the page whose next 5 bytes had that hash; a page has 4096 positions, one entry each if the hash
spread them perfectly. 2048 and 8192 entries were measured on the Mi 9T and not taken, see
[chapter 9](#ch-matcher--hash). The `?:` is a constant expression the compiler evaluates, so it can size an array.
The kernel's copy has `12U` there.

`SEQLZ_MAX_SEQUENCES` is the most sequences a page can have. Every sequence but the last has a match of
at least 4 bytes, so at most $4096 / 4 = 1024$ of them fit a page, plus the last: {{const.MAX_SEQUENCES}} on 4 KiB pages,
{{k16.const.MAX_SEQUENCES}} on 16 KiB pages. `seqlz_encode()` rejects more, `seqlz_find()` needs room for that many, and the
decoder's work per page is bounded by the same count.

@listing src/seqlz.h 393-403

`struct seqlz_state` is the compressor's work memory, one per CPU: zram compresses on every CPU at the
same time, and each needs its own hash table. The table is cleared for every page, so nothing of one
page survives into the next.

The union gives the same memory a second use. Once the matcher has gone through the page, the hash
table is not needed any more, and `code_literals()` needs a page of room for a copy of the raw literals
while it writes the coded ones over their old place in the output ([chapter 11](#ch-literals-enc)). A
union of `u16 table[4096]` and `u8 literals[4096]` is {{size.state}} bytes, the larger of the two; a
`static_assert` in `seqlz_compress.c` checks that the table alone has room for a page of literals, so
the union costs no byte more than the table. On 16 KiB pages both members are {{k16.size.state|grp}} bytes. The members
of an anonymous union are used as if they were the struct's own, `st->table` and `st->literals`, which
C11 and C++ both allow.

Before, `code_literals()` needed room for the raw literals, the coded ones and the bitstream at once in
the output, which cost memory with an output buffer of one page; [chapter 11](#ch-literals-enc--in-place)
has the numbers. In the kernel, the hash table and the scratch of the decoder share one allocation of
8 KiB per CPU, see [chapter 15](#ch-zram-glue--backend).

@listing src/seqlz.h 405-417

`seqlz_find()` runs the compressor's matcher alone and returns the sequences instead of writing them,
"for tests and tools", e.g. to compare matchers, or to feed `seqlz_encode()`. `seq` needs room for
`SEQLZ_MAX_SEQUENCES` entries, {{const.MAX_SEQUENCES}} of {{ch06.sequence}} bytes, which is why that constant is public. The last sequence
has no match, and the literals are the bytes no match covers; the function does not copy them, a tool
that needs them takes them from the page. Like `seqlz_encode()`, it is not in the kernel's copy.

@listing src/seqlz.h 419-441

`seqlz_compress()` is the compressor: the tables, the state of this CPU, the page in `src`, and the
output buffer. It returns the length of the compressed page, or 0.

Lines 428 to 431 are its size contract. zram gives the compressor a buffer of two pages, and that is
always enough, even for random data that does not compress at all; [chapter 10](#ch-encoder) proves it
with the 31 bits of `SEQLZ_MAX_ESCAPE_LEN`. A smaller buffer works too, e.g. the single page of
zswap, the kernel's other compressed swap, a cache in RAM in front of a swap device. A page
that does not fit is then an error, and the encoder stops "up to 32 bytes before `dst_cap` is full"
because before each sequence it checks for its literals plus 32 bytes of room, `ENC_ROOM`, not for the
exact bytes. With zram's two pages that never happens.

Lines 432 to 434: the page is written with raw literals first, then the literals are coded where that
pays, over the raw ones, with the raw copy in `st` meanwhile. So a page has to fit `dst` in its raw form,
even if its coded form would be smaller. And `seqlz_compress()` writes the same bytes as
`seqlz_encode()` for the sequences `seqlz_find()` returns; the tests check that the two paths agree.

In the kernel's copy this function is `seqlz_compress_page()`, `static`, and the public
`seqlz_compress()` of `include/linux/seqlz.h` calls it with the kernel's tables, takes a level, 1 or 2,
instead of `coded`, and returns `-E2BIG` instead of 0 ([chapter 15](#ch-zram-glue)).

@listing src/seqlz.h 443-446

The compiled-in code lengths, from `seqlz_default_tables.c`, and the end of the include guard.

## Every constant, for both page sizes {#ch-header--values}

All values computed by compiling this header with `QUETSCHN_PAGE_BITS` 12 and 14 and printing them:

| name | 4 KiB pages | 16 KiB pages | what it is |
| --- | --- | --- | --- |
| `SEQLZ_PAGE` | {{ch06.PAGE}} | {{k16.ch06.PAGE}} | bytes of a page |
| `SEQLZ_MAX_BITS` | {{ch06.MAX_BITS}} | {{k16.ch06.MAX_BITS}} | longest code of a length value |
| `SEQLZ_TOKEN_BITS` | {{ch06.TOKEN_BITS}} | {{k16.ch06.TOKEN_BITS}} | longest code of a token |
| `SEQLZ_LL_BITS`, `SEQLZ_ML_BITS` | {{ch06.LL_BITS}}, {{ch06.ML_BITS}} | {{k16.ch06.LL_BITS}}, {{k16.ch06.ML_BITS}} | bits of `ll` and `ml - 4` in a token |
| `SEQLZ_OFF_CLASSES` | {{ch06.OFF_CLASSES}} | {{k16.ch06.OFF_CLASSES}} | offset classes |
| `SEQLZ_TOKEN_SYMBOLS` | {{const.TOKEN_SYMBOLS}} | {{k16.const.TOKEN_SYMBOLS}} | tokens |
| `SEQLZ_ESCAPE` | {{ch06.ESCAPE}} | {{k16.ch06.ESCAPE}} | the escape's symbol |
| `SEQLZ_ESCAPE_BITS` | {{ch06.ESCAPE_BITS}} | {{k16.ch06.ESCAPE_BITS}} | plain bits of an escaped token |
| `SEQLZ_MAX_ESCAPE_LEN` | {{ch06.MAX_ESCAPE_LEN}} | {{k16.ch06.MAX_ESCAPE_LEN}} | longest code of the escape |
| `SEQLZ_LL_CAP`, `SEQLZ_ML_CAP` | {{ch06.LL_CAP}}, {{ch06.ML_CAP}} | {{k16.ch06.LL_CAP}}, {{k16.ch06.ML_CAP}} | largest `ll` and `ml - 4` in a token |
| `SEQLZ_LEN_DIRECT` | {{ch06.LEN_DIRECT}} | {{k16.ch06.LEN_DIRECT}} | length values that are their own symbol |
| `SEQLZ_LEN_LOG_BASE` | {{ch06.LEN_LOG_BASE}} | {{k16.ch06.LEN_LOG_BASE}} | symbol of a length value minus its highest bit |
| `SEQLZ_LEN_SYMBOLS` | {{ch06.LEN_SYMBOLS}} | {{k16.ch06.LEN_SYMBOLS}} | symbols of length values |
| `SEQLZ_HEADER` | {{ch06.HEADER}} | {{k16.ch06.HEADER}} | the `u16` of every page |
| `SEQLZ_LIT_CODED` | `{{ch06.LIT_CODED}}` | `{{k16.ch06.LIT_CODED}}` | the flag for coded literals |
| `SEQLZ_CLASS5_BITS` | {{ch06.CLASS5_BITS}} | {{k16.ch06.CLASS5_BITS}} | offset bits of class 5 |
| `SEQLZ_RAW_NIBBLES` | `{{ch06.RAW_NIBBLES}}` | `{{k16.ch06.RAW_NIBBLES}}` | offset bits per class, a nibble each |
| `SEQLZ_RAW_BITS(0..5)` | {{ch06.RAW_BITS}} | {{k16.ch06.RAW_BITS}} | offset bits per class |
| `SEQLZ_OFF_SHIFT(0..5)` | {{ch06.OFF_SHIFT}} | {{k16.ch06.OFF_SHIFT}} | shift of the offset sent |
| `SEQLZ_LIT_BITS` | {{ch06.LIT_BITS}} | {{k16.ch06.LIT_BITS}} | longest literal code |
| `SEQLZ_LIT_SETS`, `SEQLZ_LIT_STREAMS` | {{ch06.LIT_SETS}}, {{ch06.LIT_STREAMS}} | {{k16.ch06.LIT_SETS}}, {{k16.ch06.LIT_STREAMS}} | literal tables, literal streams |
| `SEQLZ_LIT_ROUNDS` | {{ch06.LIT_ROUNDS}} | {{k16.ch06.LIT_ROUNDS}} | literals per stream per refill |
| `SEQLZ_LIT_HEADER(w)` | {{ch06.LIT_HEADER_MIN}} to {{ch06.LIT_HEADER_MAX}} | {{k16.ch06.LIT_HEADER_MIN}} to {{k16.ch06.LIT_HEADER_MAX}} | header bytes with coded literals |
| `SEQLZ_SIZE_BITS_MIN`, `_MAX` | {{ch06.SIZE_BITS_MIN}}, {{ch06.SIZE_BITS_MAX}} | {{k16.ch06.SIZE_BITS_MIN}}, {{k16.ch06.SIZE_BITS_MAX}} | bits per stream size |
| `SEQLZ_LIT_WIDTH_AT`, `SEQLZ_LIT_ZERO_AT` | {{ch06.LIT_WIDTH_AT}}, {{ch06.LIT_ZERO_AT}} | {{k16.ch06.LIT_WIDTH_AT}}, {{k16.ch06.LIT_ZERO_AT}} | fields of byte 2 |
| `SEQLZ_LIT_CODED_MIN` | {{ch06.LIT_CODED_MIN}} | {{k16.ch06.LIT_CODED_MIN}} | bytes coding must save beyond $\frac{1}{16}$ |
| `SEQLZ_SCRATCH` | {{const.SCRATCH}} | {{k16.const.SCRATCH}} | the decoder's buffer for coded literals |
| `SEQLZ_HASH_BITS` | {{ch06.HASH_BITS}} | {{k16.ch06.HASH_BITS}} | bits of the matcher's hash |
| `SEQLZ_MAX_SEQUENCES` | {{const.MAX_SEQUENCES}} | {{k16.const.MAX_SEQUENCES}} | most sequences per page |
| `sizeof(struct seqlz_lengths)` | {{size.lengths}} | {{k16.size.lengths}} | the code lengths of the sequence tables |
| `sizeof(struct seqlz_state)` | {{size.state}} | {{k16.size.state}} | the compressor's work memory |
| `seqlz_tables_size()` | {{size.tables}} | {{k16.size.tables}} | the built tables |

::: try Print them yourself
```c
#include <stdio.h>
#include "seqlz.h"
int main(void)
{
	unsigned int raw, extra;
	unsigned int cls = seqlz_off_class(264, 1, &raw);
	unsigned int sym = seqlz_len_symbol(100, &extra);

	printf("0x%X %u %u\n", SEQLZ_RAW_NIBBLES, SEQLZ_LEN_SYMBOLS, SEQLZ_MAX_ESCAPE_LEN);
	printf("off 264: class %u, %u bits; v 100: symbol %u, %u bits\n", cls, raw, sym, extra);
	return 0;
}
```
@program ch06-print

`gcc -O2 -Isrc t.c && ./a.out` prints

@output ch06-print

With `-DQUETSCHN_PAGE_BITS=14` the first line is `{{k16.ch06.RAW_NIBBLES}} {{k16.ch06.LEN_SYMBOLS}} {{k16.ch06.MAX_ESCAPE_LEN}}`, class 3 has {{k16.ch06.RAW_BITS3}} bits, and class 5 keeps its {{k16.ch06.RAW_BITS5}}.
:::
