# seqlz_codes.c: from code lengths to lookup tables {#ch-codes}

The format defines its {{ch17.tables}} tables by their code lengths alone, {{ch08.length_bytes}} bytes
compiled into the codec: {{tok.symbols}} for the token table, {{ll.symbols}} each for the two tables of
length values, 256 for each of the 8 literal tables, and {{own.symbols}} for `OWN`, the code a page's own
literal table is written in. `src/seqlz_codes.c` turns them into
`struct seqlz_tables`, the {{size.tables|grp}} bytes of codes and decode tables of
[chapter 7](#ch-internal). It runs once, when a program starts, and for the kernel when the kernel
is built ([below](#ch-codes--kernel)). `first_codes()` computes the
[canonical codes](#ch-compression--canonical-codes) and checks that the lengths are a complete prefix
code; `build_token()`, `build_values()`, `build_lit()` and `build_own()` write each code into the
encoder's table and into the range of decode entries that starts with it; `seqlz_tables_init()` calls
them all. The second
half of the chapter is where the lengths come from.

## The head of the file {#ch-codes--size}

@listing src/seqlz_codes.c 1-7

The file includes `seqlz.h`, the userspace interface ([chapter 6](#ch-header--tables)), for the two
functions it defines there, `seqlz_tables_init()` and `seqlz_all_symbols()`. In the kernel's copy it
includes `seqlz_tables.h` and the generated `seqlz_lengths.h` instead, and the C library's headers,
as it is a program for the build machine ([below](#ch-codes--kernel)).

## length_entry(): a length value's base and extra bits {#ch-codes--length-entry}

@listing src/seqlz_codes.c 9-19

The part of a length value's decode entry that depends only on the symbol, in the layout of
[chapter 7](#ch-internal--decode-tables): symbols below 16 are their value with no extra bits, a symbol
$s$ from 16 on has the base $2^{s - 12}$ and $s - 12$ extra bits
([chapter 6](#ch-header--length-values)). Symbol 18 gives `{{ch08.le.18}}`, base 64 and 6 extra bits;
the last symbol, {{ch08.le_last_symbol}}, has {{ch08.le_last_extra}} extra bits, and on 16 KiB pages
symbol {{k16.ch08.le_last_symbol}} has {{k16.ch08.le_last_extra}}, which still fit the field's 4 bits.
@assert ch08.le_last_symbol == 23

## first_codes(): the first code of each length {#ch-codes--first-codes}

@listing src/seqlz_codes.c 21-53

`first_codes()` is the rule of [chapter 2](#ch-compression--canonical-codes), the one of deflate
(RFC 1951, section 3.2.2) that
[`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md#prefix-codes)
specifies: `next[l]` is the first code of length `l`, and the builders hand out `next[l]`,
`next[l] + 1`, ... to the symbols of that length, in symbol order. `c = (c + of_len[l - 1]) << 1` is
`first[l] = (first[l-1] + count[l-1]) * 2`; `of_len[0]` counted the symbols without a code and is set
to 0 first, the spec's "`count[0]` is taken as 0".

The check is the sum of a code of `l` bits filling `2^(bits - l)` entries: the codes must fill the
table exactly ([chapter 2](#ch-compression--complete)). More would also run past the end of the decode
table, e.g. three codes of 1 bit get the numbers 0, 1 and 2, and code 2's range in a table of 8 entries
is 8 to 11. So this check keeps the builders inside their arrays. The test of a length against
`bits` comes before `of_len[len[s]]++`, because a length is a `u8` and `of_len` has 16 entries. With
`bits = 3`:

| lengths | `k` | |
| --- | --- | --- |
| 1, 1, 1 | $4 + 4 + 4 = 12$ | too many codes |
| 1, 2, 0 | $4 + 2 = 6$ | a gap: the indices starting with `11` belong to no code |
| 0, 0, 0 | 0 | no code at all |
| 1, 2, 4 | | $4 > 3$: returns before `k` is computed |

The test `seqlz: tables that are no prefix code are rejected` in `test/seqlz_test.cpp` builds such
tables from the real ones, and one case only the first check catches: a complete token table plus a
code of 12 bits. The sum counts lengths up to 11 only, so `k` is still exactly 2048.

For the real `ll` table, `{{ch08.ll.lengths}}`, the sum is
${{ch08.ll.fill.3}} + {{ch08.ll.fill.4}} + {{ch08.ll.fill.5}} + {{ch08.ll.fill.6}} + {{ch08.ll.fill.8}} = 256$,
and `next[3]` to `next[8]` are {{ch08.ll.next.3}}, {{ch08.ll.next.4}}, {{ch08.ll.next.5}},
{{ch08.ll.next.6}}, {{ch08.ll.next.7}} and {{ch08.ll.next.8}}. The 3-bit codes go to symbols 0, 1,
16 and 17: the values 0 and 1, literal runs of 15 and 16 bytes, and 16 to 63, runs of 31 to 78 bytes.
`next[7]` is {{ch08.ll.next.7}} although no code has 7 bits, and `next[8]` goes on from it. The token
table has no code shorter than 4 bits; its {{tok.codes_of.11}} codes of 11 bits are the numbers
{{ch08.tok.next.11}} to 2047, the last entries of the table.
@assert (ll.codes_of[1], ll.codes_of[2], ll.codes_of[3], ll.codes_of[4], ll.codes_of[5], ll.codes_of[6], ll.codes_of[7], ll.codes_of[8]) == (0, 0, 4, 3, 6, 7, 0, 4) and ll.len[0] == ll.len[1] == ll.len[16] == ll.len[17] == 3
@assert tok.codes_of[3] == 0 and tok.codes_of[4] > 0 and ch08.tok.next[11] + tok.codes_of[11] == 2048

## build_token(): a code is one range of the table {#ch-codes--build-token}

@listing src/seqlz_codes.c 55-78

The loop runs to `<=` because the escape is symbol 3072. A symbol without a code is skipped: its
`enc[]` stays 0, which is how the encoder knows to escape it ([chapter 7](#ch-internal--token-table)).
`c = next[l]++` gives the symbol the next code of its length.

A code's entries are one range because the decoder reads the bitstream most significant bit first:
the index is the top 11 bits of its register, the code $c$ of $l$ bits is the index's first $l$ bits,
and the $11 - l$ bits after it can be anything, so the indices are $c \cdot 2^{11-l}$ to
$(c + 1) \cdot 2^{11-l} - 1$. Token 49, 1 literal and a match of 7 at the repeat offset, has the
{{tok.len.49}}-bit code `{{tok.code.49}}`:

```text
first index   {{ch07.tok.49.c}} << (11 - {{tok.len.49}})        = {{ch07.tok.49.first}}   {{tok.code.49}} 0000000
last index    (({{ch07.tok.49.c}} + 1) << (11 - {{tok.len.49}})) - 1  = {{ch07.tok.49.last}}   {{tok.code.49}} 1111111
{{ch07.tok.49.entries}} entries of {{ch07.tok.49.entry}}, bytes {{ch08.tok49.byte_first}} to {{ch08.tok49.byte_last}} of the table: {{ch08.tok49.lines}} cache lines of 64 bytes
```
@assert tok.len[49] == 4 and ch07.tok[49].c == 0

The three codes of 4 bits, tokens 49 and 2097 and the escape, take the first {{ch08.tok.len4.bytes}}
bytes of the table, three sixteenths, in {{ch08.tok.len4.lines}} cache lines.
@assert tok.codes_of[4] == 3 and tok.len[49] == tok.len[2097] == tok.len[3072] == 4 and ch08.tok.len4.bytes == 1536

Read the other way, before 6th October 2026, a code's entries were spread over every cache line of
the table; [chapter 3](#ch-cpu--msb-first) has the cache lines and the measurements. One more result
belongs here: on the Mi 9T's A76, leaving the token table out of the prefetches, as the decoder then
did on that core, made cold reads 1.3 to 2.6 µs faster in every run once the codes were ranges, and
only in some builds before
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept)).

## build_values(): the same, for length values {#ch-codes--build-values}

@listing src/seqlz_codes.c 80-99

Symbol 18 of the `ll` table, the {{ll.len.18}}-bit code `{{ll.code.18}}`, gets
`enc[18] = {{ch07.ll18.c}} | {{ll.len.18}} << 16 = {{ch07.ll18.enc}}`, and the entries
{{ch07.ll18.first}} to {{ch07.ll18.last}} get `{{ch08.le.18}} | {{ll.len.18}} = {{ch07.ll18.entry}}`.
The loop stops at `SEQLZ_LEN_SYMBOLS`, so the entries `enc[{{const.LEN_SYMBOLS}}]` to `enc[31]`, which
are there for the encoder's mask, stay 0 from the `memset()` of `build_tables()`.
@assert ll.len[18] == 5

## build_lit(): the same, for a literal table {#ch-codes--build-lit}

@listing src/seqlz_codes.c 101-123

256 symbols, codes of at most 10 bits, entries in the order `lit_put()` and the decoder need
([chapter 7](#ch-internal--lit-table)). In table 0 the space, `0x20`, and `e`, `0x65`, have the first
two 4-bit codes, `{{lit.0.code.32}}` and `{{lit.0.code.101}}`: no code in table 0 is shorter, and
`0x20` comes first in symbol order. The comment is about bit order: the 8 literal streams are read
most significant bit first, like the sequences' bitstream and like `zstd` reads its Huffman coded
literals, so the codes go into the tables as `first_codes()` numbers them
([chapter 13](#ch-literals-dec)).
@assert ch08.lit[0].min == 4 and lit[0].len[32] == lit[0].len[101] == 4 and lit[0].codes_of[4] >= 2

## The literal tables and their costs {#ch-codes--lit-sets}

@listing src/seqlz_codes.c 125-140

`seqlz_all_symbols()` returns what `build_tables()` found below; the encoder reads the field
itself. `seqlz_tables_init()` takes the token and length tables as a parameter, so that the tests can
build tables from other lengths, but the literal tables always come from the global
`seqlz_lit_sets[8][256]`. The second loop fills `lit_cost` ([chapter 7](#ch-internal--lit-cost)) in
one pass over all 2048 pairs: `k % 256` is the byte value, `k / 256` the table and `8U * (k / 256)`
its lane. The `(u64)` cast matters: the `u8` is promoted to `int`, and shifting an `int` by 56 for
table 7 is undefined. For byte `0x65` the 8 lanes are `{{ch08.lit101_row}}`, which gives
`lit_cost[0x65] = {{ch07.lit_cost.101}}`, table 7 in the top byte.

## build_own(): the code of OWN, and quad[] by decoding every byte {#ch-codes--own}

@listing src/seqlz_codes.c 142-166

`OWN` is the fixed code a page's own literal table is written in ([chapter 4](#ch-format--own)), and
its lengths are not in `struct seqlz_lengths` either: `seqlz_own_lengths` is generated into the same
`.inc` file as the literal tables. The first part is `build_lit()` again for {{own.symbols}} symbols
and 8 bits, with the two halves of each entry the other way round, the code or symbol in the low byte
([chapter 7](#ch-internal--own)). `first_codes()` returns `-EINVAL` only if the generated lengths are
broken, and then `seqlz_tables_init()` fails as for any table. Unlike the other tables, a length of 0 is
`-EINVAL` too: the encoder may write any symbol of `OWN`, so each needs a code, and the loop would read
`next[0]`, which `first_codes()` never sets.

@listing src/seqlz_codes.c 167-210

`quad[k]` is filled by decoding the 8 bits `k` with the `decode[]` just built, symbol after symbol, as
far as the 8 bits go. `w` is what is left of `k` after `pos` bits, moved to the top, with zeros below;
a code that runs into those zeros is not in `k`, and `pos + l > bits` stops there. A length takes its
place in `len[]` with the run before it in `before[]`. A run reads its `nb - 1` bits below the top one
from right after its code: `w << l` drops the code, and the shift by `bits + 1 - nb` keeps the top `nb -
1` of the 8 bits. For a run of 1, `nb` is 1 and the shift is 8, which leaves 0 of an 8-bit value, so
`read(0)` needs no case of its own. The loop stops at a run whose bits do not fit, at a run above
`OWN_QUAD_RUN_MAX`, which has no room in 3 bits, and at a second run before a length: the encoder never
writes two in a row, and a page that has them is decoded one step at a time.

For `k = 0xd1`, binary `{{own.q.d1.bits}}`:

| `pos` | `w` | symbol, code | what happens |
| --- | --- | --- | --- |
| 0 | `{{own.q.d1.bits}}` | {{own.q.d1.sym}}, `{{own.code.10}}` | a run, `nb = 1`, `run = 1`, `pos = 4` |
| 4 | `{{own.q.d1.w4}}` | 8, `{{own.code.8}}` | the length 9, `before[0] = 1`, `pos = end[0] = 6` |
| 6 | `{{own.q.d1.w6}}` | 9, `{{own.code.9}}` | the length 10, `before[1] = 0`, `pos = end[1] = 8` |
@assert own.q.d1.sym == 10 and own.code[8] == "00" and own.code[9] == "01" and own.q.d1.end1 == 6

Two lengths, so the loop ends, and the entry is `{{own.q.d1.entry}}`. With no length at all, `got ==
0`, the entry is only the bit that sends the decoder to `decode[]`. With one, `end[1]` is set to
`end[0]`: the decoder reads the second end whenever the first length leaves room in the code space,
also where the entry has no second length, and then it must move by the first's bits. 256 entries are
256 runs of this loop, once per build of the tables.

## has_all_symbols(): can the encoder write every page {#ch-codes--all-symbols}

@listing src/seqlz_codes.c 212-230

The decoder can use any complete prefix code: a symbol left out cannot be in a valid page. The encoder
gets whatever page zram hands it and needs a code for every symbol that page might need: each token a
code of its own or the escape one; the escape's code at most `SEQLZ_MAX_ESCAPE_LEN` bits, so that an
escaped token with a class 3 offset takes at most 31 bits ([chapter 6](#ch-header--escape)); every
length value a code, as they have no escape; every byte a code in every literal table. A byte without
one would count 0 bits in `lit_cost`, so its table would look cheapest for exactly the pages it cannot
code. The test `seqlz: the compressor needs a code for every symbol` builds a complete `ml` table whose
last symbol has no code: the tables build, the decoder can use them, and `seqlz_compress()` returns 0.

## seqlz_tables_init(): all of it, from a clean slate {#ch-codes--init}

@listing src/seqlz_codes.c 232-262

The `memset()` does more than the comment says: tokens without a code need `enc[]` 0, the encoder's
masked index could reach the entries behind the last length symbol, `lit_cost` is built with `|=`,
and when the tests build tables twice on the same memory, nothing of the first set survives. The `||`
stops at the first table that fails, and the struct is then half built. So `seqlz_tables_init()`
builds into a static struct of its own and copies it over the global only when all of it built:
lengths that fail leave the tables the encoder and the decoder use as they were, and
`src/zram_seqlz.c` fails the zram device's setup. `build_tables()` takes the struct as a parameter,
`t`; the kernel's generator hands it a struct of its own ([below](#ch-codes--kernel)). A complete code that leaves symbols out returns 0 with
`all_symbols` false, tables the decoder can use and the encoder cannot; `seqlz_compress()` and
`seqlz_encode()` then return 0 for every page. With the tables of the format that never happens,
because the generator checks it, see below.

## In the kernel: at build time {#ch-codes--kernel}

The kernel does not run this file. `tools/kernel-port/port.py` turns it into
`lib/seqlz/gen_seqlz_tables.c`, a program for the build host, as `lib/crc/gen_crc32table.c` is for the
CRC tables: the same builders, without `seqlz_tables_init()`, `seqlz_all_symbols()` and the global, the
code lengths from `lib/seqlz/seqlz_lengths.h`, and `tools/gen-tables/gen_seqlz_tables.c` at the end:

@listing tools/gen-tables/gen_seqlz_tables.c 1-{{ch15.kport.gen_main}}

When the kernel is built, `lib/seqlz/Makefile` builds the program, runs it and compiles what it
prints, `seqlz_tables.c`: `const struct seqlz_tables seqlz_tables`, {{size.tables|grp}} bytes of the
kernel's read-only data, shared by all CPUs. Nothing builds tables at boot, and the module has no init
function. Where `seqlz_tables_init()` would leave `all_symbols` false, `build_tables()` returns
`-EINVAL` in the kernel's copy, and the build stops: a kernel never has tables that the encoder cannot
use. The program reads each value with its own type, so the build host's byte order does not matter,
and `tools/kernel-port/check.sh` compares what it prints with the tables `seqlz_tables_init()` builds
in userspace. The tables used to be built at boot, into `__ro_after_init` memory: the kernel image then
carried {{size.tables|grp}} bytes of zeros, and every boot ran this file's loops once.

## Where the code lengths come from {#ch-codes--lengths}

@listing src/seqlz_default_tables.c 1-18

The lengths are part of the format, generated from counts in the repository, and the pages they were
counted on are not the pages the benchmarks measure, so no benchmark measures the tables on their
training data. The head of the 4 KiB file, and its end with the escape and the length tables:

@listing src/seqlz_default_tables_4k.inc 1-14

@listing src/seqlz_default_tables_4k.inc 200-213

The SPDX line is in the kernel's order, as `port.py` writes it too. 16 lengths per line, so token `s`
is at line `{{ch08.inc.tok_line}} + s / 16`: line {{ch08.inc.tok49_line}} has tokens 48 to 63, and
its second number, token 49, is {{tok.len.49}}. Line {{ch08.inc.esc_line}} is the escape,
{{tok.len.3072}}.
@assert ch08.inc.tok_line + 3 == 14 and 200 <= ch08.inc.esc_line <= 213 - 7

### Training: counts, then lengths

`tools/kernel-port/gen-seqlz-lengths.py` makes the `.inc` files from `bench/seqlz_counts_4k.txt`, how
often each token, each `ll` and `ml` symbol and each byte as a literal occurred on the training pages,
and for each literal table how many pages it codes:

- Each table is an optimal **length-limited** prefix code, by package-merge (Larmore and Hirschberg):
  the code with the fewest bits for these counts among those whose longest code is 11, 8 or 10 bits. A
  plain Huffman code has no such limit.
- Every count gets 1 added, so every length value and every literal gets a code, even one that never
  occurred, which `has_all_symbols()` checks.
- Only the most frequent tokens get a code. The generator tries the 64, 128, 192, ... most frequent
  plus the escape, prices each choice on the training pages with the escaped tokens at the escape's
  code plus 12 bits, and keeps the cheapest whose escape code has at most 7 bits. For 4 KiB pages that
  is {{ch08.tok.coded}} tokens, {{ch08.tok.coded_64}} times 64.
- It checks the lengths as `seqlz_tables_init()` and `has_all_symbols()` do before it writes anything.
@assert ch08.tok.coded % 64 == 0

`tools/seqlz-ref/check.sh`, in CI, generates all four `.inc` files again and fails if one differs by a
byte. `seqlz_ref.py`, the decoder written from the spec, checks the SHA-256 of every table against
[`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md#where-the-tables-come-from),
e.g. `{{ch08.tok4k_sha}}...` for the 4 KiB token table, and the test `seqlz_train` checks that
`quetschn-seqlz-train --from-counts` in C++ gives the same tables.

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
`1.0`, the Kraft sum of a complete code.
:::

The 4 KiB counts of 13th October 2026 are from {{ch08.counts4k.pages|grp}} pages: three sessions of a
Xiaomi Mi 9T under memory pressure, the resident pages of a desktop and 200 000 pages of its zram dump
of 28th September ([chapter 4](#ch-format--tables)). On two test sessions of the phone, with web sites
none of the training sessions loaded, `seqlz-2` stores up to 1.7% less than with the tables
before, and the phone dump of 3rd October, which the tables before were trained on, 1.8% more
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-tables-trained-on-phone-sessions-up-to-17-smaller-on-them-an-old-phone-dump-18-larger)).
Nearly all of it is in the literal tables. The counts cannot give a page back: the token and length
counts are sums over all pages, each literal table's over at least
{{ch08.counts4k.lit_min_pages|grp}} pages, and a count says nothing about which symbol came after which.

### What the token table looks like

Of the 3072 tokens, {{ch08.tok.coded}} have a code of their own, {{tok.with_code}} codes with the
escape. The shortest:

| token | `ll` | `ml` | class | code |
| --- | --- | --- | --- | --- |
| 49 | 1 | 7 | 0, the repeat offset | `{{tok.code.49}}` |
| 2097 | 1 | 7 | 4, a multiple of 8 below 256 | `{{tok.code.2097}}` |
| 34 | 2 | 6 | 0 | `{{tok.code.34}}` |
| 2082 | 2 | 6 | 4 | `{{tok.code.2082}}` |
| 2112 | 0 | 8 | 4 | `{{tok.code.2112}}` |
| 2624 | 0 | 8 | 5, a multiple of 8 from 256 to 2040 | `{{tok.code.2624}}` |
@assert tok.codes_of[4] == 3 and tok.codes_of[5] == 3 and tok.len[3072] == tok.len[49] == tok.len[2097] == 4 and tok.len[34] == tok.len[2082] == tok.len[2112] == 5 and tok.len[2624] == tok.len[576] == 6

In each, `ll + ml` is 8: one sequence per 8-byte value, a pointer or a field, whose low 1 or 2 bytes
differ from a value 8 or more bytes before, or none do. Among the codes of {{tok.len.576}} bits is also
token 576, no literals and 8 bytes from offset 2, class 1: a run of one 16-bit value.

The escape's {{tok.len.3072}}-bit code is as short as the most frequent token's. Each of the
{{ch08.tok.escaped}} tokens without a code is rare, but together they are about 15 of about 205
sequences per page on desktop pages and 10 of 170 on phone pages, 16 bits each before the offset
(20 000 pages of each dump, `seqlz_find()` and the tables of 14th October 2026, same-filled pages left
out, measured on 14th October). [Chapter 6](#ch-header--escape) has why they do not all get a code.

## The literal tables {#ch-codes--lit-sets-file}

@listing src/seqlz_lit_sets.c 1-20

**k-means** puts each training page into the group of the table that codes its literals in the fewest
bits, makes each table the Huffman code of its group, and repeats until the groups stop changing.
Where it ends depends on where it starts, so the trainer runs it from 32 starting points. The tables
of 13th October 2026 are in the order that fits the tables before them best, not by use, so that table
0 is still the one for text; the order means nothing to the format.

@listing src/seqlz_lit_sets_4k.inc 1-17

Table 0 starts at line {{ch08.litinc.line}}, 16 bytes per line. Line {{ch08.litinc.line60}} is the
bytes `0x60` to `0x6f`, the backtick and `a` to `o`: 4 to 8 bits except the backtick and `j`, with `e`
at {{lit.0.len.101}}. The other 7 differ: tables 1 and 4 give `0x00` 2 bits, table 7 gives `0xff`
{{lit.7.len.255}} bits. Every table has codes of 10 bits.
@assert ch08.litinc.line60 <= 17 and min(lit[0].len[0x61], lit[0].len[0x62], lit[0].len[0x63], lit[0].len[0x64], lit[0].len[0x65], lit[0].len[0x66], lit[0].len[0x67], lit[0].len[0x68], lit[0].len[0x69], lit[0].len[0x6b], lit[0].len[0x6c], lit[0].len[0x6d], lit[0].len[0x6e], lit[0].len[0x6f]) == 4 and max(lit[0].len[0x61], lit[0].len[0x62], lit[0].len[0x63], lit[0].len[0x64], lit[0].len[0x65], lit[0].len[0x66], lit[0].len[0x67], lit[0].len[0x68], lit[0].len[0x69], lit[0].len[0x6b], lit[0].len[0x6c], lit[0].len[0x6d], lit[0].len[0x6e], lit[0].len[0x6f]) == 8 and lit[0].len[0x60] > 8 and lit[0].len[0x6a] > 8 and lit[1].len[0] == lit[4].len[0] == 2 and lit[7].len[0xff] == 2 and ch08.lit[0].max == ch08.lit[1].max == ch08.lit[2].max == ch08.lit[3].max == ch08.lit[4].max == ch08.lit[5].max == ch08.lit[6].max == ch08.lit[7].max == 10

The file ends with `seqlz_own_lengths`, the {{own.symbols}} code lengths of `OWN`, which the same
generator makes from the line `own` of the counts ([chapter 4](#ch-format--tables)), and which
`build_own()` above turns into a code.

::: measured 8 literal tables, not 1, 16 or 64
k-means trained on the resident pages of a desktop and priced on its zram pages, as the share of
`seqlz-1`'s output saved: 1 table 6.8%, 4 tables 9.2%, 8 tables of 10 bits 11.4%, 16 tables
11.9%, 64 tables 12.9%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
16 tables were built and measured in the kernel VM: 5 bytes per page less, and twice the decode
tables made cold reads 0.27 µs slower, so they were dropped
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)).
:::

## Fixed tables, part of the format {#ch-codes--fixed}

Everything in this chapter happens once, before the first page, and no zram device has tables of its
own. A page carries no token or length table; at level 3 it may carry a literal table, which the
decoder builds per page in [chapter 13](#ch-literals-dec--own), written in the fixed code `OWN`. [Chapter 4](#ch-format--tables) has the measurements of both, and why a
page needs no version of its tables. That is also why the tables could be trained again on 13th and
14th October 2026: nobody uses the format yet, and new tables only need new hashes in `format.md`.

The 16 KiB variants, `seqlz_default_tables_16k.inc` and `seqlz_lit_sets_16k.inc`, are the same
construction with counts from {{ch08.counts16k.pages|grp}} pages of a zram dump of the Android 17
emulator with 16 KiB pages. They give {{k16.ch08.tok.coded}} tokens a code, with a
{{k16.tok.len.3072}}-bit escape, below that page size's limit of $31 - 14 - 12 = 5$ bits. On pages of
the emulator that were not in the training, they made `seqlz-2` 2.6% smaller than the 16 KiB
tables before them, which were trained on desktop pages
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#android-17-in-the-emulator-the-4-kib-tables-fit-the-16-kib-ones-trained-again-26-smaller)).
That is one emulator and no real phone, and the format for 16 KiB pages is not fixed yet
([format.md](https://github.com/martinus/quetschn/blob/main/docs/format.md#status)).
