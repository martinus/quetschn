# What this book is, and how to read it {#preface}

seqlz is a compressor for single memory pages, written for zram, the compressed swap device of Linux.
On the pages a desktop swapped into zram, `seqlz-fast-lit` stores a page in 24% and 21% fewer bytes
than `lzo-rle`, zram's default, and takes 22% and 24% more time than `lz4` to write a page and 7% and
13% more to read one. `zstd` stores pages 2% and 10% smaller still, for twice the time to write and
about twice the time to read. Each pair of numbers is two zram dumps of one desktop, measured in the
real kernel in a virtual machine on a Ryzen 9 7950X fixed at 4.5 GHz, on 5th October 2026 with the
tables seqlz had before 7th October; the setup is in
[`docs/seqlz.md`](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#the-result-lz4s-time-zstds-size-almost).

All of that is about 2600 lines of C in one directory, `src/`, plus the trained code lengths in four
`.inc` files, two for each page size.
This book goes through every one of those lines. It shows the code in pieces, as it is in the
repository, and after each piece says what it does, how it does it at the level of C and the CPU,
and why it is written this way and not another. The "why" is the part that is usually missing: most
lines of `src/` are the result of a measurement, and the repository records them in
[`docs/explored-designs.md`](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md),
a log of every design that was tried, including the ones that lost. The book links to it wherever a
line has a number behind it.

## Who this is for {#preface--who}

You know C, or at least a language close to it, and you are comfortable with pointers, bit operations
and structs. You have never written kernel code, and you have never written a compressor. Both are
explained from zero where the code first needs them:

- what zram does with a page, why every page is compressed on its own, and why the memory a codec uses
  counts per CPU;
- how LZ compression turns a page into literals and matches, and how a Huffman code turns numbers
  into bits;
- why a table lookup that misses the cache, a mispredicted `if` and a chain of dependent instructions
  decide the speed of a decoder, and why the small in-order cores of phones make all three worse.

If you know some of this already, skip the chapter; nothing in Part II depends on reading Part I
word for word.

## How the book is organised {#preface--parts}

**Part I, before the code**, has four chapters of background: [zram](#ch-zram),
[compression](#ch-compression), [the CPU](#ch-cpu), and [the format on one page](#ch-format). The last
one is the map for everything after it: the bytes of a compressed page, with one page taken apart bit
by bit.

**Part II, the code**, goes through `src/` file by file, in the order a reader needs it. The headers
come first, because every `.c` file builds on their constants and structs. Then the code that builds
the Huffman tables, then the compressor, then the decompressor, then the glue that makes seqlz a zram
backend.

**Part III** follows [one page through every function](#ch-trace), compressing and decompressing it
with the real numbers each step produces, and then collects [the design decisions](#ch-decisions)
with what they were measured against. [The glossary](#glossary) is at the end.

## The files, and where each is explained {#preface--files}

| file | lines | what it is | chapter |
| --- | --- | --- | --- |
| `src/seqlz_compat.h` | 193 | the kernel's types and helpers, also outside the kernel | [5](#ch-compat) |
| `src/seqlz.h` | 436 | the format's constants, the public functions | [6](#ch-header) |
| `src/seqlz_internal.h` | 146 | the layout of the tables in memory, the token table's entries | [7](#ch-internal) |
| `src/seqlz_codes.c` | 186 | builds codes and decode tables from code lengths | [8](#ch-codes) |
| `src/seqlz_default_tables.c`, `src/seqlz_lit_sets.c` and their `.inc` files | 19 + 19 + 2 × 213 + 2 × 154 | the trained code lengths, part of the format | [8](#ch-codes) |
| `src/page_lz.h` | 183 | the matcher: finds the repeats in a page | [9](#ch-matcher) |
| `src/seqlz_compress.c` | 628 | the encoder, and the coding of literals | [10](#ch-encoder), [11](#ch-literals-enc) |
| `src/seqlz_decompress.c` | 688 | the decoder | [12](#ch-dec-basics), [13](#ch-literals-dec), [14](#ch-decode) |
| `src/zram_seqlz.c` | 147 | seqlz as a zram backend, for the benchmarks | [15](#ch-zram-glue) |

How the pieces call each other, for the compressor on the left and the decompressor on the right:

<pre class="mermaid">
flowchart TB
    Z["zram: a page to store"] --> C["seqlz_compress()"]
    C --> CP["compress_page()"]
    CP --> M["seqlz_match_page()<br/>page_lz.h, the matcher"]
    M -->|"each sequence<br/>as it is found"| E["encode_emit()<br/>token, offset,<br/>length values"]
    CP --> F["encoder_finish()<br/>bitstream behind<br/>the literals"]
    C --> L["code_literals()<br/>Huffman codes<br/>the literals<br/>if that saves enough"]
    R["zram: a page to read"] --> D["seqlz_decode()"]
    D --> DL["decode_literals()<br/>8 streams side by side"]
    D --> LOOP["the loop over<br/>the sequences,<br/>one table lookup each"]
    T["seqlz_tables_init()<br/>once, from the<br/>trained lengths"] -.->|"read only"| C
    T -.->|"read only"| D
</pre>

## Conventions {#preface--conventions}

**Listings are the real files.** Every listing names its file and its line numbers, and the build
script takes the lines from the repository at the commit named on the title page. If you open the
file at that commit, the line numbers match. That commit is the last one that changed `src/`,
`a7a6313` of 10th October 2026. A few chapters name `a2d57dd`, a later commit that changed only
files outside `src/`, so `src/` is the same at both. The kernel's coding style indents with tabs of 8
columns and keeps lines within 80 columns, and the listings show it that way.

**4 KiB pages unless said otherwise.** seqlz is written for pages of 4 KiB and of 16 KiB; the
constant `QUETSCHN_PAGE_BITS` is 12 or 14. Linux on x86-64 and on most phones has 4 KiB pages, and
the kernel's copy of seqlz is built for those only. Numbers in the text are for 4 KiB pages; where 16
KiB pages change something, the text says so.

**Boxes.** Background that a reader may know already is in boxes, so that it is easy to skip:
**Kernel background**, **CPU background**. **Why** boxes hold a design decision and what it was
measured against. **Measured** boxes hold a measurement with its setup, **Careful** boxes a point that
is easy to get wrong, and **Try it** boxes something you can run yourself.

**Numbers have a source.** A number in this book is either computed from the code (and then you can
compute it again) or measured, and then the text says on what and links to where the repository
recorded it. Measurements are from the time they were made; the tables and the code changed since,
so a number from September 2026 is not always the number you would get today.

**Dumps, points and issues.** Most measurements run on zram dumps of the project's desktop: "the
first" and "the second desktop dump" are the ones of 23rd and 24th September 2026, and a third, of
28th September, is named by its date. Memory is often given in **points**: a point is 1% of a page,
41 bytes on 4 KiB pages. "0.3% smaller" is a share of the stored size instead, the bytes zram spends
on the pages ([chapter 1](#ch-zram--size-classes)). A number like #153 is an issue in the project's
GitHub repository, e.g. [#153](https://github.com/martinus/quetschn/issues/153), and the text links
each one where it names it.

**`src/` is not exactly what the kernel gets.** The kernel's copy of seqlz is generated from `src/`
by a script, `tools/kernel-port/port.py`, which replaces the compatibility header with the kernel's own
headers, drops what only the benchmarks and tests use, and adds the kernel's interface around it.
[Chapter 15](#ch-zram-glue) shows what changes and why. Everything in `src/` is written so that this
copy is a mechanical step: kernel C, no libc, no floating point, no inline assembly.
