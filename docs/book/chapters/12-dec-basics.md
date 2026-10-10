# The decoder's tools: prefetches, copies and a bit reader {#ch-dec-basics}

`src/seqlz_decompress.c` is the decoder, 688 lines. Its last function, `seqlz_decode()`, is the loop
over the sequences of a page, and [chapter 14](#ch-decode) goes through it. Before it, the file has two
groups of helpers. The first 243 lines, this chapter, are what every decoder of an LZ format needs in
some form: a way to get the tables into the cache before they are used, copies of 8 and 16 bytes,
a copy for the literals, a copy for matches that may overlap the bytes they write, and a reader that
takes bits off the bitstream. Lines 245 to 427 decode the coded literals, see
[chapter 13](#ch-literals-dec).

Every function here is `static` and `inline` or `__always_inline`. With `-O2` and `-O3`, and the
kernel copy is built with `-O3`, none of them is a call in the machine code: the compiler copies their
bodies into `seqlz_decode()` and `decode_literals()`, and then optimizes the result as one function.
Only `__always_inline` forces that; with `-Os`, gcc keeps `prefetch_lines()`, `seqlz_br_refill()` and
`seqlz_br_value()`, which are plain `inline`, as functions of their own. They exist so that the loop in
chapter 14 reads as a list of steps, and so that each step can be explained on its own terms. The
tests and the fuzzers reach them only through `seqlz_decode()`.

## The file starts with the shared header {#ch-dec-basics--head}

@listing src/seqlz_decompress.c 1-5

Line 1 is the licence ([chapter 5](#ch-compat--top)). The comment names the one function that the rest of
the kernel can call; everything else in the file is `static`. `seqlz_internal.h` brings in `seqlz.h`,
which brings in `seqlz_compat.h`: the format's constants ([chapter 6](#ch-header)), the layout of the
tables and the token table's entries ([chapter 7](#ch-internal)), and the kernel's names for types and
helpers such as `u8`, `get_unaligned_le64()` and `prefetch()` ([chapter 5](#ch-compat)).

## The tables are prefetched, 8 lines per iteration {#ch-dec-basics--prefetch}

A swap-in decodes one page and then the kernel goes on with something else. When the next swap-in
comes, the decoder's tables may no longer be in the CPU's caches: the token table alone is 8 KiB, and
whatever ran in between used the cache for its own data. Then every first lookup into a line of the
table is a cache miss, and the decoder waits for memory.

A miss costs about 100 ns. The phone's little Cortex-A55 is an in-order core and takes its misses one
after the other; prefetches issued back to back turn that row of misses into one wait
([chapter 3](#ch-cpu--prefetch)).

@listing src/seqlz_decompress.c 7-26

`prefetch_lines()` asks for every line of a table. `p` is `const void *`, so that the callers can pass
a table of `u32` or of `u16` without a cast, and line 19 turns it into a byte pointer: arithmetic on
`q` is in bytes. `end` on line 20 is one past the last byte of the table, a pointer C allows. The outer
loop on line 23 moves `q` by `PREFETCH_STEP`, 512 bytes. The inner loop on line 24 calls `prefetch()`
for `q`, `q + 64`, up to `q + 448`, 8 lines. `L1_CACHE_BYTES` is the kernel's name for the size of a
cache line, 64 on x86-64 and arm64. `prefetch()` is the kernel's: on arm64 a `prfm` instruction, on
x86-64 `prefetcht0` with a patch to the kernel that [chapter 5](#ch-compat) explains.

The `U` on `512U` makes the constant `unsigned int`, the type of `k`, so that the compare on line 24 is
between two unsigned numbers and no compiler warns about mixed signs. The inner loop's bounds are both
constants, so the compiler unrolls it. gcc 16.2.1 with `-O3` and the kernel's flags for x86-64 writes
8 `prefetcht0` instructions in a row for each round of the token table's loop, and the 1 KiB tables of
the length values, 2 rounds each, without any loop at all: 16 `prefetcht0` each.

The sizes, with the tables of [chapter 7](#ch-internal):

| table | entries | bytes | rounds of 512 bytes | lines |
| --- | --- | --- | --- | --- |
| token, `t->token.decode` | 2048 of 4 bytes | 8192 | 16 | 128 |
| `ll` length values, `t->ll.decode` | 256 of 4 bytes | 1024 | 2 | 16 |
| `ml` length values, `t->ml.decode` | 256 of 4 bytes | 1024 | 2 | 16 |
| one literal table, `t->lit[i].decode` | 1024 of 2 bytes | 2048 | 4 | 32 |

That is 160 lines for a page with raw literals and 192, 12 KiB, for a page with coded literals. The
same with 16 KiB pages: the tables do not grow with the page.

::: why 8 lines per round, not 1, not 128
The comment on lines 11 to 13 gives the reason: with one line per round, the loop's own instructions,
the add, the compare and the branch, were more than the prefetches. That was measured twice. On the
Mi 9T's little core, prefetching 8 lines per round took the decoder from 16 809 to 16 547 instructions
per page and from 16 676 to 16 247 cycles
([explored-designs.md, "seqlz's decoder for an in-order core"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).
And when the plain loop was measured again in October 2026, the Mi 9T's big Cortex-A76 read pages with coded
literals cold in 8.15 to 8.84 µs over the rounds with it, against 7.38 to 7.99 µs for two builds of the
code as it is ([explored-designs.md, "Five simplifications"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
Why 8 and not all 128 lines of the token table without a loop is not recorded; 8 already takes the
loop's overhead to an eighth.
:::

::: measured What the prefetches are worth on a phone
On the Mi 9T, in its own kernel, 20 000 pages of the second phone dump, clocks fixed: without any table
prefetch, the Cortex-A55 read cold pages 14.5 µs slower on average and 30 µs slower at p99; even the 32
lines of the two length tables were worth 2.8 µs to it. The A76 was 1.2 µs faster without any
prefetch in that run
([explored-designs.md, "Six choices made on the PC", item 4](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#4-prefetching-the-tables-the-a55-needs-it-the-a76-is-faster-without)).
For a while the decoder therefore prefetched the token table only on in-order cores. Since 10th
October 2026 every core gets the prefetch ([chapter 17](#ch-decisions--one-loop-for-all-cores)), and on
the A76 the cost did not show again: cold reads in
7.44 and 7.65 µs with it, 8.07 and 7.60 µs without, in two runs
([explored-designs.md, "The kernel copy's choices"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16)).
:::

::: warn "Every cache line" only if the table starts at a line
`prefetch_lines()` prefetches the line of `q`, then the lines 64, 128, ... bytes further. If `p` is
not at the start of a line, the table covers one line more than it has rounds × 8, and the last one is
not prefetched. In `struct seqlz_tables` on a 64-bit build, the token table is at offset 0, `ll.decode`
at 40 968 and `ml.decode` at 42 120, both 8 bytes past a multiple of 64, and the literal tables at
14 340, 17 412 and so on, 4 bytes past one.

Where the struct starts decides the rest. The harness allocates it with `quetschn_zalloc()`, which
aligns a block of 4096 bytes or more to a page. There the token table starts at a line, the last 8
bytes of each length table, entries 254 and 255, are in a 17th line, and the last 4 bytes of a literal
table, entries 1022 and 1023, in a 33rd. In the kernel, the tables are one static object,
`seqlz_fixed_tables` in `tools/kernel-port/api_codes.c`, and C only promises it the alignment of its
widest member, the `u64` of `lit_cost`, 8 bytes. The object files of a kernel build have its section
aligned to 32 bytes with gcc for x86-64, and to 8 with gcc for arm64 and with clang for x86-64. So in
the kernel, any of the tables, the token table too, can start inside a line and miss its last one,
depending on where the linker puts the struct. The literal tables miss theirs in every build: 4 bytes
past a multiple of 64, in a struct that starts at a multiple of 8, is never the start of a line.

With canonical codes, the entries at the end of a table belong to the longest codes, the rarest
symbols ([chapter 2](#ch-compression)), so a missed last line costs a miss only on a rare code.
:::

@listing src/seqlz_decompress.c 28-34

The loop above prefetches `q + k` for every `k` up to `q + size - 64` only if `size` is a multiple of
512; otherwise the last round would form pointers past `end`, which C does not allow, and prefetch lines
behind the table. These three `static_assert`s make the build fail when a table's size stops being a
multiple of 512. `static_assert(condition, message)` is C11's `_Static_assert`: the compiler evaluates
the condition, and if it is 0, it stops with the message. Nothing of it is in the machine code.

`sizeof_field(type, member)` is the kernel's macro for the size of a struct member without an object
of the struct: `sizeof(((type *)0)->member)`. `sizeof` does not evaluate its operand, so the null
pointer is never read; the expression only has a type. For the token table that is
`sizeof(u32[2048])`, 8192, and 8192 % 512 is 0. If someone made the length values' codes at most 6 bits,
`SEQLZ_MAX_BITS` 6, their tables would have 64 entries of 4 bytes, 256 bytes, and the second assert
would stop the build with "the length values' tables".

## Copies of 8 and 16 bytes, and what overlap each allows {#ch-dec-basics--copies}

@listing src/seqlz_decompress.c 36-58

`copy8()` copies 8 bytes from `s` to `d`, `copy16()` 16. Both go through a `u64` in a register, and both
use `__builtin_memcpy()` with a constant size. That is how C says "load 8 bytes from any address" without
undefined behaviour: `*(const u64 *)s` would be undefined when `s` is not a multiple of 8, and
`d` and `s` here are anywhere in the page. The compiler knows what `__builtin_memcpy()` of 8 bytes means
and writes one 8-byte load and one 8-byte store on x86-64 and arm64, which both load from any address.

Why the builtin and not `memcpy()`: with `CONFIG_FORTIFY_SOURCE`, `memcpy()` can become a call, which
the top of `seqlz.h` explains ([chapter 6](#ch-header--summary)); the comment on line 37 points there.

`__always_inline` is the kernel's spelling of `inline __attribute__((__always_inline__))`. Plain `inline`
is a hint the compiler may ignore; this one it may not. A copy of 8 bytes that becomes a call costs more
than the copy.

The difference between the two functions is the order of loads and stores, and that decides when each
may be used.

- `copy8()` loads all 8 bytes, then stores them. Its source may be bytes that the store just before it
  wrote, as long as the source ends before `d`: `s + 8 <= d`. Then each load reads only finished bytes.
- `copy16()` loads 16 bytes, both halves, before it stores any. If the source overlaps the destination,
  e.g. `s = d - 10`, then its second load reads `d - 2` to `d + 5`, six bytes that this very copy was
  about to write, and gets the old contents. So `copy16()` is only for copies whose source and
  destination do not overlap at all: literals, which come from the compressed page or the scratch and go
  to the output page.

A worked example of why that matters for matches. The output has `0123456789` before `d`, and a match
has offset 10 and length 16. The format copies byte by byte ([chapter 4](#ch-format)), each byte from
10 places back, so the 16 bytes are `0123456789012345`: the last 6 repeat the first 6 of the match
itself. Two `copy8()` calls get that right. The first loads `d - 10` to `d - 3`, `01234567`, and stores
them at `d`. The second loads `d - 2` to `d + 5`: `8`, `9`, and the `012345` the first one just
stored, and writes `89012345` at `d + 8`. One `copy16()` from `d - 10` would have loaded `d` to `d + 5`
before anything was written there.

::: cpu A load right after a store to the same bytes
A store does not go to the cache at once; it waits in a store buffer. A load that wants bytes a recent
store wrote can take them from there, **store forwarding**, but on most cores only when the load reads
bytes of one store, not parts of two. With offsets from 9 to 15, the second `copy8()` reads 8 bytes of
which some come from the store just before and some from older memory, and then the load waits until
the store reached the cache. `ls_bad_status2.stli_other`, a counter of the Ryzen 9 7950X, counted 359
such loads per page for `seqlz-fast`'s decoder and 363 for `lz4`'s in September 2026
([explored-designs.md, "zram: prefetch the compressed data", the paragraph "A fast path in seqlz's
decoder"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
That is the price of correct overlapping copies. Offsets below 8 cannot use `copy8()` at all, and the
copy they get below stores one register again and again, with no load after the first.
:::

## copy_literals(): 16 bytes at a time, also past the end {#ch-dec-basics--literals}

@listing src/seqlz_decompress.c 60-88

`copy_literals()` copies the `nl` literals of one sequence from `lit` to `d`. Its one caller, the slow
path of `seqlz_decode()`, has checked
that `nl` literals are there and that they fit into the page; `s_end` is not the end of the literals but
the end of the memory behind them that may be read, `lit_bound` in [chapter 14](#ch-decode): the end of
the compressed page for raw literals, the end of the scratch buffer for coded ones.

The idea is that copying 16 bytes is cheaper than copying exactly `nl` bytes. A copy of exactly 3 bytes
needs either a loop of 3 rounds or a branch on the length to pick a 2-byte and a 1-byte copy. A copy of
16 bytes is two loads and two stores, always the same instructions. The bytes it writes past `nl` are
not lost data: the next sequence writes its match right after the literals, at `d + nl`, over them.

Line 76 checks that 16 bytes fit into the page and that 16 bytes can be read from `lit`. Both
differences are of pointers, of type `ptrdiff_t`, a signed type; they are never negative here, and the
cast to `unsigned int` makes each compare one between two unsigned numbers, the same on 32-bit and
64-bit builds. If both hold, line 78 copies the first 16 bytes, without looking at `nl`: also for `nl =
0`, where all 16 bytes are overwritten later. The comment on lines 72 to 75 says why the first copy is
not part of the loop: most sequences have fewer than 16 literals, and a loop that runs once or not at
all, depending on the data, is a branch that mispredicts on a page the CPU has not seen.

The loop on lines 81 to 85 continues with 16 bytes at a time while `k < nl` and 16 more bytes fit after
`k` on both sides. The last loop, lines 86 and 87, copies the rest one byte at a time. It only runs near
the end of the page or of the readable input: if 16 more bytes fit on both sides, the `while` loop takes
them. And since the caller checked that `nl` fits, what is left for the byte loop is fewer than 16
bytes, at most 15, as the comment on line 65 says.

Three cases with numbers:

| `nl` | room in page and input | what happens | bytes written |
| --- | --- | --- | --- |
| 3 | 4000 and 500 | one `copy16()`; loop: `16 < 3` is false | 16, 13 of them overwritten by the match |
| 40 | 4000 and 500 | `copy16()` at 0; loop at 16 and 32; `k = 48` | 48 |
| 10 | 10 and 500, the last sequence | no `copy16()`, `10 >= 16` is false; loop does not run | 10, byte by byte |

A slower `copy_literals()` with an exact length would give the same bytes. That is the point of the
over-copy: it changes the time, never the result, as long as the writes stay inside the page and the
reads inside readable memory. The checks on lines 76, 77, 81 and 82 make sure they do.

## Offsets below 8: one register, stored again and again {#ch-dec-basics--pattern}

A match can start fewer bytes back than it is long. The format defines the copy byte by byte, so such a
match repeats the bytes it has just written: offset 1 after an `a` with length 6 writes `aaaaaa`, and
that is how a run of zeros, which pages are full of, compresses to a few bits. `copy8()` is correct only
for offsets of 8 or more, see above. For offsets 1 to 7 the decoder builds the repeated pattern in a
register first.

@listing src/seqlz_decompress.c 90-94

@listing src/seqlz_decompress.c 96-115

`repeat_pattern(d, off)` returns the 8 bytes that a byte-by-byte copy would write at `d`. Line 113
loads 8 bytes from `d - off`, as a little-endian number, so that the byte at `d - off` is the lowest byte.
Of these 8 bytes, the first `off` are the pattern, and the other `8 - off` are at `d` and after,
not written yet, whatever the page buffer held before. `~0ULL >> (64U - 8U * off)` is a mask of the low
`8 * off` bits: for `off = 3`, `~0ULL >> 40` is `0xffffff`. The `&` keeps the pattern and clears the
rest. Then the multiply by `repeat[off]` puts a copy of the pattern at every byte where the constant has
a 1.

`repeat[3]` is `0x0001000001000001`: ones in bytes 0, 3 and 6. The pattern `abc` is `0x636261` as a
little-endian number. The product is `0x636261 + (0x636261 << 24) + (0x636261 << 48)`; the three
copies do not overlap, so there is no carry from one byte into the next, and the third copy's `c`, at
byte 8, falls off the top of the 64 bits, which unsigned arithmetic in C allows. The result is
`0x6261636261636261`, bytes `a b c a b c a b`. All offsets, each with the `off` bytes before `d` being
`a`, `ab`, up to `abcdefg`, and other bytes after it, computed with Python:

| `off` | pattern | mask | `repeat[off]` | product | bytes at `d` | `repeat_step[off]` |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `a` | `0xff` | `0x0101010101010101` | `0x6161616161616161` | `aaaaaaaa` | 8 |
| 2 | `ab` | `0xffff` | `0x0001000100010001` | `0x6261626162616261` | `abababab` | 8 |
| 3 | `abc` | `0xffffff` | `0x0001000001000001` | `0x6261636261636261` | `abcabcab` | 6 |
| 4 | `abcd` | `0xffffffff` | `0x0000000100000001` | `0x6463626164636261` | `abcdabcd` | 8 |
| 5 | `abcde` | `0xffffffffff` | `0x0000010000000001` | `0x6362616564636261` | `abcdeabc` | 5 |
| 6 | `abcdef` | `0xffffffffffff` | `0x0001000000000001` | `0x6261666564636261` | `abcdefab` | 6 |
| 7 | `abcdefg` | `0xffffffffffffff` | `0x0100000000000001` | `0x6167666564636261` | `abcdefga` | 7 |

`repeat_step[off]` is the largest multiple of `off` up to 8. The same 8 bytes fit again that far on:
for `off = 3`, the bytes at `d + 6` start with `a` again, because 6 is a multiple of 3. So after the first
store of the register at `d`, a store of the same register at `d + 6`, `d + 12`, `d + 18`, ... writes
the rest of the match, and no further load is needed. For offsets 1, 2 and 4 the step is the full 8
bytes. Entry 0 of both tables is a placeholder: an offset of 0 is invalid and never gets here, and for it
the mask would be a shift by 64, which C does not define.

Why `get_unaligned_le64()` and `put_unaligned_le64()`, little endian, and not a plain load: the
multiply needs the first byte of the pattern in the lowest byte of the number, so that the shifted
copies land at higher addresses. On a little-endian CPU, x86-64 and arm64 as Linux runs them, that is a
plain load. On a big-endian one, s390x, the helpers swap the bytes, and the result in memory is the
same. CI checks that on s390x under qemu, with the same bytes out of `test/seqlz_endian.c`.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 300" role="img" aria-label="A match with offset 3 and length 20: the register abcabcab is stored at d, d plus 6, d plus 12 and d plus 18, each store overlapping the one before by 2 bytes">
  <g font-size="12">
    <text x="10" y="34">page</text>
    <rect x="100" y="20" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="106" y="34">a</text><rect x="120" y="20" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="126" y="34">b</text><rect x="140" y="20" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="146" y="34">c</text>
    <rect x="160" y="20" width="520" height="20" fill="none" stroke="currentColor" stroke-dasharray="3 3"/><text x="300" y="34">not written yet</text>
    <text x="96" y="14" font-size="11">d - 3</text><text x="160" y="14" font-size="11">d</text>
    <text x="10" y="78">pat</text>
    <g class="acc"><rect x="160" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text x="166" y="78">a</text><rect x="180" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text x="186" y="78">b</text><rect x="200" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text x="206" y="78">c</text><rect x="220" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text x="226" y="78">a</text><rect x="240" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text x="246" y="78">b</text><rect x="260" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text x="266" y="78">c</text><rect x="280" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text x="286" y="78">a</text><rect x="300" y="64" width="20" height="20" class="fill-acc" stroke="currentColor"/><text x="306" y="78">b</text></g>
    <text x="330" y="78" font-size="11">repeat_pattern(d, 3) = 0x6261636261636261</text>
    <text x="10" y="122">k = 0</text><rect x="160" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="166" y="122">a</text><rect x="180" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="186" y="122">b</text><rect x="200" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="206" y="122">c</text><rect x="220" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="226" y="122">a</text><rect x="240" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="246" y="122">b</text><rect x="260" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="266" y="122">c</text><rect x="280" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="286" y="122">a</text><rect x="300" y="108" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="306" y="122">b</text>
    <text x="10" y="152">k = 6</text><rect x="280" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="286" y="152">a</text><rect x="300" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="306" y="152">b</text><rect x="320" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="326" y="152">c</text><rect x="340" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="346" y="152">a</text><rect x="360" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="366" y="152">b</text><rect x="380" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="386" y="152">c</text><rect x="400" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="406" y="152">a</text><rect x="420" y="138" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="426" y="152">b</text>
    <text x="10" y="182">k = 12</text><rect x="400" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="406" y="182">a</text><rect x="420" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="426" y="182">b</text><rect x="440" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="446" y="182">c</text><rect x="460" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="466" y="182">a</text><rect x="480" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="486" y="182">b</text><rect x="500" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="506" y="182">c</text><rect x="520" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="526" y="182">a</text><rect x="540" y="168" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="546" y="182">b</text>
    <text x="10" y="212">k = 18</text><rect x="520" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="526" y="212">a</text><rect x="540" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="546" y="212">b</text><rect x="560" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="566" y="212">c</text><rect x="580" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="586" y="212">a</text><rect x="600" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="606" y="212">b</text><rect x="620" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="626" y="212">c</text><rect x="640" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="646" y="212">a</text><rect x="660" y="198" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="666" y="212">b</text>
    <text x="10" y="262">result</text>
    <rect x="160" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="166" y="262">a</text><rect x="180" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="186" y="262">b</text><rect x="200" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="206" y="262">c</text><rect x="220" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="226" y="262">a</text><rect x="240" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="246" y="262">b</text><rect x="260" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="266" y="262">c</text><rect x="280" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="286" y="262">a</text><rect x="300" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="306" y="262">b</text><rect x="320" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="326" y="262">c</text><rect x="340" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="346" y="262">a</text><rect x="360" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="366" y="262">b</text><rect x="380" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="386" y="262">c</text><rect x="400" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="406" y="262">a</text><rect x="420" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="426" y="262">b</text><rect x="440" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="446" y="262">c</text><rect x="460" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="466" y="262">a</text><rect x="480" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="486" y="262">b</text><rect x="500" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="506" y="262">c</text><rect x="520" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="526" y="262">a</text><rect x="540" y="248" width="20" height="20" class="fill-sheet" stroke="currentColor"/><text x="546" y="262">b</text>
    <g class="mark"><rect x="560" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text x="566" y="262">c</text><rect x="580" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text x="586" y="262">a</text><rect x="600" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text x="606" y="262">b</text><rect x="620" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text x="626" y="262">c</text><rect x="640" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text x="646" y="262">a</text><rect x="660" y="248" width="20" height="20" class="fill-mark" stroke="currentColor"/><text x="666" y="262">b</text></g>
    <text x="160" y="290" font-size="11">d .. d + 19: the match, 20 bytes</text><text x="560" y="290" font-size="11">d + 20 .. d + 25: past it</text>
  </g>
</svg>
</div>
<figcaption>A match of offset 3 and length 20: one register, stored 4 times 6 bytes apart; the stores overlap by 2 bytes, and the last one writes 6 bytes past the match, which the next sequence overwrites.</figcaption>
</figure>

## copy_match(): the slow path's match copy {#ch-dec-basics--copy-match}

`copy_match()` is the match copy for the sequences that do not take the fast path of
[chapter 14](#ch-decode): those with a length value, which can be up to a page long, and those near the
end of the page. The fast path has its own copy, `copy_match_fast()`, which needs no checks of room.

@listing src/seqlz_decompress.c 117-134

The caller has checked `len` against the room in the page and `off` against the bytes written so far,
so `d - off` is inside the page and `d + len` is at most `d_end`. Like `copy_literals()`, the function
writes 8 bytes at a time also past `len`, as long as 8 bytes fit before `d_end`, and copies one byte at a
time only in the last 7 bytes of the page.

The first paragraph of the comment has a measurement behind it. The first decoder of seqlz, in September
2026, used its fast copies only for a match that fit into the page with room to spare; a match that ran
up to the end of the page went byte by byte, all of it. On a page that ends in a long run, e.g. of zeros,
that is up to 4000 single bytes with offset 1, each load waiting for the store before it. The slowest 1% of pages took 5700 ns there, where
`lz4` took 500
([explored-designs.md, "seqlz: lz4's matches, Huffman coded sequences with static tables"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-lz4s-matches-huffman-coded-sequences-with-static-tables)).

The second paragraph describes the branch for offsets below 8, the next listing.

@listing src/seqlz_decompress.c 135-152

Line 138: with fewer than 8 bytes left in the page, no store of 8 bytes fits, and everything is left to
the byte loop at the end of the function, which then copies all `len` bytes, fewer than 8. Otherwise
line 139 takes the step from the table. `off & 7U` changes nothing here, `off` is below 8 on this line.
It is left from the older form of the line, `step = off >= 8 ? 8U : repeat_step[off & 7U]`, which
commit `b163902` moved into this branch, and which goes back to the first version of the copy in
September 2026. There too the index was only used for `off < 8`. The repository does not say why the
mask is there; in the `?:` form it would keep the load in the table if a compiler loaded the entry
before it knew which side it needs.

The comment on lines 140 to 143 is the bound for the load in `repeat_pattern()`: it reads `d - off` to
`d - off + 7`. `off >= 1`, so the last byte read is at most `d + 6`, below `d + 8`, and `d + 8 <= d_end` was
just checked. The load does not read past the page, also when the page is nearly full.

Line 146 stores the pattern at `d`. The loop on lines 148 to 151 stores it again every `step` bytes while
`k < len` and 8 bytes fit. Each round is one store and no load. For `off = 3, len = 20` that is `k = 0,
6, 12, 18`, as in the figure above; at `k = 24` the loop ends because `24 < 20` is false. For `off = 2,
len = 4094`, the match of the example page in [chapter 4](#ch-format--example), it ends at `k = 4088`
because `4094 >= 4096` is false: 8 more bytes would not fit. Then the byte loop at the end copies the last
6 bytes, `k = 4088` to `4093`, each from 2 places back.

::: measured Stores only, no loads, for short offsets
Before 24th September 2026, the code built the first 8 bytes in a register as now, and then copied
`step` bytes on by loading what the store before had written. Since the loaded 8 bytes are the same as
the register, the loads can go: commit `8e8f0d1` stores the register at every step. Decode cycles per
page went from 7750 to 7276 in a loop over 2000 pages on the PC, cold p50 from 1405 to 1380 ns in the
harness; in the kernel VM the change was within the scatter. Earlier, a copy of the first 4 bytes one
by one, as `lz4` does it, was slower: the load after four single-byte stores waits for them
([explored-designs.md, "Second round"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
The format of that time differs from today's; the numbers say which way the change went, not what it is
worth now.
:::

@listing src/seqlz_decompress.c 153-174

For an offset of 8 or more, the function copies with `copy8()`, which is correct for these offsets: each
load ends at `d + k - off + 7`, at most `d + k - 1`, before the bytes the store will write, and the bytes
it reads are either from before the match or written by an earlier `copy8()` of this one.

If 16 bytes fit into the page, lines 163 and 164 copy the first 16 without a loop. As the comment says,
the second copy reads bytes the first wrote, or that were there before: with `off = 10` it reads `d - 2`
to `d + 5`, the worked example of the copies above. `k = 16` then marks 16 bytes as done, also if `len` was
shorter; those extra bytes are inside the page and the next sequence overwrites them. The loop on lines
167 to 170 copies 8 more bytes per round while `k < len` and 8 bytes fit before `d_end`. The byte loop on
lines 172 and 173 is shared by both branches: it copies what is left, one byte at a time, `d[k] = *(d + k
- off)`, which is the format's definition of a match. It runs only within the last 7 bytes of the page,
or for all of a short match when the page had fewer than 8 or 16 bytes left.

::: why 16 bytes first, not 32
Most matches are short. 79% of them are at most 16 bytes, and 32 bytes cover 91%
([explored-designs.md, "Numbers that were only in the codec's comments"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).
When this function still copied every match, 32 bytes first made the median page faster and the slow
pages slower: p99 in a decode loop on the PC, with the format of that time, went from 3570 to 4150 ns,
because the slowest pages are the ones with many short matches, and each of them copied 32 bytes
([explored-designs.md, "Second round"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
Today the fast path takes the short matches, and this function gets mostly those with a length value,
35 bytes and more. Measured again in a swap-in in the kernel VM, on a Ryzen 9 7950X at 4.5 GHz with
gcc 16.2.1, 32 bytes first was 3.5 ns faster on the mean of 6 boots, less than the boots scatter, so 16
stayed
([explored-designs.md, "A branch or more work, again in a swap-in"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained)).
:::

Until 10th October 2026 the function had a variable `back` and a second loop after both branches,
which only ever ran for offsets of 8 and more. Moving that loop into its branch gave the same results
and changed the decoder's timing by no more than moving its code to another address does, so the simpler
code was kept
([explored-designs.md, "copy_match() without its dead branch"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#copy_match-without-its-dead-branch-within-what-the-codes-place-moves-kept)).

## The bit reader: 64 bits, most significant bit first {#ch-dec-basics--bit-reader}

After the literals, a compressed page has the bitstream: per sequence the token's code, the offset
bits, and the length values, without any alignment to bytes ([chapter 4](#ch-format--layout)). The
decoder reads it with a bit reader that keeps up to 64 bits in a register and hands out the next ones.

@listing src/seqlz_decompress.c 176-190

The four fields:

- `p` is the next byte of the bitstream that is not yet in the register, or not all of it.
- `end` is one past the last byte of the bitstream, the end of the compressed page.
- `bits` holds the next bits of the stream, **left-aligned**: the next bit to read is bit 63, the one
  after it bit 62, and so on. The stream is read most significant bit first ([chapter 4](#ch-format)),
  so this is the natural order: the next `n` bits as a number are `bits >> (64 - n)`, and the next code
  indexes a table with its top bits.
- `count` says how many of the top bits are real stream bits. Below them the register can hold bits of a
  byte that is only partly in, or zeros.

`count` is an `int` and not `unsigned`, on purpose. A valid page never takes more bits than it has. A
damaged one can: the decoder does not check before every read, it takes what is in the register, and past
the end of the stream that is zeros. Each read subtracts its bits from `count`, and `count` goes
negative. The decoder checks once, at the end of the page, that `count` is not negative
([chapter 14](#ch-decode)). The comment on lines 180 to 183 is the argument for why that is safe: reading
zeros instead of real bits can give wrong lengths and offsets, never a wrong memory access, because every
length and every offset is checked against the room in the page and the literals where it is used. Wrong
values make a page invalid, which the end check then reports.

### seqlz_br_refill(): one load of 8 bytes, and count ends at 56 to 63

@listing src/seqlz_decompress.c 192-215

The refill has two halves. While at least 8 bytes of the stream are left, line 194, it loads 8 bytes at
once. In the last 7 bytes it goes byte by byte, so that it never reads past `end`: zram hands the decoder
a buffer of exactly the compressed size, and the byte after it may belong to another allocation.

**The fast half.** `get_unaligned_be64(r->p)` on line 201 loads 8 bytes as a big-endian number: the
byte at `p` becomes the top byte, bits 63 to 56, the next one bits 55 to 48, and so on. That is the
stream's order, most significant bit first. On x86-64 and arm64 it is a load and a byte swap.

Line 202 shifts the loaded value right by `count` and ORs it in. The `count` real bits are at the top of
the register; `v >> count` puts the first loaded byte right below them. Of the 64 loaded bits, `64 -
count` fit, the rest fall off the bottom.

Line 207 moves `p` on, but only by the bytes that fit in whole: `(63 - count) >> 3`. Line 208 sets the new
count with `count |= 56`. Both look odd; a worked example makes them plain. Write `count` as `8a + b`,
with `b` from 0 to 7. Then `63 - count` is `8(7 - a) + (7 - b)`, so `(63 - count) >> 3` is `7 - a`: that
many bytes fit in whole below the `count` bits. The `8 - b` bits left at the bottom of the register
hold the first bits of the next byte, which is not counted. The new count is `8a + b + 8(7 - a) = 56 + b`. And `count | 56` is
`56 + b` too, because `8a` is at most 56 and only has bits that 56 has, 8, 16 and 32. So after a refill
`count` is between 56 and 63, and `p` points to the first byte that did not go in whole. That is the
"56 to 63" of the comment, and why the rest of the decoder can count on 56 bits after any refill. The
identity `count + 8 * ((63 - count) >> 3) == (count | 56)` holds for all 64 values of `count`; checked
in Python.

What about the bits of the partial byte that the load put at the bottom of the register, below `count`?
They are the first bits of the byte at `p`. The next refill loads from `p` again and shifts that same byte
to exactly the same position, so the OR writes the same bits over them. The OR needs no mask.

A numeric example, with a stream of the made-up bytes `dc 65 ef db 8a 11 22 33 44 55 66 77 88 99 aa bb
...`, simulated in Python:

| step | `bits` | `count` | `p` |
| --- | --- | --- | --- |
| start | `0x0000000000000000` | 0 | 0 |
| refill: `v = 0xdc65efdb8a112233`, `v >> 0` | `0xdc65efdb8a112233` | 0 \| 56 = 56 | 0 + 63 / 8 = 7 |
| drop 15 bits | `0xf7edc50891198000` | 41 | 7 |
| refill: `v = 0x33445566778899aa`, `v >> 41 = 0x19a22a` | `0xf7edc5089119a22a` | 41 \| 56 = 57 | 7 + 22 / 8 = 9 |
| drop 23 bits | `0x84488cd115000000` | 34 | 9 |
| refill: `v = 0x5566778899aabbcc`, `v >> 34` | `0x84488cd115599de2` | 34 \| 56 = 58 | 9 + 29 / 8 = 12 |

The first refill loads 8 bytes and counts 7: the byte `0x33` is in the register, at the bottom, but not
counted. After dropping 15 bits it sits at bits 22 to 15, `0x198000`. The second refill loads from `p = 7`,
so `v` starts with `0x33`, and `v >> 41` puts it at bits 22 to 15 again: `0x19a22a` is `0x33 << 15`,
`0x44 << 7` and the top 7 bits of `0x55`. `p` moves on by 2, over `0x33` and `0x44`, and stops at `0x55`,
of which only 7 bits went in.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 250" role="img" aria-label="The register before and after the second refill of the example: 41 counted bits and the byte 0x33 below them; the loaded value shifted right by 41 brings 0x33, 0x44 and 7 bits of 0x55">
  <g font-size="12">
    <text x="70" y="18" font-size="11">bit 63</text><text x="710" y="18" font-size="11" text-anchor="end">bit 0</text>
    <text x="10" y="51">before</text>
    <rect x="70" y="36" width="410" height="22" class="fill-sheet" stroke="currentColor"/><text x="275" y="51" text-anchor="middle">41 counted bits, count = 41</text>
    <g class="mark"><rect x="480" y="36" width="80" height="22" class="fill-mark" stroke="currentColor"/><text x="520" y="51" text-anchor="middle">0x33</text></g>
    <rect x="560" y="36" width="150" height="22" fill="none" stroke="currentColor"/><text x="635" y="51" text-anchor="middle">15 zeros</text>
    <text x="480" y="76" font-size="11">from the last load, not counted</text>
    <text x="10" y="115">v &gt;&gt; 41</text>
    <rect x="70" y="100" width="410" height="22" fill="none" stroke="currentColor" stroke-dasharray="3 3"/><text x="275" y="115" text-anchor="middle">41 zeros</text>
    <g class="acc"><rect x="480" y="100" width="80" height="22" class="fill-acc" stroke="currentColor"/><text x="520" y="115" text-anchor="middle">0x33</text><rect x="560" y="100" width="80" height="22" class="fill-acc" stroke="currentColor"/><text x="600" y="115" text-anchor="middle">0x44</text></g>
    <g class="mark"><rect x="640" y="100" width="70" height="22" class="fill-mark" stroke="currentColor"/><text x="675" y="115" text-anchor="middle">55</text></g>
    <text x="70" y="140" font-size="11">v = 0x33445566778899aa, loaded from p = 7; of 0x55 the top 7 bits fit</text>
    <text x="10" y="185">after</text>
    <rect x="70" y="170" width="410" height="22" class="fill-sheet" stroke="currentColor"/><text x="275" y="185" text-anchor="middle">the same 41 bits</text>
    <g class="acc"><rect x="480" y="170" width="80" height="22" class="fill-acc" stroke="currentColor"/><text x="520" y="185" text-anchor="middle">0x33</text><rect x="560" y="170" width="80" height="22" class="fill-acc" stroke="currentColor"/><text x="600" y="185" text-anchor="middle">0x44</text></g>
    <g class="mark"><rect x="640" y="170" width="70" height="22" class="fill-mark" stroke="currentColor"/><text x="675" y="185" text-anchor="middle">55</text></g>
    <text x="70" y="212" font-size="11">bits |= v &gt;&gt; 41; count = 41 | 56 = 57; p = 7 + (63 - 41) / 8 = 9</text>
    <text x="70" y="232" font-size="11">0x33 ORed over the same 8 bits; the 7 bits of 0x55 are not counted, p points to it</text>
  </g>
</svg>
</div>
<figcaption>The second refill of the example: v &gt;&gt; count puts the byte at p right below the counted bits, where its bits already were; count becomes 57, p moves over 2 bytes.</figcaption>
</figure>

The comment on lines 197 to 200 states the precondition: `count` is at least 0 when this half runs. A
shift by a negative amount is undefined in C. `count` only goes negative when a page takes more bits than
it has, and that can happen only after the stream ran out, which is in the other half: while 8 bytes are
left, the decoder always has real bits to take. And `count` is never 64 here, which would make `v >>
count` undefined too: this half leaves `count` at 63 at most, and only the byte half below can reach 64,
after which this half never runs again, because `p` only moves forward and `end - p` stays below 8.

**The byte half**, lines 210 to 213. Each round puts one byte right below the counted bits, at `56 -
count` from the bottom, and counts 8 more. It stops when the register has no room for a whole byte,
`count > 56`, when the stream is used up, `p == end`, or when `count` is already negative. The last
condition is not only about meaning: with `count` negative, `56 - count` would be more than 56, and
for `count` below -7 a shift by 64 or more, undefined.

The example page of [chapter 4](#ch-format--example) has a bitstream of 5 bytes, `dc 65 ef db 8a`, so its
first refill is the byte half: 5 rounds, `bits = 0xdc65efdb8a000000`, `count = 40`, `p = end`. Every later
refill on that page finds `p == end` and does nothing; the 40 bits are all there is.

### seqlz_br_drop(): taking bits

@listing src/seqlz_decompress.c 217-222

Taking `n` bits is a shift left by `n`, so that the next bits are on top, and `count - n`. The cast
`(int)n` keeps the subtraction signed. Without it, `count - n` would be computed in `unsigned int`, and
a negative result would come back into the `int` through a conversion that C leaves to the
implementation. `n` is never 64 or more, which would make the shift undefined: the largest drop is a
token's code with its offset bits, 11 + 14 bits with 16 KiB pages.

### seqlz_br_value(): a length value, with two shifts where one would do

@listing src/seqlz_decompress.c 224-243

A length value is a symbol of the `ll` or `ml` table, then `b` extra bits; the value is a base plus the
extra bits ([chapter 4](#ch-format--length-values)). The decode table does all of the table part in one
lookup ([chapter 2](#ch-compression--table-decoding)): it is indexed by the next 8 bits, the longest
code, and every entry has the code's length `n` in bits 0 to 3, the number of extra bits `x` in bits 4 to
7, and the base in bits 8 to 23 ([chapter 7](#ch-internal)). Lines 232 and 233 take them out.

The extra bits come right after the code. The obvious way to get `x` bits after `n` bits is `(bits << n)
>> (64 - x)`. For `x = 0`, a direct symbol, that is a shift by 64, which C does not define: on x86-64
the shift instruction uses only the low 6 bits of the count, so `>> 64` is `>> 0` and gives the whole
register instead of 0. Line 239 does it in two steps: `>> 1`, then `>> (63 - x)`. For `x = 0` the second
shift is by 63, and after the first shift bit 63 is 0, so the result is 0. For `x = 11` the two shifts
are 1 and 52, together 53 = 64 - 11. No branch, and every shift count is from 0 to 63.

The worked example is the match length value of the example page, with the 4 KiB tables. After the token
and the offset, the register is `0xf7edc50000000000` with `count = 25`:

- The index is the top 8 bits, `0xf7`, 247. `t->ml.decode[247]` is `0x000800b6`.
- `n = 0xb6 & 15 = 6`: the code is `111101`, 6 bits, the symbol 23.
- `x = (0xb6 >> 4) & 15 = 11`: symbol 23 has `b = 23 - 12 = 11` extra bits.
- The base is `(0x000800b6 >> 8) & 0xffff = 0x800 = 2048`, which is `2^11`.
- `bits << 6` is `0xfb71400000000000`, `>> 1` is `0x7db8a00000000000`, `>> 52` is `0x7db`, 2011: the 11
  bits `11111011011`.
- The value is `2048 + 2011 = 4059`, and 17 bits are dropped, `count = 8`.

`seqlz_decode()` adds that to the 35 of the token, and the match is 4094 bytes long, as the format's
example says. A direct symbol for comparison: if the top bits are `000...`, `t->ml.decode[0]` is
`0x00000403`, `n = 3`, `x = 0`, base 4: the symbol 4 with the code `000`, value 4, and the second shift by
63 gives 0 for the extra bits. All numbers here were printed by a small program compiled against `src/`.

The comment on lines 226 and 227 states the bits the function may take: at most `SEQLZ_MAX_BITS +
QUETSCHN_PAGE_BITS`, an 8-bit code and up to 12 extra bits with 4 KiB pages, 20, or 14 extra bits with 16
KiB pages, 22. The caller refills before each call, which leaves at least 56. The base fits the 16 bits of
its field: the largest is `2^14` with 16 KiB pages. The cast of the shifted value to `unsigned int`
drops nothing: it has at most 14 bits.

These three functions, `seqlz_br_refill()`, `seqlz_br_drop()` and `seqlz_br_value()`, plus a token
lookup that [chapter 14](#ch-decode) writes inline, are all the decoder does with the bitstream. The
literal streams of [chapter 13](#ch-literals-dec) use a different reader, without a `count`, because 8
of them have to fit into the registers at once.
