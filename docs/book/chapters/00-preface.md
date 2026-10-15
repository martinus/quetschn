# What this book is, and how to read it {#preface}

seqlz is a compressor for single memory pages, written for zram, the compressed swap device of Linux.
On the pages a desktop swapped into zram, `seqlz-2` stores a page in 24% fewer bytes than
`lzo-rle`, zram's default, and takes 17% more time than `lz4` to write a page and 9% more to read
one. `zstd` stores pages 4% smaller still, for twice the time to write and about twice the time to
read. Measured in the real kernel in a virtual machine on a Ryzen 9 7950X fixed at 4.5 GHz, on 14th
October 2026; the setup is in
[`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-result-lz4s-time-zstds-size-almost).

All of that is {{ch00.src_lines|grp}} lines of C in one directory, `src/`, plus the trained code
lengths in four `.inc` files, two for each page size.
@assert ch00.inc_files == 4
This book goes through every one of those lines, in pieces as they are in the repository, and after
each piece says what it does that is not plain from the C, and why it is written this way. The "why"
is mostly a measurement: the repository logs every design it tried, also the ones that lost, in
[`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md),
and the book links to it wherever a line has a number behind it.

## Who this is for {#preface--who}

You know C and C++ well: pointers, the integer rules, undefined behaviour, what a compiler inlines.
The book does not explain the language. You have never written kernel code or a compressor, and Part
I explains what the code needs of both: what zram does with a page and why memory per CPU counts, how
LZ matches and Huffman codes work, and why cache misses, mispredicted branches and chains of
dependent instructions decide the speed of a decoder, more so on the small in-order cores of phones.
Skip what you know; Part II links back where it needs a term.

## How the book is organised {#preface--parts}

**Part I, before the code**: [zram](#ch-zram), [compression](#ch-compression), [the CPU](#ch-cpu),
and [the format on one page](#ch-format), which takes two compressed pages apart bit by bit and is the
map for everything after it.

**Part II, the code**, goes through `src/` file by file: the headers first, because every `.c` file
builds on their constants and structs, then the code that builds the Huffman tables, the compressor,
the decompressor, and the glue that makes seqlz a zram backend.

**Part III** follows [one page through every function](#ch-trace) with the numbers each step
produces, and collects [the design decisions](#ch-decisions) with what they were measured against.
[The glossary](#glossary) is at the end.

## The files, and where each is explained {#preface--files}

| file | lines | what it is | chapter |
| --- | --- | --- | --- |
| `src/seqlz_compat.h` | {{ch00.lines.seqlz_compat_h}} | the kernel's types and helpers, also outside the kernel | [5](#ch-compat) |
| `src/seqlz.h` | {{ch00.lines.seqlz_h}} | the format's constants, the public functions | [6](#ch-header) |
| `src/seqlz_internal.h` | {{ch00.lines.seqlz_internal_h}} | the layout of the tables in memory, the token table's entries | [7](#ch-internal) |
| `src/seqlz_codes.c` | {{ch00.lines.seqlz_codes_c}} | builds codes and decode tables from code lengths | [8](#ch-codes) |
| `src/seqlz_default_tables.c`, `src/seqlz_lit_sets.c` and their `.inc` files | ${{ch00.lines.seqlz_default_tables_c}} + {{ch00.lines.seqlz_lit_sets_c}} + 2 \times {{ch00.lines.seqlz_default_tables_4k_inc}} + 2 \times {{ch00.lines.seqlz_lit_sets_4k_inc}}$ | the trained code lengths, part of the format | [8](#ch-codes) |
| `src/page_lz.h` | {{ch00.lines.page_lz_h}} | the matcher: finds the repeats in a page | [9](#ch-matcher) |
| `src/seqlz_compress.c` | {{ch00.lines.seqlz_compress_c}} | the encoder, and the coding of literals | [10](#ch-encoder), [11](#ch-literals-enc) |
| `src/seqlz_decompress.c` | {{ch00.lines.seqlz_decompress_c}} | the decoder | [12](#ch-dec-basics), [13](#ch-literals-dec), [14](#ch-decode) |
| `src/zram_seqlz.c` | {{ch00.lines.zram_seqlz_c}} | seqlz as a zram backend, for the benchmarks | [15](#ch-zram-glue) |
@assert len(ch00.lines) == 14
@assert ch00.lines.seqlz_default_tables_4k_inc == ch00.lines.seqlz_default_tables_16k_inc and ch00.lines.seqlz_lit_sets_4k_inc == ch00.lines.seqlz_lit_sets_16k_inc

How the pieces call each other, the compressor on the left and the decompressor on the right:

<pre class="mermaid">
flowchart TB
    Z["zram: a page to store"] --> C["seqlz_compress()"]
    C --> CP["compress_page()"]
    CP --> M["seqlz_match_page()<br/>page_lz.h, the matcher"]
    M -->|"each sequence<br/>as it is found"| E["encode_emit()<br/>token, offset,<br/>length values"]
    CP --> F["encoder_finish()<br/>bitstream behind<br/>the literals"]
    C --> L["code_literals()<br/>Huffman codes<br/>the literals<br/>if that saves enough"]
    R["zram: a page to read"] --> D["seqlz_decode()"]
    D --> DL["decode_literals()<br/>{{ch00.LIT_STREAMS}} streams side by side"]
    D --> LOOP["the loop over<br/>the sequences,<br/>one table lookup each"]
    T["seqlz_tables_init()<br/>once, from the<br/>trained lengths"] -.->|"read only"| C
    T -.->|"read only"| D
</pre>

## Conventions {#preface--conventions}

**Listings are the real files.** Every listing names its file and its line numbers, taken from the
repository at the commit named on the title page, the last one that changed `src/`. A few chapters
name `a2d57dd` of 10th October 2026, an older commit, for a count or a layout taken there. The code is
in the kernel's style: tabs of 8 columns, lines within 80 columns.

**4 KiB pages unless said otherwise.** seqlz is written for pages of 4 KiB and of 16 KiB,
`QUETSCHN_PAGE_BITS` 12 or 14. Linux on x86-64 and on most phones has 4 KiB pages, and the kernel's
copy of seqlz is built for those only. Where 16 KiB pages change something, the text says so.

**Boxes.** **Kernel background** and **CPU background** boxes are easy to skip if you know the
subject. **Why** boxes hold a design decision and what it was measured against, **Measured** boxes a
measurement with its setup, **Careful** boxes a point that is easy to get wrong, and **Try it** boxes
something you can run.

**Numbers have a source.** A number is either computed from the code when the book is built, or
measured, and then the text says on what and links to where the repository recorded it. Measurements
are from the day they were made; the code and the tables changed since, so a number from September
2026 is not always what you would get today.

**Dumps, points and issues.** Most measurements run on zram dumps, copies of the pages that a desktop
or a phone had in zram, and a number is the mean over the dumps it was measured on. A **point** is 1%
of a page, 41 bytes on 4 KiB pages. "0.3% smaller" is a share of the stored size, the bytes zram
spends on the pages ([chapter 1](#ch-zram--size-classes)). A number like
[#153](https://github.com/martinus/quetschn/issues/153) is an issue of the project on GitHub.

**`src/` is not exactly what the kernel gets.** A script, `tools/kernel-port/port.py`, generates the
kernel's copy from `src/`: it replaces the compatibility header with the kernel's own headers, drops
what only the benchmarks and tests use, and adds the kernel's interface around it
([chapter 15](#ch-zram-glue)). The codec's files are kernel C already, without libc, floating point
or inline assembly, so that copy is a mechanical step.
