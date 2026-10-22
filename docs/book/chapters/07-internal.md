# seqlz_internal.h: tables shaped for the decoder {#ch-internal}

`src/seqlz_internal.h` is what the three `.c` files of the codec share and nobody else sees: the
compressor in `seqlz_compress.c`, the decompressor in `seqlz_decompress.c`, and `seqlz_codes.c`, which
builds the tables both read. `seqlz.h` only declares `struct seqlz_tables`; this file says what is
inside. Almost all of its {{ch07.lines}} lines are memory layout: the bytes of each table, the bits
of each entry, and `static_assert()`s that the fields are wide enough.

The format stores a table as code lengths ([chapter 4](#ch-format--codes)). The encoder needs each
symbol's code as a number to shift into its bit buffer, the decoder a table it can index with the next
bits of the stream. `struct seqlz_tables` holds both forms of all {{ch17.tables}} tables, {{size.tables|grp}}
bytes, built once by `seqlz_tables_init()` ([chapter 8](#ch-codes)) and then only read, by every CPU.
The part to read closely is the end: `token_entry()` packs everything the decoder needs to know about
a token into one 32-bit number, so that the loop over the sequences never takes a token apart.

## The head of the file {#ch-internal--head}

@listing src/seqlz_tables.h 1-11

The kernel's copy renames the guard to `_LIB_SEQLZ_SEQLZ_INTERNAL_H`, after its place in `lib/seqlz/`.
The constants come from `seqlz.h` ([chapter 6](#ch-header)); this chapter uses their values for 4 KiB
pages.

## A decode table is indexed by the next bits {#ch-internal--decode-tables}

@listing src/seqlz_tables.h 12-35

The first paragraph of the comment is [table decoding](#ch-compression--table-decoding): a code of $l$
bits in a table of $2^L$ entries fills the $2^{L-l}$ entries that start with its bits, and they all say
the same, this symbol, $l$ bits long. An entry of a table of length values, e.g. symbol 18 of the `ll`
table, the values 64 to 127:

| bits | field | symbol 18 of `ll` |
| --- | --- | --- |
| 0 to 3 | the code's length | {{ll.len.18}} |
| 4 to 7 | extra bits after the code | 6 |
| 8 to 23 | the base value | 64 |

Its {{ll.len.18}}-bit code is `{{ll.code.18}}`, so the entry
`64 << 8 | 6 << 4 | {{ll.len.18}} = {{ch07.ll18.entry}}` fills the {{ch07.ll18.entries}} entries
{{ch07.ll18.first}} to {{ch07.ll18.last}} of the 256, and the value is `64 + read(6)`
(`seqlz_br_value()`, [chapter 12](#ch-dec-basics)). The largest base is {{ch07.len_base_max}} on
4 KiB pages and {{k16.ch07.len_base_max}} on 16 KiB pages, more than 8 bits, so the entry is a `u32`.

The third paragraph explains a habit of the encoder: it masks values that are already in range. A
kernel with UBSan, which distributions enable, adds a compare and a branch to every shift and index
the compiler cannot prove in range, and a mask is such a proof ([chapter 3](#ch-cpu--ubsan)). So
`enc_put()` shifts by `n & 63U` ([chapter 10](#ch-encoder)), and `put_len_value()` indexes the
length values' `enc[]` with `seqlz_len_symbol(v, &extra) & (ENC_LEN_SYMBOLS - 1U)`. The symbol is at
most {{ch08.le_last_symbol}} ({{k16.ch08.le_last_symbol}} on 16 KiB pages), and `& 31` changes nothing;
it only tells the compiler what the code knows. For that `enc[]` needs a power of 2 entries,
`ENC_LEN_SYMBOLS`, 32, where {{const.LEN_SYMBOLS}} would do ({{k16.const.LEN_SYMBOLS}} on 16 KiB
pages). On 23rd September 2026 the masks took the compressor to 4.7% fewer instructions
([chapter 3](#ch-cpu--ubsan) has the cycles).

The first `static_assert` keeps the mask from folding a real symbol onto another one, if a change
ever gave the page more bits. The second is for `first_codes()` ([chapter 8](#ch-codes--first-codes)),
which counts the codes of each length in arrays of 16 indexed by the length, so no table may allow
codes of 16 bits. The conditions are in parentheses, one per line, because the kernel's `checkpatch`
wants the continuation lines of a call aligned with its first argument
([chapter 15](#ch-zram-glue--check)).

## The table of length values {#ch-internal--value-table}

@listing src/seqlz_tables.h 37-40

`decode[256]` is the decoder's form, indexed by the top 8 bits of its bit register,
{{ch07.size.value_decode}} bytes. `enc[32]` is the encoder's, the code in the low 16 bits and its
length from bit 16 on: symbol 18 of `ll` is `{{ch07.ll18.c}} | {{ll.len.18}} << 16 = {{ch07.ll18.enc}}`,
which `enc_put_code()` takes apart with `entry >> 16 & 15U` and `entry & 0xffffU`
([chapter 10](#ch-encoder)). Both are in one struct of {{ch07.size.value_table}} bytes because one
computation makes them: `build_values()` ([chapter 8](#ch-codes--build-values)) works out each
symbol's code once and writes it to both. No function reads both, so a CPU loads only the cache lines
of the array it reads.

## The token table: 8 KiB for the decoder, 6 KiB for the encoder {#ch-internal--token-table}

@listing src/seqlz_tables.h 42-57

The decode table has 2048 entries of 4 bytes, 8 KiB; what an entry holds is the subject of
[the last sections](#ch-internal--token-entry). The encoder's `enc[]` has one `u16` per symbol, the
3072 tokens and the escape, {{ch07.size.token_enc}} bytes. The compressor reads the matcher's hash
table of 8 KiB at every position of the page ([chapter 9](#ch-matcher)) and `enc[]` for every
sequence, and 14 KiB together leave room in an L1 data cache of 32 KiB for the page and the rest.
Until 23rd September 2026 the entries were `u32`, {{ch07.size.token_enc_u32|grp}} bytes, 20 KiB with
the hash table (commit `c84ba25`).

An entry is the code in bits 0 to 11 and its length in bits 12 to 15, as `token_code()` reads them
([chapter 10](#ch-encoder--codes)):

| token | what it is | code | length | `enc[]` |
| --- | --- | --- | --- | --- |
| 49 | 1 literal, match of 7, repeat offset | `{{tok.code.49}}` | {{tok.len.49}} | `{{ch07.tok.49.enc}}` |
| 3072 | the escape | `{{tok.code.3072}}` | {{tok.len.3072}} | `{{ch07.tok.3072.enc}}` |
| 1797 | 5 literals, match of 20, class 3 | none | {{tok.len.1797}} | `{{ch07.tok.1797.enc}}` |
@assert tok.len[1797] == 0 and ch07.tok[49].c == 0

Token 49's code is the number 0, but its entry is not 0, because the length is in it too. So an entry
of 0 means only one thing, no code, and `token_code()` then writes the escape's code and the token in
12 plain bits; no flag is needed. The two asserts check that 11 bits of code fit below bit 12 and 4
bits of length fit the `u16`.

## The literal tables {#ch-internal--lit-table}

@listing src/seqlz_tables.h 59-62

Each of the 8 literal tables has two forms again. `decode[1024]` has entries of 2 bytes, the code's
length in the low byte and the literal in the high byte. In table 0, byte `0x65`, the letter `e`, has
the {{lit.0.len.101}}-bit code `{{lit.0.code.101}}`, so the {{ch07.lit_e.entries}} entries
{{ch07.lit_e.first}} to {{ch07.lit_e.last}} are `{{lit.0.len.101}} | 0x65 << 8 =
{{ch07.lit_e.entry}}`. A byte and a length of at most 10 need no more, and the page's literal table is
half the cache lines it would be with 4 bytes. `enc[256]` has the length in the low byte and the code
from bit 8 on: `{{lit.0.len.101}} | {{ch07.lit_e.c}} << 8 = {{ch07.lit_e.enc}}` for `e`.

Here the length is below the code, in the other tables above it, because of what `lit_put()` does
with an entry ([chapter 11](#ch-literals-enc--lit_put-one-code-into-an-accumulator)): `acc = acc << (e & 63) | e >> 8`, then
`*sum += e`. The shift count is the low bits as they are, the code one shift away, and adding whole
entries adds up the lengths in the low byte of `sum`; the code bits above it are dropped later with
`& 255`. The 8 tables are {{ch07.size.lit_table}} bytes each, {{ch07.size.lit|grp}} together, and a
page reads one of them.

## One add per literal for all 8 tables {#ch-internal--lit-cost}

@listing src/seqlz_tables.h 64-71

To choose the literal table, the encoder needs the sum of the page's code lengths in each of the 8
tables: naively 8 lookups and 8 adds per literal. `lit_cost[b]` is a `u64` whose byte $j$ is the code
length of byte value `b` in table $j$, so one lookup and one add per literal sum all 8, as long as no
lane passes 255: SWAR, SIMD within a register ([chapter 3](#ch-cpu--swar)). With the real tables:

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
    <text x="4" y="69" font-size="11">{{ch07.lit_cost.0}}</text>
    <text x="4" y="97">+ lit_cost[0x65]</text>
    <text x="4" y="111" font-size="11">{{ch07.lit_cost.101}}</text>
    <text x="4" y="147">= the sums</text>
    <text x="4" y="161" font-size="11">{{ch07.lit_cost.sum}}</text>
  </g>
  <g stroke="currentColor" stroke-width="1">
    <rect x="200" y="40" width="480" height="30" class="fill-sheet"/>
    <rect x="200" y="82" width="480" height="30" class="fill-sheet"/>
    <rect x="200" y="132" width="480" height="30" class="fill-paper"/>
    <rect x="380" y="132" width="60" height="30" class="fill-acc"/>
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
    <text x="230" y="60">{{lit.7.len.0}}</text><text x="290" y="60">{{lit.6.len.0}}</text><text x="350" y="60">{{lit.5.len.0}}</text>
    <text x="410" y="60">{{lit.4.len.0}}</text><text x="470" y="60">{{lit.3.len.0}}</text><text x="530" y="60">{{lit.2.len.0}}</text>
    <text x="590" y="60">{{lit.1.len.0}}</text><text x="650" y="60">{{lit.0.len.0}}</text>
    <text x="230" y="102">{{lit.7.len.101}}</text><text x="290" y="102">{{lit.6.len.101}}</text><text x="350" y="102">{{lit.5.len.101}}</text>
    <text x="410" y="102">{{lit.4.len.101}}</text><text x="470" y="102">{{lit.3.len.101}}</text><text x="530" y="102">{{lit.2.len.101}}</text>
    <text x="590" y="102">{{lit.1.len.101}}</text><text x="650" y="102">{{lit.0.len.101}}</text>
    <text x="230" y="152">{{ch07.lit_sum.7}}</text><text x="290" y="152">{{ch07.lit_sum.6}}</text><text x="350" y="152">{{ch07.lit_sum.5}}</text>
    <text x="410" y="152" class="on-fill">{{ch07.lit_sum.4}}</text><text x="470" y="152">{{ch07.lit_sum.3}}</text><text x="530" y="152">{{ch07.lit_sum.2}}</text>
    <text x="590" y="152">{{ch07.lit_sum.1}}</text><text x="650" y="152">{{ch07.lit_sum.0}}</text>
  </g>
</svg>
</div>
<figcaption>Two literals, 0x00 and 0x65, priced in all 8 literal tables with one 64-bit add; table {{ch07.lit_best}} codes them in {{ch07.lit_best_bits}} bits.</figcaption>
</figure>

The zero byte has a 2-bit code in tables 1 and 4, which are for pages with many zeros; `e` has a
{{lit.0.len.101}}-bit code in table 0, which fits text. For these two literals table
{{ch07.lit_best}} wins with {{ch07.lit_best_bits}} bits, against 16 as raw bytes;
[chapter 11](#ch-literals-enc--pricing) prices four literals the same way.
@assert lit[1].len[0] == lit[4].len[0] == 2 and min(lit[0].len[0], lit[2].len[0], lit[3].len[0], lit[5].len[0], lit[6].len[0], lit[7].len[0]) > 2 and ch07.lit_best == 4

`LIT_COST_WORDS` is 1 word of 8 lanes; written this way it would grow with more tables, but byte 2 of
the format holds the number at 8. `LIT_COST_RUN` is `255 / 10 = 25`: 25 codes of at most 10 bits sum
to at most 250, 26 could carry into the next lane. So `lit_cost()` ([chapter 11](#ch-literals-enc))
adds 25 literals per stream into 8-bit lanes, then spreads them into two words of 16-bit lanes with
the mask `0x00ff00ff00ff00ff` and starts again, every 200 literals of the page.

## Byte 2 and the largest stream {#ch-internal--byte2}

@listing src/seqlz_tables.h 72-78

These asserts tie the constants of `seqlz.h` to byte 2 of a page with coded literals
([chapter 4](#ch-format--layout)): the 8 fixed tables are the codes 0 to 7, the page's own table the
code after them, 8, and all of them fit the 4 bits below the width; 3 bits hold the 8 widths 5 to 12,
and the width field ends where the delta bit, bit 7, starts. The first assert would fail for a 9th fixed
table, which would collide with `SEQLZ_LIT_OWN`, and the decoder's `s[2] & (SEQLZ_LIT_SETS - 1U)` would
no longer pick the table.

@listing src/seqlz_tables.h 79-87

`SEQLZ_DELTA_LAST` stores a literal as its delta to the byte as far back as the offset of the sequence
before ([chapter 6](#ch-header--literals)), and the encoder's `seqlz_delta_literals()` and the decoder's
`undelta_literals()` and `undelta_fast()` both need that distance, so it is here, in the header both
include. An offset of 4 or more is the distance as it is; below 4 it is the smallest multiple of the
offset that is at least 4:

| `last` | 1 | 2 | 3 | 4 | 5 | 8 |
| --- | --- | --- | --- | --- | --- | --- |
| `seqlz_delta_last(last)` | {{delta.last.1}} | {{delta.last.2}} | {{delta.last.3}} | {{delta.last.4}} | {{delta.last.5}} | {{delta.last.8}} |
@assert delta.last[1] == delta.last[2] == delta.last[4] == 4 and delta.last[3] == 6 and delta.last[8] == 8

A match with offset 3 repeats a pattern of 3 bytes, so the byte 6 back is the byte 3 back, the one the
match would have copied next; with offset 1 or 2, the byte 4 back is. The decoder relies on the
distance being at least 4: it adds the bytes back 4 at a time, and each of the 4 bytes it adds must
already be the page's own, so none of them may lie in the word it writes.

@listing src/seqlz_tables.h 89-90

`LIT_STREAM_MAX` is the largest stream, every 8th
literal with a code of 10 bits, $(512 \cdot 10 + 7) / 8 = {{ch07.lit_stream_max}}$ bytes on 4 KiB
pages and {{k16.ch07.lit_stream_max}} on 16 KiB pages, and the assert proves that a size of 12 bits
holds it, so the encoder can write the sizes whatever the literals are. The last assert guards code,
not the format: the encoder's `lit_streams()` and the decoder's `seqlz_decode_literals()`
([chapter 13](#ch-literals-dec)) are written out by hand for 8 streams.

## The code of a page's own table, and two lengths per lookup {#ch-internal--own}

@listing src/seqlz_tables.h 92-116

`OWN`, the fixed code that describes a page's own literal table ([chapter 4](#ch-format--own)), gets
three arrays. `enc` is the encoder's, the code in the low byte and its length above it, the other way
round from `struct lit_table`'s `enc`. `decode` is indexed by the next 8 bits, as the token's decode
table is by the next 11: the symbol in the low byte and the code's length above it. And `quad`, also
256 entries, decodes more than one symbol per lookup.

A description is a chain like a literal stream: where the next symbol starts is known only once this
one is decoded. Most of its symbols are lengths of 7 to 10 bits, which have codes of 2 and 3 bits, so
8 bits often hold two of them whole, sometimes with a run before one of them. `quad[k]` has, for the 8
bits `k`, up to two lengths, how many bits each takes from the start of `k` with the run before it,
and the runs. E.g. for the 8 bits `{{own.q.d1.bits}}`: `{{own.code.10}}` is a run of
{{own.q.d1.run1}}, `{{own.code.8}}` the length {{own.q.d1.l1}}, `{{own.code.9}}` the length
{{own.q.d1.l2}}, so `quad[0xd1]` is `{{own.q.d1.entry}}`:

| bits | field | value |
| --- | --- | --- |
| 0 to 3 | the first length | {{own.q.d1.l1}} |
| 4 to 7 | the second | {{own.q.d1.l2}} |
| 8 to 11 | bits up to the end of the first, run and length | {{own.q.d1.end1}} |
| 12 to 15 | bits up to the end of the second | {{own.q.d1.end2}} |
| 16 to 18 | the run before the first | {{own.q.d1.run1}} |
| 19 to 21 | the run before the second | {{own.q.d1.run2}} |
| 31 | alone | {{own.q.d1.alone}} |
@assert own.q.d1.l1 == 9 and own.q.d1.l2 == 10 and own.q.d1.end1 == 6 and own.q.d1.end2 == 8 and own.q.d1.run1 == 1 and own.q.d1.alone == 0

`OWN_QUAD_NONE`, {{own.quad_none}}, marks a missing second length; it is no length, and the decoder
turns it into a bucket nobody reads and $1024 \gg 11 = 0$ code space, so that one entry with one
length needs no branch ([chapter 13](#ch-literals-dec--own)). A run of more than 7 does not fit 3 bits,
and the long runs have codes of 8 bits anyway. 8 bits that start with such a run, e.g.
`{{own.q.fc.bits}}`, symbol {{own.q.fc.sym}}, a run of 32 whose 5 more bits are in the next byte, get
only bit 31, `{{own.q.fc.entry}}`: the decoder then takes the symbol from `decode[]` and the run's bits
one by one. [Chapter 8](#ch-codes--own) builds the three arrays.
@assert own.q.fc.alone == 1 and own.q.fc.sym == 15

## struct seqlz_tables: {{size.tables|grp}} bytes, once {#ch-internal--tables}

@listing src/seqlz_tables.h 118-128

The layout on x86-64, from `sizeof` and `offsetof`; arm64 aligns a `u64` the same way:

| field | offset | bytes | read by |
| --- | --- | --- | --- |
| `token.decode` | 0 | {{ch07.size.token_decode}} | decoder, every sequence |
| `token.enc` | {{ch07.off.token_enc}} | {{ch07.size.token_enc}} | encoder, every sequence |
| `lit[0]` to `lit[7]` | {{ch07.off.lit|grp}} | $8 \times {{ch07.size.lit_table}} = {{ch07.size.lit|grp}}$ | decoder: one `decode`; encoder: one `enc` |
| (padding) | {{ch07.off.pad|grp}} | {{ch07.pad}} | |
| `lit_cost` | {{ch07.off.lit_cost|grp}} | {{ch07.size.lit_cost}} | encoder, every literal |
| `own` | {{ch07.off.own|grp}} | {{ch07.size.own}} | encoder and decoder, a page's own table |
| `ll` | {{ch07.off.ll|grp}} | {{ch07.size.value_table}} | both, for long literal runs |
| `ml` | {{ch07.off.ml|grp}} | {{ch07.size.value_table}} | both, for long matches |
| `all_symbols` | {{ch07.off.all_symbols|grp}} | 1, then {{ch07.tail_pad}} of padding | the harness's encoder |

The 16 KiB build has the same size, {{k16.size.tables|grp}} bytes: its length tables have
{{k16.const.LEN_SYMBOLS}} symbols, and `enc[]` has 32 entries either way. A page touches much less.
The decoder reads `token.decode`, the `decode` of `ll` and `ml`, and with coded literals that of one
literal table, $8 + 1 + 1 + 2 = 12$ KiB, which fits the L1 cache ([chapter 3](#ch-cpu--cache)); with a
table of the page's own, `own.quad` and `own.decode`, 1.5 KiB, instead of the 2 KiB literal table,
and it builds the 2 KiB table in the page. The
encoder reads `token.enc`, 6 KiB, the `enc` of `ll` and `ml`, `lit_cost`, 2 KiB, and one literal
table's `enc`, 1 KiB, at level 3 also `own.enc`.

`all_symbols` is what `seqlz_all_symbols()` returns ([chapter 8](#ch-codes--all-symbols)); `port.py`
deletes it in the kernel's copy, whose tables are checked before they are compiled in. There the
struct is one global variable, `seqlz_fixed_tables`, built at boot and read-only after it
([chapter 15](#ch-zram-glue--init)). The userspace harness, `src/zram_seqlz.c`, allocates one per
zram device.

## The fields of a token entry {#ch-internal--token-entry}

@listing src/seqlz_tables.h 130-137

The asserts check the fields of the entry described next. The 5 bits of `DROP` hold up to 31, and the
most a token with a code of its own moves the bitstream is an 11-bit code plus class 3's offset bits,
$11 + 12 = 23$, 25 on 16 KiB pages. `ll` has 4 bits, `ml` 6 bits for at most 35, the shift 2 bits;
`SEQLZ_OFF_SHIFT()` reads a whole nibble ([chapter 6](#ch-header--offsets)), so a shift of 4 would fail
here instead of being cut to 0. Class 5's offsets must stay inside the page, and 12 bits must hold
every token.

@listing src/seqlz_tables.h 138-154

Every field is something the decoder would otherwise compute from the token's number for every
sequence: split off `ll`, `ml` and the class, look up the class's offset bits and shift, add 4 to
`ml`, compare `ll` and `ml` with their caps. On an in-order core such as the Cortex-A55 an instruction
saved is time saved, almost one for one ([chapter 3](#ch-cpu--in-order)).

::: measured The token unpacked in the table: 1418 fewer instructions per page on the A55
On the Cortex-A55 of a Xiaomi Mi 9T, `simpleperf` showed that 18 of about 60 instructions per
sequence went into unpacking the token. With 4-byte entries holding the fields ready to use, the
decode loop went from 16 547 to 15 539 instructions per page and from 16 247 to 15 776 cycles, on
phone pages. The bit for "a length value follows" saved another 410 instructions and no cycles: there
the chain from one token's lookup to the next was the limit. The table grew from 4 KiB to 8 KiB, which
did not make cold reads slower. That was 3rd October 2026, commit `5c769f5`
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).
:::

The entry, bit 31 on the left, for token 2098 with its {{tok.len.2098}}-bit code and for the escape
with its {{tok.len.3072}}-bit code:

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
    <text x="50" y="42" class="on-fill">E</text><text x="70" y="42">V</text><text x="90" y="42">-</text>
    <text x="160" y="42">ML</text><text x="260" y="42">LL</text><text x="350" y="42" class="on-fill">DROP</text>
    <text x="420" y="41" class="on-fill" font-size="10">SHIFT</text><text x="500" y="42" class="on-fill">BACK</text><text x="620" y="42" class="on-fill">N</text>
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
    <text x="50" y="83">{{ch07.tok.2098.bits.escape}}</text><text x="70" y="83">{{ch07.tok.2098.bits.value}}</text><text x="90" y="83">{{ch07.tok.2098.bits.free}}</text>
    <text x="160" y="83">{{ch07.tok.2098.bits.ml}}</text><text x="260" y="83">{{ch07.tok.2098.bits.ll}}</text><text x="350" y="83">{{ch07.tok.2098.bits.drop}}</text>
    <text x="420" y="83">{{ch07.tok.2098.bits.shift}}</text><text x="500" y="83">{{ch07.tok.2098.bits.back}}</text><text x="620" y="83">{{ch07.tok.2098.bits.n}}</text>
  </g>
  <g font-size="12" text-anchor="middle" class="acc">
    <text x="160" y="108">ml {{ch07.tok.2098.ML}}</text><text x="260" y="108">ll {{ch07.tok.2098.LL}}</text><text x="350" y="108">{{ch07.tok.2098.DROP}}</text>
    <text x="420" y="108">{{ch07.tok.2098.SHIFT}}</text><text x="500" y="108">{{ch07.tok.2098.BACK}}</text><text x="620" y="108">{{ch07.tok.2098.N}}</text>
  </g>
  <g font-size="11">
    <text x="40" y="128">{{ch07.tok.2098.entry}}: a {{tok.len.2098}}-bit code, then 5 offset bits, the offset is them times 8</text>
  </g>
  <g font-size="12" text-anchor="middle">
    <text x="50" y="165">{{ch07.tok.3072.bits.escape}}</text><text x="70" y="165">{{ch07.tok.3072.bits.value}}</text><text x="90" y="165">{{ch07.tok.3072.bits.free}}</text>
    <text x="160" y="165">{{ch07.tok.3072.bits.ml}}</text><text x="260" y="165">{{ch07.tok.3072.bits.ll}}</text><text x="350" y="165">{{ch07.tok.3072.bits.drop}}</text>
    <text x="420" y="165">{{ch07.tok.3072.bits.shift}}</text><text x="500" y="165">{{ch07.tok.3072.bits.back}}</text><text x="620" y="165">{{ch07.tok.3072.bits.n}}</text>
  </g>
  <g font-size="12" text-anchor="middle" class="mark">
    <text x="50" y="190">esc</text><text x="350" y="190">{{ch07.tok.3072.DROP}}</text>
  </g>
  <g font-size="11">
    <text x="40" y="214">{{ch07.tok.3072.entry}}: drop the escape's {{tok.len.3072}} bits, then read the token in 12 bits</text>
  </g>
</svg>
</div>
<figcaption>A token table entry: bits 0 to 18 tell the decoder how to cut the offset out of the bitstream, bits 19 to 28 are the lengths, the top bits flags.</figcaption>
</figure>

@listing src/seqlz_tables.h 155-168

The macros shift a field down and mask it to its width; bits 30 and 31 the decoder tests directly,
and bit 29 is free. `TOK_N()` and `TOK_BACK()` are shift counts for the decoder's 64-bit bit
register, and their mask of 63 is again a proof for the compiler that the shift is defined, so
`CONFIG_UBSAN_SHIFT` has nothing to check there.

## token_entry(): the fields from a token's number {#ch-internal--token-entry-code}

@listing src/seqlz_tables.h 170-185

`token_entry(s, n)` is the entry for token `s` with a code of `n` bits. It is in the header because
two files call it: `build_token()` ([chapter 8](#ch-codes--build-token)) fills the table with it, and
`seqlz_decode()` calls it at run time for an escaped token, with `n = 0`. For the escape the entry is
bit 31 and `n` in `DROP`, so the decoder can drop the escape's code before it reads the token's 12
bits; `cls` and `raw_bits` are computed for it too, class 6, nibble 6 of `{{ch06.RAW_NIBBLES}}`, 0,
and the compiler drops them. The `(u32)` cast keeps every term of the OR unsigned. Two fields are
not the token's own numbers.

**`ml + 4U`.** The entry stores the match length itself, 4 to 35, not the token's field. That is one
add per sequence less in the decoder, which copies `TOK_ML(tok)` bytes, and its test for a length
value is `len == SEQLZ_ML_CAP + 4U`. The price is 6 bits instead of 5, and bit 29 is still free.

**`(64U - raw_bits) & 63U`.** This is `BACK`, the right shift that brings the offset bits down after
the code was shifted out on top of the 64-bit register: for class 3, $64 - 12 = 52$. Class 0 has no
offset bits, and a shift of a `u64` by 64 is undefined, and 64 does not fit 6 bits either. `& 63U`
turns it into 0 and leaves 52, 56, 59 and 63 as they are. So class 0's `BACK` is 0, and the decoder
uses that as its test, `off = back ? raw : last`: every other class has a `BACK` of at least 50
(class 3 on 16 KiB pages). One field is a shift and a flag at once, and the decoder needs no compare
of the class.

## The entries, worked out {#ch-internal--examples}

::: try Print any token's entry
```c
#include <stdio.h>
#include "seqlz.h"

int main(void)
{
	unsigned int s = 2098, n = seqlz_default_lengths.token[s];
	u32 e = token_entry(s, n);

	printf("%#010x n %u back %u shift %u drop %u ll %u ml %u value %u\n",
	       e, TOK_N(e), TOK_BACK(e), TOK_SHIFT(e), TOK_DROP(e), TOK_LL(e),
	       TOK_ML(e), (e >> TOK_VALUE_AT) & 1);
	return 0;
}
```
@program ch07-entry

Build it from the repository's root with `gcc -O2 -Isrc t.c src/seqlz_default_tables.c` and run it.
It prints

@output ch07-entry
:::

### Token 2098: two literals, a match of 7, 16 bytes back

[Chapter 4](#ch-format--token) uses this sequence as its example: offset 16 is a multiple of 8 below
256, class 4, sent as $16 / 8 = 2$ in 5 bits. Its code has {{tok.len.2098}} bits, `{{tok.code.2098}}`,
and the entry is `{{ch07.tok.2098.entry}}`, the upper row of the diagram: `N` {{ch07.tok.2098.N}},
`BACK` `(64 - 5) & 63` = {{ch07.tok.2098.BACK}}, `SHIFT` {{ch07.tok.2098.SHIFT}}, `DROP`
${{tok.len.2098}} + 5 = {{ch07.tok.2098.DROP}}$, `LL` {{ch07.tok.2098.LL}}, `ML`
{{ch07.tok.2098.ML}}. The decoder uses it on these lines of `decode_block()`, the body of `seqlz_decode()`
([chapter 14](#ch-decode)):

@listing src/seqlz_decompress.c 491-495

Say the register holds the code, then `{{ch07.reg_raw_bits}}`, then whatever follows:
`{{ch07.reg}}`, the first bit of the stream as bit 63. The top 11 bits are `{{ch07.reg_index_bits}}`,
index {{ch07.reg_index}}, one of the {{ch07.tok.2098.entries}} entries {{ch07.tok.2098.first}} to
{{ch07.tok.2098.last}} of this code. Then:

```text
br.bits                   {{ch07.reg_bin}} ... {{ch07.reg_low}}    {{ch07.reg}}
br.bits << TOK_N   ({{ch07.tok.2098.N}})    {{ch07.reg_shl_bin}} ... {{ch07.reg_shl_low}}    {{ch07.reg_shl}}
             >> TOK_BACK  ({{ch07.tok.2098.BACK}})                         {{ch07.reg_raw_bits}}   {{ch07.reg_raw}}
             << TOK_SHIFT ({{ch07.tok.2098.SHIFT}})                                  {{ch07.reg_off}}
seqlz_br_drop(&br, TOK_DROP) moves on by {{ch07.tok.2098.DROP}} bits, to the next sequence
```

The left shift throws the code away, the right shift keeps the 5 bits behind it, and the shift by 3
multiplies by 8. The same three shifts work for every class, with the numbers from the entry. For
token 49, 1 literal and 7 bytes at the repeat offset, `BACK` is 0: the decoder computes a `raw` it
ignores and takes `last`.

### A class 3 token, and one with a length value

Token 1552 is no literals and a match of 5 bytes at an offset in 12 bits. It has a
{{tok.len.1552}}-bit code: on desktop pages 29% of the matches with a class 3 offset are 5 bytes long,
against 16% of all matches (20 000 pages of each desktop dump, `seqlz_find()` and the classes of 14th
October 2026, same-filled pages left out, measured on 14th October). Its entry `{{ch07.tok.1552.entry}}`
has `BACK` {{ch07.tok.1552.BACK}} and `DROP` ${{tok.len.1552}} + 12 = {{ch07.tok.1552.DROP}}$. Token
1567 is the same with `ll` 15. Its entry `{{ch07.tok.1567.entry}}` has bit 30 set, so the decoder
leaves the fast path and, after the offset, reads an `ll` value and adds it to 15.
@assert ch07.tok[1567].value == 1 and ch07.tok[1552].value == 0

### The escape

The escape's {{tok.len.3072}}-bit code `{{tok.code.3072}}` fills the indices {{ch07.tok.3072.first}}
to {{ch07.tok.3072.last}} with `1U << 31 | {{tok.len.3072}} << 14 = {{ch07.tok.3072.entry}}`, the
lower row of the diagram. The decoder handles it before everything else:

@listing src/seqlz_decompress.c 473-485

Token 1797, 5 literals and a match of 20 at a class 3 offset, has no code. The encoder writes the
escape's `{{tok.code.3072}}`, 1797 in 12 bits, `{{ch07.esc1797.bits}}`, and then the offset, say 1000,
in 12 bits. The decoder's index is {{ch07.esc1797.index}}, in the escape's range; bit 31 is set, so it
drops {{ch07.tok.3072.DROP}} bits, refills, reads 1797, checks it against 3072 and calls
`token_entry(1797, 0)`. That entry describes a token whose code is gone already, `N` 0, `BACK`
{{ch07.esc1797.BACK}}, `DROP` {{ch07.esc1797.DROP}}, `{{ch07.esc1797.entry}}`, and from there the
escaped token goes through the same lines as any other. This is the one place where the decoder takes
a token apart at run time. With the tables and classes of 14th October 2026 that is about 15 of about
205 sequences per page on desktop pages and 10 of 170 on phone pages, measured as above; those pay
for a refill and 12 more bits anyway.
