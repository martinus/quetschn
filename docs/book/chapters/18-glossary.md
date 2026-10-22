# Glossary {#glossary}

The terms of the book a C or C++ programmer may not know, in five groups, each in alphabetical order,
with the chapter that explains them. Many compression terms sound alike; the book uses each in the one
sense given here.

## Compression and the format {#glossary--format}

- **ANS, FSE**: a way to code symbols that can spend a fraction of a bit on one; FSE is `zstd`'s form of
  it. [Chapter 17](#ch-decisions--bet)
- **bitstream**: codes and plain bits one after the other, not aligned to bytes; seqlz reads the
  sequences' bitstream most significant bit first. [Chapter 4](#ch-format)
- **canonical code**: a prefix code that follows from the code lengths alone by a fixed rule, so a table
  stores only lengths. [Chapter 2](#ch-compression--canonical-codes)
- **code, code length**: the bits written for one symbol, {{ch17.tok_shortest}} to {{ch17.tok_longest}}
  for tokens, and how many there are. A table is a list of code lengths. [Chapter 2](#ch-compression)
- **coded literals**: literals written as Huffman codes of one literal table in 8 streams, from level 2
  on where that saves $\frac{1}{16}$ of them plus {{ch15.coded_min}} bytes.
  [Chapter 11](#ch-literals-enc)
- **complete**: a prefix code whose codes use up every bit pattern, Kraft sum 1, so a decode table has no
  empty entry. [Chapter 2](#ch-compression--complete)
- **decoder, encoder, matcher**: the matcher finds the matches of a page (`src/page_lz.h`), the encoder
  writes them in the format, the decoder turns a compressed page back into the page.
  [Chapters 9 to 14](#ch-matcher)
- **delta, `dt`**: at level 3, a literal stored as the literal minus an earlier byte of the page,
  modulo 256, before it is coded, one of three ways per page, which the byte `dt` after the stream
  sizes names: the byte 4 or 8 before, or the byte at the offset of the sequence before; bit 7 of
  byte 2 says the page has deltas. The decoder adds the byte back. [Chapter 4](#ch-format--deltas)
- **dictionary**: data compressor and decoder share before the first byte; zram can pass one, seqlz's
  kernel backend refuses it. [Chapter 15](#ch-zram-glue--head)
- **entropy**: the fewest bits per symbol any code can reach for given frequencies,
  $\sum p \log_2 \frac{1}{p}$. [Chapter 2](#ch-compression--entropy)
- **escape**: the one code of all tokens without a code of their own, followed by the token in
  {{ch17.escape_bits}} bits; {{ch17.tok_own}} of {{const.TOKEN_SYMBOLS}} tokens have one.
  [Chapter 6](#ch-header--escape)
- **fast path**: the decoder's path for a sequence without a length value and with room at every end:
  fixed copies, no loop, no checks. [Chapter 14](#ch-decode--fast)
- **greedy, lazy matching**: a greedy matcher takes the first match it finds; lazy matching checks whether
  the next position starts a longer one, which seqlz does not. [Chapter 9](#ch-matcher)
- **hash table**: the matcher's {{ch17.hash_slots}} slots of 2 bytes, cleared for each page, the newest
  position per hash of 5 bytes. [Chapter 9](#ch-matcher--hash)
- **header**: the u16 with the literal count and a flag, and with coded literals byte 2, with deltas the
  byte that says which, and the stream sizes: $3 + w$ bytes, and with deltas one byte more. [Chapter 6](#ch-header--literals)
- **Huffman code**: the prefix code with the fewest bits for given frequencies; seqlz's are limited to
  {{ch18.bits.token}}, {{ch18.bits.lit}} and {{ch18.bits.len}} bits. [Chapter 2](#ch-compression--huffman)
- **k-means**: the trainer's clustering for the literal tables: each page goes to the table that codes it
  in the fewest bits, each table is rebuilt from its pages, until nothing moves.
  [Chapter 8](#ch-codes--lit-sets-file)
- **Kraft sum**: the sum of $2^{-l}$ over a table's code lengths, at most 1 for a prefix code, exactly 1
  for a complete one. [Chapter 2](#ch-compression--complete)
- **length value**: the rest of `ll` or `ml` when it is too large for the token, a symbol of the `ll` or
  `ml` table and plain bits. [Chapter 4](#ch-format--length-values)
- **literal, match, offset**: a literal is a byte stored as it is; a match copies `ml` bytes from `offset`
  bytes back. [Chapter 2](#ch-compression--lz77)
- **lazy step**: a look at the next position before a match is taken, for a longer one there; level 3
  does it for matches below 20 bytes. [Chapter 9](#ch-matcher--lazy)
- **literal table**: one of the 8 fixed tables for coded literals, or a page's own table; a page uses
  one. Not related to the 8 streams. [Chapter 8](#ch-codes--lit-sets-file)
- **`ll`, `ml`**: literal length and match length of a sequence, `ml` 4 or more.
  [Chapter 4](#ch-format)
- **LZ77**: the family of compressors that write data as literals and matches; `lz4`, `lzo`, `zstd` and
  seqlz are. [Chapter 2](#ch-compression--lz77)
- **`mlf`**: the token's match length field, `ml - 4`; its largest value says a length value follows.
  [Chapter 4](#ch-format--token)
- **MSB first**: most significant bit first, so the next token is the top {{ch18.bits.token}} bits of a
  64-bit register. [Chapter 2](#ch-compression--msb)
- **own table, `OWN`**: a literal table of the page's own, at level 3: code lengths for the byte values
  the page has, sent in front of the streams as symbols of the fixed code `OWN`, in byte order, with
  runs for the byte values without a code. [Chapter 4](#ch-format--own)
- **offset bits, offset class**: the class, in the token, says how the offset is stored: the repeat
  offset in 0 bits, offset 2 in {{ch18.class_bits.1}}, below 256 and below the page size in
  {{ch18.class_bits.2}} and {{ch18.class_bits.3}}, multiples of 8 below 256 and 2048, divided by 8, in
  {{ch18.class_bits.4}} and {{ch18.class_bits.5}}. The offset bits follow the token.
  [Chapter 6](#ch-header--offsets)
- **optimal parser**: a matcher that searches for the sequences that make a page smallest; seqlz's was
  `seqlz-opt`, removed. [Chapter 17](#ch-decisions--not-kept)
- **prefix code**: codes of which none is the start of another, so a decoder knows where each ends.
  [Chapter 2](#ch-compression--prefix-codes)
- **record**: at level 3, the {{delta.rec}} bytes the encoder writes in front of a sequence's literals, its position,
  `ll` and the offset before, for `delta_literals()`. [Chapter 10](#ch-encoder--records)
- **repeat offset**: the offset of the match before, used again for 0 bits; 1 at the start of a page.
  [Chapter 4](#ch-format--offsets)
- **scratch**: the per-CPU buffer the decoder decodes coded literals into, a page plus 16 bytes.
  [Chapter 13](#ch-literals-dec)
- **sentinel**: the 1 bit a literal stream keeps below its valid bits, whose position counts the bits
  used. [Chapter 13](#ch-literals-dec--sentinel)
- **sequence**: `ll` literals and one match; the last sequence of a page has literals only and no token.
  [Chapter 4](#ch-format)
- **stream**: one of the 8 parts of the coded literals, literal $k$ in stream $k \bmod 8$.
  [Chapter 11](#ch-literals-enc--streams)
- **symbol**: what a code stands for: a token, a byte value, a length value's symbol.
  [Chapter 2](#ch-compression)
- **table**: the code lengths of one kind of symbol; seqlz has {{ch17.tables}}, fixed and part of the
  format, each with a decode table built once. [Chapter 7](#ch-internal), [chapter 8](#ch-codes)
- **token**: one number for `ll`, `ml − 4` and the offset class, {{const.TOKEN_SYMBOLS}} values, coded by
  the token table. [Chapter 4](#ch-format--token)
- **trained tables, trainer**: code lengths counted on {{ch17.train_pages|grp}} pages, in
  `bench/seqlz_counts_*.txt`; `quetschn-seqlz-train` counts and builds them.
  [Chapter 8](#ch-codes--lengths)
- **wild copy**: a copy of a fixed 16 bytes or more where fewer are needed, the rest written over by the
  next sequence. [Chapter 3](#ch-cpu--wild-copy)
- **work memory**: the codec's buffers per CPU, the hash table to compress and the scratch to decompress;
  not the tables, which all CPUs share. [Chapter 1](#ch-zram--work-memory)

## The kernel {#glossary--kernel}

- **backend**: a codec as zram calls it, through `struct zcomp_ops`. [Chapter 1](#ch-zram--zcomp)
- **checkpatch**: `scripts/checkpatch.pl`, the kernel's style checker for patches.
  [Chapter 15](#ch-zram-glue--check)
- **`EXPORT_SYMBOL_GPL`**: makes a function callable from modules with a GPL-compatible licence.
  [Chapter 15](#ch-zram-glue--interface)
- **`FORTIFY_SOURCE`**: a kernel option that checks the sizes of `memcpy()` and its relatives, and can
  make each a call; seqlz's short copies use `__builtin_memcpy()`. [Chapter 6](#ch-header--summary)
- **fuzz target**: a function's entry point for a fuzzer, libFuzzer or AFL++, which feeds it millions of
  mutated inputs. [Chapter 15](#ch-zram-glue--fuzzing)
- **`__init`, initcall**: `__init` code runs while the kernel starts and is freed after; an initcall,
  e.g. `subsys_initcall()`, is such a function the kernel calls at boot. [Chapter 8](#ch-codes--kernel),
  [chapter 15](#ch-zram-glue--init)
- **KASAN**: the kernel's address sanitizer. [Chapter 15](#ch-zram-glue--check)
- **kernel copy**: the code `tools/kernel-port/port.py` writes into a kernel tree from `src/`, generated,
  never edited. [Chapter 15](#ch-zram-glue--port)
- **kernel-doc**: the kernel's comment format for functions, which `scripts/kernel-doc` checks.
  [Chapter 15](#ch-zram-glue--interface)
- **`kmalloc()`, `vmalloc()`, `kvzalloc()`**: `kmalloc()` hands out objects of powers of 2 up to 2 pages,
  {{const.SCRATCH}} bytes as 8 KiB; `vmalloc()` whole pages, contiguous in virtual addresses only;
  `kvzalloc()` tries the first, falls back to the second, and zeroes. [Chapter 1](#ch-zram--work-memory)
@assert ch15.mem.dctx_gets == 8192
- **KUnit, UML**: the kernel's unit test framework, run in User-Mode Linux, the kernel as an ordinary
  program. [Chapter 15](#ch-zram-glue--check)
- **module**: kernel code built as a file of its own and loaded into a running kernel.
  [Chapter 15](#ch-zram-glue--init)
- **page, page fault**: the unit of memory the kernel maps and swaps, 4096 bytes on x86-64 and on phones
  today, and Android is moving to 16 KiB; a page fault is a touch of a page that is not mapped, e.g.
  swapped out. [Chapter 1](#ch-zram--swap)
- **per-CPU**: data the kernel keeps once per CPU; zram's streams, with the codec's work memory, are.
  [Chapter 1](#ch-zram--work-memory)
- **`__ro_after_init`**: data written while the kernel starts and read-only after, as the kernel copy's
  tables. [Chapter 8](#ch-codes--kernel)
- **same-filled page**: a page of one repeated 8-byte value, which zram stores without the codec.
  [Chapter 1](#ch-zram)
- **size class**: one of zsmalloc's fixed slot sizes; bytes saved within a class save nothing, and from
  3625 bytes on 4 KiB pages zram stores a page uncompressed. [Chapter 1](#ch-zram--size-classes)
- **swap, swap-in, swap-out**: moving pages not used for a while out of RAM, and back when touched.
  [Chapter 1](#ch-zram--swap)
- **UBSan**: the undefined behaviour sanitizer; the kernel's `CONFIG_UBSAN_BOUNDS` and
  `CONFIG_UBSAN_SHIFT` add a check to each index and shift the compiler cannot prove in range.
  [Chapter 3](#ch-cpu--ubsan)
- **`W=1`**: a kernel build with extra warnings. [Chapter 15](#ch-zram-glue--check)
- **zram, zsmalloc, zswap**: zram is a block device in RAM that stores pages compressed, mostly as swap,
  in zsmalloc's size classes; zswap is a compressed cache in front of a swap device, which gives a codec
  one page of output where zram gives two. [Chapter 1](#ch-zram)

## The CPU {#glossary--cpu}

- **branch misprediction**: a wrong guess of a branch's outcome, which throws away the work done ahead,
  about 15 to 20 cycles. [Chapter 3](#ch-cpu--branches)
- **cache line, L1**: caches hold data in lines of 64 bytes; L1, a few dozen KiB, answers in about 1 ns,
  RAM in about 100 ns. [Chapter 3](#ch-cpu--cache)
- **dependency chain**: steps that each need the result of the one before, so the CPU cannot overlap
  them. [Chapter 3](#ch-cpu--chains)
- **in-order, out-of-order core**: an in-order core, e.g. the Cortex-A55, runs instructions in program
  order and stops at a load that misses; an out-of-order core, e.g. the Cortex-A76 or a Ryzen 9 7950X,
  runs later independent ones meanwhile. [Chapter 3](#ch-cpu--in-order)
- **IPC**: instructions per cycle. [Chapter 3](#ch-cpu--ipc)
- **prefetch**: asking the CPU to load a line into the cache before it is needed. [Chapter 3](#ch-cpu--prefetch)
- **software pipelining**: starting the loads of a loop's next round while the current one is worked on,
  so that an in-order core does not wait. [Chapter 3](#ch-cpu--in-order)
- **store forwarding**: a load takes bytes a recent store wrote from the store buffer, if all come from
  one store; else it waits. [Chapter 12](#ch-dec-basics--copies)
- **SWAR**: SIMD within a register, several small values in one 64-bit integer handled by one instruction.
  [Chapter 3](#ch-cpu--swar)

## Codecs, code names and machines {#glossary--names}

- **`bytelz`**: an experiment with seqlz's matcher and a byte format, not kept.
  [Chapter 17](#ch-decisions--not-kept)
- **dump**: a copy of all pages in zram at one moment, from one desktop, the Mi 9T or the Android 17
  emulator; their contents are never published, since they hold keys and passwords.
  [Chapter 17](#ch-decisions--unknown)
- **kernel VM**: `tools/zram-vm/run.sh`, a kernel with the codecs in a virtual machine on one core of a
  Ryzen 9 7950X fixed at 4.5 GHz; its times decide. [Chapter 17](#ch-decisions)
- **level**: zram's knob per device for how hard a codec works; seqlz has 1 to 3, `seqlz-1` to
  `seqlz-3`, 2 the default, all in one format. [Chapter 6](#ch-header--compressor)
- **`lz4`, `lz4hc`, `lzo`, `lzo-rle`, `zstd`**: zram's codecs: `lz4` the fastest, `lz4hc` its slow
  compressor for the same format, `lzo-rle` zram's default, `zstd` the smallest, level 3 its default in
  zram. [Chapter 1](#ch-zram--codecs)
- **Mi 9T**: the Xiaomi Mi 9T test phone, a Snapdragon 730 with Cortex-A76 and Cortex-A55 cores,
  Android 11 and Linux 4.14. [Chapter 3](#ch-cpu)
- **quetschn, seqlz**: quetschn is the project, its binaries and options; seqlz the codec, one format
  with three levels of the compressor and one decoder. [Preface](#preface)
- **`seqlz-1`, `seqlz-2`, `seqlz-3`**: seqlz at levels 1 to 3: raw literals; coded literals where they
  pay; and also a lazy step, a table of the page's own and literals as deltas. [Chapter 10](#ch-encoder),
  [chapter 11](#ch-literals-enc)
- **`seqlz-opt`**: an optimal parser for the same format, for zram's recompression, removed.
  [Chapter 17](#ch-decisions--not-kept)

## Measurements {#glossary--measurements}

- **C1 to C6**: the criteria of `docs/plan.md`: C1 memory against `lzo-rle` and `lz4`, C2 and C3 p99
  reads and writes against `lz4`, C4 against `lz4` with a dictionary, C5 work memory per CPU, C6 a decoder
  safe and bounded for any input. C6 is the only hard limit, C5 a cost in the score, C2 and C3 guards, C1
  and C4 reported. [Chapter 1](#ch-zram--goal)
- **cold, warm**: a cold read finds the compressed data in no CPU cache, a warm one finds it there.
  [Chapter 3](#ch-cpu)
- **exchange rate, lambda, hull**: lambda is what one µs per page written is worth in bytes; the hull, the
  lower left convex hull of (time, bytes), has the codec with the lowest score for each lambda.
  [Chapter 1](#ch-zram--score)
- **hot loop**: a codec in a loop over pages in the cache, in userspace, counted in cycles: good for
  comparing code, too kind for latency. [Chapter 3](#ch-cpu--ipc)
- **point**: 1% of a page, 41 bytes on 4 KiB pages, not 1% of the stored size.
  [Chapter 4](#ch-format--offsets)
- **score**: bytes per page plus lambda times the time per page written; lower is better.
  [Chapter 1](#ch-zram--score)
- **stored size, Σ zsmalloc cost**: what zsmalloc spends on compressed pages, rounded up to their size
  classes; summed over pages, often as a share of their size. [Chapter 1](#ch-zram--size-classes)
- **swap-in fault, `MODE=swap`**: the kernel VM's benchmark that swaps each page out and in once, so the
  branch predictor has not seen it. [Chapter 17](#ch-decisions--score)
- **time per page written**: the mean time to write a page plus 0.73 times the mean time to read one, the
  reads per write of a phone in daily use over 7.1 days; a desktop had 0.34. [Chapter 1](#ch-zram--score)
