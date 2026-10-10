# seqlz_internal.h: tables shaped for the decoder {#ch-internal}

`src/seqlz_internal.h` is what the three `.c` files of the codec share and nobody else sees: the
compressor in `seqlz_compress.c`, the decompressor in `seqlz_decompress.c`, and the code in
`seqlz_codes.c` that builds the tables both of them read. The public header, `seqlz.h`, only
declares `struct seqlz_tables`; this file says what is inside it. It is 146 lines, and almost all of
them are about memory layout: how many bytes each table has, which bits of an entry mean what, and
`static_assert()`s that prove the fields are wide enough.

The format stores a table as a list of code lengths, one byte per symbol (see
[chapter 4](#ch-format)). Neither the encoder nor the decoder can use code lengths directly. The
encoder needs each symbol's code as a number it can shift into its bit buffer, and the decoder needs a
table it can index with the next bits of the stream. `struct seqlz_tables` holds both forms, for all
11 tables of the format, in 43 280 bytes. It is built once from the lengths by `seqlz_tables_init()`
([chapter 8](#ch-codes)) and only read after that, by every CPU. The most interesting part of the
file is at the end: `token_entry()`, which packs everything the decoder needs to know about a token
into one 32-bit number, so that the decoder's loop over the sequences never has to compute it.

## The head of the file {#ch-internal--head}

@listing src/seqlz_internal.h 1-11

Line 1 is the licence and lines 2, 3 and 146 the include guard, as in every file of `src/`
([chapter 5](#ch-compat--top)). The kernel's copy, which `tools/kernel-port/port.py` writes, renames
the guard to `_LIB_SEQLZ_SEQLZ_INTERNAL_H`, after the file's place in the kernel tree, `lib/seqlz/`.

The comment says what the file is for, and line 10 pulls in `seqlz.h`, which brings the format's
constants (`SEQLZ_TOKEN_BITS`, `SEQLZ_LIT_SETS`, ...) and through `seqlz_compat.h` the kernel's
types `u8` to `u64` and `static_assert()`. [Chapter 6](#ch-header) explains those constants; this
chapter uses their values for 4 KiB pages.

## A decode table is indexed by the next bits {#ch-internal--decode-tables}

@listing src/seqlz_internal.h 12-27

The first paragraph is the idea of [table decoding](#ch-compression--table-decoding) in three
lines. A table whose longest code has `L` bits gets `2^L` entries. A code of `l` bits is the first
`l` bits of `2^(L - l)` indices, one for each value of the `L - l` bits after it, and all of those
entries say the same thing: this symbol, `l` bits long. The decoder takes the next `L` bits of the
stream as an index, reads one entry, and knows the symbol and how many bits to drop, without looking
at the bits one by one. [Chapter 8](#ch-codes) shows how `seqlz_codes.c` fills these entries.

The second paragraph is the format of an entry of the two tables of **length values**, the `ll` and
`ml` values that follow a token when a length is too large for it
([chapter 4](#ch-format--length-values)). A 32-bit entry has three fields:

| bits | field | example: symbol 18 of the `ll` table |
| --- | --- | --- |
| 0 to 3 | the code's length | 4 |
| 4 to 7 | how many extra bits follow the code | 6 |
| 8 to 23 | the base value | 64 |

Symbol 18 stands for the values 64 to 127: its code is followed by 6 plain bits, the value is
`64 + read(6)`. In the real `ll` table it has the 4-bit code `1010`, and its entry is
`64 << 8 | 6 << 4 | 4 = 0x4064`, in the 16 entries 160 to 175 of the 256. The decoder's
`seqlz_br_value()` ([chapter 12](#ch-dec-basics)) takes these fields apart with `& 15U`,
`(e >> 4) & 15U` and `(e >> 8) & 0xffffU`. The base needs 16 bits because the largest is
`2^12 = 4096` on 4 KiB pages and `2^14 = 16384` on 16 KiB pages, so the entry does not fit 16 bits
and is a `u32`.

The third paragraph explains a habit of the encoder that looks odd at first: it masks values that
are already in range.

A kernel built with the undefined behaviour sanitizer, UBSan, checks every shift and every array
index the compiler cannot prove in range, with a compare and a branch, and distributions enable it
in their normal kernels; a masked value needs no check ([chapter 3](#ch-cpu--ubsan)).

So the encoder masks: `enc_put()` in [chapter 10](#ch-encoder) shifts its bit buffer by
`n & 63U`, and `put_len_value()` indexes the length values' `enc[]` with
`seqlz_len_symbol(v, &extra) & (ENC_LEN_SYMBOLS - 1U)`. For every length a page can have, the
symbol is at most 23 (25 on 16 KiB pages, see [chapter 6](#ch-header--length-values)), and `& 31`
changes nothing. The mask only tells the compiler what the code already knows. For that, `enc[]`
needs a power of 2 entries, which is the next line. On 23rd September 2026, masked indices and shifts
took the compressor to 4.7% fewer instructions ([chapter 3](#ch-cpu--ubsan) has the cycles).

@listing src/seqlz_internal.h 28-33

`ENC_LEN_SYMBOLS` is that power of 2: 32 entries in the encoder's table of length values, where
`SEQLZ_LEN_SYMBOLS` is 25 for 4 KiB pages and 27 for 16 KiB pages. The entries 25 to 31 are never
used for a valid length; `seqlz_tables_init()` sets them to 0 with the rest of the struct.

A `static_assert(condition, message)` is checked by the compiler, not at run time: if the condition
is false, the build fails with the message. It costs nothing in the program. `seqlz_compat.h` maps
it to C11's `_Static_assert` where the kernel's headers do not have it. The two here prove:

- `SEQLZ_LEN_SYMBOLS <= ENC_LEN_SYMBOLS`, `25 <= 32`: every length value's symbol has an entry in
  `enc[]`, so the mask never folds a real symbol onto another one. If a later change gave the page
  more bits, e.g. `QUETSCHN_PAGE_BITS` 20, the symbols would be 33 and the build would stop here
  instead of the encoder writing wrong codes.
- the longest code of each table, 11, 8 and 10 bits, is below 16. `first_codes()` in
  [chapter 8](#ch-codes) counts the codes of each length in an array of 16, `of_len[16]`, indexed by
  the length, and keeps the first code of each length in `next[16]`. A table that allowed codes of
  16 bits would write past both arrays.

The assert's condition is in parentheses and spread over lines, one condition per line, because
the kernel's `checkpatch`, the style checker a patch for the kernel has to pass
([chapter 15](#ch-zram-glue--check)), wants the continuation lines of a call aligned with its first
argument.

## The table of length values {#ch-internal--value-table}

@listing src/seqlz_internal.h 35-38

`struct value_table` is the `ll` table or the `ml` table, in both forms:

- `decode[256]`: `1U << SEQLZ_MAX_BITS` entries of 4 bytes, 1024 bytes, in the format described
  above. The decoder indexes it with the top 8 bits of its bit register.
- `enc[32]`: for each symbol its code in the low 16 bits and its length from bit 16 on, 128 bytes.
  Symbol 18 of the `ll` table, code `1010` of 4 bits, has `10 | 4 << 16 = 0x4000a`. The encoder's
  `enc_put_code()` ([chapter 10](#ch-encoder)) takes the length back out with `entry >> 16 & 15U`
  and the code with `entry & 0xffffU`.

The struct is 1152 bytes. Why are the encoder's codes and the decoder's table in one struct, when
no function reads both? Because they come from one computation. `build_values()` in
[chapter 8](#ch-codes) works out each symbol's code once and writes it to `enc[s]` and to the
entries of `decode[]` in the same step. At run time the arrangement costs nothing: the encoder reads
only `enc[]` and the decoder only `decode[]`, and the cache lines a CPU loads are those of the array
it reads. The one price is memory, and that is paid once for all CPUs, not per CPU.

## The token table: 8 KiB for the decoder, 6 KiB for the encoder {#ch-internal--token-table}

@listing src/seqlz_internal.h 40-47

The **token** is the symbol that holds a sequence's `ll`, `ml - 4` and offset class, 3072 of them
plus the escape ([chapter 4](#ch-format--token)). Its decode table has `1U << SEQLZ_TOKEN_BITS`,
2048 entries of 4 bytes: 8 KiB. What an entry holds is the subject of the last section,
`token_entry()`.

The encoder's side, `enc[]`, has one entry per symbol, `SEQLZ_TOKEN_SYMBOLS + 1 = 3073` of them,
the `+ 1` being the escape, 2 bytes each: 6146 bytes. The comment gives the reason for 2 bytes: the
matcher's hash table is 8 KiB ([chapter 9](#ch-matcher)), and the compressor reads the first at
every position of the page and the second for every sequence. 8 KiB plus 6 KiB is 14 KiB, which
leaves room in an L1 data cache of 32 KiB for the page itself and the rest. Until 23rd September 2026 the entries were `u32`, 12 292 bytes, and the two
tables together 20 KiB (commit `c84ba25`).

@listing src/seqlz_internal.h 49-55

An entry of `token.enc[]` is the code in bits 0 to 11 and the code's length in bits 12 to 15:

| token | what it is | code | length | `enc[]` |
| --- | --- | --- | --- | --- |
| 34 | 2 literals, match of 6, repeat offset | `0000` | 4 | `0x4000` |
| 49 | 1 literal, match of 7, repeat offset | `0001` | 4 | `0x4001` |
| 2098 | 2 literals, match of 7, offset a multiple of 8 below 256 | `101100111` | 9 | `0x9167` |
| 3072 | the escape | `0011` | 4 | `0x4003` |
| 1797 | 5 literals, match of 20, class 3 | none | 0 | `0` |

`TOK_ENC_LEN_AT` is where the length starts, `TOK_ENC_CODE_MASK` is `0xfff`, the mask for the code.
The encoder's `token_code()` ([chapter 10](#ch-encoder)) reads `te >> TOK_ENC_LEN_AT` and
`te & TOK_ENC_CODE_MASK`.

Note token 34. Its code is the number 0, but its entry is not 0, because the length is in it too.
So an entry of 0 means one thing only: this token has no code. `token_code()` relies on that: if
`enc[tok]` is 0, it writes the escape's code and then the token in 12 plain bits. No separate flag
is needed, and no code can be confused with "none", because every real code has a length of at
least 1.

The two asserts prove the layout:

- `SEQLZ_TOKEN_BITS <= TOK_ENC_LEN_AT`, `11 <= 12`: a code of at most 11 bits fits below bit 12 and
  does not run into the length.
- `SEQLZ_TOKEN_BITS < 16U`: the length, at most 11, fits into 4 bits, which hold up to 15. And
  `TOK_ENC_LEN_AT + 4U <= 16U`, `12 + 4 <= 16`: those 4 bits end within the `u16`.

## The literal tables {#ch-internal--lit-table}

@listing src/seqlz_internal.h 57-60

There are 8 literal tables, one chosen per page for its **coded literals**
([chapter 4](#ch-format--layout)), and each has its codes in two forms again:

- `decode[1024]`: `1U << SEQLZ_LIT_BITS` entries of 2 bytes, 2 KiB. An entry is the code's length in
  the low byte and the literal itself, the byte value, in the high byte. In table 0, byte `0x65`, the
  letter `e`, has the 4-bit code `0001`. That code is the top 4 bits of the 10-bit indices 64 to 127,
  and each of those 64 entries is `4 | 0x65 << 8 = 0x6504`. A literal is one byte and a length at
  most 10, so 2 bytes per entry are enough, and the decoder's table of the page is half the size it
  would be with 4.
- `enc[256]`: one `u32` per byte value, the code's length in the low byte and the code from bit 8 on.
  Byte `0x65` in table 0 has `4 | 1 << 8 = 0x104`.

Why is the length below the code here, and above it in the other two tables? Because of how the
encoder uses it. `lit_put()` in [chapter 11](#ch-literals-enc) does
`acc = acc << (e & 63) | e >> 8` and then `*sum += e`: the shift count is the low bits as they are,
the code is one shift away, and adding whole entries into `sum` adds up the lengths in its low byte
(the code bits above it are thrown away later with `& 255`). Each table's layout follows what its
user does with an entry.

The 8 tables are `lit[8]` in `struct seqlz_tables`, 3072 bytes each, 24 576 together. A page reads
one of them.

## One add per literal for all 8 tables {#ch-internal--lit-cost}

@listing src/seqlz_internal.h 62-70

To choose a page's literal table, the encoder needs, for each of the 8 tables, the number of bits
the page's literals would take in it: the sum of their code lengths. Done naively that is 8 lookups
and 8 adds per literal. The comment describes the trick that makes it one of each.

A 64-bit register holds 8 such sums side by side, one per byte, and one add adds all 8 as long as no
sum passes 255: SWAR, SIMD within a register ([chapter 3](#ch-cpu--swar)).

For each byte value `b`, `lit_cost[b]` is a `u64` whose lane `j`, bits `8 * j` to `8 * j + 7`, is
`b`'s code length in table `j`. [Chapter 8](#ch-codes) shows the loop in `build_lit_sets()` that
packs them. With the real tables:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 170" role="img" aria-label="The lit_cost words of bytes 0x00 and 0x65, eight 8-bit lanes each, and their sum, where lane 3 is the smallest">
  <g font-size="11">
    <text x="230" y="16" text-anchor="middle">table 7</text>
    <text x="290" y="16" text-anchor="middle">table 6</text>
    <text x="350" y="16" text-anchor="middle">table 5</text>
    <text x="410" y="16" text-anchor="middle">table 4</text>
    <text x="470" y="16" text-anchor="middle">table 3</text>
    <text x="530" y="16" text-anchor="middle">table 2</text>
    <text x="590" y="16" text-anchor="middle">table 1</text>
    <text x="650" y="16" text-anchor="middle">table 0</text>
    <text x="230" y="30" text-anchor="middle">bits 63..56</text>
    <text x="650" y="30" text-anchor="middle">bits 7..0</text>
  </g>
  <g font-size="12">
    <text x="4" y="55">lit_cost[0x00]</text>
    <text x="4" y="69" font-size="11">0x0803090202020208</text>
    <text x="4" y="97">+ lit_cost[0x65]</text>
    <text x="4" y="111" font-size="11">0x0a0a0909050a0904</text>
    <text x="4" y="147">= the sums</text>
    <text x="4" y="161" font-size="11">0x120d120b070c0b0c</text>
  </g>
  <g stroke="currentColor" stroke-width="1">
    <rect x="200" y="40" width="480" height="30" class="fill-sheet"/>
    <rect x="200" y="82" width="480" height="30" class="fill-sheet"/>
    <rect x="200" y="132" width="480" height="30" class="fill-paper"/>
    <rect x="440" y="132" width="60" height="30" class="fill-acc"/>
    <line x1="260" y1="40" x2="260" y2="70"/><line x1="320" y1="40" x2="320" y2="70"/>
    <line x1="380" y1="40" x2="380" y2="70"/><line x1="440" y1="40" x2="440" y2="70"/>
    <line x1="500" y1="40" x2="500" y2="70"/><line x1="560" y1="40" x2="560" y2="70"/>
    <line x1="620" y1="40" x2="620" y2="70"/>
    <line x1="260" y1="82" x2="260" y2="112"/><line x1="320" y1="82" x2="320" y2="112"/>
    <line x1="380" y1="82" x2="380" y2="112"/><line x1="440" y1="82" x2="440" y2="112"/>
    <line x1="500" y1="82" x2="500" y2="112"/><line x1="560" y1="82" x2="560" y2="112"/>
    <line x1="620" y1="82" x2="620" y2="112"/>
    <line x1="260" y1="132" x2="260" y2="162"/><line x1="320" y1="132" x2="320" y2="162"/>
    <line x1="380" y1="132" x2="380" y2="162"/><line x1="440" y1="132" x2="440" y2="162"/>
    <line x1="500" y1="132" x2="500" y2="162"/><line x1="560" y1="132" x2="560" y2="162"/>
    <line x1="620" y1="132" x2="620" y2="162"/>
    <line x1="200" y1="122" x2="680" y2="122"/>
  </g>
  <g font-size="13" text-anchor="middle">
    <text x="230" y="60">8</text><text x="290" y="60">3</text><text x="350" y="60">9</text>
    <text x="410" y="60">2</text><text x="470" y="60">2</text><text x="530" y="60">2</text>
    <text x="590" y="60">2</text><text x="650" y="60">8</text>
    <text x="230" y="102">10</text><text x="290" y="102">10</text><text x="350" y="102">9</text>
    <text x="410" y="102">9</text><text x="470" y="102">5</text><text x="530" y="102">10</text>
    <text x="590" y="102">9</text><text x="650" y="102">4</text>
    <text x="230" y="152">18</text><text x="290" y="152">13</text><text x="350" y="152">18</text>
    <text x="410" y="152">11</text><text x="470" y="152" class="acc">7</text><text x="530" y="152">12</text>
    <text x="590" y="152">11</text><text x="650" y="152">12</text>
  </g>
</svg>
</div>
<figcaption>Two literals, 0x00 and 0x65, priced in all 8 literal tables with one 64-bit add; table 3 codes them in 7 bits.</figcaption>
</figure>

The zero byte has a 2-bit code in tables 1 to 4, which are for pages with many zeros; `e` has a 4-bit
code in table 0, which fits text. For these two literals table 3 wins with 7 bits, against 16 bits
as raw bytes. `docs/seqlz.md` has the same example with four literals
([seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-literals-one-table-per-page-in-8-streams)).

The two macros size this:

- `LIT_COST_WORDS` is `(SEQLZ_LIT_SETS + 7U) / 8U`, the number of `u64` needed for 8 lanes per word,
  rounded up: `(8 + 7) / 8 = 1`. So `lit_cost` is `u64 [256][1]`, 2048 bytes. Written this way the
  code would still work with more tables, one more word per 8, but byte 2 of the format has 3 bits
  for the table, and the next `static_assert` holds the number at 8.
- `LIT_COST_RUN` is `255U / SEQLZ_LIT_BITS`, `255 / 10 = 25` in integer division: 25 codes of at
  most 10 bits add up to at most 250, which fits a lane; 26 could reach 260 and carry. So the encoder's
  `lit_cost()` ([chapter 11](#ch-literals-enc)) adds 25 literals per stream into 8-bit lanes, then
  spreads the lanes into 16-bit ones, every second byte with the mask `0x00ff00ff00ff00ff`, into two
  words, and starts the 8-bit lanes again. With 8 streams that is every 200 literals of the page.

What the whole choice of the table costs per write is in [chapter 11](#ch-literals-enc--pricing).

## Byte 2 and the largest stream {#ch-internal--byte2}

@listing src/seqlz_internal.h 71-83

A page with coded literals has a third header byte, after the 2-byte literal count: the table in
bits 0 to 2, the width `w - 5` of the stream sizes in bits 3 to 5, and two zero bits
([chapter 4](#ch-format--layout)). Nothing in this block generates code. It ties the constants of
`seqlz.h` to that byte, so that a change to one of them cannot quietly break the format:

- `SEQLZ_LIT_SETS == 8U`: 3 bits hold the numbers 0 to 7, so exactly 8 tables. Fewer would leave
  table numbers that a page could name but that do not exist.
- `SEQLZ_SIZE_BITS_MAX - SEQLZ_SIZE_BITS_MIN == 7U`, `12 - 5`: the widths 5 to 12 are the 8 values
  `w - 5 = 0` to `7`, again 3 bits.
- `SEQLZ_LIT_ZERO_AT - SEQLZ_LIT_WIDTH_AT == 3`, `6 - 3`: the width field is 3 bits wide, from bit 3
  up to the zero bits at 6.

`LIT_STREAM_MAX` is the largest a literal stream can be, in bytes. Literal `k` goes to stream
`k % 8`, so a stream holds at most every 8th literal of a page, `4096 / 8 = 512` literals, and each
takes at most 10 bits: 5120 bits. `+ 7U` and `/ 8U` round the bits up to whole bytes: `(5120 + 7) /
8 = 640`. On 16 KiB pages it is 2560. The assert `LIT_STREAM_MAX < 1U << SEQLZ_SIZE_BITS_MAX`,
`640 < 4096` and `2560 < 4096`, proves that the widest size field, 12 bits, holds any stream. So the
encoder can always write the sizes, whatever the literals. For 4 KiB pages 10 bits would always do,
`640 < 1024`; the format allows 12 for the 16 KiB pages.

The last assert, `SEQLZ_LIT_STREAMS == 8U`, is of another kind: it guards code, not the format. The
encoder's `lit_streams()` writes the streams in two halves of 4 with 4 accumulators each, written
out by hand, and the decoder's `decode_literals()` ([chapter 13](#ch-literals-dec)) keeps 8 stream
states. Changing the number of streams means rewriting both, and the assert says so at compile
time.

## struct seqlz_tables: 43 280 bytes, once {#ch-internal--tables}

@listing src/seqlz_internal.h 85-91

This is the whole struct, all 11 tables of the format plus the literal costs. Its layout, from a
small C program that prints `sizeof` and `offsetof` on x86-64 (arm64 aligns a `u64` the same
way):

| field | offset | bytes | read by |
| --- | --- | --- | --- |
| `token.decode` | 0 | 8192 | decoder, every sequence |
| `token.enc` | 8192 | 6146 | encoder, every sequence |
| `lit[0]` to `lit[7]` | 14 340 | 8 × 3072 = 24 576 | decoder: one `decode`; encoder: one `enc` |
| (padding) | 38 916 | 4 | |
| `lit_cost` | 38 920 | 2048 | encoder, every literal |
| `ll` | 40 968 | 1152 | both, for long literal runs |
| `ml` | 42 120 | 1152 | both, for long matches |
| `all_symbols` | 43 272 | 1, then 7 of padding | the harness's encoder |

43 280 bytes in all. The two holes are alignment: a `u64` must start at a multiple of 8 on x86-64
and arm64, and 38 916 is not one, so the compiler skips 4 bytes before `lit_cost`; and the size of a
struct is a multiple of its strictest alignment, so the 1-byte `bool` at the end is followed by 7
bytes. On 32-bit x86, where a `u64` needs only 4-byte alignment, the struct is 43 272 bytes. The
16 KiB variant has the same size: its length tables have 27 symbols, and `enc[]` has 32 entries
either way.

What a page touches is much less than 43 280 bytes. The decoder reads `token.decode`, the `decode` of
`ll` and `ml`, and with coded literals the `decode` of one literal table: 8 + 1 + 1 + 2 = 12 KiB,
small enough for the L1 cache, one of the reasons `docs/seqlz.md` gives for the decoder's speed
([seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#why-it-is-nearly-as-fast-as-lz4)).
The encoder reads `token.enc`, 6 KiB, the 128-byte `enc` of `ll` and `ml`, `lit_cost`, 2 KiB, and
one literal table's `enc`, 1 KiB.

The field `all_symbols` says whether the tables have a code for every symbol the encoder might
need to write; `seqlz_tables_init()` computes it once, and [chapter 8](#ch-codes) explains why the
decoder does not need it. The kernel's copy has no such field: there the tables are the format's
own, checked before they were compiled in, and `port.py` deletes the line.

In the kernel the struct is one global variable, `seqlz_fixed_tables`, built at boot, read-only
after it and shared by every CPU ([chapter 15](#ch-zram-glue--init)); the per-CPU work memory is
something else ([chapter 15](#ch-zram-glue--backend)). In the userspace harness, `src/zram_seqlz.c`
allocates the tables once per zram device instead.

## The fields of a token entry {#ch-internal--token-entry}

The rest of the file is the token table's entry. Before the description, the file proves that the
fields it is about to describe are wide enough:

@listing src/seqlz_internal.h 93-99

Each condition is about one field of the entry, and refers to the layout below:

- `SEQLZ_TOKEN_BITS + SEQLZ_RAW_BITS(3) <= 31U`, `11 + 12 = 23`: the field for "how far the
  bitstream moves on" has 5 bits, so it holds up to 31. The most a token with its own code moves is
  an 11-bit code plus class 3's 12 offset bits. On 16 KiB pages it is `11 + 14 = 25`.
- `SEQLZ_TOKEN_BITS + SEQLZ_RAW_BITS(5) <= 31U`, `11 + 9 = 20`: the same for class 5. Class 5 has 3
  bits fewer than class 3 by definition, so today this follows from the first line; it would catch a
  change of `SEQLZ_RAW_NIBBLES` that gave class 5 more bits.
- `SEQLZ_LL_CAP <= 15U`: `ll` goes into a field of 4 bits, and the token's largest `ll` is 15.
- `SEQLZ_ML_CAP + 4U <= 63U`, `31 + 4 = 35`: the entry stores the match length itself, not
  `ml - 4`, so it needs 35 in a field of 6 bits.
- `SEQLZ_OFF_SHIFT(5) <= 3U`: the shift of classes 4 and 5 is 3, in a field of 2 bits.

The second assert is about the escape: a token sent after the escape takes `SEQLZ_ESCAPE_BITS`, 12
plain bits, which can say 0 to 4095, so all 3072 tokens fit. A 4097th token would need 13.

@listing src/seqlz_internal.h 100-115

The comment is the specification of the entry. Every field is something the decoder would otherwise
compute from the token's number for every sequence: split off `ll`, `ml` and the class with shifts
and masks, look up the class's number of offset bits, decide whether the offset is divided by 8,
add 4 to `ml`, compare `ll` and `ml` with their caps. The last sentence says what that cost.

On an in-order core such as the phone's Cortex-A55 an instruction saved is time saved, almost one
for one ([chapter 3](#ch-cpu--in-order)).

::: measured The token unpacked in the table: 1418 fewer instructions per page on the A55
On the Cortex-A55 of a Xiaomi Mi 9T, `simpleperf` showed that 18 of about 60 instructions per
sequence went into unpacking the token: the class, its number of offset bits from a packed
constant, the mask, the shift of classes 4 and 5. With 4-byte entries holding the fields ready to
use, the decode loop went from 16 547 to 15 539 instructions per page and from 16 247 to 15 776
cycles, on phone pages. The bit for "a length value follows" saved another 410 instructions and no
cycles: there the chain from one token's lookup to the next was the limit. The table grew from 4 KiB
to 8 KiB, which did not make cold reads slower. That was 3rd October 2026, commit `5c769f5`
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).
:::

The entry, bit 31 on the left, with two examples that the next sections compute: token 2098 with
its 9-bit code, and the escape with its 4-bit code.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 230" role="img" aria-label="The 32 bits of a token table entry: escape, value flag, an unused bit, ml, ll, drop, shift, back and n, with the bits of token 2098 and of the escape">
  <g font-size="11" text-anchor="middle">
    <text x="50" y="14">31</text><text x="70" y="14">30</text><text x="90" y="14">29</text>
    <text x="110" y="14">28</text><text x="210" y="14">23</text><text x="230" y="14">22</text>
    <text x="290" y="14">19</text><text x="310" y="14">18</text><text x="390" y="14">14</text>
    <text x="410" y="14">13</text><text x="430" y="14">12</text><text x="450" y="14">11</text>
    <text x="550" y="14">6</text><text x="570" y="14">5</text><text x="670" y="14">0</text>
  </g>
  <g stroke="currentColor" stroke-width="1">
    <rect x="40" y="22" width="20" height="30" class="fill-mark"/>
    <rect x="60" y="22" width="20" height="30" class="fill-sheet"/>
    <rect x="80" y="22" width="20" height="30" fill="none"/>
    <rect x="100" y="22" width="120" height="30" class="fill-sheet"/>
    <rect x="220" y="22" width="80" height="30" class="fill-sheet"/>
    <rect x="300" y="22" width="100" height="30" class="fill-acc"/>
    <rect x="400" y="22" width="40" height="30" class="fill-acc"/>
    <rect x="440" y="22" width="120" height="30" class="fill-acc"/>
    <rect x="560" y="22" width="120" height="30" class="fill-acc"/>
  </g>
  <g font-size="12" text-anchor="middle">
    <text x="50" y="42">E</text><text x="70" y="42">V</text><text x="90" y="42">-</text>
    <text x="160" y="42">ML</text><text x="260" y="42">LL</text><text x="350" y="42">DROP</text>
    <text x="420" y="42">SHIFT</text><text x="500" y="42">BACK</text><text x="620" y="42">N</text>
  </g>
  <g font-size="11">
    <text x="4" y="82">2098</text>
    <text x="4" y="164">esc</text>
  </g>
  <g stroke="currentColor" stroke-width="1" fill="none">
    <rect x="40" y="66" width="640" height="24"/>
    <rect x="40" y="148" width="640" height="24"/>
    <line x1="100" y1="66" x2="100" y2="90"/><line x1="220" y1="66" x2="220" y2="90"/>
    <line x1="300" y1="66" x2="300" y2="90"/><line x1="400" y1="66" x2="400" y2="90"/>
    <line x1="440" y1="66" x2="440" y2="90"/><line x1="560" y1="66" x2="560" y2="90"/>
    <line x1="60" y1="66" x2="60" y2="90"/><line x1="80" y1="66" x2="80" y2="90"/>
    <line x1="100" y1="148" x2="100" y2="172"/><line x1="220" y1="148" x2="220" y2="172"/>
    <line x1="300" y1="148" x2="300" y2="172"/><line x1="400" y1="148" x2="400" y2="172"/>
    <line x1="440" y1="148" x2="440" y2="172"/><line x1="560" y1="148" x2="560" y2="172"/>
    <line x1="60" y1="148" x2="60" y2="172"/><line x1="80" y1="148" x2="80" y2="172"/>
  </g>
  <g font-size="12" text-anchor="middle">
    <text x="50" y="83">0</text><text x="70" y="83">0</text><text x="90" y="83">0</text>
    <text x="160" y="83">000111</text><text x="260" y="83">0010</text><text x="350" y="83">01110</text>
    <text x="420" y="83">11</text><text x="500" y="83">111011</text><text x="620" y="83">001001</text>
  </g>
  <g font-size="12" text-anchor="middle" class="acc">
    <text x="160" y="108">ml 7</text><text x="260" y="108">ll 2</text><text x="350" y="108">14</text>
    <text x="420" y="108">3</text><text x="500" y="108">59</text><text x="620" y="108">9</text>
  </g>
  <g font-size="11">
    <text x="40" y="128">0x0393bec9: a 9-bit code, then 5 offset bits, the offset is them times 8</text>
  </g>
  <g font-size="12" text-anchor="middle">
    <text x="50" y="165">1</text><text x="70" y="165">0</text><text x="90" y="165">0</text>
    <text x="160" y="165">000000</text><text x="260" y="165">0000</text><text x="350" y="165">00100</text>
    <text x="420" y="165">00</text><text x="500" y="165">000000</text><text x="620" y="165">000000</text>
  </g>
  <g font-size="12" text-anchor="middle" class="mark">
    <text x="50" y="190">esc</text><text x="350" y="190">4</text>
  </g>
  <g font-size="11">
    <text x="40" y="214">0x80010000: drop the escape's 4 bits, then read the token in 12 bits</text>
  </g>
</svg>
</div>
<figcaption>A token table entry: bits 0 to 18 tell the decoder how to cut the offset out of the bitstream, bits 19 to 28 are the lengths, the top bits flags.</figcaption>
</figure>

@listing src/seqlz_internal.h 116-129

The first group of macros is where each field starts, the second takes a field out of an entry `e`:
shift it down, mask it to its width. `TOK_N()` needs no shift, the field starts at bit 0. The masks
are the fields' widths, `63U` for 6 bits, `3U` for 2, `31U` for 5, `15U` for 4. There is no macro for
bits 30 and 31: the decoder tests them directly, `tok >> TOK_ESCAPE_AT` and
`tok & (1U << TOK_VALUE_AT)`. Bit 29 is free, as the diagram shows.

`TOK_N()` and `TOK_BACK()` are shift counts for the decoder's 64-bit bit register, and both fields are
6 bits wide. So the value a macro returns is at most 63, the mask says so to the compiler, and a
shift of a `u64` by at most 63 is defined in C. As with the encoder's masks, `CONFIG_UBSAN_SHIFT`
finds nothing to check there.

## token_entry(), line by line {#ch-internal--token-entry-code}

@listing src/seqlz_internal.h 131-146

`token_entry(s, n)` computes the entry for token `s` whose code has `n` bits. It is `static inline`
in a header because two files call it: `build_token()` in `seqlz_codes.c` fills every entry of the
token table with it at start, and `seqlz_decode()` calls it at run time for an escaped token, with
`n = 0`. Line by line:

- Line 133: `cls`, the offset class, is the token divided by 512, `s >> (4 + 5)`. The token is
  `ll + 16 * (ml - 4) + 512 * class` with `ll` and `ml - 4` capped at 15 and 31, so the class is all
  bits from bit 9 on.
- Line 134: `raw_bits`, how many offset bits follow the token, from `SEQLZ_RAW_BITS(cls)`: the
  `cls`-th nibble of `0x95C840`, which is 0, 4, 8, 12, 5, 9 for classes 0 to 5 ([chapter 6](#ch-header)).
- Line 135: `drop`, the code and the offset bits together, how far the bitstream moves on after
  them. `ll` is the low 4 bits, `s & 15`.
- Line 136: `ml` is bits 4 to 8, `(s >> 4) & 31`. Careful with the name: this is `ml - 4` of the
  sequence, 0 to 31, the field as it is in the token. The 4 is added at line 142.
- Lines 138 and 139: the escape, `s == 3072`. Its entry has bit 31 set, and `n` in the field
  `DROP`, so that the decoder can drop the escape's code before it reads the 12 bits of the token.
  All other fields are 0. `cls` and `raw_bits` were computed for it anyway, `3072 >> 9 = 6` and
  nibble 6 of `0x95C840`, which is 0, and are not used. The kernel's style declares variables at the
  top of a block, and since the function is inlined, the compiler drops what is not used.
- Lines 140 to 143: every other token. Each field is shifted to its place and the fields are ORed
  together. `<<` binds tighter than `|` in C, so `n | x << 6 | y << 12` is `n | (x << 6) | (y << 12)`
  without parentheses.

Three details of these lines need more than a sentence.

**`1U`, not `1`.** Line 139 writes `1U << TOK_ESCAPE_AT`. A plain `1` is a signed `int`, and
`1 << 31` gives 2<sup>31</sup>, which an `int` of 32 bits cannot hold: undefined behaviour in C11.
`1U` is unsigned, and `1U << 31` is `0x80000000`. Line 143 casts the result of `||`, an `int` 0 or 1,
to `u32` before the shift. `1 << 30` would still fit an `int`, so this one is not about undefined
behaviour; it keeps every term of the OR unsigned, as the function's `u32` result is.

**`ml + 4U`.** The entry stores the match length the decoder needs, 4 to 35, not the token's field
0 to 31. That is one add per sequence less in the decoder, which uses `TOK_ML(tok)` as the length to
copy. It also gives the decoder its two tests in the terms it thinks in: a length value follows if
`len == SEQLZ_ML_CAP + 4U`, 35, and the last sequence of a page must have `len == 4U`, a token with
`ml - 4 = 0`. The price is a field of 6 bits instead of 5, and the entry had the bit to spare: bit
29 is still free.

**`(64U - raw_bits) & 63U`.** This is `BACK`, the shift that brings the offset bits down. After the
decoder has shifted the code out on top of its 64-bit register, the offset's `raw_bits` bits are the
top bits, and shifting right by `64 - raw_bits` leaves exactly them. For class 3 that is
`64 - 12 = 52`. Class 0 has no offset bits, and `64 - 0 = 64`: a shift of a `u64` by 64 is undefined
in C, and 64 does not fit into 6 bits either. `& 63U` turns 64 into 0, and leaves 52, 55, 56, 59 and
60 as they are. So class 0's `BACK` is 0, and the decoder uses exactly that as its test for class 0,
`off = back ? raw : last`: with `back` 0 it ignores whatever `raw` became and takes the repeat
offset. Every other class has a `BACK` of at least 50 (class 3 on 16 KiB pages), never 0. One field is a shift and a flag at once,
and the decoder needs no compare of the class.

## The entries, worked out {#ch-internal--examples}

The numbers below are from the tables compiled in, `src/seqlz_default_tables_4k.inc`, and were
computed with a few lines of C that call `token_entry()` and `seqlz_tables_init()` from `src/`.

::: try Print any token's entry
```c
#include <stdio.h>
#include "seqlz_internal.h"

int main(void)
{
	unsigned int s = 2098, n = seqlz_default_own.token[s];
	u32 e = token_entry(s, n);

	printf("%#010x n %u back %u shift %u drop %u ll %u ml %u value %u\n",
	       e, TOK_N(e), TOK_BACK(e), TOK_SHIFT(e), TOK_DROP(e), TOK_LL(e),
	       TOK_ML(e), (e >> TOK_VALUE_AT) & 1);
	return 0;
}
```
Build it from the repository's root with
`gcc -O2 -Isrc t.c src/seqlz_default_tables.c` and run it.
:::

### Token 49: one literal, a match of 7, the repeat offset

Token 49 is `1 + 16 * 3 + 512 * 0`: `ll` 1, `ml - 4` 3, class 0. It is one of the three tokens with
the shortest code, 4 bits, `0001`, and `docs/seqlz.md` shows where it comes from: in an array of
pointers, each pointing 16 bytes further than the one before, every 8-byte pointer is one literal,
its low byte, which changes, and a match of its 7 other bytes, 8 bytes back, the same offset as the
sequence before
([seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#one-sequence-a-token-and-the-offset)).

| field | computation | value |
| --- | --- | --- |
| `N` | `n` | 4 |
| `BACK` | `(64 - 0) & 63` | 0 |
| `SHIFT` | `SEQLZ_OFF_SHIFT(0)` | 0 |
| `DROP` | `4 + 0` | 4 |
| `LL` | `49 & 15` | 1 |
| `ML` | `((49 >> 4) & 31) + 4` | 7 |
| value bit | `1 == 15 || 3 == 31` | 0 |

The entry is `4 | 4 << 14 | 1 << 19 | 7 << 23 = 0x03890004`, in binary
`0000 0011 1000 1001 0000 0000 0000 0100`. A 4-bit code is the top 4 bits of 128 of the 2048
indices, 128 to 255, and all 128 entries hold this number. The decoder looks up the top 11 bits of
its register, gets `0x03890004` whatever the 7 bits after the code are, computes a `raw` it then
ignores, takes `last` as the offset because `BACK` is 0, drops 4 bits, and has `ll` 1 and `ml` 7.
The value bit is 0, so the sequence can take the fast path.

### Token 2098: two literals, a match of 7, 16 bytes back

`docs/seqlz.md` uses this sequence as its example: 2 literals, a match of 7 bytes, offset 16. The
offset is a multiple of 8 below 256, class 4, sent as `16 / 8 = 2` in 5 bits. The token is
`2 + 16 * 3 + 512 * 4 = 2098`, and its code in the table has 9 bits, `101100111`.

| field | computation | value |
| --- | --- | --- |
| `N` | `n` | 9 |
| `BACK` | `(64 - 5) & 63` | 59 |
| `SHIFT` | `SEQLZ_OFF_SHIFT(4) = (4 >> 2) * 3` | 3 |
| `DROP` | `9 + 5` | 14 |
| `LL` | `2098 & 15` | 2 |
| `ML` | `((2098 >> 4) & 31) + 4` | 7 |
| value bit | `2 == 15 || 3 == 31` | 0 |

The entry is `0x0393bec9`, the upper row of the diagram above. Here is what the decoder does with it,
on these lines of `seqlz_decode()` ([chapter 14](#ch-decode)):

@listing src/seqlz_decompress.c 607-611

Say the register holds the code, then `00010`, then whatever follows: `0xb38bffffffffffff`, with the
first bit of the stream as bit 63. The top 11 bits are `10110011100`, index 1436, one of the 4
entries 1436 to 1439 of this 9-bit code. Then:

```text
br.bits                   1011 0011 1000 1011 1111 ... 1111    0xb38bffffffffffff
br.bits << TOK_N   (9)    0001 0111 1111 1111 1111 ... 0000    0x17fffffffffffe00
             >> TOK_BACK  (59)                         00010   2
             << TOK_SHIFT (3)                                  16
seqlz_br_drop(&br, TOK_DROP) moves on by 14 bits, to the next sequence
```

The left shift throws the code away, the right shift by 59 keeps the 5 bits that were behind it, and
the shift by 3 multiplies by 8. No branch on the class, no table of offset bits: the same three
shifts for every class, with the numbers from the entry.

### A class 3 token, and one with a length value

Token 1552 is `0 + 16 * 1 + 512 * 3`: no literals, a match of 5, an offset below the page size in 12
bits. On the two desktop dumps, 29% and 36% of the matches at a class 3 offset are 5 bytes long
([seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#one-sequence-a-token-and-the-offset)),
and this token has an 8-bit code.

| field | value |
| --- | --- |
| `N` | 8 |
| `BACK` | `64 - 12 = 52` |
| `SHIFT` | 0 |
| `DROP` | `8 + 12 = 20` |
| `LL`, `ML` | 0, 5 |
| value bit | 0 |

The entry is `0x02850d08`. The decoder computes `(br.bits << 8) >> 52`, the 12 bits after the code,
and moves on by 20 bits.

Token 1567 is the same with `ll` 15: `15 + 16 * 1 + 512 * 3`. Its code has 9 bits, and its entry
`0x42fd4d09` has `LL` 15 and bit 30 set, `ll == SEQLZ_LL_CAP`. The decoder sees bit 30, does not take
the fast path, and after the offset reads an `ll` value from the `ll` table and adds it to 15. The
same bit is set for `ml - 4 = 31`, e.g. token 2032, `ML` 35, entry `0x51850d08`.

### The escape

Symbol 3072 has a 4-bit code, `0011`, so it fills the indices 384 to 511 with
`token_entry(3072, 4) = 1U << 31 | 4 << 14 = 0x80010000`, the lower row of the diagram. The decoder
handles it before everything else:

@listing src/seqlz_decompress.c 589-601

Take token 1797, `5 + 16 * 16 + 512 * 3`: 5 literals, a match of 20, class 3. It is too rare to have
a code, its `enc[]` is 0, so the encoder writes the escape's `0011`, then 1797 in 12 bits,
`011100000101`, then the offset, say 1000, in 12 bits. The decoder's top 11 bits are
`00110111000`, index 440, in the escape's range. Bit 31 is set: it drops `TOK_DROP` = 4 bits,
refills, reads the top 12 bits, 1797, drops them, and checks `1797 < 3072`. Then it calls
`token_entry(1797, 0)`.

With `n = 0`, the entry describes a token whose code is already gone: `N` 0, `BACK` 52, `SHIFT` 0,
`DROP` `0 + 12 = 12`, `LL` 5, `ML` 20, `0x0a2b0d00`. From here the escaped token goes through the same
lines as any other: `(br.bits << 0) >> 52` is the next 12 bits, 1000, and the decoder moves on by 12.
This is the one place where the decoder does take a token apart at run time, by calling
`token_entry()`: for an escaped token, which is about 14 to 38 of some 200 sequences per page on the
project's dumps ([seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#one-sequence-a-token-and-the-offset)).
Those already pay for a refill and 12 more bits.

### The last sequence

The last sequence of a page has no match. Its token has `ml - 4 = 0` and class 0 by the format's rule
([chapter 4](#ch-format--validity)), e.g. token 0 when it has no literals, whose 8-bit code gives
`0x02020008`: `N` 8, `BACK` 0, `DROP` 8, `LL` 0, `ML` 4. When the decoder has filled the page with
this sequence's literals, it checks `len != 4U || TOK_BACK(tok) != 0U` and rejects the page if
either holds: the `ML` of 4 and the `BACK` of 0 that this chapter built are the format's rule for the
last token, read straight from the entry.
