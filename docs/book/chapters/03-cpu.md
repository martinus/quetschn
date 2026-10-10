# What makes code slow on a CPU {#ch-cpu}

seqlz's decoder runs 2.1 times as many instructions as lz4's: 25 284 against 11 899 per page. It
takes 7609 cycles for them, against lz4's 4992, so it is 1.5 times as slow, not 2.1. In a real
swap-in on the PC it takes 2.0 µs against 1.4. The difference between instruction count and time
is what this chapter is about. Most choices in `src/` that look odd at first, a table of exactly
2048 entries, a bitstream read from the top bit, 8 streams of literals, a copy of 16 bytes for 3
literals, an `& 63U` on a shift that is never 64, follow from a few facts about how a CPU runs code.

Each section explains one of them and shows where seqlz depends on it, with the function and the
chapter where the code is explained line by line. Three facts decide most of the speed: waiting for
memory, wrong guesses at branches, and chains of steps that wait for each other. The small cores of
phones make all three worse. The rest of the chapter is about tools the code uses on the way:
registers, loads at any address, byte order, several numbers in one register, copies of more bytes
than needed, and the undefined behaviour of C that the code has to steer around.

## Instructions are not time {#ch-cpu--ipc}

A CPU core runs at a clock, e.g. 4.5 GHz, which is 4.5 billion **cycles** per second, 0.22 ns each.
A modern core does not run one instruction per cycle. It fetches several instructions ahead,
finds the ones whose inputs are ready, and runs several of them in the same cycle on separate units:
adders, shifters, load units. The number of instructions it completes per cycle, the **IPC**, says
how much of that parallel work a piece of code lets it do.

::: measured seqlz against lz4, the decoder alone
A loop over 2000 pages of the first desktop dump, the pages in the cache, on the Ryzen 9 7950X of
the development machine, per page, as of 5th October 2026
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-numbers-again-with-the-format-as-it-is-now-reads-03-µs-faster-than-on-29th-september)):

| codec | instructions | cycles | instructions per cycle |
| --- | ---: | ---: | ---: |
| `lz4` | 11 899 | 4992 | 2.4 |
| `seqlz-fast-lit` | 25 284 | 7609 | 3.3 |

