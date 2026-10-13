# seqlz_codes.c: from code lengths to lookup tables {#ch-codes}

The format defines its 11 tables by their code lengths alone: one byte per symbol, {{tok.symbols}} for the
token table, {{ll.symbols}} each for the two tables of length values, 256 for each of the 8 literal tables.
That is {{ch08.length_bytes}} bytes, and they are compiled into the codec. Neither the encoder nor the decoder can
use them as they are. `src/seqlz_codes.c` turns them into `struct seqlz_tables`, the {{size.tables|grp}} bytes
of codes and decode tables that [chapter 7](#ch-internal) took apart: the encoder's code for every
symbol, the decoder's table entries for every index, and the literal costs the encoder uses to
pick a literal table. It runs once, at boot in the kernel, and never again.

The file is {{ch08.lines}} lines and does three things. `first_codes()` computes the
[canonical codes](#ch-compression--canonical-codes) from the lengths and checks that they are a
complete prefix code. `build_token()`, `build_values()` and `build_lit()` write each code into the
encoder's table and into the range of decode entries that starts with it. `seqlz_tables_init()`
calls them all. After that, this chapter looks at where the lengths come from: two small `.c` files
that include generated tables, the training that made them, and what the token table looks like.

## The size, for callers that cannot see the struct {#ch-codes--size}

@listing src/seqlz_codes.c 1-11

The SPDX line is the licence, as in every file of `src/` ([chapter 5](#ch-compat--top)). The comment
names the file's main function, the one that callers build the tables with. Line 6 includes
`seqlz_internal.h`, which brings `seqlz.h` and the layout of the tables.

`seqlz_tables_size()` returns `sizeof(struct seqlz_tables)`, {{size.tables|grp}} on x86-64. It exists because
`seqlz.h` only declares the struct, `struct seqlz_tables;`, without its fields. A file that includes
only `seqlz.h`, e.g. `src/zram_seqlz.c` or the C++ test harness, has an incomplete type and cannot
write `sizeof`. It calls this function and allocates that many bytes. The kernel's copy has one
global struct and no caller of this function, and `port.py` cuts it out.

## length_entry(): a length value's base and extra bits {#ch-codes--length-entry}

@listing src/seqlz_codes.c 13-23

A **length value** is the rest of an `ll` or `ml` that does not fit into the token
([chapter 4](#ch-format--length-values)). Its symbols 0 to 15 are the values 0 to 15. A symbol $s$
from 16 on stands for the $2^b$ values from $2^b$ on, $b = s - 12$, and its code is followed by $b$
plain bits that say which one. `length_entry()` computes the part of a decode entry that depends only
on the symbol, in the layout of [chapter 7](#ch-internal--decode-tables): the base value from bit 8,
the number of extra bits from bit 4. `build_values()` ORs the code's length into bits 0 to 3.

- `s < SEQLZ_LEN_DIRECT`, below 16: the base is `s`, no extra bits. Symbol 5 gives `5 << 8 = {{ch08.le.5}}`.
- otherwise: the base is `1U << (s - SEQLZ_LEN_LOG_BASE)`, `SEQLZ_LEN_LOG_BASE` being 12, and
  `s - 12` extra bits.

| symbol | values | base | extra bits | `length_entry()` |
| --- | --- | --- | --- | --- |
| 5 | 5 | 5 | 0 | `{{ch08.le.5}}` |
| 16 | 16 to 31 | 16 | 4 | `{{ch08.le.16}}` |
| 18 | 64 to 127 | 64 | 6 | `{{ch08.le.18}}` |
| 23 | 2048 to 4095 | 2048 | 11 | `{{ch08.le.23}}` |

@assert ch08.le_last_symbol == 23

Symbol 23 is the largest for 4 KiB pages, `SEQLZ_LEN_SYMBOLS - 1`, and its {{ch08.le_last_extra}} extra bits fit the
field of 4 bits; on 16 KiB pages symbol {{k16.ch08.le_last_symbol}} has {{k16.ch08.le_last_extra}}. The decoder's `seqlz_br_value()` ([chapter 12](#ch-dec-basics)) adds the base
and the extra bits; for the value 100 that is symbol 18, base 64, and the 6 bits `100100`, 36.

## first_codes(): the first code of each length {#ch-codes--first-codes}

@listing src/seqlz_codes.c 25-36

This comment is the whole theory of the file. A canonical Huffman code is a rule that turns lengths
into codes without any other information, the rule of deflate (RFC 1951, section 3.2.2) that
[format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#prefix-codes) specifies:
shorter codes come first, codes of the same length are consecutive numbers in the order of the
symbols, and each length starts where the one before ended, made one bit longer. `first_codes()`
computes, for each length `l`, the number of the first code of that length, `next[l]`. The builders
then hand out `next[l]`, `next[l] + 1`, ... to the symbols of length `l`, in symbol order.

The second paragraph is the check. A table is **complete** if its codes use up every bit pattern:
every sequence of `bits` bits starts with exactly one code. Then the decode table has no entry that
no code fills, and the decoder never has to ask whether the bits it looked up are a code. The
comment says Huffman codes always are complete: a Huffman code is built as a binary tree where every
inner node has two children, so no branch is left empty. A list of lengths from anywhere else, e.g.
a damaged table or a typo, might not be, and `first_codes()` returns `-EINVAL` for it.

Allowing only complete codes took out the decoder's check for bits that are no code, 6% of its
instructions, measured on the PC in September 2026 ([chapter 2](#ch-compression--complete)).

@listing src/seqlz_codes.c 37-57

Line by line:

- Lines 37 and 38: `len` is the list of `n` lengths, `bits` the longest code the table allows, 11 for
  tokens, 8 for length values, 10 for literals. `next[16]` is the output. As a parameter it is a
  pointer, `unsigned int *next`; the 16 says how large the caller's array must be, and
  `seqlz_internal.h` asserts that `bits` is below 16.
- Line 40: `of_len[l]` will count the symbols with a code of `l` bits. `= { 0 }` zeroes all 16. `k`
  sums up how much of the table the codes fill, `c` is the running code.
- Lines 42 to 46: count the lengths. The check comes before the increment, and that order matters: a
  length is a `u8`, up to 255, and `of_len[len[s]]` with a length of 20 would write past the array.
  A length above `bits` returns at once.
- Lines 47 and 48: a code of `l` bits fills `2^(bits - l)` entries of a table of `2^bits`: all the
  indices that start with it. `of_len[l] << (bits - l)` is how many entries the codes of length `l`
  fill together, and `k` sums that over all lengths from 1. Symbols without a code, length 0, are
  not counted.
- Lines 49 and 50: the codes must fill the table exactly, `k == 2^bits`. With more, there are not
  enough code numbers: three codes of 1 bit would get the numbers 0, 1 and 2, and 2 is no 1-bit code.
  Its range in a decode table of 8 entries would be 8 to 11, past the end, so this check also keeps
  the builders below inside their arrays. With fewer, some entries would belong to no code, and the
  decoder would read them as if they did. With no codes at all, `k` is 0.
- Line 51: `of_len[0]` counted the symbols without a code. The next loop reads `of_len[l - 1]` from
  `l = 1` on, so `of_len[0]` must be 0 there, or those symbols would take up code numbers.
  `format.md` says the same: "`count[0]` is taken as 0".
- Lines 52 to 55: `c = (c + of_len[l - 1]) << 1`. The codes of length `l - 1` are the numbers from
  `next[l - 1]` on, `of_len[l - 1]` of them; the first number after them is
  `next[l - 1] + of_len[l - 1]`. Doubling it appends a 0 bit, which makes it one bit longer, and that
  is the first code of length `l`. Since `c` holds `next[l - 1]` from the round before, the line is
  the rule `first[l] = (first[l-1] + count[l-1]) * 2` of `format.md`.

Lengths 1 to `bits` get a `next[]`; `next[0]` is never set and never read.

### A small example, step by step

Five symbols, the longest code 3 bits, with the lengths 2, 0, 3, 1, 3: symbol 1 has no code.

- Lines 42 to 46: `of_len[0] = 1`, `of_len[1] = 1`, `of_len[2] = 1`, `of_len[3] = 2`.
- Lines 47 to 50: `k = (1 << 2) + (1 << 1) + (2 << 0) = 4 + 2 + 2 = 8 = 1 << 3`. Complete.
- Line 51: `of_len[0] = 0`.
- Lines 52 to 55: `next[1] = (0 + 0) << 1 = 0`, `next[2] = (0 + 1) << 1 = 2`,
  `next[3] = (2 + 1) << 1 = 6`.

The builders then go through the symbols in order. Symbol 0, 2 bits, takes `next[2]`, 2, code `10`.
Symbol 1 has none. Symbol 2, 3 bits, takes 6, `110`. Symbol 3, 1 bit, takes 0, `0`. Symbol 4, 3
bits, takes 7, `111`. The shortest code went to symbol 3 although it comes later: shorter codes
first, then symbol order within a length.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 200" role="img" aria-label="Five symbols with code lengths 2, 0, 3, 1, 3, their canonical codes, and the decode table of 8 entries each code fills as one range">
  <g font-size="12">
    <text x="20" y="40">symbol</text><text x="100" y="40">length</text><text x="180" y="40">code</text>
    <text x="20" y="66">0</text><text x="100" y="66">2</text><text x="180" y="66">10</text>
    <text x="20" y="88">1</text><text x="100" y="88">0</text><text x="180" y="88">none</text>
    <text x="20" y="110">2</text><text x="100" y="110">3</text><text x="180" y="110">110</text>
    <text x="20" y="132">3</text><text x="100" y="132">1</text><text x="180" y="132">0</text>
    <text x="20" y="154">4</text><text x="100" y="154">3</text><text x="180" y="154">111</text>
  </g>
  <line x1="20" y1="47" x2="250" y2="47" stroke="currentColor" stroke-width="1"/>
  <g font-size="11" text-anchor="middle">
    <text x="353" y="40">000</text><text x="399" y="40">001</text><text x="445" y="40">010</text>
    <text x="491" y="40">011</text><text x="537" y="40">100</text><text x="583" y="40">101</text>
    <text x="629" y="40">110</text><text x="675" y="40">111</text>
  </g>
  <g stroke="currentColor" stroke-width="1">
    <rect x="330" y="50" width="184" height="40" class="fill-acc"/>
    <rect x="514" y="50" width="92" height="40" class="fill-sheet"/>
    <rect x="606" y="50" width="46" height="40" class="fill-mark"/>
    <rect x="652" y="50" width="46" height="40" class="fill-paper"/>
  </g>
  <g stroke="currentColor" stroke-width="1" stroke-dasharray="3 3">
    <path d="M376,50 V61 M376,81 V90 M422,50 V61 M422,81 V90 M468,50 V61 M468,81 V90" class="on-fill"/>
    <path d="M560,50 V61 M560,81 V90"/>
  </g>
  <g font-size="12" text-anchor="middle">
    <text x="422" y="75" class="on-fill">symbol 3</text><text x="560" y="75">symbol 0</text>
    <text x="629" y="75" class="on-fill">2</text><text x="675" y="75">4</text>
    <text x="422" y="110">code 0: entries 0 to 3</text><text x="560" y="110">10: 4, 5</text>
    <text x="629" y="110">110</text><text x="675" y="110">111</text>
  </g>
  <g font-size="12">
    <text x="330" y="145">next[1] = 0, next[2] = 2, next[3] = 6</text>
    <text x="330" y="168">k = 4 + 2 + 1 + 1 = 8 = 2^3: complete</text>
  </g>
</svg>
</div>
<figcaption>Canonical codes from lengths, and the decode table they fill: each code is the first bits of one range of indices.</figcaption>
</figure>

Three lists that fail, with `bits = 3`:

| lengths | `k` | why it fails |
| --- | --- | --- |
| 1, 1, 1 | $4 + 4 + 4 = 12$ | too many codes: there are only two codes of 1 bit |
| 1, 2, 0 | $4 + 2 = 6$ | a gap: the indices starting with `11` belong to no code |
| 0, 0, 0 | 0 | no code at all |
| 1, 2, 4 | | $4 > 3$: line 43 returns before `k` is computed |

The test `seqlz: tables that are no prefix code are rejected` in `test/seqlz_test.cpp` builds such
tables from the real ones: a length one too long, three codes of 1 bit, an empty table, a gap of one
11-bit code. It also has a case that only the length check catches: a complete token table plus one
code of 12 bits. The loop at lines 47 and 48 counts lengths up to 11 only, so `k` is still exactly
2048, and line 43 is what stops it.

### The real tables

The `ll` table has {{ll.symbols}} lengths: `{{ch08.ll.lengths}}`. Four codes of 3
bits, three of 4, six of 5, seven of 6, none of 7, four of 8:

```text
k = {{ll.codes_of.3}} * 32 + {{ll.codes_of.4}} * 16 + {{ll.codes_of.5}} * 8 + {{ll.codes_of.6}} * 4 + {{ll.codes_of.7}} * 2 + {{ll.codes_of.8}} * 1 = {{ch08.ll.fill.3}} + {{ch08.ll.fill.4}} + {{ch08.ll.fill.5}} + {{ch08.ll.fill.6}} + {{ch08.ll.fill.8}} = 256 = 1 << 8

next[1] = {{ch08.ll.next.1}}       next[5] = ({{ch08.ll.next.4}} + {{ll.codes_of.4}}) << 1 = {{ch08.ll.next.5}}
next[2] = {{ch08.ll.next.2}}       next[6] = ({{ch08.ll.next.5}} + {{ll.codes_of.5}}) << 1 = {{ch08.ll.next.6}}
next[3] = {{ch08.ll.next.3}}       next[7] = ({{ch08.ll.next.6}} + {{ll.codes_of.6}}) << 1 = {{ch08.ll.next.7}}
next[4] = ({{ch08.ll.next.3}} + {{ll.codes_of.3}}) << 1 = {{ch08.ll.next.4}}                   next[8] = ({{ch08.ll.next.7}} + {{ll.codes_of.7}}) << 1 = {{ch08.ll.next.8}}
```

@assert (ll.codes_of[1], ll.codes_of[2], ll.codes_of[3], ll.codes_of[4], ll.codes_of[5], ll.codes_of[6], ll.codes_of[7], ll.codes_of[8]) == (0, 0, 4, 3, 6, 7, 0, 4) and ll.len[0] == ll.len[1] == ll.len[16] == ll.len[17] == 3 and ll.len[2] == ll.len[3] == ll.len[4] == 4

The four 3-bit codes go to symbols 0, 1, 16 and 17, `{{ll.code.0}}`, `{{ll.code.1}}`, `{{ll.code.16}}` and `{{ll.code.17}}`: the values 0
and 1, literal runs of 15 and 16 bytes, and the values 16 to 63, which are the literal runs of 31 to 78
bytes. The 4-bit codes start at {{ch08.ll.next.4}}, `{{ll.code.2}}`, for symbols 2, 3 and 4. The four 8-bit codes are {{ch08.ll.next.8}} to 255, `{{ll.code.20}}` to
`{{ll.code.23}}`, the last four entries of the table. `next[7]` is {{ch08.ll.next.7}} although no code has 7 bits;
`next[8]` then continues from it.

The token table, with `bits = 11`, has no code shorter than 4 bits: `next[1]` to `next[4]` are 0, then
`next[5] = {{ch08.tok.next.5}}`, `next[6] = {{ch08.tok.next.6}}`, `next[7] = {{ch08.tok.next.7}}`, `next[8] = {{ch08.tok.next.8}}`, `next[9] = {{ch08.tok.next.9}}`, `next[10] = {{ch08.tok.next.10}}`
and `next[11] = {{ch08.tok.next.11}}`. Its {{tok.codes_of.11}} codes of 11 bits are the numbers {{ch08.tok.next.11}} to 2047, the last {{tok.codes_of.11}} entries.

@assert tok.codes_of[3] == 0 and tok.codes_of[4] > 0 and ch08.tok.next[11] + tok.codes_of[11] == 2048

## build_token(): a code is one range of the table {#ch-codes--build-token}

@listing src/seqlz_codes.c 59-82

`build_token()` fills `struct token_table` ([chapter 7](#ch-internal--token-table)):

- Line 66: `next[16]` is filled by `first_codes()` for the lengths 1 to 11, the only ones read.
- Lines 68 and 69: the canonical codes of all {{tok.symbols}} symbols, the escape included, with codes of at
  most `SEQLZ_TOKEN_BITS`. If they are not a complete code, the token table stays empty and the
  function fails.
- Lines 70 to 74: each symbol in order, 0 to 3072, `<=` because the escape is symbol 3072. A symbol
  without a code is skipped: its `enc[]` stays 0, which is how the encoder knows to send it escaped,
  and it gets no decode entries.
- Line 75: `c = next[l]++` gives this symbol the next free code of its length, and moves `next[l]` on
  by one for the next symbol of that length. That is the "consecutive numbers, in the order of the
  symbols" of the comment above `first_codes()`.
- Line 76: the encoder's entry, the code with its length at bit 12. The `(u16)` cast says that the
  narrowing from `unsigned int` is meant; `seqlz_internal.h` asserts that a code and its length fit.
- Lines 77 to 79: every index from `c << (11 - l)` up to, not including, `(c + 1) << (11 - l)` gets
  the entry `token_entry(s, l)`.

Why are a code's entries one range? Because the decoder reads the bitstream most significant bit
first: its index is the top 11 bits of its register, the first bit of the stream as the top bit of
the index. A code $c$ of $l$ bits is the first $l$ bits of the index, and the $11 - l$ bits after it
can be anything. All indices with the first $l$ bits equal to $c$ are $c \cdot 2^{11-l}$ up to
$(c + 1) \cdot 2^{11-l} - 1$, one block. Shifting $c$ left by $11 - l$ is that multiplication.

Token 49, one literal and a match of 7 at the repeat offset, has the {{tok.len.49}}-bit code `{{tok.code.49}}`, `c = {{ch07.tok.49.c}}`:

```text
first index   {{ch07.tok.49.c}} << (11 - {{tok.len.49}})        = {{ch07.tok.49.first}}   {{tok.code.49}} 0000000
last index    (({{ch07.tok.49.c}} + 1) << (11 - {{tok.len.49}})) - 1  = {{ch07.tok.49.last}}   {{tok.code.49}} 1111111
{{ch07.tok.49.entries}} entries of {{ch07.tok.49.entry}}, bytes {{ch08.tok49.byte_first}} to {{ch08.tok49.byte_last}} of the table: {{ch08.tok49.lines}} cache lines of 64 bytes
```

@assert tok.len[49] == 4 and ch07.tok[49].c == 0

The two codes of 4 bits are token 49 and the escape, `{{tok.code.49}}` and `{{tok.code.3072}}`. They take the
entries 0 to {{ch08.tok.len4.last}} in that order, the first KiB of the table, an eighth of it, in {{ch08.tok.len4.lines}} cache lines.
Token 2098, with its {{tok.len.2098}}-bit code `{{tok.code.2098}}`, `c = {{ch07.tok.2098.c}}`, has the {{ch07.tok.2098.entries}} entries {{ch07.tok.2098.first}} to {{ch07.tok.2098.last}}.

@assert tok.codes_of[4] == 2 and tok.len[49] == tok.len[3072] == 4 and ch08.tok.len4.bytes == 1024

The codes are ranges because the bitstream is read most significant bit first, since 6th October
2026; read the other way, a code's entries were spread over every cache line of the table.
[Chapter 3](#ch-cpu--msb-first) has the cache lines and the measurements. One more result belongs
here: on the Mi 9T's A76, leaving the token table out of the prefetches, as the decoder then did on
that core, made cold reads 1.3 to 2.6 µs faster in every run once the codes were ranges, and only in
some builds before
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept)).

The loop at lines 77 and 78 computes its end, `(c + 1U) << (11 - l)`, in every iteration. The
compiler can hoist it, and if not, it does not matter: the function runs once, and writes 2048
entries in all, because the ranges of a complete code cover the table exactly once.

## build_values(): the same, for length values {#ch-codes--build-values}

@listing src/seqlz_codes.c 84-103

The same steps for the `ll` and `ml` tables, with three differences: {{const.LEN_SYMBOLS}} symbols ({{k16.const.LEN_SYMBOLS}} on 16 KiB pages),
codes of at most `SEQLZ_MAX_BITS`, 8, and the entries of [chapter 7](#ch-internal--value-table). The
encoder's entry is `c | l << 16` in a `u32`, no cast needed. The decode entry is
`length_entry(s) | l`, the base and extra bits with the code's length in bits 0 to 3.

Symbol 18 of the `ll` table has the {{ll.len.18}}-bit code `{{ll.code.18}}`, `c = {{ch07.ll18.c}}`: `enc[18] = {{ch07.ll18.c}} | {{ll.len.18}} << 16 = {{ch07.ll18.enc}}`,
and the {{ch07.ll18.entries}} entries `{{ch07.ll18.c}} << 4 = {{ch07.ll18.first}}` to {{ch07.ll18.last}} get `{{ch08.le.18}} | {{ll.len.18}} = {{ch07.ll18.entry}}`.

@assert ll.len[18] == 5

The loop runs to `SEQLZ_LEN_SYMBOLS`, so `enc[{{const.LEN_SYMBOLS}}]` to `enc[31]`, which the encoder's array has for its
mask ([chapter 7](#ch-internal--decode-tables)), are not written here. They stay 0 from the
`memset()` in `seqlz_tables_init()`.

## build_lit(): the same, for a literal table {#ch-codes--build-lit}

@listing src/seqlz_codes.c 105-127

The third builder fills one `struct lit_table`: 256 symbols, the byte values, codes of at most
`SEQLZ_LIT_BITS`, 10, so 1024 decode entries. The encoder's entry is `l | c << 8`, length low and code
above it, because that is the order `lit_put()` needs ([chapter 7](#ch-internal--lit-table)). The
decode entry is `(u16)(l | s << 8)`, the length and the byte itself, cast to the 2 bytes of an entry.

In table 0, the space, `0x20`, and the letter `e`, `0x65`, have the first two 4-bit codes, `{{lit.0.code.32}}` and
`{{lit.0.code.101}}`: no code in table 0 is shorter than 4 bits, so `next[4]` is 0, and `0x20` comes first in
symbol order. The space fills the entries {{ch07.lit_space.first}} to {{ch07.lit_space.last}} with `{{ch07.lit_space.entry}}`, `e` the entries {{ch07.lit_e.first}} to {{ch07.lit_e.last}} with
`{{ch07.lit_e.entry}}`, and `enc[0x65]` is `{{lit.0.len.101}} | {{ch07.lit_e.c}} << 8 = {{ch07.lit_e.enc}}`.

@assert ch08.lit[0].min == 4 and lit[0].len[32] == lit[0].len[101] == 4 and lit[0].codes_of[4] >= 2

The comment is about bit order. The literals' 8 streams are read most significant bit first, like
the sequences' bitstream and like `zstd` reads its Huffman coded literals, so the codes go into the
tables as `first_codes()` numbers them, not bit-reversed. The decoder's side of that is in
[chapter 13](#ch-literals-dec).

## The literal tables and their costs {#ch-codes--lit-sets}

@listing src/seqlz_codes.c 129-150

`seqlz_all_symbols()` returns what `seqlz_tables_init()` worked out once, below. It is a function and
not a field the caller reads, because the struct's fields are not visible outside the codec.

The comment above `build_lit_sets()` points at a difference between the tables. `seqlz_tables_init()`
takes the token and length tables as a parameter, a `struct seqlz_lengths`, so that the tests can
build tables from other lengths, e.g. ones that are no prefix code. The 8 literal tables are not in
that struct. They always come from the global array `seqlz_lit_sets[8][256]` of
`seqlz_lit_sets.c`.

`build_lit_sets()` does two things:

- Lines 142 to 144: build the 8 literal tables, `lit[0]` to `lit[7]`, with `build_lit()`. If one of
  them is not a complete code, it fails.
- Lines 145 to 148: fill `lit_cost`, for each byte value its code length in all 8 tables, 8 bits per
  table, one `u64` ([chapter 7](#ch-internal--lit-cost)). One loop over `k` from 0 to 2047 does all
  of it: `k % 256` is the byte value, `k / 256` the table, `k / 256 / 8` the word, which is always 0
  with 8 tables, and `8U * (k / 256 % 8)` the lane's first bit.

The cast `(u64)` on line 147 is needed. `seqlz_lit_sets[j][b]` is a `u8`, and in an expression C
promotes it to `int`, 32 bits. Shifting an `int` by 56 for table 7 is undefined behaviour; shifting a
`u64` is not. The `|=` relies on `lit_cost` being 0 before, which the `memset()` of
`seqlz_tables_init()` ensures.

For the byte `0x65`, the 8 iterations with `k % 256 == 0x65` are `k = 0x65 + 256 * j`, and they OR
these lanes together:

```text
table j                  0    1    2    3    4    5    6    7
code length of 0x65{{ch08.lit101_row}}
as a byte, at bit 8 * j  {{ch07.lit_lane.101.0}}   {{ch07.lit_lane.101.1}}   {{ch07.lit_lane.101.2}}   {{ch07.lit_lane.101.3}}   {{ch07.lit_lane.101.4}}   {{ch07.lit_lane.101.5}}   {{ch07.lit_lane.101.6}}   {{ch07.lit_lane.101.7}}

lit_cost[0x65][0] = {{ch07.lit_cost.101}}    table 7 in the top byte, table 0 in the low byte
lit_cost[0x00][0] = {{ch07.lit_cost.0}}
```

The zero byte has a 2-bit code in tables 1 and 4 and a {{lit.0.len.0}}-bit one in table 0, which is for text.

@assert lit[1].len[0] == lit[4].len[0] == 2

## has_all_symbols(): can the encoder write every page {#ch-codes--all-symbols}

@listing src/seqlz_codes.c 152-170

The decoder can use any complete prefix code. If the lengths leave a symbol out, a page that uses
that symbol cannot be written, and the decoder never meets it: every bit pattern is the code of some
other symbol. The encoder is in a different position: it gets
whatever page zram hands it, and must have a code for every symbol that page might need. This
function checks that, once, for the lengths given:

- Lines 160 to 162: each of the 3072 tokens needs a code of its own, or the escape needs one, which
  the encoder then sends with the token in 12 bits.
- Line 164: the escape's code must be short enough. `SEQLZ_MAX_ESCAPE_LEN` is
  $31 - 12 - 12 = 7$ bits on 4 KiB pages, 5 on 16 KiB pages: an escaped token with class 3's offset
  is then at most 31 bits, which the encoder's bound on the compressed size relies on, so that zram's
  two pages are always room enough ([chapter 10](#ch-encoder)). The real escape has {{tok.len.3072}} bits. If the
  escape has no code, length 0, this line passes, and the token loop above is what fails, unless
  every token has a code.
- Lines 165 and 166: every length value needs a code in both tables. There is no escape for them.
- Lines 167 and 168: every byte needs a code in every literal table. A byte without a code would
  count 0 bits in `lit_cost`, so its table would look cheapest for exactly the pages it cannot code.

`all &= condition` instead of returning at the first failure keeps the function one straight list of
conditions; it runs once, so the extra iterations do not matter. The result, an `unsigned int` 0 or
1, becomes the `bool` the function returns.

The test `seqlz: the compressor needs a code for every symbol` in `test/seqlz_test.cpp` builds an
`ml` table whose last symbol has no code and that is still complete: the tables build, the
decoder can use them, `seqlz_all_symbols()` is false, and `seqlz_compress()` returns 0.

## seqlz_tables_init(): all of it, from a clean slate {#ch-codes--init}

@listing src/seqlz_codes.c 172-186

`seqlz_tables_init()` is the function that builds the tables. `seqlz.h` declares it with its
kernel-doc, next to the two small ones above, `seqlz_tables_size()` and `seqlz_all_symbols()`.

Line 179 sets all {{size.tables|grp}} bytes to 0 first. The comment gives two reasons, and the code relies on two
more:

- a token without a code must have `enc[]` 0, which is how `token_code()` knows to send it escaped
  ([chapter 7](#ch-internal--token-table)); the builders skip such symbols and write nothing for them;
- the entries behind the last symbol, `enc[{{const.LEN_SYMBOLS}}]` to `enc[31]` of the length values, must be defined,
  because the encoder's masked index could in principle reach them;
- `lit_cost` is built with `|=`, so it must start at 0;
- if a function is called twice on the same memory, as the tests do with other lengths, nothing of
  the first set of tables survives into the second.

Lines 180 to 183 build the tables in order. `||` evaluates from left to right and stops at the first
operand that is not 0, so the first table that fails ends the build, and the function returns
`-EINVAL`, the kernel's convention of a negative error number. The struct is then half built, and
the caller must not use it: `src/zram_seqlz.c` frees it and fails the zram device's setup. Line 184
stores whether the encoder can use the tables. Line 185 returns 0.

Note that a valid code that leaves symbols out returns 0 with `all_symbols` false. That is a table
the decoder can use and the encoder cannot, and the encoder's entry points `seqlz_compress()` and
`seqlz_encode()` check `t->all_symbols` and return 0 for every page, the value that otherwise means
the page does not fit. With the
tables of the format that never happens: the generator that wrote them checks it, see below.

## In the kernel: once, at boot {#ch-codes--kernel}

In the kernel, all of this runs once. `tools/kernel-port/port.py` makes every function of the file
`static __init`, cuts out `seqlz_tables_size()`, `seqlz_all_symbols()` and `has_all_symbols()`, and
appends `tools/kernel-port/api_codes.c`:

@listing tools/kernel-port/api_codes.c 1-30

`__init` code is freed after boot, `__ro_after_init` data is read-only after it, and
`subsys_initcall()` has the kernel call `seqlz_init()` at boot, before zram starts;
[chapter 15](#ch-zram-glue--init) explains all three.

So there is one `struct seqlz_tables` in the whole kernel, `seqlz_fixed_tables`, {{size.tables|grp}} bytes that
all CPUs share. `__ro_after_init` data takes room in the kernel image, so the image carries {{size.tables|grp}}
bytes of zeros until `seqlz_init()` fills them at boot. `docs/plan.md` notes this as a point for the cover letter to the
kernel's maintainers, and tables built at compile time as an open question, issue
[#153](https://github.com/martinus/quetschn/issues/153) of the project's GitHub repository.

The two `BUILD_BUG_ON()`s are compile-time checks like `static_assert()`: the kernel's names for the
work memory, `SEQLZ_MEM_COMPRESS` and `SEQLZ_MEM_DECOMPRESS`, must be the sizes the codec uses.

`seqlz_init()` returns what `seqlz_tables_init()` returns, and that cannot fail: since commit `4c42cc5`
the script that writes the kernel's tables checks the lengths first, see
[chapter 15](#ch-zram-glue--init).

Line 1 marks where the appended part starts in the kernel's `seqlz_codes.c`: from there on it is the
interface of `include/linux/seqlz.h`. The last lines are what a module needs. `module_exit()` names the
function the kernel calls when the module is unloaded; `seqlz_exit()` has nothing to free, the tables
are a global variable, but a module with an init function and no exit function cannot be unloaded at
all. `MODULE_LICENSE("Dual MIT/GPL")` is the kernel's spelling of the two licences, which counts as
GPL-compatible, and `MODULE_DESCRIPTION()` is the line `modinfo` shows. Built into the kernel,
`seqlz_exit()` is thrown away with the rest of the `__exit` code, since built-in code is never
unloaded.

## Where the code lengths come from {#ch-codes--lengths}

The lengths are in two small `.c` files that each include one of two generated `.inc` files, by page
size.

@listing src/seqlz_default_tables.c 1-19

`seqlz_default_tables.c` defines `seqlz_default_own`, the lengths of the token table and the two
tables of length values, by including `seqlz_default_tables_4k.inc` or, for 16 KiB pages,
`seqlz_default_tables_16k.inc`. `QUETSCHN_PAGE_BITS` is 12 unless the build sets it
([chapter 6](#ch-header)). The comment says three things that matter: the lengths are part of the
format, a page decodes only with the tables it was compressed with; they are generated from counts in
the repository; and the pages they were counted on are not the pages the benchmarks measure, so no
benchmark measures the tables on their own training data.

The head of the 4 KiB file and its first tokens:

@listing src/seqlz_default_tables_4k.inc 1-14

and its end, the last tokens, the escape, and the two tables of length values:

@listing src/seqlz_default_tables_4k.inc 200-213

Lines 1 to 8 are the generator's: the SPDX line in the kernel's order, `GPL-2.0-only OR MIT`, as
`port.py` writes it too, and the command that made the file, so that nobody edits it by hand.

`const struct seqlz_lengths seqlz_default_own = { .token = {...}, .ll = {...}, .ml = {...} };` is a C99
designated initializer, each field by name; being `const`, the compiler puts it in read-only data.
There are 16 lengths per line, so token `s` is at line `{{ch08.inc.tok_line}} + s / 16`, column `s % 16`. Line {{ch08.inc.tok34_line}} has
tokens 32 to 47, and its third number, token 34, is {{tok.len.34}}. Line {{ch08.inc.tok49_line}} has tokens 48 to 63, and token 49 is {{tok.len.49}}
as well. Line {{ch08.inc.esc_line}} is the escape, {{tok.len.3072}}. The `ll` and `ml` lines are the {{const.LEN_SYMBOLS}} lengths each that
`first_codes()` turned into codes above.

@assert ch08.inc.tok_line + 3 == 14 and 200 <= ch08.inc.esc_line <= 213 - 7

### Training: counts, then lengths

The `.inc` files are made by `tools/kernel-port/gen-seqlz-tables.py` from
`bench/seqlz_counts_4k.txt`. That file has how often each token, each `ll` and `ml` symbol, and each
byte as a literal occurred on the training pages, and for each literal table, how many pages it codes.
The generator turns counts into lengths:

- Each table is an optimal **length-limited** prefix code, by package-merge (Larmore and Hirschberg):
  the code with the fewest bits for these counts among those whose longest code is 11, 8 or 10 bits. A
  plain Huffman code has no such limit, and a rare symbol can get a code longer than the table.
- Every count gets 1 added, so every length value and every literal gets a code, even if it never
  occurred in training. That is what `has_all_symbols()` checks for.
- Only the most frequent tokens get a code. The generator tries the 64, 128, 192, ... most frequent
  ones plus the escape, computes the bits each choice costs on the training pages, the escaped tokens
  at their escape code plus 12 bits, and keeps the cheapest whose escape code is at most
  $31 - 12 - 12 = 7$ bits. For 4 KiB pages that is {{ch08.tok.coded}} tokens, {{ch08.tok.coded_64}} times 64.
- Before it writes anything, it checks the lengths as `seqlz_tables_init()` and `has_all_symbols()`
  do, and exits if they fail.

@assert ch08.tok.coded % 64 == 0

Three checks keep the generated files honest. `tools/seqlz-ref/check.sh`, which CI runs, regenerates
all four `.inc` files from the counts and fails if one differs by a byte, and checks that the
generator refuses lengths that `seqlz_tables_init()` refuses. `tools/seqlz-ref/seqlz_ref.py`, the
decoder written from the spec, checks the SHA-256 of every table's lengths against the table in
[format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#where-the-tables-come-from),
e.g. `{{ch08.tok4k_sha}}...` for the 4 KiB token table. And `quetschn-seqlz-train --from-counts` in C++ must give
the same tables, which the test `seqlz_train` checks.

::: try Check a table's hash yourself
From the repository's root:
```python
import hashlib, re
t = open("src/seqlz_default_tables_4k.inc").read()
tok = [int(x) for x in re.findall(r"\d+", re.search(r"\.token = \{(.*?)\}", t, re.S).group(1))]
print(len(tok), hashlib.sha256(bytes(tok)).hexdigest())
print(sum(1 for l in tok[:3072] if l), "tokens with a code, escape", tok[3072], "bits")
print(sum(2.0 ** -l for l in tok if l))
```
This prints {{tok.symbols}} and the `TOK_4k` hash of `format.md`, `{{ch08.tok.coded}} tokens with a code, escape {{tok.len.3072}} bits`, and
`1.0`: the sum of $2^{-l}$ over all codes, the Kraft sum, which is 1 for a complete code.
:::

The 4 KiB counts are from {{ch08.counts4k.pages|grp}} pages: the resident pages of a desktop, 60 132 pages of that
desktop's zram dump of 28th September 2026, and a zram dump of a Xiaomi Mi 9T, five times. Swapped
pages made the desktop's zram pages 3.0% smaller than tables trained without them, and each copy more
of the phone dump gave some of that back to the phone; with five, the phone was within 0.07% of the
tables before and the desktop 2.1% smaller. Trained on the measured pages themselves, which no real
table can be, the tables would be only 2.0% smaller on the desktop and 0.7% on the phone: that is the
bound of what any training of these tables could still get
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tables-trained-again-4-less-on-one-desktop-dump-the-phone-the-same)).

The pages themselves are not in the repository, because memory pages hold keys and passwords. The
counts are, and they cannot give those back: the token and length counts are sums over all {{ch08.counts4k.pages|grp}}
pages, each literal table's over at least {{ch08.counts4k.lit_min_pages}} pages (the phone's counted five times), and a count says
nothing about which symbol came after which or on
which page ([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#where-the-tables-come-from)).

### What the token table looks like

Of the 3072 tokens, {{ch08.tok.coded}} have a code of their own. With the escape, those are {{tok.with_code}} codes:

| code length | codes | share of the table's 2048 entries |
| --- | --- | --- |
| 4 | 3 tokens and the escape | ${{tok.codes_of.4}} \times 128 = {{ch08.tok.fill.4}}$ |
| 5 | {{tok.codes_of.5}} | ${{tok.codes_of.5}} \times 64 = {{ch08.tok.fill.5}}$ |
| 6 | {{tok.codes_of.6}} | ${{tok.codes_of.6}} \times 32 = {{ch08.tok.fill.6}}$ |
| 7 | {{tok.codes_of.7}} | ${{tok.codes_of.7}} \times 16 = {{ch08.tok.fill.7}}$ |
| 8 | {{tok.codes_of.8}} | ${{tok.codes_of.8}} \times 8 = {{ch08.tok.fill.8}}$ |
| 9 | {{tok.codes_of.9}} | ${{tok.codes_of.9}} \times 4 = {{ch08.tok.fill.9}}$ |
| 10 | {{tok.codes_of.10}} | ${{tok.codes_of.10}} \times 2 = {{ch08.tok.fill.10}}$ |
| 11 | {{tok.codes_of.11}} | ${{tok.codes_of.11}} \times 1 = {{ch08.tok.fill.11}}$ |

@assert tok.codes_of[1] + tok.codes_of[2] + tok.codes_of[3] == 0 and tok.codes_of[12] + tok.codes_of[13] + tok.codes_of[14] + tok.codes_of[15] + tok.codes_of[16] == 0 and tok.len[3072] == 4

The entries add up to 2048, the table is complete. The shortest codes say what memory pages are made
of:

| token | `ll` | `ml` | class | code |
| --- | --- | --- | --- | --- |
| 49 | 1 | 7 | 0, the repeat offset | `{{tok.code.49}}` |
| 34 | 2 | 6 | 0 | `{{tok.code.34}}` |
| 2097 | 1 | 7 | 4, a multiple of 8 below 256 | `{{tok.code.2097}}` |
| 2082 | 2 | 6 | 4 | `{{tok.code.2082}}` |
| 2112 | 0 | 8 | 4 | `{{tok.code.2112}}` |
| 2624 | 0 | 8 | 5, a multiple of 8 from 256 | `{{tok.code.2624}}` |

@assert tok.codes_of[4] == 2 and tok.codes_of[5] == 5 and tok.len[3072] == tok.len[49] == 4 and tok.len[34] == tok.len[2082] == tok.len[2097] == tok.len[2112] == tok.len[2624] == 5

In each of these 6 tokens, `ll + ml` is 8. That fits one sequence per 8-byte value, a pointer or a
field of 8 bytes, whose low 1 or 2 bytes differ from a value 8 or more bytes before, or none do. The
format's offset classes for multiples of 8 are there for the same reason
([chapter 4](#ch-format--offsets)).

The escape has a {{tok.len.3072}}-bit code, as short as the most frequent token's. Each of the {{ch08.tok.escaped}} tokens without a
code is rare, but together they are frequent: on desktop pages, 14 to 38 of about 200 sequences per
page take the escape, at 16 bits each before the offset
([seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#numbers-the-book-quotes)).
Giving more of them codes would take code space from the frequent ones: 11 bits have room for 2048
codes, fewer than the 3072 tokens. The other direction, a token table of 10 bits, in which 320
tokens got a code instead of 512 at the time, was measured on the Mi 9T and not taken; the numbers
are in [chapter 6](#ch-header--token).

## The literal tables {#ch-codes--lit-sets-file}

@listing src/seqlz_lit_sets.c 1-19

`seqlz_lit_sets.c` is the same construction for the 8 literal tables, `seqlz_lit_sets[8][256]`. Its
comment describes how they were made. **k-means** is a clustering method: start with 8 tables, put
each training page into the group of the table that codes its literals in the fewest bits, make each
table the Huffman code of its group's literals, and repeat until the groups stop changing. Where it
ends depends on where it starts, so the trainer runs it from 32 starting points and keeps the set with
the fewest bits on the training pages. The tables are then sorted by how many pages use them, table
0 first; the tables of 13th October 2026 are in the order that fits the tables before them best
instead, which keeps the table for text at 0. The order means nothing to the format.

@listing src/seqlz_lit_sets_4k.inc 1-17

Table 0 starts at line {{ch08.litinc.line}}, 16 bytes per line: byte `0x00` is {{lit.0.len.0}} bits, `0x0a`, the newline, {{lit.0.len.10}}, and line
{{ch08.litinc.line20}} has `0x20`, the space, at {{lit.0.len.32}}. Line {{ch08.litinc.line60}} is the bytes `0x60` to `0x6f`, the backtick and the letters
`a` to `o`: 4 to 8 bits, except the backtick and `j`, with `e` at {{lit.0.len.101}}. Table 0 is a table for text. The
other 7 differ: tables 1 and 4 give `0x00` 2 bits, table 7 gives `0xff` {{lit.7.len.255}} bits. The shortest code is
2 bits in {{ch08.lit.min2}} of the tables and 3 to 5 bits in the others, and every table has codes of 10 bits.

@assert ch08.litinc.line60 <= 17 and min(lit[0].len[0x61], lit[0].len[0x62], lit[0].len[0x63], lit[0].len[0x64], lit[0].len[0x65], lit[0].len[0x66], lit[0].len[0x67], lit[0].len[0x68], lit[0].len[0x69], lit[0].len[0x6b], lit[0].len[0x6c], lit[0].len[0x6d], lit[0].len[0x6e], lit[0].len[0x6f]) == 4 and max(lit[0].len[0x61], lit[0].len[0x62], lit[0].len[0x63], lit[0].len[0x64], lit[0].len[0x65], lit[0].len[0x66], lit[0].len[0x67], lit[0].len[0x68], lit[0].len[0x69], lit[0].len[0x6b], lit[0].len[0x6c], lit[0].len[0x6d], lit[0].len[0x6e], lit[0].len[0x6f]) == 8 and lit[0].len[0x60] > 8 and lit[0].len[0x6a] > 8 and lit[1].len[0] == lit[4].len[0] == 2 and lit[7].len[0xff] == 2 and ch08.lit[0].max == ch08.lit[1].max == ch08.lit[2].max == ch08.lit[3].max == ch08.lit[4].max == ch08.lit[5].max == ch08.lit[6].max == ch08.lit[7].max == 10

::: measured 8 literal tables, not 1, 16 or 64
k-means trained on the resident pages of a desktop and priced on its zram pages, as the share of
`seqlz-fast`'s output saved: 1 table 6.8%, 4 tables 9.2%, 8 tables of 10 bits 11.4%, 16 tables
11.9%, 64 tables 12.9%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
16 tables were built and measured in the kernel VM: 5 bytes per page less, and twice the decode
tables made cold reads 0.27 µs slower, so they were dropped
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)).
:::

## Fixed tables, part of the format {#ch-codes--fixed}

Everything in this chapter happens once, before the first page. No page carries a table, and no zram
device has tables of its own. That is a decision of the format, with numbers behind it:

- **Not per page.** "A page of 4 KiB is too small to carry tables of its own"
  ([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#the-idea)). A page's own
  literal table, where it saved 16 bytes, together with one of 4 token tables made writes 44% slower
  for pages 4.6% smaller
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#per-page-its-own-literal-table-and-one-of-4-token-tables-measured-not-kept)).
  And the decoder would have to build a table for every page, where now it reads one that is already
  built.
- **Not per device.** zram can hand a codec a dictionary per device, which could have carried tables
  trained on that device's pages. Trained on a zram dump of the Mi 9T, such tables saved 0.6% on other
  zram pages of the same phone and cost 1.0% on a desktop's; even on their own pages they saved only
  1.4%. So the tables became part of the format on 5th October 2026, and the
  dictionary is ignored, as `lzo` ignores it
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-format-written-down-one-set-of-tables-and-stream-sizes-that-hold)).
- **No version.** A page does not say which tables it was written with. zram's pages live only as
  long as the kernel that wrote them, so a kernel with new tables never reads a page written with the
  old ones ([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#no-version-in-the-page)).
  That is why the format could still be retrained on 7th October 2026: nobody uses it yet, and a new
  set of tables only needs new hashes in `format.md`.

The 16 KiB variants, `seqlz_default_tables_16k.inc` and `seqlz_lit_sets_16k.inc`, are the same
construction with counts from {{ch08.counts16k.pages|grp}} pages of a zram dump of the Android 17 emulator with 16 KiB
pages. They give {{k16.ch08.tok.coded}} tokens a code, again with a {{k16.tok.len.3072}}-bit escape, below that page size's limit of
$31 - 14 - 12 = 5$ bits. On zram pages of the emulator that were not in the training, they made
`seqlz-fast-lit` 2.6% smaller than the 16 KiB tables before them, which were trained on desktop pages
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#android-17-in-the-emulator-the-4-kib-tables-fit-the-16-kib-ones-trained-again-26-smaller)).
That is one emulator and no real phone, and the format for 16 KiB pages is not fixed yet; the
kernel's copy is built for 4 KiB pages only
([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#status)).
