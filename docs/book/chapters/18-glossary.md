# Glossary {#glossary}

The terms of the book, in five groups: compression and the format, the kernel, the CPU, the names of
codecs and machines, and measurements. Within a group they are in alphabetical order. Each entry links to
the chapter that explains the term in depth. Many of the compression terms sound alike and are not the
same thing; the book uses each in the one sense given here, throughout.

## Compression and the format {#glossary--format}

- **ANS, FSE**: asymmetric numeral systems, a way to code symbols that can spend a fraction of a bit on
  a frequent one; FSE is `zstd`'s form of it. seqlz uses Huffman codes instead, which lose 0.2% to 0.3%
  against it. [Chapter 17](#ch-decisions)
- **bitstream**: codes and plain bits written one after the other, not aligned to bytes. In seqlz the
  sequences' bitstream follows the literals and is read most significant bit first. [Chapter 4](#ch-format)
- **canonical code**: a prefix code that follows from the code lengths alone by a fixed rule, as in
  deflate: shorter codes first, within a length in symbol order. That is why a table stores only code
  lengths. [Chapter 2](#ch-compression), [chapter 8](#ch-codes)
- **code**: the bits written for one symbol, e.g. {{ch17.tok_shortest}} bits for the most frequent tokens and {{ch17.tok_longest}} for rare
  ones. [Chapter 2](#ch-compression)
- **code length**: how many bits a symbol's code has. A table is a list of code lengths. [Chapter 2](#ch-compression)
- **coded literals**: literals written as Huffman codes of one literal table instead of as bytes, in 8
  streams. Only `seqlz-fast-lit` writes them, and only where they save more than $\frac{1}{16}$ of the literals plus {{ch15.coded_min}}
  bytes. [Chapter 11](#ch-literals-enc), [chapter 13](#ch-literals-dec)
- **complete**: a prefix code whose codes use up every bit pattern (its Kraft sum is exactly 1), so every
  bit pattern starts some code and a decode table has no empty entry. All of seqlz's tables are
  complete. [Chapter 2](#ch-compression--complete), [chapter 8](#ch-codes)
- **compressed page**: what seqlz writes for one page: a header, the literals, then the bitstream.
  [Chapter 4](#ch-format)
- **decoder**: the code that turns a compressed page back into its 4096 bytes, `seqlz_decode()` and
  `decode_literals()`. [Chapters 12 to 14](#ch-dec-basics)
- **dictionary**: data a compressor and decoder share before the first byte, so that the first matches
  can point into it. zram can pass one to its codec; seqlz ignores it, its tables are fixed.
  [Chapter 17](#ch-decisions)
- **encoder**: the second half of the compressor: it writes the sequences the matcher finds and the
  literals in the format. In seqlz it runs in the same loop as the matcher. [Chapter 10](#ch-encoder)
- **entropy**: the least number of bits per symbol any code can reach for given symbol frequencies, the
  sum of $p \times \log_2 \frac{1}{p}$. A Huffman code is within one bit per symbol of it. [Chapter 2](#ch-compression)
- **entropy coder**: the part of a codec that writes symbols in codes of their frequency, Huffman codes
  or ANS, instead of in a fixed number of bits. [Chapter 2](#ch-compression--entropy)
- **escape**: the code shared by all tokens that have no code of their own, followed by the token in {{ch17.escape_bits}}
  plain bits. Only {{ch17.tok_own}} of the {{const.TOKEN_SYMBOLS}} tokens have their own code. [Chapter 4](#ch-format)
- **fast path**: the decoder's path for a sequence with no length value and room to the ends of the page,
  the literals and the input: fixed copies of 16 bytes and more, no loop. [Chapter 14](#ch-decode)
- **greedy**: a matcher that takes the first match it finds and does not check whether one position
  later starts a longer one. [Chapter 9](#ch-matcher)
- **hash**: a number computed from some bytes, here from 5 bytes to a slot from 0 to {{ch18.slot_max}}. Two different
  byte strings with the same hash are a **collision**. [Chapter 9](#ch-matcher)
- **hash table**: the matcher's table of positions, {{ch17.hash_slots}} slots of 2 bytes, 8 KiB, cleared for each page.
  It is a cache, not a map: one position per slot, the newest wins. [Chapter 9](#ch-matcher)
- **header**: the first bytes of a compressed page: the literal count and a flag, and with coded literals
  the literal table's number, the width $w$ and the 8 stream sizes, $3 + w$ bytes in all.
  [Chapter 6](#ch-header)
- **Huffman code**: the prefix code with the fewest bits for given symbol frequencies. seqlz's tables are
  Huffman codes limited to {{ch18.bits.token}} bits (tokens), {{ch18.bits.lit}} bits (literals) and {{ch18.bits.len}} bits (length values).
  [Chapter 2](#ch-compression)
- **Kraft sum**: the sum of $2^{-l}$ over the lengths $l$ of all codes of a table. It is at most 1 for a
  prefix code, and exactly 1 for a complete one. [Chapter 2](#ch-compression--complete)
- **k-means**: the clustering the trainer uses for the literal tables: each page goes to the table that
  codes it in the fewest bits, each table is rebuilt from its pages, repeated until nothing moves.
  [Chapter 8](#ch-codes)
- **lazy matching**: after a match is found, checking whether the next position starts a longer one. seqlz
  does not do it; it cost more time than it saved. [Chapter 9](#ch-matcher)
- **length value**: the rest of `ll` or `ml` when it is too large for the token, a symbol of the `ll` or
  `ml` table plus plain bits. [Chapter 4](#ch-format)
- **literal**: a byte stored as it is, because the matcher found no earlier copy of it. [Chapter 2](#ch-compression)
- **literal table**: one of the 8 fixed tables for coded literals; a page uses one of them for all its
  literals. Not related to the 8 streams. [Chapter 8](#ch-codes)
- **`ll`, `ml`**: literal length and match length: how many literals a sequence has, 0 or more, and how
  many bytes its match copies, 4 or more. [Chapter 4](#ch-format)
- **LZ77**: the family of compressors that describe data as literals and matches, "copy `ml` bytes from
  `offset` back"; `lz4`, `lzo`, `zstd` and seqlz all are. [Chapter 2](#ch-compression)
- **match**: bytes that appeared earlier in the page, written as a length and an offset. [Chapter 2](#ch-compression)
- **matcher**: the first half of the compressor, which walks through the page and finds matches;
  `seqlz_match_page()` in `src/page_lz.h`. [Chapter 9](#ch-matcher)
- **MSB first**: most significant bit first. The sequences' bitstream is read from the top bit of a
  64-bit register, so the next token is its top {{ch18.bits.token}} bits. [Chapter 12](#ch-dec-basics)
- **offset**: how many bytes back the copy of a match starts. [Chapter 2](#ch-compression)
- **offset bits**: the plain bits after the token that hold the offset, 0 to {{ch18.class_bits.3}} on 4 KiB pages, as many
  as its class says. [Chapter 4](#ch-format)
- **offset class**: one of 6 ways to store an offset: the repeat offset in 0 bits, offsets below 16, 256
  and the page size in {{ch18.class_bits.1}}, {{ch18.class_bits.2}} and {{ch18.class_bits.3}} bits, and multiples of 8 sent divided by 8 in {{ch18.class_bits.4}} and {{ch18.class_bits.5}} bits.
  [Chapter 6](#ch-header)
- **optimal parser**: a matcher that searches for the sequences that make a page smallest, not the
  first match it finds; far slower than a greedy one. seqlz's was `seqlz-opt`. [Chapter 17](#ch-decisions--not-kept)
- **prefix code**: a set of codes where no code is the start of another, so a decoder knows where a code
  ends. `0`, `10`, `11` is one; `0`, `01` is not. [Chapter 2](#ch-compression)
- **repeat offset**: the offset of the match before, used again in class 0 for 0 bits. It starts at 1 on
  every page. seqlz keeps one, `zstd` three. [Chapter 4](#ch-format)
- **scratch**: the buffer the decoder decodes coded literals into, `SEQLZ_SCRATCH`, a page plus 16 bytes,
  one per CPU. [Chapter 6](#ch-header), [chapter 13](#ch-literals-dec)
- **sentinel**: the 1 bit a literal stream keeps below its last real bit, so that the stream needs no
  count of its bits: the position of that 1 is the count. [Chapter 13](#ch-literals-dec--sentinel)
- **sequence**: `ll` literals followed by one match; a page is a list of them, and the last one has
  literals only and no token. [Chapter 4](#ch-format)
- **stream**: one of the 8 parts coded literals are split into, literal $k$ into stream $k \bmod 8$, so the
  decoder can work on 8 at a time. [Chapter 11](#ch-literals-enc)
- **symbol**: whatever a code stands for: a token, a byte value, a length value symbol. [Chapter 2](#ch-compression)
- **table**: the code lengths of all symbols of one kind. seqlz has {{ch17.tables}}, all fixed and part of the format:
  the token table, the `ll` and `ml` tables for length values, and 8 literal tables. In memory each also
  has a decode table built from it once. [Chapter 7](#ch-internal), [chapter 8](#ch-codes)
- **token**: one number for `ll`, `ml − 4` and the offset class of a sequence,
  `min(ll, 15) + 16 × min(ml − 4, 31) + 512 × class`, {{const.TOKEN_SYMBOLS}} values; the token is what the token table
  codes. [Chapter 4](#ch-format)
- **trained tables**: tables whose code lengths come from counting symbols on many pages, here {{ch17.train_pages|grp}}
  of a desktop and a phone; the counts are in `bench/seqlz_counts_*.txt`. The **trainer**,
  `quetschn-seqlz-train`, is the program that counts and builds them. [Chapter 8](#ch-codes--lengths)
- **wild copy**: a copy of a fixed 16 bytes or more where fewer are needed; the bytes past the real
  length are written over by the next sequence. It needs room at the ends of the buffers.
  [Chapter 3](#ch-cpu--wild-copy), [chapter 14](#ch-decode--rooms)
- **work memory**: the codec's own buffers per CPU: the hash table to compress, the scratch to
  decompress. Not the tables, which all CPUs share. [Chapter 1](#ch-zram), [chapter 17](#ch-decisions)

## The kernel {#glossary--kernel}

- **backend**: a codec as zram calls it, through a small set of functions in
  `drivers/block/zram/backend_*.c`. `src/zram_seqlz.c` is seqlz's backend for the benchmarks.
  [Chapter 15](#ch-zram-glue)
- **checkpatch**: `scripts/checkpatch.pl`, the kernel's style checker for patches; `--strict` adds the
  checks maintainers of new code expect. [Chapter 15](#ch-zram-glue--check)
- **`EXPORT_SYMBOL_GPL`**: makes a kernel function callable from modules whose license is compatible with
  the GPL. The kernel copy exports `seqlz_compress()` and `seqlz_decompress()` this way.
  [Chapter 15](#ch-zram-glue)
- **`FORTIFY_SOURCE`**: a kernel option that checks the sizes of `memcpy()` and friends where it can;
  with some configurations it makes each `memcpy()` a call. seqlz uses `__builtin_memcpy()` for its short
  copies, as `lib/lz4` does. [Chapter 3](#ch-cpu), [chapter 6](#ch-header)
- **fuzzer, fuzz target**: a program that feeds a function millions of random and mutated inputs and
  reports every crash, sanitizer finding or hang; a fuzz target is the function's entry point for it.
  `fuzz/` has targets for libFuzzer and AFL++, two such programs, and ClusterFuzzLite runs them every
  day for the repository. [Chapter 15](#ch-zram-glue--fuzzing)
- **`__init`**: marks a function the kernel needs only while it starts; its code is freed afterwards. The
  code that builds seqlz's tables is `__init` in the kernel copy. [Chapter 15](#ch-zram-glue)
- **initcall**: a function the kernel calls once while it starts, registered with a macro such as
  `subsys_initcall()`. The kernel copy builds the tables in one, before a built-in zram needs them.
  [Chapter 15](#ch-zram-glue)
- **KASAN**: the kernel address sanitizer, which catches reads and writes outside an allocation. The
  KUnit tests and the stress test run with it. AddressSanitizer is the same for userspace programs,
  and MemorySanitizer catches reads of memory nobody wrote; the tests and fuzz targets run with them.
  [Chapter 15](#ch-zram-glue)
- **kernel copy**: the code `tools/kernel-port/port.py` writes into a kernel tree from `src/`: kernel
  headers instead of `seqlz_compat.h`, `static` and `__init` where they fit, the zram backend and the
  tests. It is generated, never edited. [Chapter 15](#ch-zram-glue)
- **kernel-doc**: the kernel's comment format for functions, `/** ... */` with `@arg:` lines, which
  `scripts/kernel-doc` checks and turns into documentation. [Chapter 15](#ch-zram-glue--interface)
- **`kmalloc()`**: the kernel's allocator for small objects. Up to `KMALLOC_MAX_CACHE_SIZE`, 2 pages, it
  hands out objects of fixed sizes, powers of 2; a larger request gets whole pages, again a power of 2
  of them: {{const.SCRATCH}} bytes come as 8 KiB, {{ch15.state_plus_scratch|grp}} bytes as 16 KiB.
@assert ch15.mem.dctx_gets == 8192 and ch15.mem.struct_gets == 16384
  [Chapter 1](#ch-zram)
- **KUnit**: the kernel's framework for unit tests that run inside a kernel. seqlz's KUnit tests check the
  tables, the spec's example, round trips and damaged pages. [Chapter 15](#ch-zram-glue)
- **`kvzalloc()`**: allocates zeroed memory with `kmalloc()` and falls back to `vmalloc()` when that
  fails. seqlz's backend allocates its work memory with it, as `zstd`'s does. [Chapter 15](#ch-zram-glue)
- **module**: kernel code built as a file of its own and loaded into a running kernel, instead of built
  in. [Chapter 15](#ch-zram-glue--init)
- **page**: the unit of memory the kernel maps, swaps and zram compresses: 4096 bytes on x86-64 and on
  phones today. Android is moving to 16 KiB; the project has no phone with them yet, and its 16 KiB
  pages come from the Android 17 emulator. [Chapter 1](#ch-zram)
- **page fault**: what happens when a program touches memory that is not mapped, e.g. a page that was
  swapped out. The program waits while the kernel brings the page back. [Chapter 1](#ch-zram)
- **per-CPU**: data the kernel keeps once for each CPU, so that CPUs do not share it. zram has a stream
  per CPU, each with the codec's work memory, which is why that memory counts once per CPU.
  [Chapter 1](#ch-zram)
- **`__ro_after_init`**: marks data that is written while the kernel starts and is read-only after
  that. seqlz's tables, `seqlz_fixed_tables`, are such data in the kernel copy, {{size.tables|grp}} bytes.
  [Chapter 15](#ch-zram-glue)
- **same-filled page**: a page of one repeated 8-byte value, e.g. all zeros. zram stores it without the
  codec, so the metrics of `docs/plan.md` leave it out. [Chapter 1](#ch-zram)
- **size class**: one of zsmalloc's fixed slot sizes. A compressed page takes the smallest slot it fits
  in, so bytes saved within a class save nothing. **huge_class_size**, 3625 bytes on 4 KiB pages, is the
  cliff above which zram stores a page uncompressed. [Chapter 1](#ch-zram)
- **swap, swap-in, swap-out**: moving pages that were not used for a while out of RAM, and back in when
  they are touched. zram is a swap device in RAM that keeps them compressed. [Chapter 1](#ch-zram)
- **UBSan**: the undefined behaviour sanitizer. `CONFIG_UBSAN_BOUNDS` and `CONFIG_UBSAN_SHIFT`, which
  distributions enable, add a compare and a branch to each index and shift the compiler cannot prove in
  range; seqlz masks some of them for that reason. [Chapter 3](#ch-cpu--ubsan)
- **UML**: User-Mode Linux, the kernel built as an ordinary program. CI runs the KUnit tests in it.
  [Chapter 15](#ch-zram-glue)
- **`W=1`**: a kernel build with extra compiler warnings; the kernel copy builds with it without a
  warning. [Chapter 15](#ch-zram-glue--check)
- **`vmalloc()`**: allocates memory that is contiguous only in virtual addresses, in whole pages. `lz4`
  asks it for 16 416 bytes and gets 5 pages, 20 480 bytes. [Chapter 1](#ch-zram)
- **zram**: a block device in RAM whose pages are stored compressed, mostly used as swap. [Chapter 1](#ch-zram)
- **zsmalloc**: the allocator zram stores compressed pages in, with size classes about 16 bytes apart on
  4 KiB pages. [Chapter 1](#ch-zram)
- **zswap**: the kernel's other compressed swap, a cache in RAM in front of a swap device on disk. It
  gives a codec an output buffer of one page, where zram gives two. [Chapter 6](#ch-header--compressor),
  [chapter 10](#ch-encoder)

## The CPU {#glossary--cpu}

- **branch, branch misprediction**: a branch is an `if` or the end of a loop. The CPU guesses its outcome
  and works ahead; a wrong guess throws that work away, about 15 to 20 cycles. [Chapter 3](#ch-cpu)
- **cache, cache line, L1**: caches are small fast memories in the CPU that hold recently used data in
  lines of 64 bytes. L1 is the smallest and fastest, a few dozen KiB, about 1 ns; RAM takes about
  100 ns. seqlz's tables are sized to stay in L1. [Chapter 3](#ch-cpu)
- **ctz, clz**: count trailing zeros and count leading zeros, one instruction each on x86-64 and arm64,
  used through `__builtin_ctzll()` and `__builtin_clz()`. The matcher finds the first differing byte of
  two words with ctz. [Chapter 5](#ch-compat), [chapter 9](#ch-matcher)
- **cycle**: one tick of the CPU's clock, 0.22 ns at 4.5 GHz. [Chapter 3](#ch-cpu)
- **dependency chain**: steps that each need the result of the step before, so the CPU cannot run them
  side by side. The decoder's chain from one token to the next sets its speed. [Chapter 3](#ch-cpu)
- **endianness**: the order of the bytes of a number in memory: little-endian (x86-64, arm64 as used)
  puts the lowest byte first, big-endian (s390) the highest. seqlz reads with
  `get_unaligned_le64()` and friends so that every CPU writes the same bytes. [Chapter 5](#ch-compat)
- **in-order core**: a core that runs instructions strictly in program order, so a load that misses
  stops it, e.g. the Cortex-A55 of phones. [Chapter 3](#ch-cpu)
- **IPC**: instructions per cycle. seqlz's decoder runs about 3.3 on the PC, `lz4`'s 2.4. [Chapter 3](#ch-cpu)
- **out-of-order core**: a core that runs later independent instructions while an earlier one waits, e.g.
  the Ryzen 9 7950X and the Cortex-A76. [Chapter 3](#ch-cpu)
- **prefetch**: asking the CPU to load a line into the cache before it is needed, `prefetch()` in the
  kernel. seqlz prefetches its tables and the compressed data before decoding. [Chapter 3](#ch-cpu),
  [chapter 12](#ch-dec-basics)
- **register**: a variable inside the CPU itself, 64 bits on x86-64 and arm64; x86-64 has 16 for general
  use, arm64 31. Loops that need more spill values to the stack. [Chapter 3](#ch-cpu)
- **store forwarding**: a load that reads bytes a store just wrote gets them from the store directly; a
  load that overlaps a store only in part waits for it. That decides how the decoder copies overlapping
  matches. [Chapter 12](#ch-dec-basics)
- **software pipelining**: starting the loads of the next round of a loop while the current one is
  still being worked on, so that an in-order core does not wait for them. The matcher does it.
  [Chapter 3](#ch-cpu--in-order), [chapter 9](#ch-matcher)
- **SWAR**: SIMD within a register: several small values packed into one 64-bit integer and handled with
  one instruction, e.g. the costs of a byte in all 8 literal tables summed with one add.
  [Chapter 3](#ch-cpu), [chapter 11](#ch-literals-enc)
- **unaligned load**: a load from an address that is not a multiple of its size. x86-64 and arm64 do it
  in one instruction; C needs `memcpy()` or the kernel's `get_unaligned_*()` to say so without undefined
  behaviour. [Chapter 5](#ch-compat)

## Codecs, code names and machines {#glossary--names}

- **`bytelz`**: an experiment with seqlz's matcher and a byte-oriented format, 29.4% against 26.4%, not
  kept. [Chapter 17](#ch-decisions)
- **dump**: a copy of all pages in zram at one moment, read with `dd` from `/dev/zram0`. The book's
  numbers come from dumps of one desktop, of September 2026, and of the Mi 9T, each number the mean
  over the dumps it was measured on; the 16 KiB numbers come from dumps of the Android 17 emulator.
  Their contents are never published, since they hold keys and passwords. [Chapter 17](#ch-decisions--unknown)
- **kernel VM**: `tools/zram-vm/run.sh`, a kernel built with the codecs and booted in a virtual machine
  on one core of a Ryzen 9 7950X fixed at 4.5 GHz. Its swap-in and swap-out times are the ones the
  project decides with. [Chapter 17](#ch-decisions)
- **levels 1 and 2**: zram's level parameter for seqlz: 1 is `seqlz-fast`, 2, the default, is
  `seqlz-fast-lit`. Levels 3 and 4, a deeper matcher, were measured and not kept. [Chapter 15](#ch-zram-glue)
- **`lz4`, `lz4hc`**: the fastest common codec in zram, with a byte format and no tables; `lz4hc` is its
  slow, thorough compressor for the same format. [Chapter 2](#ch-compression)
- **`lzo`, `lzo-rle`**: a byte-format codec, and its variant with a faster path for runs of zeros,
  zram's default. [Chapter 2](#ch-compression)
- **Mi 9T**: the Xiaomi Mi 9T test phone, Snapdragon 730 with big Cortex-A76-based cores and little
  Cortex-A55-based ones, Android 11 and Linux 4.14. The project measures on one core of each kind.
  [Chapter 3](#ch-cpu)
- **quetschn**: the name of the project, its binaries and options. The codec is seqlz.
  [Preface](#preface)
- **seqlz**: the codec this book is about, one format with two compressors, `seqlz-fast` and
  `seqlz-fast-lit`, and one decoder. [Preface](#preface)
- **`seqlz-fast`**: seqlz with raw literals, level 1. [Chapter 10](#ch-encoder)
- **`seqlz-fast-lit`**: seqlz with coded literals where they pay, level 2 and the default; the same
  format and decoder as `seqlz-fast`. [Chapter 11](#ch-literals-enc)
- **`seqlz-opt`**: an optimal parser for the same format, made for zram's recompression, removed.
  [Chapter 17](#ch-decisions--not-kept)
- **`zstd`**: the codec that compresses best in zram, with tables built for the data; level 3 is zram's
  default for it. [Chapter 2](#ch-compression)

## Measurements {#glossary--measurements}

- **C1 to C6**: the six criteria of `docs/plan.md` for a merge: C1 memory (at least 8% less than the
  better of `lzo-rle` and `lz4`), C2 p99 cold reads against `lz4`, C3 p99 writes within 1.2 times
  `lz4`'s, C4 better than `lz4` with a dictionary, C5 work memory per CPU at most `lz4`'s 16 416 bytes,
  C6 a decoder safe and bounded for any input. C5 and C6 are hard limits, C2 and C3 guards.
  [Chapter 1](#ch-zram--goal)
- **cold, warm**: a cold read finds the compressed data in no CPU cache, as for a page swapped out a
  while ago; a warm read finds it there. [Chapter 3](#ch-cpu)
- **exchange rate, lambda**: how many bytes of stored size one µs per page written is worth to you. The
  hull says which codec is best for which range of it. [Chapter 1](#ch-zram--score),
  [chapter 17](#ch-decisions)
- **hot loop**: a codec run in a loop over pages that are in the cache, in userspace, counted in cycles.
  Good for comparing code, too kind for latency. [Chapter 17](#ch-decisions)
- **hull**: the codecs on the lower left convex hull of (time, bytes): each has the lowest score for some
  exchange rate. [Chapter 1](#ch-zram--score), [chapter 17](#ch-decisions)
- **mean, median, p50**: the mean is the average over all pages, the total time divided by the pages;
  the median or p50 is the time half of the pages stay below. [Chapter 1](#ch-zram--score)
- **p99**: the time 99% of the pages stay below; of 20 000 sorted times, the one at position 19 800. It
  shows whether some pages are much slower than the rest. [Chapter 1](#ch-zram--score)
- **point**: 1% of a page, 41 bytes on 4 KiB pages. "0.6 points" is 0.6% of the page size, not 0.6% of
  the stored size. [Chapter 4](#ch-format--offsets)
- **score**: stored bytes plus lambda times the time per page written; lower is better. It replaced the
  bars C1 to C3 as the target for design choices. [Chapter 1](#ch-zram--score), [chapter 17](#ch-decisions)
- **stored size**: what zsmalloc spends on compressed pages, their lengths rounded up to their size
  classes, with the pages over the cliff at 4096 bytes. For a set of pages it is the sum, **Σ zsmalloc
  cost**, often given as a share of the pages' size; per page it is that sum divided by the pages, the
  "bytes per page" of the score. [Chapter 1](#ch-zram--size-classes)
- **swap-in fault, `MODE=swap`**: the kernel VM's benchmark that swaps each page out and back in once, as
  a real page fault does. Other benchmarks decode a page more than once and train the branch predictor
  on it. [Chapter 3](#ch-cpu), [chapter 17](#ch-decisions)
- **time per page written**: the mean time to write a page plus 0.34 times the mean time to read one; the
  0.34 is the reads per write a desktop measured over 27.5 days. [Chapter 1](#ch-zram--score)
