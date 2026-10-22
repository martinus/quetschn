# seqlz.h: the format in constants {#ch-header}

`src/seqlz.h` is the header every file of the codec includes, and the one the tests, the benchmarks
and the zram glue include to call it. It has the format as `#define`s, three small functions that turn
a sequence into the numbers the format stores, `seqlz_len_symbol()`, `seqlz_off_class()` and
`seqlz_token()`, and the interface: the structs and functions that build the tables, compress a page
and decompress one. [Chapter 4](#ch-format) has the format in words, so this chapter spends its words
on why each number has its value. How the tables lie in memory is `seqlz_internal.h`,
[chapter 7](#ch-internal); a caller never sees it.

## The top of the file: the format in a comment {#ch-header--summary}

@listing src/seqlz_format.h 1-27

The guard `_LINUX_SEQLZ_H` is the kernel's convention for a header in `include/linux/`. The kernel's
copy has two headers: `include/linux/seqlz.h`, the interface zram calls, which `port.py` writes from
`tools/kernel-port/seqlz.h`, and `lib/seqlz/seqlz.h`, made from this file, with the guard
`_LIB_SEQLZ_SEQLZ_H` and `asm/page.h`, `linux/minmax.h` and `linux/types.h` in place of
`seqlz_compat.h`.

The comment is the format short enough to keep in your head while reading the code. Two of its
details are easy to read past. "1 to the page size" for the offset is generous: a match needs at least
one byte before it, so 4095 is the largest offset on a 4 KiB page, which is what class 3's 12 bits
hold. And the token's formula in line 24 is exactly what `seqlz_token()` computes at the end of the
file.

@listing src/seqlz_format.h 28-58

The classes are the table of [chapter 4](#ch-format--offsets), which also has the numbers for offset
2. The multiples of 8 have numbers too: on the zram pages of a desktop, 64% of the offsets from 16 to
255 that are not the repeat offset are multiples of 8, and 52% of those from 256 on
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)).
The last sequence has no bits at all, which [chapter 4](#ch-format--end) explains. In the kernel's
copy line 57 says "Documentation/staging/seqlz.rst specifies the format.": `port.py` writes that file
from `docs/format.md`, and the kernel's copy may not name the repository.

@listing src/seqlz_internal.h 1-21

The kernel has `__ffs64()`, `__fls()` and `fls()` for what the two bit builtins do, but on x86-64 they
are inline assembly, and the compiler can neither fold a constant through assembly nor merge it with
the code around it. With the kernel's `__ffs()` and `fls()`, gcc wrote `seqlz_decode_literals()` and
`seqlz_code_literals()` "in another shape" in a kernel build (commit `e814b1c`).

`__builtin_memcpy()` is for the fixed copies of 8 and 16 bytes. A kernel built with
`CONFIG_FORTIFY_SOURCE` replaces `memcpy()` with a version that checks sizes, and not every compiler
inlines it: on the Mi 9T's 4.14 kernel, built with clang, a module of the codec had 63 calls to
`memcpy()`, one per such copy in the matcher, the encoder and the decoder, where the module before had
1 (commit `48a7043`). `__builtin_bswap64()` turns a literal stream's bits around once per flush, in
`lit_flush()` ([chapter 11](#ch-literals-enc)).

## The page size and the token {#ch-header--token}

@listing src/seqlz_format.h 60-81

`QUETSCHN_PAGE_BITS` is the codec's one setting, 12 for 4 KiB pages and 14 for 16 KiB, where CMake
passes `-DQUETSCHN_PAGE_BITS=14`. The kernel's copy has `SEQLZ_PAGE_BITS`, 12, whatever the kernel's
page size is: it compresses blocks of 4 KiB, and the format for 16 KiB pages is not fixed yet
([`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md#status)).

The two longest codes set the sizes of the decode tables. A table-driven decoder looks up the next $L$
bits in a table of $2^L$ entries ([chapter 2](#ch-compression--table-decoding)), so 8 bits for length
values give 256 entries of 4 bytes, 1 KiB, and 11 bits for tokens 2048 entries of 4 bytes, 8 KiB. The
size is what a **cold** read pays for, a read of a page swapped out a while ago, whose bytes and tables
are not in the CPU's caches: every cache line of a table the page touches is a wait for memory
([chapter 3](#ch-cpu--cache)). Codes of length values were once allowed 9 bits, and a cold read took
about 80 ns longer on the PC
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow),
the row "length values with codes of at most 8 bits").
@assert ch06.MAX_BITS == 8 and ch06.TOKEN_BITS == 11

The token table is the larger trade. With 10 bits it would be 4 KiB, but fewer tokens get a code of
their own, more take the escape and a second lookup, and pages get larger. It was measured last on
11th October 2026, with a decoder that prefetches the token table on every core
([chapter 14](#ch-decode)), so 10 bits meant 64 cache lines less to load on every core
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-token-table-in-10-bits-again-the-a55s-cold-reads-13-µs-faster-x86-64-2-to-4-slower-not-kept)).
Pages were 3.5 bytes larger on average. On the Mi 9T's A55, the little in-order core, cold reads of
pages with coded literals were 1.3 µs faster, about 0.9 µs less per page written with 0.73 reads per write. On the A76, the big
out-of-order core, the cold reads did not change and the warm reads were 0.16 µs slower; in the kernel
VM on x86-64 swap-ins were 2% to 4% slower. On 5th October 10 bits had read cold 2.0 to 2.7 µs faster
on the A76 too
([item 1](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#1-the-token-table-in-10-bits-cold-reads-2-to-3-µs-faster-on-both-cores-for-3-to-6-bytes)).
That gain went when the bitstream became most significant bit first: a code's entries now sit next to
each other, and a page touches fewer lines of the table anyway ([chapter 3](#ch-cpu--msb-first)). So
10 bits pays only on the in-order core, and 11 bits stay.

@listing src/seqlz_format.h 82-98

A length value after the token costs a second lookup, in another table, so the fewer sequences need
one, the better. With 4 bits for `ml - 4` a match length value followed 18.7% of the matches, with 5
bits 8.5%, at the same memory, and the decoder went from 9400 to 9180 cycles per page on the PC,
measured before the token had an offset class
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes),
"Rarer escapes: a token of 512 symbols"). The two numbers cannot grow, because the decoder's entry for
a token has 4 bits for `ll` and 6 for `ml` itself, the cap plus 4: $31 + 4 = 35$ fits 6 bits, with 6
bits of `ml - 4` it would be $63 + 4 = 67$, which does not ([chapter 7](#ch-internal--token-entry)).

The class takes the top of the token: `SEQLZ_TOKEN_SYMBOLS` is `6 << 9`, {{const.TOKEN_SYMBOLS}}, and
token `t` has `ll` in bits 0 to 3, `ml - 4` in bits 4 to 8 and the class in bits 9 to 11, as
[chapter 4](#ch-format--token) draws it for token 2098.

"Two more save little": `tools/seqlz-bound/space.py` priced layouts of 6 to 8 classes, each a number of
bits and a divisor of 1, 2, 4 or 8, on 20 000 pages of each of seven dumps, the token codes of each dump
trained on the other six. Against the classes before 14th October 2026, today's 6 save 5.15 bytes per
page of tokens and offset bits, the mean of the seven dumps, and the best 7 save 6.24. Offsets that need
no bits save more, e.g. 2 and 8 as classes of their own, 8.46 bytes with 8 classes. But then the
decoder chooses from three for each offset, its bits, a fixed one or the repeat offset, and with that
one more select the Mi 9T's A55 wrote 1.0% and read warm pages 1.7% slower, its A76 read warm pages
2.2% slower
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-2-in-1-bit-multiples-of-8-below-2048-in-8-03-to-08-smaller-at-the-same-speed-kept)).

## The escape and the caps {#ch-header--escape}

@listing src/seqlz_format.h 99-118

A code of $l$ bits uses $2^{-l}$ of the code space, and the sum may not exceed 1
([chapter 2](#ch-compression--complete)), so codes of at most 11 bits have room for 2048 codes, and
3072 tokens need at least 1.5 times that. With the 4 KiB tables {{ch04.tok_coded}} tokens have a code
of their own, with the 16 KiB tables {{k16.ch04.tok_coded}}. All others are sent as the escape,
symbol {{ch06.ESCAPE}}, followed by the token in 12 plain bits, the fewest that hold 3071. The values
3072 to 4095 are no token, and the decoder rejects a page that sends one. The escape as a whole is
frequent, with a {{ch06.esc_len}}-bit code in both tables.
@assert ch06.esc_len == k16.ch06.esc_len

"A code for every token would take code space from the frequent ones" was measured when the token had
1536 symbols: with 11 bits each needs at least $\frac{1}{2048}$ of the code space, 75% of it together.
Pages took 27.5% of their size instead of the 26.6% the frequencies promised; with the escape and the
same 11-bit table, 26.8%
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).

`SEQLZ_MAX_ESCAPE_LEN` is a limit for the tables that comes from the encoder. The encoder knows that
zram's two pages hold every compressed page from an argument that needs at most 31 bits for a token and
its offset ([chapter 10](#ch-encoder)). The longest such pair is an escaped token with a class 3
offset, so the escape's code may have $31 - 12 - 12 = {{ch06.MAX_ESCAPE_LEN}}$ bits on 4 KiB pages and
{{k16.ch06.MAX_ESCAPE_LEN}} on 16 KiB pages. With the trained {{ch06.esc_len}} bits the longest pair
is {{ch06.esc_pair}} bits on 4 KiB pages; any other token code is at most 11 bits, $11 + 12 = 23$.
`has_all_symbols()` checks the limit when the tables are built ([chapter 8](#ch-codes--all-symbols)).
@assert ch06.esc_len <= ch06.MAX_ESCAPE_LEN and k16.ch06.esc_len <= k16.ch06.MAX_ESCAPE_LEN and ch04.longest.tok == 11

The caps are the largest value of each field and its flag at once: a token with `ll` 15 means "15 or
more", and `ll - 15` follows as a length value.

## Length values {#ch-header--length-values}

@listing src/seqlz_format.h 119-138

A length value of 16 or more, with its highest set bit at position $b$, is the symbol $12 + b$ and the
$b$ bits below that bit. $b$ is at least 4, so the first such symbol is 16, and the base of 12 makes
the two kinds of symbols meet without a gap. Each symbol covers twice the range of the one before:
16 is 16 to 31, 17 is 32 to 63, and the last on 4 KiB pages, {{ch06.last_sym}}, is 2048 to 4095.
That is {{ch06.LEN_SYMBOLS}} symbols, {{k16.ch06.LEN_SYMBOLS}} on 16 KiB pages.
@assert ch06.last_sym == 23

The largest length value of a 4 KiB page is `ll - 15` for 4092 literals before a match of 4 bytes,
{{ch06.max_ll_value}}, or `ml - 35` for a match of 4095 bytes after 1 literal, {{ch06.max_ml_value}}:
both symbol {{ch06.max_ll_sym}}, the last, as the comment says. Until 11th October 2026 there was one
symbol more, 4096 to 8191, which no valid page sent. As the encoder wants a code for every length
value symbol, the trained tables still gave it 8 bits, 1/256 of each table's code space; it went when
the tables were trained again ([#182](https://github.com/martinus/quetschn/issues/182)).
[Chapter 4](#ch-format--length-values) has why the symbol is not sent in 5 plain bits instead.
@assert ch06.max_ll_sym == ch06.max_ml_sym == ch06.last_sym and k16.ch06.max_ll_sym == k16.ch06.max_ml_sym == k16.ch06.last_sym

`SEQLZ_LIT_CODED` can be the top bit of the `u16` because the count never reaches it: at most 4096
literals on 4 KiB pages and 16384 on 16 KiB pages, 15 bits. The decoder takes the count as
`n & (SEQLZ_LIT_CODED - 1U)` ([chapter 14](#ch-decode)). In the kernel's copy the comment says "at
most 4 KiB of literals".

## The tables as code lengths {#ch-header--lengths}

@listing src/seqlz_format.h 140-168

The canonical code follows from the code lengths alone
([chapter 2](#ch-compression--canonical-codes)), so `struct seqlz_lengths`, one byte per symbol, is
all the format fixes for the sequences: {{size.lengths}} bytes on 4 KiB pages, {{k16.size.lengths}} on
16 KiB pages. The set compiled in, `seqlz_default_lengths` from `seqlz_default_tables.c`, is part of the
format, and `docs/format.md` lists its SHA-256 so that a decoder written from the spec can check it has
the right one. The 8 literal tables are `seqlz_lit_sets[][]`, further down.

`seqlz_len_symbol()` is the scheme of length values in code. `put_len_value()` calls it
([chapter 10](#ch-encoder--put)) and writes the low `*extra_bits` bits of `v` after the symbol's code.
`31 - __builtin_clz(v)` is the position of the highest set bit; the builtin is undefined for 0, and
`v` is at least 16 here. For `v = 100`, `{{ch06.lensym.100.bin}}`, the highest bit is bit 6, so the
symbol is {{ch06.lensym.100.sym}}, and the 6 bits below it, {{ch06.lensym.100.low}}, follow. The
highest bit is not sent, the symbol says where it is. The match of 4094 bytes in the format's worked
example ([chapter 4](#ch-format--example)) has `ml - 35 = 4059`, symbol {{ch06.lensym.4059.sym}} and
{{ch06.lensym.4059.extra}} plain bits. gcc compiles the function with one branch, for `v < 16`, and a
`bsr`; the encoder calls it only for the few sequences with a length value.
@assert ch06.lensym[100].extra == 6 and ch04.ab.s[0].mlval == 4059 and ch04.ab.s[0].ml == 4094

## Offset classes without a branch {#ch-header--offsets}

@listing src/seqlz_format.h 170-187

Class 5 holds the multiples of 8 below `8 << 8`, {{ch06.class5_end}}, on both page sizes. On 16 KiB
pages 10 bits could reach 8 KiB back, but that made the pages no smaller on average
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#offset-2-in-1-bit-multiples-of-8-below-2048-in-8-03-to-08-smaller-at-the-same-speed-kept)).
@assert ch06.CLASS5_BITS == k16.ch06.CLASS5_BITS == 8 and ch06.class5_end == 2048

The encoder and the decoder need each class's number of offset bits, {{ch06.RAW_BITS}} on 4 KiB pages.
Instead of a `switch` or an array, `SEQLZ_RAW_NIBBLES` holds them as one nibble per class, and
`SEQLZ_RAW_BITS(cls)` shifts and masks:

```text
class:     5  4  3  2  1  0
nibble:    8  5  C  8  1  0     SEQLZ_RAW_NIBBLES = 0x85C810 on 4 KiB pages, 0x85E810 on 16 KiB
```
@assert ch06.RAW_NIBBLES == "0x85C810" and k16.ch06.RAW_NIBBLES == "0x85E810"

For class 3, `0x85C810 >> 12` is `0x85C`, and `& 15` leaves `0xC`, 12. The page size goes into the
fourth nibble as `QUETSCHN_PAGE_BITS << 12`, and the compiler folds the whole expression into one
constant: gcc's code for `seqlz_off_class()` loads `${{ch06.RAW_NIBBLES_dec}}`. When class 3's bits were
computed from `QUETSCHN_PAGE_BITS` in code, gcc branched on the class; a shift and a mask cannot become
a branch. `SEQLZ_SHIFT_NIBBLES` is the same trick for how far the offset is shifted right before it is
sent: `0x330010`, 1 for class 1 and 3 for classes 4 and 5. The token's decode table entry has 2 bits
for that shift ([chapter 7](#ch-internal--token-entry)).
@assert ch06.OFF_SHIFT == k16.ch06.OFF_SHIFT == "0, 1, 0, 0, 3, 3"

@listing src/seqlz_format.h 189-210

The encoder calls `seqlz_off_class()` once per match, and the classes of consecutive matches follow no
pattern a branch predictor could learn ([chapter 3](#ch-cpu--branches)), so it is arithmetic:

- `is_new` is a mask, 0 for the repeat offset and `0xFFFFFFFF` for a new one.
- `aligned` is 1 for a multiple of 8 below 2048. `&` instead of `&&` keeps it a plain AND of two
  values that are 0 or 1; `&&` may not evaluate its right side, and compilers often keep that as a
  branch.
- The class starts at 3, goes 1 down below 256 and 1 more for offset 2, and 2 up for `aligned`, which
  moves class 2 to 4 and class 3 to 5. Offset 2 is not aligned, so the smallest result is 1 and nothing
  wraps. `& is_new` makes it 0 for the repeat offset.

| `off` | `aligned` | class | offset bits | value sent |
| --- | --- | --- | --- | --- |
| 2 | {{ch06.off.2.aligned}} | {{ch06.off.2.cls}} | {{ch06.off.2.raw}} | {{ch06.off.2.sent}} |
| 8 | {{ch06.off.8.aligned}} | {{ch06.off.8.cls}} | {{ch06.off.8.raw}} | {{ch06.off.8.sent}} |
| 255 | {{ch06.off.255.aligned}} | {{ch06.off.255.cls}} | {{ch06.off.255.raw}} | {{ch06.off.255.sent}} |
| 2040 | {{ch06.off.2040.aligned}} | {{ch06.off.2040.cls}} | {{ch06.off.2040.raw}} | {{ch06.off.2040.sent}} |
| 2048 | {{ch06.off.2048.aligned}} | {{ch06.off.2048.cls}} | {{ch06.off.2048.raw}} | {{ch06.off.2048.sent}} |
@assert ch06.off[8].cls == 4 and ch06.off[2048].cls == 3 and ch06.off[2].cls == 1 and ch06.off[2040].sent == 255

Offset 8 goes to class 4 in 5 bits, where class 2 could send it in 8. 2048 is a multiple of 8 beyond
class 5, so it is class 3. With `off == last` the class is 0 and no bits follow; `last` is 1 at the
start of a page, so a first match at offset 1, a run of one byte, is class 0 too. gcc 16.2.1 at `-O2`
for x86-64 compiles the function without a jump: `setcc` for the comparisons, an `add`, a `sub` and a
`lea` for the sum, a conditional move for the repeat offset.

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

## The tables, behind a pointer {#ch-header--tables}

@listing src/seqlz.h 1-17

`struct seqlz_sequence`, three `u16`, is a sequence for code outside the compressor: `seqlz_find()`
fills an array of them, `seqlz_encode()` writes a page from one. The compressor itself does not use it;
its matcher hands each sequence to the encoder as three arguments, as soon as it is found
([chapter 9](#ch-matcher)).

`struct seqlz_tables` is opaque here; `seqlz_internal.h` defines it ([chapter 7](#ch-internal)): the
codes for the encoder and the decode tables, {{size.tables|grp}} bytes on both page sizes. They are
built once and then only read, by every CPU. In the kernel they are one global struct, built at boot
or when the module loads and marked `__ro_after_init`, which makes the memory read-only after
initialization ([chapter 15](#ch-zram-glue--init)), so they are no part of the per-CPU work memory.
Outside the kernel a caller allocates `seqlz_tables_size()` bytes, as the benchmark's zram glue does.
The `/**` comments are kernel-doc, the kernel's format for documenting a function
([chapter 15](#ch-zram-glue--interface)).
@assert size.tables == k16.size.tables

The kernel's copy leaves out what only the harness uses: `struct seqlz_sequence`,
`seqlz_tables_size()`, `seqlz_all_symbols()`, `seqlz_encode()`, `seqlz_find()` and
`SEQLZ_MAX_SEQUENCES`.

@listing src/seqlz.h 19-40

The decoder needs only a complete prefix code ([chapter 2](#ch-compression--complete)): every pattern
of bits then starts with a code, and a symbol without one cannot be in a valid page. The encoder needs a
code for every symbol it may have to write, as it cannot refuse a page for a rare token: for a token a
code or the escape, for a length value and a literal a code. `seqlz_tables_init()` checks that and
stores the answer that `seqlz_all_symbols()` returns; `seqlz_encode()` and `seqlz_compress()` return 0
when it is false. The kernel's copy has no such check: since commit `4c42cc5` the script that writes
the kernel's tables checks the lengths, so building them at boot cannot fail.

The four failures of `seqlz_tables_init()` are the ways lengths are no complete prefix code: a code
longer than its table, e.g. a token code of 12 bits; a sum of $2^{-l}$ above 1, where codes would share
entries; below 1, a gap; or no code at all. `-EINVAL` is the kernel's convention, a negative error
number, 0 for success. The tests pass made-up lengths to check that bad ones are refused. In the
kernel's copy the function is `static` and `__init`, code the kernel frees after boot, and `port.py`
moves its kernel-doc to the definition, as for `seqlz_decode()` and `seqlz_compress()`.

## The literal tables and their header {#ch-header--literals}

@listing src/seqlz_format.h 212-227
@listing src/seqlz_internal.h 23-27

A literal table codes the 256 byte values. With codes of at most 10 bits its decode table has 1024
entries of 2 bytes, 2 KiB; with 9 bits it would be half the cache lines on a cold read, but on the
Mi 9T that was no faster on either core, and pages got up to 64 bytes larger
([item 2 of "Six choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#2-the-literal-tables-in-9-bits-no-faster-up-to-64-bytes-larger-not-kept)).
[Chapter 8](#ch-codes--lit-sets-file) has the numbers of 16 tables against 8.

The decoder knows where literal $k + 1$ starts only after it has looked up literal $k$, so one stream
is one chain of lookups, each waiting for the one before ([chapter 3](#ch-cpu--chains)). 8 streams
are 8 chains a CPU works on side by side; 4 are measured in [chapter 13](#ch-literals-dec--cost). That
there are 8 tables and 8 streams is a coincidence: all streams of a page use the page's one table.

`SEQLZ_LIT_ROUNDS` is `56 / 10 = 5`. After a load of 8 bytes, up to 7 bits of the first byte are used
already and one bit is the decoder's marker of where the valid bits end
([chapter 13](#ch-literals-dec)), so $64 - 7 - 1 = 56$ bits are left: 5 codes of 10 bits, 40
literals for all 8 streams per refill.

@listing src/seqlz_format.h 228-249

```text
byte 2:      bit 7 | bit 6  bit 5  bit 4 | bit 3  bit 2  bit 1  bit 0
             deltas|       w - 5         |      the literals' code
byte 3:      with bit 7 set, which delta, 1 to 3, see below
bytes 3 + e: the 8 stream sizes in bytes, w bits each, lowest bit first, w bytes together,
             from byte 3 without deltas, e = 0, from byte 4 with them, e = 1
```

8 sizes of `w` bits are `w` bytes, so the header is $3 + w$, 8 to 15 bytes, one more with deltas, and
`SEQLZ_LIT_HEADER(0)` is 3, the offset of the delta's byte with deltas and of the first size byte
without. The header reads in order: byte 2 says whether the delta's byte is there, and it comes
next, before the sizes. A page whose largest stream has 100 bytes takes 7 bits per size and a header
of 10 bytes. A stream holds every 8th literal of at most 10 bits,
at most 640 bytes on 4 KiB pages and 2560 on 16 KiB pages, which need 10 and 12 bits. Widths of 3 and
4 bits would need a fourth bit for the width, and only 1.8% of the pages with coded literals would use
them
([`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#stream-sizes-in-as-many-bits-as-the-largest-needs-kept)).
The code has 4 bits for 9 values: 0 to 7 are the fixed tables, `SEQLZ_LIT_OWN` below. The codes 9
to 15 are invalid, so that a future format could tell itself apart
([`docs/format.md`](https://github.com/martinus/quetschn/blob/main/docs/format.md#no-version-in-the-block)).
Bit 7 was such a free bit too, which had to be 0, until level 3 began to store its literals as
deltas on 21st October 2026. E.g. the text page of [chapter 4](#ch-format--own) at level 3 has byte 2
`{{own.text.t}}`: code {{own.text.lcode}} in bits 0 to 3, $w - 5 = {{own.text.w5}}$ in bits 4 to 6,
and bit 7 0, its literals are as they are. `SEQLZ_LIT_CODE_MASK`, `SEQLZ_LIT_WIDTH_AT` and
`SEQLZ_LIT_DELTA_AT` are the masks and shifts that pick the fields, and `seqlz_internal.h` checks with
`static_assert`s that they fit together ([chapter 7](#ch-internal--byte2)).
@assert own.text.lcode == own.lit_own and own.text.w5 + 5 == own.text.w and int(own.text.t, 16) < 128

@listing src/seqlz_format.h 250-266

The encoder subtracts the earlier byte from the literal, the decoder adds it back, both modulo 256,
with the byte the encoder used, which the decoder has written to the page by then. What the delta is
for: a literal that equals the byte it is to becomes 0, and one that is a little larger or smaller
becomes a small number or one just below 256, e.g. `0x01` or `0xff`. When many of a page's literals do
that, their deltas are a handful of values that come again and again, and a literal table codes those
in fewer bits than the literals as they are.

The page of 512 pointers of [chapter 9](#ch-matcher--back), `0x7f1234560010 + 16 * i`, shows the
second choice. At level 3 its sequences are mostly 1 literal, the low byte of a pointer, and the 7
bytes after it copied from the pointer before, offset 8. `SEQLZ_DELTA_8` stores each such literal as
its delta to the low byte of the pointer before, 8 bytes back: the literal `0x30` minus `0x20`, which
gives `0x10`. `SEQLZ_DELTA_LAST` would take the same bytes after the first sequence, as every offset
before them is 8, and its price is the same; the encoder keeps the first of equal prices
([chapter 11](#ch-literals-enc--deltas)). Of the {{delta.ptr.n_lit}} literals,
{{delta.ptr.distinct.plain}} different byte values as they are become {{delta.ptr.distinct.deltas}}
as deltas, and {{delta.ptr.h.10}} of them are `0x10`. Level 2 stores the page in {{delta.ptr.l2.len}} bytes
with raw literals, as coding them saves too little; level 3 without deltas would store it in
{{delta.ptr.l3plain.len}} bytes, with them in {{delta.ptr.l3.len}}. Its byte 2 is `{{delta.ptr.l3.t}}`:
code {{delta.ptr.l3.code}}, the page's own table, $w = {{delta.ptr.l3.w}}$, bit 7 set, and byte
{{delta.ptr.l3.dt_at}}, right before the sizes, is {{delta.ptr.l3.dt}}, `SEQLZ_DELTA_8`.
@assert delta.ptr.l3.dt == ch06.DELTA_8 and delta.ptr.price[2] == delta.ptr.price[3] and delta.ptr.recs_last8 == delta.ptr.recs - 1 and delta.ptr.l3.code == own.lit_own and delta.ptr.l3.len < delta.ptr.l3plain.len < delta.ptr.l2.len and delta.ptr.l3.dt_at == 3

This page shows why a delta and not a XOR. A XOR of the same literals with the same bytes, which
level 3 wrote before the deltas, gives {{delta.ptr.xor.distinct}} values: only {{delta.ptr.xor.h.10}}
are `0x10`, the others `0x30`, `0x70`, `0xf0` and more, wherever adding `0x10` carries into the
upper bits of the low byte, e.g. from `0x30` to `0x40`, where `0x30` XOR `0x40` is `0x70`. Pointers to
objects in a row, counters and sizes grow by a little from one field to the next, and a subtraction
turns that into one small value where a XOR gives one value per pattern of carries.
@assert delta.ptr.xor.distinct > delta.ptr.distinct.deltas and F("delta.ptr.xor.h.10") < F("delta.ptr.h.10") and min(F("delta.ptr.xor.h.30"), F("delta.ptr.xor.h.70"), F("delta.ptr.xor.h.f0")) > 0

0 is not one of the values: bit 7 already says whether there are deltas, and the decoder rejects a byte
of 0 or above 3. The byte is byte 3 with every code, a fixed table's or the page's own, and the sizes,
the description and the streams move one byte up behind it. That `SEQLZ_DELTA_LAST` takes the
smallest multiple of the offset that is 4 or more, 4 for an offset of 1 or 2 and 6 for 3, is a rule
for the decoder: it adds 4 or 8 bytes at a time, which only works if every byte it adds lies before
the word it writes, at least 4 bytes back. After a match with an offset of 3 the bytes repeat every 3
bytes, so the byte 6 back is the byte 3 back. `seqlz_delta_last()` in [chapter 7](#ch-internal--byte2)
computes that distance, the encoder picks the delta in `seqlz_delta_literals()`
([chapter 11](#ch-literals-enc--deltas)), and the decoder adds the bytes back in `undelta_literals()`
([chapter 13](#ch-literals-dec--undelta)). On ten samples of 20 000 pages, level 3 stores 869.7 bytes
per page with deltas, 915.6 without, 5.0% less, measured on 21st and 22nd October 2026
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-stores-its-literals-as-deltas-not-xors-084-fewer-bytes-than-the-xor-kept),
[one rule](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#one-rule-for-which-literals-are-deltas-045-fewer-bytes-a-simpler-spec-kept));
[chapter 17](#ch-decisions--deltas) has what it costs in time.

@listing src/seqlz_format.h 267-292
@listing src/seqlz.h 42-48

The comment is the format of [a page's own table](#ch-format--own) in a few lines. "A run of k bits"
means a run whose number has `k` bits: a run of 32 is `0b100000`, 6 bits, so it is symbol
`SEQLZ_OWN_RUN + 6`, 15, followed by the 5 bits below its top bit, `00000`. The top bit is not sent,
the symbol says it, as for the [length values](#ch-header--length-values). `SEQLZ_OWN_SYMBOLS` is the 10
lengths, symbols 0 to 9, and the 9 run widths, symbols 10 to 18: {{own.symbols}}. A code of at most
`SEQLZ_OWN_CODE_BITS`, 8 bits, makes the decoder's table of `OWN` 256 entries, which one byte of the
description indexes ([chapter 13](#ch-literals-dec--own)). `seqlz_own_lengths` is generated into
`seqlz_lit_sets_4k.inc` and `_16k.inc`, after the 8 literal tables ([chapter 8](#ch-codes--lit-sets-file)).

The last paragraph is the reason a page's own table costs the decoder no memory. The decoder needs a
decode table of 1024 entries, 2 KiB, for the literals, and work memory is what the project counts
([chapter 1](#ch-zram--work-memory)). The output page is free while the literals are decoded: they go
into the scratch first, and only then does the loop over the sequences write the page
([chapter 13](#ch-literals-dec--own)).

@listing src/seqlz_internal.h 28-51

The rule of `SEQLZ_LIT_CODED_MIN` in the encoder is `d + coded + 51 < n - n / 16`, with `coded` the
bytes of the 8 streams, `d` those of a page's own table, 0 for a fixed one, and `n` the literals. With 1000 literals and a fixed table the streams may take at most 886 bytes,
11.4% less than the raw literals; with 100 they must stay below 43 bytes, and few pages with so few
literals get there. [Chapter 11](#ch-literals-enc--rule) has the measurements behind the 51 bytes.
They also cover the larger header, at most 13 bytes more than the raw page's 2, which a `static_assert`
in `seqlz_compress.c` checks, because the coded literals are written over the raw ones in place
([chapter 11](#ch-literals-enc--in-place)).
<!-- undated: arithmetic on the rule of SEQLZ_LIT_CODED_MIN, no measurement -->
@assert ch06.LIT_CODED_MIN == 51

The scratch is the decoder's buffer for coded literals, one per CPU and part of the work memory,
{{const.SCRATCH}} bytes on 4 KiB pages and {{k16.const.SCRATCH|grp}} on 16 KiB pages. The loop over the
sequences then reads the literals from there as if they were raw, and its fast path copies 16 bytes at
a time ([chapter 14](#ch-decode)): the 16 bytes more are what the last copy may read past the last
literal, and what the last step of `seqlz_decode_literals()` may write
([chapter 13](#ch-literals-dec)). For raw literals the bytes after them in the compressed page play
that part.

## Decoding a page {#ch-header--encode-decode}

@listing src/seqlz.h 50-64

Lines 362 to 364 are the most important lines of this header. zram stores no checksum, so a page
damaged in memory, or by a bug elsewhere, reaches the decoder as it is, and a decoder that wrote outside
its buffers would turn a damaged page into a damaged kernel. [Chapter 14](#ch-decode--safety) has the
checks and the fuzzing that keep the promise. The page does not say which tables wrote it. `scratch` may
be `NULL` for a device at level 1, `seqlz-1`, whose compressor never codes literals, and saves that
memory per CPU ([chapter 15](#ch-zram-glue--backend)). There is no partial result: either the page is
valid and `dst` holds it, or `-EINVAL`.

@listing src/seqlz.h 66-75

Only a sequence with a match has a token, so `ml` is at least 4; the last sequence's `ml` of 0 would
wrap, and `min()` would make it 31. Until 11th October 2026 the last sequence had a token, and the line
was `ml ? min(ml - 4, SEQLZ_ML_CAP) : 0`. Two tokens:

| `ll` | `ml` | class | token |
| --- | --- | --- | --- |
| 1 | 7 | 0 | ${{ch06.tok.1.a}} + {{ch06.tok.1.b16}} + {{ch06.tok.1.c512}} = {{ch06.tok.1.tok}}$ |
| 20 | 100 | 3 | ${{ch06.tok.3.a}} + {{ch06.tok.3.b16}} + {{ch06.tok.3.c512}} = {{ch06.tok.3.tok}}$ |
@assert ch06.tok[1].tok == 49 and ch06.tok[3].tok == 2047

Token 49 is the sequence of an array of pointers, 1 literal and 7 bytes at the repeat offset
([chapter 9](#ch-matcher--back)), with a {{tok.len.49}}-bit code. Token 2047 has both fields at their
caps, so both length values follow, `ll - 15 = 5` and `ml - 35 = 65`. gcc compiles the function with a
conditional move per cap and no branch.

## The compressor's state {#ch-header--compressor}

@listing src/seqlz_internal.h 53-63
@listing src/seqlz.h 76-78

The first comment is the compressor's design: the matcher and the encoder are one loop over the page
([chapter 9](#ch-matcher), [chapter 10](#ch-encoder)). Each entry of the hash table is the position of
the last place in the page whose next 5 bytes had that hash: 4096 entries of 2 bytes on 4 KiB pages,
one per position if the hash spread them perfectly. [Chapter 9](#ch-matcher--hash) has the
measurements of 2048 and 8192 entries. The kernel's copy has `12U` in place of the `?:`.

`SEQLZ_MAX_SEQUENCES` is $4096 / 4 + 1 = {{const.MAX_SEQUENCES}}$ on 4 KiB pages,
{{k16.const.MAX_SEQUENCES}} on 16 KiB pages. `seqlz_encode()` rejects more, `seqlz_find()` needs room
for that many, and the decoder's work per page is bounded by the same count.

@listing src/seqlz_internal.h 64-75

A **level** is what zram calls the knob a user sets per device, `level=` in
`/sys/block/zram0/algorithm_params` ([chapter 15](#ch-zram-glue--backend)). seqlz's three levels write
the same format, and one decoder reads them all; they differ in how hard the compressor works:

| level | harness name | matcher | literals |
| --- | --- | --- | --- |
| 1 | `seqlz-1` | greedy | raw |
| 2, `SEQLZ_LEVEL_CODED` | `seqlz-2` | greedy | one of the 8 fixed tables, where it saves enough |
| 3, `SEQLZ_LEVEL_OWN` | `seqlz-3` | a lazy step for matches below 20 bytes, each match extended backwards | also a table of the page's own, and stored as deltas to earlier bytes of the page first |

The code compares with `>=`, not `==`: "from `SEQLZ_LEVEL_CODED` on" is a rule that a level 4 would
inherit. The lazy step and the extension backwards are [chapter 9](#ch-matcher--lazy), the own table
[chapter 11](#ch-literals-enc--own), the deltas [chapter 11](#ch-literals-enc--deltas) too. On the Mi 9T's Cortex-A76, in the phone's own kernel on 15th
October 2026, level 3 stored a page of a phone session in 952.0 bytes of zsmalloc memory against 997.0
for level 2, and took about 16 µs to write one against 12 µs
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-a-lazy-matcher-and-a-literal-table-of-the-pages-own-zstd-3s-memory-in-less-than-half-its-time-on-the-phone-kept)),
before the deltas, which made level 3's writes about 4 µs slower on the A76 in userspace
([chapter 17](#ch-decisions--deltas)).
@assert ch09.lazy_len == 20

@listing src/seqlz_internal.h 77-89
@listing src/seqlz_internal.h 137-151

`struct seqlz_state` is the compressor's work memory, one per CPU, as zram compresses on every CPU at
once. Once the matcher is through the page, the hash table is free, and `seqlz_code_literals()` keeps a copy
of the raw literals there while it writes the coded ones over their old place in the output
([chapter 11](#ch-literals-enc--in-place)); at level 3 with deltas the copy is the deltas,
which `seqlz_delta_literals()` writes there. A `static_assert` in `seqlz_compress.c` checks that the
table alone has room for a page, so the union is {{size.state}} bytes, no more than the table, and
{{k16.size.state|grp}} on 16 KiB pages. `work` is the same bytes again as `u64`, for level 3:
`struct own_work`, what the encoder needs for a page's own table, lies at byte {{own.enc.work_at}},
behind the copy of the literals, which on a page that gets such a table is never longer
([chapter 11](#ch-literals-enc--own)).
`(2U << SEQLZ_HASH_BITS) / 8U` is the table's bytes in `u64`s, so `work` adds no byte to the union.
What it adds is the alignment of 8: a union of `u16` and `u8` alone could start at any even address,
and `struct own_work` has `u32` members. In the kernel the hash table and the decoder's scratch share
one allocation of 8 KiB per CPU ([chapter 15](#ch-zram-glue--backend)).

@listing src/seqlz.h 80-93

`seqlz_find()` runs the matcher alone, e.g. to compare matchers or to feed `seqlz_encode()`, with the
matcher of the level it is given: at level 3 the sequences are other ones. It does not copy the
literals; a tool takes them from the page.

@listing src/seqlz.h 95-125

`seqlz_encode()` writes a page from sequences it is given. It needs the state only at level 3, as work
memory for a page's own table ([chapter 11](#ch-literals-enc--own)) and for the deltas; below
it `st` may be `NULL`, and level 3 without it returns 0, as a level outside 1 to 3 does. `page` is the
page the sequences describe, for level 3's deltas, which take the bytes they are to from the page,
also bytes of matches, which the literals alone do not have. With `NULL` the literals are not stored as deltas,
and below level 3 `page` is not read. Nothing checks that the page fits the sequences: a wrong one
gives a page that decodes to something else.

`seqlz_encode()` is for the tests, the fuzzer that feeds the encoder raw sequences, and
`tools/seqlz-worst/`, which writes valid pages from sequences the matcher would never find, to search
for the pages slowest to decode. It checks the rules of lines 455 to 458, the ones the decoder checks,
before it writes anything. It did not always: a match shorter than 4 bytes made `ml - 4` wrap to near
$2^{32}$, its length value took more than 31 bits, and 1025 such sequences wrote past the two pages of
`dst`. `fuzz/seqlz_encode_fuzz.c` came with the fix (commit `256768e`). 0 can mean failure because
every page has at least its 2-byte header.

@listing src/seqlz.h 127-152

`seqlz_compress()`'s size contract: zram's buffer of two pages is always enough, also for random data,
which [chapter 10](#ch-encoder) proves with the 31 bits of `SEQLZ_MAX_ESCAPE_LEN`. A smaller buffer
works too, e.g. the single page of zswap, the kernel's other compressed swap, a cache in RAM in front of
a swap device. A page that does not fit is then an error up to 32 bytes early, because before each
sequence the encoder checks for its literals plus `ENC_ROOM`, 32 bytes, not for the exact bytes. The
page has to fit with raw literals even where its coded form would be smaller, since the coded literals
go over the raw ones afterwards. At level 3 the encoder needs {{delta.rec}} bytes more per sequence with
literals, for a record that `seqlz_delta_literals()` reads ([chapter 10](#ch-encoder--records)); in a
`dst` too small for those the page is matched a second time without them, and stored without deltas. The tests check that `seqlz_compress()` writes the same bytes as
`seqlz_encode()` for the sequences of `seqlz_find()`, at each level.

In the kernel's copy this function is `seqlz_compress_page()`, `static`; the public `seqlz_compress()`
of `include/linux/seqlz.h` calls it with the kernel's tables, returns `-EINVAL` for a level outside 1
to 3, and `-E2BIG` instead of 0 ([chapter 15](#ch-zram-glue)).