This loop decodes every page several times, which flatters seqlz, see
[the branches](#ch-cpu--branches). In a swap-in in the kernel, measured in a VM on the same CPU,
`zcomp_decompress()` takes 2.0 µs for seqlz and 1.4 µs for lz4
([docs/seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#why-it-is-nearly-as-fast-as-lz4)).
:::

seqlz gets more instructions per cycle because more of its work is independent: 8 streams of
literals decoded side by side, a token's lookup that does not wait for the copies of the sequence
before. So the count of instructions is a poor guide to time. What follows are the three things that make a core wait.

## Memory comes in cache lines {#ch-cpu--cache}

RAM is far from the core. A load whose data is in no cache takes about 100 ns, 450 cycles at 4.5
GHz. So the core has caches: small, fast copies of recently used memory. The smallest and fastest,
**L1**, holds a few dozen KiB per core and answers in about 1 ns. L2 and L3 are larger and slower.
The unit of a cache is the **cache line**, 64 bytes on every CPU this book is about: a load of one
byte brings the whole aligned 64 bytes around it into L1, and the next load in the same line is
cheap.

::: cpu Cold and warm in zram
A page in zram was swapped out because nobody used it for a while. When a program touches it again,
its compressed bytes are usually in no cache: the page is **cold**. The decoder's tables, 43 280 bytes
shared by all CPUs, may also have been pushed out of the caches by whatever ran since the last
swap-in. A benchmark that reads the same page twice, or decodes pages in a tight loop, has all of
that **warm**, and measures something else than a swap-in.
:::

seqlz's decoder needs its tables on every page, so their size is chosen for L1. These are the tables
of 4 KiB pages, from `struct seqlz_tables` in `src/seqlz_internal.h` ([chapter 7](#ch-internal)):

| table | entries | bytes | cache lines | read |
| --- | --- | ---: | ---: | --- |
| token decode table | 2048 of 4 bytes | 8192 | 128 | once per sequence |
| `LL` and `ML` decode tables | 256 of 4 bytes, each | 1024 each | 16 each | for length values |
| one literal decode table, of 8 | 1024 of 2 bytes | 2048 | 32 | once per coded literal |
| token codes for the encoder | 3073 of 2 bytes | 6146 | about 96 | once per sequence |
| the matcher's hash table, per CPU | 4096 of 2 bytes | 8192 | 128 | once per position |

The decoder of a page with coded literals reads from 12 KiB of tables, the encoder from the 6 KiB of
token codes next to the matcher's 8 KiB. Each table's size is a format decision. The token table has
2048 entries because token codes have at most 11 bits (`SEQLZ_TOKEN_BITS`), the length tables 256
because their codes have at most 8 (`SEQLZ_MAX_BITS`), the literal tables 1024 because literal codes
have at most 10 (`SEQLZ_LIT_BITS`). Longer codes would compress a little better and double a table.

::: measured Tables have to stay in L1
In an early version of the format, a token table for codes of up to 12 bits, 8 KiB of 2-byte entries
then, made the slowest 1% of cold reads 650 ns slower than a smaller token. With codes of up to 11
bits, a table of 4 KiB, the p99 of cold reads went from 5070 to 4350 ns. Measured in the userspace
harness on the Ryzen 9 7950X
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
The comment at `SEQLZ_MAX_BITS` in `src/seqlz.h` says the same for the length tables: one twice as
large made cold reads slower.
:::

## Prefetching: many misses at once {#ch-cpu--prefetch}

A cache miss costs about 100 ns, but a core can have several misses in flight at once. Ten misses one
after the other cost ten times 100 ns; ten misses that start together cost little more than one. An
out-of-order core finds such misses on its own when the addresses are known early. When they are
known only late, e.g. because each table index depends on the bits the lookup before consumed, the
code has to ask for the lines ahead of time. That is a **prefetch**: an instruction that asks the
CPU to bring a line into the cache and does not wait for it. On x86-64 it is `prefetcht0`, on arm64
`prfm`, and the kernel's name for it is `prefetch()`.

`seqlz_decode()` prefetches every line of its tables before it reads the first byte of the page:

@listing src/seqlz_decompress.c 7-26

`prefetch_lines()` takes the table as a `const void *` and copies it into `q`, a `const u8 *`,
because C defines pointer arithmetic only for pointers to complete types: `q + size` is `size` bytes
on. It walks the table in steps of `PREFETCH_STEP`, 512 bytes, and the inner loop
issues one `prefetch()` every `L1_CACHE_BYTES`, 64 bytes, so 8 per step. For the token table, 8192
bytes, that is 16 steps and 128 prefetches; for each length table 2 steps and 16 prefetches; for the
page's literal table, in `decode_literals()`, 4 steps and 32. The inner loop has a constant count of
8, which lets the compiler unroll it: gcc 16.2.1 at `-O3`, the level the kernel copy is built with,
writes the prefetches out one after the other, and the loop's own instructions, the add and the
compare, run far less often than once per line. With one line per iteration,
the comment says, the loop took more instructions than the prefetches. That was measured on the
phone: a plain loop made the big core's cold reads of pages with coded literals 0.8 µs slower
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
The three `static_assert()` lines after it check that each table's size is a multiple of 512 bytes,
which `prefetch_lines()` needs ([chapter 12](#ch-dec-basics)).

::: measured What the prefetches give
On the Mi 9T's little core, a Cortex-A55, in the phone's own kernel: without any table prefetch, cold
reads were 14.5 µs slower on average and 30 µs slower at p99, 77.28 against 62.76 µs and 120.45
against 90.26
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#4-prefetching-the-tables-the-a55-needs-it-the-a76-is-faster-without)).
On the PC the prefetches cover the tables so well that reading every line of them before the decode
made the decode in a swap-in only 6 to 25 ns faster
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-028-µs-slower-than-in-zrams-read-benchmark-warm-caches-give-back-012)).
The same idea for the compressed data, in zram before the decoder runs, took `lz4`'s p99 of cold
reads from 5600 to 3891 ns in the kernel VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression)).
:::

Two traps around prefetches are handled in `src/seqlz_compat.h`, see [chapter 5](#ch-compat). The
kernel builds x86-64 without SSE, and clang then drops `__builtin_prefetch()` without a word; the
project carries a kernel patch for it. And the Mi 9T's 4.14 kernel says that arm64 cache lines are
128 bytes, while its cores have 64: with 128, `prefetch_lines()` would skip every second line.

## Which cache lines a lookup touches {#ch-cpu--msb-first}

A decode table is indexed by the next 11 bits of the bitstream
([chapter 2](#ch-compression--table-decoding)). A code of `l` bits fills
`2^(11 - l)` entries: all indices that start with its bits, whatever bits come after it. Which
entries those are depends on the order the bits are read in, and that decides how many cache lines a
page touches.

seqlz reads its bitstream **most significant bit first** ([chapter 4](#ch-format--bitstream)). The
next code is then the top bits of the index, and the `2^(11 - l)` entries of a code are one range.
Token 49's code is `0001`: its entries are the indices `0001xxxxxxx`, 128 to 255. At 4 bytes per entry
and 16 entries per line, that is lines 8 to 15. Token 0's code `10001010` has the 8 entries 1104 to
1111, all in line 69.

Read **least significant bit first**, as seqlz did until 6th October 2026, the code is in the low
bits of the index, and its entries are every `2^l`-th entry of the table. Token 49's 128 entries are
then indices 8, 24, 40, and so on, one in every one of the 128 lines, and token 0's 8 entries are
in 8 lines, 5, 21, 37 and so on, 16 lines apart:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 212" role="img" aria-label="Two grids of the 128 cache lines of the token table. Read most significant bit first, the 4-bit code of token 49 fills lines 8 to 15 and token 0 one line, 69; read least significant bit first, token 49 has an entry in every line and token 0 in 8 lines" font-size="12">
  <text x="186" y="28" text-anchor="middle" font-weight="bold">most significant bit first</text>
  <rect x="50" y="40" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="67" y="40" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="84" y="40" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="101" y="40" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="118" y="40" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="135" y="40" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="152" y="40" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="169" y="40" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <g class="acc"><rect x="186" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="203" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="220" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="237" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="254" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="271" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="288" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="305" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <rect x="50" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="67" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="84" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="101" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="118" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="135" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="152" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="169" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="186" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="203" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="220" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="237" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="254" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="271" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="288" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="305" y="55" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="50" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="67" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="84" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="101" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="118" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="135" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="152" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="169" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="186" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="203" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="220" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="237" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="254" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="271" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="288" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="305" y="70" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="50" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="67" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="84" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="101" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="118" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="135" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="152" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="169" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="186" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="203" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="220" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="237" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="254" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="271" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="288" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="305" y="85" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="50" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="67" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="84" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="101" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="118" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <g class="mark"><rect x="135" y="100" width="17" height="15" class="fill-mark" fill-opacity="0.75" stroke="currentColor" stroke-width="0.5"/></g>
  <rect x="152" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="169" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="186" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="203" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="220" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="237" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="254" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="271" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="288" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="305" y="100" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="50" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="67" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="84" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="101" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="118" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="135" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="152" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="169" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="186" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="203" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="220" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="237" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="254" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="271" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="288" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="305" y="115" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="50" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="67" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="84" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="101" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="118" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="135" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="152" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="169" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="186" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="203" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="220" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="237" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="254" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="271" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="288" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="305" y="130" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="50" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="67" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="84" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="101" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="118" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="135" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="152" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="169" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="186" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="203" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="220" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="237" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="254" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="271" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="288" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="305" y="145" width="17" height="15" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <text x="44" y="51" text-anchor="end" class="mut" font-size="11">0</text>
  <text x="44" y="66" text-anchor="end" class="mut" font-size="11">16</text>
  <text x="44" y="81" text-anchor="end" class="mut" font-size="11">32</text>
  <text x="44" y="96" text-anchor="end" class="mut" font-size="11">48</text>
  <text x="44" y="111" text-anchor="end" class="mut" font-size="11">64</text>
  <text x="44" y="126" text-anchor="end" class="mut" font-size="11">80</text>
  <text x="44" y="141" text-anchor="end" class="mut" font-size="11">96</text>
  <text x="44" y="156" text-anchor="end" class="mut" font-size="11">112</text>
  <text x="546" y="28" text-anchor="middle" font-weight="bold">least significant bit first</text>
  <g class="acc"><rect x="410" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="427" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="444" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="461" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="478" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="495" y="40" width="17" height="15" class="fill-mark" fill-opacity="0.75" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="512" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="529" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="546" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="563" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="580" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="597" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="614" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="631" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="648" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="665" y="40" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="410" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="427" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="444" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="461" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="478" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="495" y="55" width="17" height="15" class="fill-mark" fill-opacity="0.75" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="512" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="529" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="546" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="563" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="580" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="597" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="614" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="631" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="648" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="665" y="55" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="410" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="427" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="444" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="461" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="478" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="495" y="70" width="17" height="15" class="fill-mark" fill-opacity="0.75" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="512" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="529" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="546" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="563" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="580" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="597" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="614" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="631" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="648" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="665" y="70" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="410" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="427" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="444" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="461" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="478" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="495" y="85" width="17" height="15" class="fill-mark" fill-opacity="0.75" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="512" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="529" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="546" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="563" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="580" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="597" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="614" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="631" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="648" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="665" y="85" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="410" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="427" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="444" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="461" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="478" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="495" y="100" width="17" height="15" class="fill-mark" fill-opacity="0.75" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="512" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="529" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="546" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="563" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="580" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="597" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="614" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="631" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="648" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="665" y="100" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="410" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="427" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="444" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="461" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="478" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="495" y="115" width="17" height="15" class="fill-mark" fill-opacity="0.75" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="512" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="529" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="546" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="563" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="580" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="597" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="614" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="631" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="648" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="665" y="115" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="410" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="427" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="444" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="461" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="478" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="495" y="130" width="17" height="15" class="fill-mark" fill-opacity="0.75" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="512" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="529" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="546" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="563" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="580" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="597" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="614" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="631" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="648" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="665" y="130" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="410" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="427" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="444" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="461" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="478" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="495" y="145" width="17" height="15" class="fill-mark" fill-opacity="0.75" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="512" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="529" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="546" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="563" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="580" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="597" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="614" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="631" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="648" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="665" y="145" width="17" height="15" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <text x="404" y="51" text-anchor="end" class="mut" font-size="11">0</text>
  <text x="404" y="66" text-anchor="end" class="mut" font-size="11">16</text>
  <text x="404" y="81" text-anchor="end" class="mut" font-size="11">32</text>
  <text x="404" y="96" text-anchor="end" class="mut" font-size="11">48</text>
  <text x="404" y="111" text-anchor="end" class="mut" font-size="11">64</text>
  <text x="404" y="126" text-anchor="end" class="mut" font-size="11">80</text>
  <text x="404" y="141" text-anchor="end" class="mut" font-size="11">96</text>
  <text x="404" y="156" text-anchor="end" class="mut" font-size="11">112</text>
  <g class="acc"><rect x="50" y="186" width="14" height="14" class="fill-acc" fill-opacity="0.3" stroke="currentColor"/></g>
  <text x="70" y="198" text-anchor="start">lines with an entry of token 49, code 0001</text>
  <g class="mark"><rect x="410" y="186" width="14" height="14" class="fill-mark" fill-opacity="0.75" stroke="currentColor"/></g>
  <text x="430" y="198" text-anchor="start">lines with an entry of token 0, code 10001010</text>
</svg>
</div>
<figcaption>The 128 cache lines of the token table, 16 per row. Most significant bit first, a code&#39;s entries are one range; least significant bit first, they are spread over the table.</figcaption>
</figure>

The lookup reads one entry, and which one depends on the bits after the code too. Most significant
bit first, the frequent short codes keep coming back to the same few lines. Least significant bit
first, they land on lines all over the table. Counted on 20 000 pages, a page touches 42.5 of the
token table's 128 lines most significant bit first, and 55.3 least significant bit first. The bytes
of the pages are the same either way: the change was the order, not the codes.

::: measured The bit order on the phone
On the Mi 9T's big core, a Cortex-A76, with the token table not prefetched on that core as it was
then: with the table's codes in one range, cold reads were 9.57 to 9.73 µs in every run; with the codes spread, one build of the same decoder read in 9.39 µs and
another in 10.50, depending on where the module's tables landed in memory. On the PC the change
cost nothing: 7370 against 7410 cycles per page in the decode loop
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-sequences-bitstream-most-significant-bit-first-the-token-tables-codes-in-one-range-on-every-cpu-kept)).
:::

The price of most significant bit first is a byte swap on every load of the bitstream, see
[byte order](#ch-cpu--endian). It costs one instruction per refill, not per code.

## Branches, and a predictor that learns the page {#ch-cpu--branches}

A **branch** is an instruction that decides where the program goes next: an `if`, the end test of a
loop, a `switch`. A core works many instructions ahead, so it has to know the outcome of a branch
long before the branch's condition is computed. It guesses, with a **branch predictor** that
remembers how each branch went the last times, and how the branches before it went. When the guess
is right, the branch is nearly free. When it is wrong, a **misprediction**, the core throws away the
work it did on the wrong path and starts again at the right one. That costs about 15 to 20 cycles.

::: measured What a misprediction costs, and that the predictor learns a page
In the kernel VM on the Ryzen 9 7950X at 4.5 GHz, `seqlz-fast-lit`'s decoder in a swap-in mispredicted
200.9 branches per page. When the same page was decoded once right before, it mispredicted 89.5, and
took 1867 cycles less, with the same instructions and the same L1 misses: 111 mispredictions fewer for
1867 cycles is about 17 cycles each
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-decoder-in-a-fault-found-110-branch-mispredictions-per-page-that-a-decode-of-the-same-page-before-hides)).
:::

The second number is the surprise: a branch predictor learns a page of 4 KiB after one decode.
In userspace it is the same: 130 mispredictions per page for a page decoded once, 11.6 for a page
decoded right after itself. Every benchmark that decodes a page more than once, a loop over pages in
the cache, the project's read benchmark in the kernel VM that reads each page six times in a row,
measures a decoder that knows the page. A swap-in decodes a
page once. Since then, the decode times the project trusts come from `MODE=swap` of its kernel VM,
which swaps each page in once per pass, with 20 000 other pages in between.

Three branches of the sequence loop made 58% of the mispredictions of a page seen once:
whether the bit buffer needs a refill, 32 per page; whether the sequence takes the fast path, 24;
and whether the match's offset is below 8, 20 to 22
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#which-branches-three-in-the-sequence-loop-are-58-of-the-130-misses-of-a-page-seen-once)).
The first one could be removed. Whether 23 bits are left in the buffer depends on the lengths of the
codes before, which no predictor knows for a new page. But a refill leaves at least 56 bits, and two
fast sequences take at most 50. So the fast path refills every second time, without asking:

@listing src/seqlz_decompress.c 500-514

The four lines inside the `if` are the refill of `seqlz_br_refill()` without its check that 8 bytes of
input are left: the fast path runs only where they are, see `FAST_IN_ROOM` below. `get_unaligned_be64()`
loads the next 8 bytes with the first byte on top, `v >> br->count` moves them below the `count` bits
that are still in the register, `br->p` moves on by the bytes that went in whole, and `count |= 56` is
the new count, explained in [registers](#ch-cpu--registers). The last line looks up the next token by
the top 11 bits. `*skip ^= 1U` flips between 0 and 1, so the `if` alternates: refill, no refill, refill. That is a
branch every predictor gets right. After a slow sequence, which may have taken more bits,
`seqlz_decode()` sets `skip` to 0, so the next fast one refills. In the kernel VM this took the
decoder in a swap-in from 1995 to 2008 ns to 1936 to 1942 ns, 63 ns less
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-refill-without-its-branch-every-second-fast-sequence-63-ns-less-per-swap-in-on-x86-64-and-the-a76-kept)).
The function is explained in [chapter 14](#ch-decode).

### Code without a branch

The other way to avoid a misprediction is code without a branch: compute the outcome with
arithmetic. `seqlz_off_class()` picks the offset class of a match for the encoder. The classes come
in no order a predictor could learn, so it has no `if`:

@listing src/seqlz.h 186-206

A comparison in C is an `int`, 0 or 1, and the code uses it as a number. For `off = 24` after a match
with `last = 16`:

- `is_new = (24 == 16) - 1U = 0 - 1U`, which wraps to `0xffffffff`: all bits set.
- `aligned = (24 >= 16) & ((24 & 7) == 0) = 1 & 1 = 1`.
- `cls = (1 + 1 + 0 + 2 * 1) & 0xffffffff = 4`.

For `off = 24` after `last = 24`, `is_new = 1 - 1U = 0`, and the `& 0` makes the class 0, the repeat
offset. For `off = 300`, `aligned` is 0 because `300 & 7 = 4`, and the class is `1 + 1 + 1 = 3`. The
offset bits of the class come from `SEQLZ_RAW_BITS()`, a shift and a mask of the constant `0x95C840`,
one hex digit per class: `(0x95C840 >> 16) & 15 = 5` bits for class 4. The comment above it in
`seqlz.h` says why it is a constant: computing class 3's bits from the page size made gcc branch on
the class. [Chapter 6](#ch-header) goes through both.

Code without branches is not always faster. It runs every instruction of both outcomes, and a branch
that the predictor gets right costs almost nothing. An early version of the decoder replaced its
branches with masks: the mispredictions fell from 155 to 68 per page, but the instructions rose by
15 000 per page, and the cycles from 9400 to 12 000. "At 3.4 to 3.9 instructions per cycle, instructions are
as expensive as mispredictions here"
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
The fast path's copies show the same in a swap-in: fixed copies of 32 or 40 bytes instead of copies
behind two branches had 14 fewer mispredictions per page and took more cycles
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#a-branch-or-more-work-again-in-a-swap-in-the-old-choices-hold-and-loops-over-2000-pages-were-never-trained)).
And a `?:` in C is not a promise of code without a branch: the compiler decides, and the same
measurement found two `?:` of the decoder compiled to branches that made 30% and 28% of all
mispredictions.

### One branch for two questions

The matcher in `src/page_lz.h` asks two questions at every position of the page: do the next 4 bytes
match at the repeat offset, and do they match at the candidate from the hash table? Both answers are
close to random. It reads both candidates first and decides with one branch on `rep_hit | cand_hit`.
The listing is in [in-order cores](#ch-cpu--in-order) below. With three
branches per position, the matcher mispredicted 539 times per page; with one branch, 329, and the p99 of
compressing a cold page went from 15.4 to 12.6 µs
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-seqlz-fast)).

## Chains of dependent steps {#ch-cpu--chains}

Two instructions can run in the same cycle only if neither needs the other's result. A **chain** is a
sequence of steps where each needs the result of the one before. A chain sets a lower bound on the
time, whatever the width of the core: if each step takes 5 cycles and there are 100 of them, it takes
500 cycles, even if the core could run 6 instructions per cycle.

Decoding Huffman codes is such a chain. To decode a literal, the decoder looks at the next 10 bits of
its stream, looks them up in the literal table, writes the byte, and shifts the stream by the code's
length:

@listing src/seqlz_decompress.c 291-298

`bits >> (64U - SEQLZ_LIT_BITS)` is the top 10 bits of the stream, the index into the page's literal
table `lt`. The entry `e` has the byte in bits 8 to 15 and the code's length in its low bits, so
`(u8)(e >> 8)` is the literal and `e & mask` how far the stream moves on. `mask` is 63 for a literal
that counts. In the last step of a page it is 0 for a stream that has no literal left: that stream
decodes a byte too, which lands behind the literals, but does not move on
([chapter 13](#ch-literals-dec)). The next literal's 10 bits are known only after the shift, and the
shift needs the code's length, which is in the entry the load returns. So in one stream every literal waits for the load and the
shift of the one before: an L1 load takes a few cycles, the shift one more. That is the chain of
the top row in the figure. The 8 streams of a page with coded literals are 8 such chains that do not
depend on each other ([chapter 4](#ch-format--layout)): the decoder steps all 8 in turn, and while one
stream's load is on its way, the next stream's load can start.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 296" role="img" aria-label="A schematic timeline: one stream decodes literal after literal, each lookup waiting for the shift before it; 8 streams run 8 such chains side by side, so 16 literals take the time of 2 steps" font-size="12">
  <text x="20" y="30" text-anchor="start" font-weight="bold">one stream</text>
  <g class="acc"><rect x="120" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="152.0" y="32" text-anchor="middle" font-size="11">load 0</text>
  <rect x="184" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="200" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="232.0" y="32" text-anchor="middle" font-size="11">load 1</text>
  <rect x="264" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="280" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="312.0" y="32" text-anchor="middle" font-size="11">load 2</text>
  <rect x="344" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="360" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="392.0" y="32" text-anchor="middle" font-size="11">load 3</text>
  <rect x="424" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="440" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="472.0" y="32" text-anchor="middle" font-size="11">load 4</text>
  <rect x="504" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="520" y="18" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="552.0" y="32" text-anchor="middle" font-size="11">load 5</text>
  <rect x="584" y="18" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="608" y="32" text-anchor="start">...</text>
  <text x="20" y="74" text-anchor="start" font-weight="bold">8 streams</text>
  <text x="110" y="74" text-anchor="end" class="mut" font-size="11">0</text>
  <g class="acc"><rect x="120" y="60" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="152.0" y="74" text-anchor="middle" font-size="11">load 0</text>
  <rect x="184" y="60" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="200" y="60" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="232.0" y="74" text-anchor="middle" font-size="11">load 8</text>
  <rect x="264" y="60" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="98" text-anchor="end" class="mut" font-size="11">1</text>
  <g class="acc"><rect x="126" y="84" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="158.0" y="98" text-anchor="middle" font-size="11">load 1</text>
  <rect x="190" y="84" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="206" y="84" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="238.0" y="98" text-anchor="middle" font-size="11">load 9</text>
  <rect x="270" y="84" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="122" text-anchor="end" class="mut" font-size="11">2</text>
  <g class="acc"><rect x="132" y="108" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="164.0" y="122" text-anchor="middle" font-size="11">load 2</text>
  <rect x="196" y="108" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="212" y="108" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="244.0" y="122" text-anchor="middle" font-size="11">load 10</text>
  <rect x="276" y="108" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="146" text-anchor="end" class="mut" font-size="11">3</text>
  <g class="acc"><rect x="138" y="132" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="170.0" y="146" text-anchor="middle" font-size="11">load 3</text>
  <rect x="202" y="132" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="218" y="132" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="250.0" y="146" text-anchor="middle" font-size="11">load 11</text>
  <rect x="282" y="132" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="170" text-anchor="end" class="mut" font-size="11">4</text>
  <g class="acc"><rect x="144" y="156" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="176.0" y="170" text-anchor="middle" font-size="11">load 4</text>
  <rect x="208" y="156" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="224" y="156" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="256.0" y="170" text-anchor="middle" font-size="11">load 12</text>
  <rect x="288" y="156" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="194" text-anchor="end" class="mut" font-size="11">5</text>
  <g class="acc"><rect x="150" y="180" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="182.0" y="194" text-anchor="middle" font-size="11">load 5</text>
  <rect x="214" y="180" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="230" y="180" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="262.0" y="194" text-anchor="middle" font-size="11">load 13</text>
  <rect x="294" y="180" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="218" text-anchor="end" class="mut" font-size="11">6</text>
  <g class="acc"><rect x="156" y="204" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="188.0" y="218" text-anchor="middle" font-size="11">load 6</text>
  <rect x="220" y="204" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="236" y="204" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="268.0" y="218" text-anchor="middle" font-size="11">load 14</text>
  <rect x="300" y="204" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <text x="110" y="242" text-anchor="end" class="mut" font-size="11">7</text>
  <g class="acc"><rect x="162" y="228" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="194.0" y="242" text-anchor="middle" font-size="11">load 7</text>
  <rect x="226" y="228" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <g class="acc"><rect x="242" y="228" width="64" height="20" class="fill-acc" stroke="currentColor" fill-opacity="0.3"/></g>
  <text x="274.0" y="242" text-anchor="middle" font-size="11">load 15</text>
  <rect x="306" y="228" width="16" height="20" class="fill-sheet" stroke="currentColor"/>
  <line x1="120" y1="266" x2="690" y2="266" stroke="currentColor" marker-end="url(#ch-cpu-arrow)"/>
  <text x="690" y="284" text-anchor="end" class="mut">time</text>
  <defs><marker id="ch-cpu-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0 0 L10 5 L0 10 z" fill="currentColor"/></marker></defs>
  <g class="acc"><rect x="400" y="62" width="14" height="14" class="fill-acc" fill-opacity="0.3" stroke="currentColor"/></g>
  <text x="420" y="74" text-anchor="start">table lookup of a literal</text>
  <rect x="400" y="86" width="14" height="14" class="fill-sheet" stroke="currentColor"/>
  <text x="420" y="98" text-anchor="start">shift the stream by its code length</text>
</svg>
</div>
<figcaption>Schematic, not to scale: one stream is one chain, lookup then shift then the next lookup. 8 streams are 8 chains that start one or two cycles apart.</figcaption>
</figure>

::: measured 4 streams and 8
With 4 streams, a literal took about 2.9 cycles to decode. With 8, decoding a page in the cache took
8676 instead of 9070 cycles, and in the kernel, cold, p50 and p99 went from 2940 and 4810 ns to 2860
and 4531 ns, for 8 more bytes of header with the stream sizes of 2 bytes of the time
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
Measured again in October with today's header, 4 streams made reads 0.7% to 1.8% slower in the kernel
VM and saved 1.1 to 2.8 bytes per page, so 8 stayed
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#five-simplifications-measured-in-the-vm-and-on-the-phone-the-literal-streams-as-arrays-kept-four-not)).
:::

The encoder has the same chain the other way round: appending a code to a stream is a shift and an
or on that stream's accumulator. `lit_streams()` in `src/seqlz_compress.c` writes 4 streams at a time,
each with its own accumulator ([chapter 11](#ch-literals-enc)); one stream after the other took about
3.5 cycles per literal.

The sequences have a chain too, and it is the reason for the format's main idea. Each sequence's
token is found by a table lookup, and the bits of the next token start where this sequence's bits
end, which is known only from the entry. So the chain from one sequence to the next is lookup, shift,
lookup. Everything that adds a step to that chain costs on every sequence. Putting the offset class
into the token, instead of a symbol of its own after it, took a second lookup off the chain: 8168
instead of 8758 cycles per page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).
The copies of the literals and the match hang off this chain but are not on it: the next lookup does
not wait for them.

## In-order cores wait at every load {#ch-cpu--in-order}

The cores of a PC and the big cores of a phone are **out of order**: they look at a window of the
next few hundred instructions and run whichever have their inputs ready, in any order. When a load
misses the cache, they keep working on everything that does not need it. The Mi 9T, the project's
test phone, has both kinds: big Cortex-A76 cores, out of order, and little Cortex-A55 cores, which are
**in order**. An A55 issues instructions strictly in program order, at most two per cycle. When an
instruction needs a value that is not there yet, the result of a load that has not arrived, it
stops, and every instruction behind it stops too, even those that could have run.

A swap-in can run on either kind of core, so the project measures on both. Three consequences of
the in-order core show in `src/`:

**Every instruction costs.** Out of order, extra instructions that are not on a chain often fit into
cycles that would be idle anyway. In order, they do not. On the A55 the decoder's loop ran at 0.99
instructions per cycle, so fewer instructions were faster almost one for one. That is why the token
table's entries are 4 bytes with every field ready to use ([chapter 7](#ch-internal)): taking the
token apart at run time took 18 of about 60 instructions per sequence. The entries of 4 bytes, with a
flag for "a length value follows", took the decoder from 16 547 to 15 129 instructions per page on
the A55, and together with three smaller changes from 17 842
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlzs-decoder-for-an-in-order-core-15-fewer-instructions-07-to-08-µs-less-at-cold-p99-on-the-a55-kept)).

**A load has to start early.** If a value is loaded right before it is used, an in-order core waits
for the whole latency of the load. If the load is issued a few instructions earlier, the instructions
in between fill the wait. Compilers do some of this, but only within what they can prove is safe and
worth it. The matcher does it by hand: it loads the next position's 8 bytes, hash, table entry and
candidate bytes while it compares the current one. This is called **software pipelining**:

@listing src/page_lz.h 124-149

`v_next` and `h_next` are the next position's bytes and hash, computed at the top of the loop, before
anything about the current position is decided. On a miss, the next position's table entry `cand` and
its bytes `cand_bytes` are loaded right away, so the next iteration starts with them on their way. The
same lines show the [one branch for two questions](#ch-cpu--branches): `rep_hit` and `cand_hit` are
computed as numbers, and only `!(rep_hit | cand_hit)` is a branch. On the A55 the loads ahead, with two smaller changes to the same loop, took
compression from 43 841 to 42 349 cycles per page, 3.4%, with the same output; on the PC from 16 489
to 16 148
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-on-an-in-order-core-34-fewer-compress-cycles-on-the-a55-same-output-kept)).
An earlier try of the same idea on the PC alone did nothing: an out-of-order core already loads the
next position ahead by itself. [Chapter 9](#ch-matcher) explains the loop.

The decoder does the same: in the fast path, `next_token_fast()` looks up the next token before the
copies of the current sequence, and the copies fill the time the load takes. On the phone that made
cold reads at the median 162 ns closer to `lz4` on the little core and 126 ns on the big one
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-next-token-before-the-copies-reads-faster-on-both-phone-cores-kept)).

**A miss it did not see coming stops it.** An out-of-order core finds the table lookups of the next
sequences in its window and starts their misses early. An in-order core reaches each one only when it
gets there. So the table prefetches above matter far more on the A55: 14.5 µs per cold read. On the PC,
prefetching the tables once made about 100 ns of difference, within the scatter of that measurement
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-third-decoder-round-one-repeat-offset-offset-class-in-the-token-and-why-cold-is-slow)).

::: why One decode loop for all cores
Until 10th October 2026, the arm64 build of the decoder asked which core it ran on and took a loop
written for in-order cores on a Cortex-A55. The kernel copy now has one loop for every core: nothing
else in `lib/` reads the CPU's model, and the list of in-order cores would need an entry for every new
little core. Measured on the Mi 9T, the out-of-order loop costs the A55 at most about 0.3 µs per warm
read, 3%, and nothing that shows on the A76
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-kernel-copys-choices-measured-in-one-boot-and-on-the-phone--o3-kept-one-decode-loop-prefetch-no-__aligned64-no-load16)).
:::

## Registers and the 64-bit bit buffer {#ch-cpu--registers}

A **register** is a variable inside the core. Arithmetic works on registers; memory is loaded into
them and stored from them. x86-64 has 16 general-purpose registers of 64 bits, arm64 has 31. When a
function needs more live values than there are registers, the compiler keeps some on the stack and
loads them back when needed, a **spill**. A spill is an extra load and store, and in a hot loop it can
put a load onto a chain.

seqlz reads its bitstream through one register. The decoder's `struct seqlz_bit_reader` has a `u64
bits` with the next bits on top and an `int count` of how many of them are real. A refill loads 8
bytes at once and ORs them in below the bits that are left, and the code then takes tokens, offsets
and length values off the top with shifts. One load of 8 bytes serves two or three sequences: a
refill leaves at least 56 bits, and a sequence on the fast path takes at most 11 bits of token and
14 of offset (12 on 4 KiB pages). [Chapter 12](#ch-dec-basics) shows the reader line by line; one detail shows how tight
it is. After a refill, `count |= 56` sets `count` to the number of bits now in the register: with 10
bits left, the refill adds 6 whole bytes, and `10 | 56 = 58 = 10 + 48`.

The 8 literal streams need 8 such buffers at once, and a pointer each. With a count per stream as
well, they did not fit into the registers of x86-64. So a stream has no count: its 64 bits carry a
1 below the last real bit, a marker, and the number of bits used is the position of that 1, found
with one count-trailing-zeros instruction when the stream is refilled ([chapter 13](#ch-literals-dec)).
Even so, on the A55 the 8 stream pointers did not fit and went to the stack; they are needed only
at a refill, once per 5 literals of a stream.

Registers also decided a choice in the matcher. Like lz4, it once moved on faster and faster when it
found no match for a while. That step needs one more value in the loop, and "the matcher's loop has no
register to spare". Without it, pages that compress are written 1% to 3% faster in the kernel VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept)).

## Loads at any address {#ch-cpu--unaligned}

A compressed page is a string of bytes, and the code reads 2, 4 and 8 of them at a time at any
position. The obvious C, `*(u64 *)p`, is wrong for two reasons. C requires a pointer to a `u64` to be
aligned for a `u64`, usually to 8 bytes, and `p` is not; the behaviour is undefined, and some
architectures fault on such a load. And reading bytes through a pointer of another type breaks C's
aliasing rules, which the compiler may use to reorder loads and stores. The kernel builds with
`-fno-strict-aliasing`, but the alignment rule stays.

The answer in `src/seqlz_compat.h` is `memcpy()` into a local variable; the kernel's own
`get_unaligned_le64()` gets the same single load another way. This is the version for builds outside
the kernel ([chapter 5](#ch-compat)):

@listing src/seqlz_compat.h 163-169

`SEQLZ_LE64()` is nothing on a little-endian CPU and a byte swap on a big-endian one, see
[byte order](#ch-cpu--endian). A `memcpy()` of a constant 8 bytes is not a call: the compiler knows what it does and writes one load.
x86-64 and arm64 both load from any address. gcc 16.2.1 at `-O2` turns `get_unaligned_le64()` into
`movq (%rdi), %rax`, and clang for arm64 into `ldr x0, [x0]`.

There is one catch in the kernel. With `CONFIG_FORTIFY_SOURCE`, `memcpy()` is a checked wrapper, and
the Mi 9T's 4.14 kernel and its clang did not inline it: a module had 63 calls to `memcpy()`, one per
copy of 8 or 16 bytes in the matcher, the encoder and the decoder. So the copies in `src/` call
`__builtin_memcpy()`, as lib/lz4's `LZ4_memcpy()` and lib/zstd's `ZSTD_memcpy()` do, which the
compiler always inlines (commit `48a7043`). `copy8()` and `copy16()` in [chapter 12](#ch-dec-basics)
are such copies.

## Byte order {#ch-cpu--endian}

A number of several bytes can be stored with its lowest byte first, **little endian**, or its highest
byte first, **big endian**. x86-64 and arm64 as Linux runs it are little endian; s390x, IBM's
mainframe, which Linux also runs on, is big endian. The format fixes the order where it matters: the
2-byte header and the stream sizes are little endian ([chapter 4](#ch-format--layout)), and the bits
of the bitstream are read from the top bit of each byte down. `src/seqlz_compat.h` gives each load
its order explicitly, `get_unaligned_le64()` or `get_unaligned_be64()`, and on a CPU of the other
order the load includes a byte swap. So a page has the same bytes on every CPU. Before commit
`91fa53e` some loads used the CPU's own order, and on s390x under qemu every page failed to decode
again, 4656 of 4656; CI now compresses 600 pages on x86-64, on 32-bit x86 and on s390x and compares
the bytes.

Both orders are used on purpose:

**Little endian for comparing bytes.** `seqlz_match_len()` in [chapter 9](#ch-matcher) finds where two
strings of bytes first differ. It loads 8 bytes of each as little-endian numbers and XORs them: the
result has 0 bits where the bytes agree. Byte 0 of the string is the lowest byte of the number, so the
first differing byte holds the lowest set bit. Counting the trailing zero bits, `__builtin_ctzll()`,
and dividing by 8 gives its position. For `abcdefgh` against `abcXefgh`, only byte 3 differs, `d` is
`0x64` and `X` is `0x58`, their XOR `0x3c` has its lowest set bit at bit 2, so the count is `24 + 2 =
26`, and `26 >> 3 = 3`.

**Big endian for the bitstream.** The decoder wants the next bits on top of a 64-bit register. If 8
bytes are loaded big endian, the first byte of the stream lands in the top 8 bits, and its bit 7 in
bit 63. For the page of `ab` in [chapter 4](#ch-format--example), the bitstream starts `dc 65 ef db
8a`; loaded big endian that is `0xdc65efdb8a......`, and its top 11 bits, `0xdc65efdb8a000000 >> 53`,
are 1763, binary `11011100011`, the index of token 1010's entry. On x86-64 the big-endian load is
`movq` and `bswap`, on arm64 `ldr` and `rev`: one instruction more per refill.

The encoder writes the literal streams the same way round. `lit_flush()` in [chapter 11](#ch-literals-enc)
swaps a stream's 64 bits once with `__builtin_bswap64()` and then stores them little endian, which
is a big-endian store; its comment says why not `put_unaligned_be64()`: in both branches that gave
`code_literals()` other code on arm64.

## Eight sums in one register {#ch-cpu--swar}

A 64-bit register can hold 8 numbers of 8 bits side by side, and one 64-bit add adds all 8 pairs at
once, as long as no lane overflows into the next. This is called **SWAR**, SIMD within a register.
The kernel may not use the vector registers of the CPU in most places, so SWAR is how seqlz does 8
things at once.

The encoder uses it to choose the literal table of a page ([chapter 11](#ch-literals-enc)). It has to
know how many bits the page's literals take in each of the 8 tables. `lit_cost` in `struct
seqlz_tables` holds, for each of the 256 byte values, one `u64` with the byte's code length in all 8
tables, table `k` in bits `8k` to `8k + 7`. For the byte `0x65`, the letter `e`, the code lengths in
tables 0 to 7 are 4, 9, 10, 5, 9, 9, 10 and 10 bits, so its `u64` is `0x0a0a0909050a0904`: table 0's 4
in the lowest byte. Adding the `u64` of every literal sums all 8 tables with one add per literal.
For the 4 literals `65 20 65 00`:

```text
  0x0a0a0909050a0904   65, the letter e
+ 0x0808090706070704   20, a space
+ 0x0a0a0909050a0904   65
+ 0x0803090202020208   00
= 0x241f241b121d1b14   table 0: 0x14 = 20 bits, table 3: 0x12 = 18 bits, ...
```

The 8 lanes are 20, 27, 29, 18, 27, 36, 31 and 36 bits: table 3 codes these 4 bytes in 18 bits, where
they take 32 raw. A lane holds at most 255. A literal code has at most 10 bits, so 25 literals are
safe, `255 / 10 = 25`, and that is `LIT_COST_RUN`. After 25 literals per stream, `lit_cost()` moves
the sums into wider lanes: `x & 0x00ff00ff00ff00ff` keeps tables 0, 2, 4 and 6 as four 16-bit lanes,
`(x >> 8) & 0x00ff00ff00ff00ff` tables 1, 3, 5 and 7. For the sum above that is `0x001f001b001d0014`,
the lanes 20, 29, 27 and 31, and `0x002400240012001b`, the lanes 27, 18, 36 and 36. In the kernel the
whole choice costs 30 to 40 ns per write, about 0.5% of a write of about 6900 ns; in userspace it was
at most 400 cycles, 1.5% of a write
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).

Three more places use a register as 8 bytes at once:

- `seqlz_match_len()` compares 8 bytes with one XOR, see [byte order](#ch-cpu--endian).
- `seqlz_hash5()` in the matcher shifts 8 loaded bytes left by 24, which drops the top 3 bytes, and
  multiplies by an odd constant, so that the top 12 bits of the product depend on all 5 bytes left.
- `repeat_pattern()` in the decoder builds 8 bytes of a short repeating match with one multiply. For
  offset 3 after `abc`, the 3 bytes as a number are `0x636261`, and times `0x0001000001000001`, which
  has a 1 in bytes 0, 3 and 6, that is `0x6261636261636261`: stored little endian, `abcabcab`.

## Copying more than needed {#ch-cpu--wild-copy}

Most sequences are short: up to 14 literals and a match of up to 34 bytes fit into the token alone,
and 79% of the matches are at most 16 bytes long. Copying exactly `ll` bytes needs a loop whose
number of rounds is the length, and the end of that loop is a branch that mispredicts often, because
the length changes from one sequence to the next. So the decoder copies a fixed 16 bytes, two loads
and two stores, whatever the length, and then moves on by the real length:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 198" role="img" aria-label="A copy of 16 literal bytes for a sequence with 3 literals: the decoder moves on by 3 and the next sequence overwrites the 13 extra bytes" font-size="12">
  <text x="20" y="56" text-anchor="start" font-weight="bold">before</text>
  <text x="20" y="136" text-anchor="start" font-weight="bold">after</text>
  <rect x="90" y="40" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <rect x="108" y="40" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <rect x="126" y="40" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <rect x="144" y="40" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <g class="acc"><rect x="162" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="180" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="198" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="216" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="234" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="252" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="270" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="288" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="306" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="324" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="342" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="360" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="378" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="396" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="414" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="acc"><rect x="432" y="40" width="18" height="22" class="fill-acc" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <rect x="450" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="468" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="486" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="504" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="522" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="540" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="558" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="576" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="594" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="612" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="630" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="648" y="40" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="90" y="120" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <rect x="108" y="120" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <rect x="126" y="120" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <rect x="144" y="120" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <rect x="162" y="120" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <rect x="180" y="120" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <rect x="198" y="120" width="18" height="22" class="fill-sheet" stroke="currentColor" stroke-width="0.5"/>
  <g class="mark"><rect x="216" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="234" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="252" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="270" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="288" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="306" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="324" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="342" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="360" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="378" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="396" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="414" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <g class="mark"><rect x="432" y="120" width="18" height="22" class="fill-mark" fill-opacity="0.3" stroke="currentColor" stroke-width="0.5"/></g>
  <rect x="450" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="468" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="486" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="504" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="522" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="540" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="558" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="576" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="594" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="612" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="630" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <rect x="648" y="120" width="18" height="22" class="fill-paper" stroke="currentColor" stroke-width="0.5"/>
  <text x="306" y="32" text-anchor="middle" font-size="11">copy16(): 16 bytes from d</text>
  <path d="M162 62 V70" stroke="currentColor"/>
  <text x="162" y="84" text-anchor="middle" font-weight="bold">d</text>
  <text x="333.0" y="112" text-anchor="middle" font-size="11">the 13 bytes past ll</text>
  <path d="M216 142 V150" stroke="currentColor"/>
  <text x="216" y="164" text-anchor="middle" font-weight="bold">d + 3</text>
  <text x="90" y="188" text-anchor="start">The next sequence writes from d + 3 on, over the 13 bytes.</text>
</svg>
</div>
<figcaption>The fixed copy for <code>ll = 3</code>: 16 bytes written, 3 of them kept. Grey are bytes of the page written before.</figcaption>
</figure>

The bytes past the real length are wrong, but the next sequence writes over them. This is sometimes
called a **wild copy**. It needs room: the copy may write past the last byte the sequence needs, and
read past the last literal. So the fast path of `seqlz_decode()` is taken only where there is enough,
which it checks with one compare per buffer against limits computed once per page
([chapter 14](#ch-decode)):

- in the page, 64 bytes, `FAST_PAGE_ROOM`: a fast sequence writes at most 54 bytes past where it
  starts, up to 14 literals and then up to 40 bytes of match;
- in the literals, 16 bytes, `FAST_LIT_ROOM`, for one copy of 16;
- in the input, 8 bytes, `FAST_IN_ROOM`, for a refill without a check.

Near the end of the page the slow path copies 16 or 8 bytes at a time while they fit, and the last
bytes one by one. The buffer coded literals are decoded into, `SEQLZ_SCRATCH`, is 4112 bytes, a page
and 16 bytes, so that the copy of 16 literal bytes never reads past it. The encoder does the same on
its side, with `ENC_ROOM`, 32 bytes, kept free between the literals and the bitstream it writes
([chapter 10](#ch-encoder)).

::: measured 16 bytes, not 32
Copying 32 bytes of every match made the mean faster and the p99 slower, 3570 to 4150 ns: the slowest
pages have many short matches, and each paid for 32 bytes
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#second-round-branches-tails-and-table-sizes)).
On the phone, fixed copies of 32 or 40 bytes made warm reads 0.1 µs slower on the big core and 0.5 to
0.8 µs on the little one
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#6-the-fast-paths-copies-as-they-are)).
The other way, copying byte by byte up to the end of the page, made the slowest pages 10 times
slower than lz4: with offset 1 every byte waited for the store before it
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#numbers-that-were-only-in-the-codecs-comments-until-10th-october-2026)).
:::

When the offset is below 16, the bytes a copy of 16 reads overlap the bytes it writes, and the copy
has to be careful:
an 8-byte load must not read bytes that the store before it has not written yet. `copy_match()` and
`copy_match_fast()` handle offsets below 8 with the pattern of `repeat_pattern()` above, and offsets
of 8 and more with copies of 8 bytes, each of which reads only bytes written at least 8 bytes
earlier. [Chapter 12](#ch-dec-basics) goes through both.

## Undefined behaviour the code steers around {#ch-cpu--ub}

C leaves some operations **undefined**: the standard says nothing about their result, and a compiler
may assume they never happen. Bit-level code meets three of them often. seqlz avoids each, in ways
that look odd until you know which one.

**A shift by the width of the type or more.** `x << n` and `x >> n` are undefined for a 64-bit `x`
when `n` is 64 or more. The hardware does not agree on a result either. x86 uses only the low 5 bits of
the count for a 32-bit shift and the low 6 bits for a 64-bit one, so a shift by the width shifts by
0 and returns `x`. 32-bit arm uses the low byte of the count, and a 32-bit value shifted by 32 is 0. Bit readers meet this whenever a field can have 0 bits: "the top `x` bits" is
`v >> (64 - x)`, a shift by 64 for `x = 0`. `seqlz_br_value()` reads the `x` extra bits of a length value,
and `x` is 0 for a value below 16, so it shifts twice:

@listing src/seqlz_decompress.c 229-243

The first line looks up the top 8 bits, `SEQLZ_MAX_BITS`, in the table of `ll` or `ml` values. Its
entry holds the code's length `n` in bits 0 to 3, the number of extra bits `x` in bits 4 to 7, and
the base value from bit 8 on, which `(e >> 8) & 0xffffU` takes out. In the sum,
`r->bits << n` drops the code of `n` bits, the `>> 1` clears the top bit, and the last shift, by `63U
- x`, is at most 63. `seqlz_br_drop()` then takes the code and the extra bits off the register, and the base plus the extra
bits is the value. [Chapter 12](#ch-dec-basics) explains the rest of the function. For the page of
`ab`, the `ML` code `111101` has `n = 6` and `x = 11`, and the expression is the 11 bits after the
code, 2011. For a value below 16, `x = 0`, the shift by 63 of a number whose top bit is clear gives
0, which is right. Written the obvious way, the same `x = 0` would shift by 64, and on x86-64 return
the whole register instead of 0.

The token table's entries use a mask instead. An entry holds the shift that cuts the offset out of
the bits, `64 - raw_bits`, but `& 63`: for class 0, without offset bits, that is 0 instead of 64, the
shift is harmless, and the decoder then takes the repeat offset because the field is 0
([chapter 7](#ch-internal)). The encoder's `enc_flush()` shifts by `(64U - e->cnt) & 63U` for the same
reason.

**Counting the zeros of 0.** `__builtin_ctzll(x)`, the number of trailing zero bits, is undefined for
`x = 0`, and here the hardware differs again. gcc writes `rep bsf`, which runs as `tzcnt` on CPUs
that have it and returns 64, and as the older `bsf` on CPUs that do not, which Intel documents as
undefined for 0. arm64 has no such instruction; clang writes a bit reversal and a count of leading
zeros, which gives 64. `seqlz_match_len()` calls it only where the XOR is not 0, and makes that plain
with `x | 1ULL << 63`, one instruction, `bts` or `orr`: a number with bit 63 set is never 0, and for a
number that is not 0, the OR does not change the count. The literal streams never pass 0 either: their marker bit, the 1 below
the last real bit ([registers](#ch-cpu--registers)), is never shifted out.

**A pointer outside its buffer.** In C, computing a pointer before the start of an array, or more
than one past its end, is undefined even if it is never used to load. `seqlz_decode()` needs the
limit "16 bytes before the end of the literals" for its fast path, and for a page with fewer than 16
bytes of input that would be a pointer before the input. So `lit_fast` and `in_fast` are `unsigned
long` numbers, not pointers, and the fast path compares addresses as numbers.

What the code relies on instead is what C defines: unsigned arithmetic wraps around. `off - 1U >= (unsigned int)(d - (u8 *)dst)` checks in one compare that an offset is at least 1 and at most the bytes
written: for `off = 0`, `off - 1U` is `0xffffffff`, larger than any page. The same wrap makes
`is_new` above all ones.

## What the kernel's sanitizers add {#ch-cpu--ubsan}

The kernel can check for some undefined behaviour at run time. `CONFIG_UBSAN_SHIFT` checks every
shift whose count the compiler cannot prove to be in range, and `CONFIG_UBSAN_BOUNDS` every array
index it cannot prove to be inside the array. Fedora's x86-64 kernel enables both, and the project
builds its benchmark copies of every codec with the same flags, `-fsanitize=shift` and
`-fsanitize=bounds-strict`. A check is a compare and a branch, on every shift and index in a hot loop.

These two functions differ only in the mask. Compiled with gcc 16.2.1, `-O2 -fsanitize=shift
-fsanitize-undefined-trap-on-error`, the last flag only to make a failed check a trap instead of a
call, so that the code stays short:

```c
u64 put(u64 acc, unsigned int n)        { return acc << n; }
u64 put_masked(u64 acc, unsigned int n) { return acc << (n & 63U); }
```

```text
put:                            put_masked:
    cmpl    $63, %esi               movq    %rdi, %rax
    ja      .L3                     movl    %esi, %ecx
    movq    %rdi, %rax              salq    %cl, %rax
    movl    %esi, %ecx              ret
    salq    %cl, %rax
    ret
```

With the mask the compiler knows the count is below 64, and the check is gone. The mask itself costs
nothing here: x86-64's shift already uses only the low 6 bits of the count, so the compiler drops the
`and`. That is why the encoder's shifts are written `<< (n & 63U)` even where `n` is never 64, and why
the encoder's array of length-value codes, `enc[]` in `struct value_table`, has 32 entries for 25 or
27 symbols: a power of 2, so that `s & (ENC_LEN_SYMBOLS - 1U)` proves every index in range
([chapter 7](#ch-internal), [chapter 10](#ch-encoder)). Masked shifts and indices took the compressor
from 25 225 to 25 064 cycles per page, 4.7% fewer instructions
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).

The decoder had 65 such checks in an early version, lz4's had 18. They were not in its hot path:
without them, the instructions per page were the same
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-lz4s-matches-huffman-coded-sequences-with-static-tables)).
The masks matter most in the encoder, whose shifts by a code's length are on every sequence and
every literal.
