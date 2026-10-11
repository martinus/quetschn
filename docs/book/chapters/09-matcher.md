# page_lz.h: the matcher finds the repeats {#ch-matcher}

`src/page_lz.h` is the LZ part of seqlz. It walks through a page, byte by byte, and at each place asks
whether the next bytes appeared earlier in the same page. What it finds are sequences: `ll` literals,
then a match of `ml` bytes from `offset` bytes back, as [the format chapter](#ch-format) defines them.
The matcher writes nothing itself. It hands each sequence to a function it is given, and in the
compressor that function is the encoder of [chapter 10](#ch-encoder), which writes the sequence's bits
before the matcher looks at the next byte.

The file has three functions and a type. `seqlz_hash5()` turns 5 bytes into a slot of a hash table,
`seqlz_match_len()` counts how long a match is, and `seqlz_match_page()` is the loop over the page. The
loop is about 90 lines with its comments, and on its own it takes about as much time as all of `lz4`
takes to compress a page: 17 300 to 17 900 cycles per page against `lz4`'s 17 203, x86-64, when it was
last measured alone
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).
That is why most lines below have a measurement behind them, and why some of them look odd at first:
a value loaded one position early, two compares joined with `|` instead of `||`, a hash of 5 bytes
where every other codec in zram hashes 4.

## A header of inline functions {#ch-matcher--file}

@listing src/page_lz.h 1-11

Line 1 is the licence and lines 2, 3 and 185 are the include guard, as in every header of `src/`
([chapter 5](#ch-compat)).

The comment on lines 5 to 9 says why this is a header and not a `.c` file. Every function in it is
`static inline`: it has no symbol of its own, and each file that includes it gets a private copy that
the compiler can merge into its caller. In `src/` the only file that includes it is
`src/seqlz_compress.c`, and there the matcher's loop and the encoder become one function, see
[below](#ch-matcher--emit). The kernel's copy goes one step further: `tools/kernel-port/port.py`
pastes the matcher into `seqlz_compress.c` right before its one caller, see
[chapter 15](#ch-zram-glue).

Line 11 brings in `seqlz.h`, which has everything the matcher needs: the types `u8`, `u16`, `u32` and
`u64`, `SEQLZ_PAGE` (4096), `SEQLZ_HASH_BITS` (12), and through `seqlz_compat.h` the loads
`get_unaligned_le32()` and `get_unaligned_le64()`. [Chapter 5](#ch-compat) explains those loads; for
this chapter it is enough that they read 4 or 8 bytes from any address, aligned or not, and return
them as a number with the first byte in the lowest bits, on every CPU.

## The hash keeps 5 bytes, not 4 {#ch-matcher--hash}

To find out whether the bytes at the current position appeared before, the matcher keeps a table:
for a few bytes of content, the last position where they were seen. A table indexed by the content
itself would need $2^{32}$ entries for 4 bytes, so the content is first squeezed into a small
number, a **hash**, and the hash is the index. Two different byte strings can get the same hash, a
**collision**; the table then holds only one of them, and the matcher has to check that the bytes
really match. The hash is computed at every position the matcher tries, about 4000 per page, so it
has to be cheap.

@listing src/page_lz.h 13-22

`seqlz_hash5()` takes 8 bytes as a `u64`, `v`, and returns a number from 0 to 4095. It is three
operations:

1. `v << 24` shifts out the top 24 bits. The load put the byte at `p` in bits 0 to 7 and the byte at
   `p + 7` in bits 56 to 63, so the top 3 bytes are the bytes at `p + 5`, `p + 6` and `p + 7`. What is
   left, in bits 24 to 63, are the 5 bytes at `p` to `p + 4`, and bits 0 to 23 are 0.
2. `* 889523592379ULL` multiplies by a 40-bit odd constant, modulo $2^{64}$, as every `u64`
   multiplication in C is: the bits above 64 are dropped, which is defined for unsigned types.
3. `>> (64U - SEQLZ_HASH_BITS)` keeps the top 12 bits of the product, a number below 4096. The cast to
   `unsigned int` cannot lose anything.

The comment's example, the bytes `10 00 56 34 12`, the start of a pointer `0x7f1234560010` as x86-64 and
arm64 store it, goes through it like this:

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 262" role="img" aria-label="The 8 bytes at p loaded as a number, shifted left by 24 so that only the 5 lowest bytes remain, multiplied by zstd's prime, and the top 12 bits of the product taken as the slot 1533.">
  <g font-size="11">
    <text x="276" y="30" text-anchor="middle">p+0</text><text x="328" y="30" text-anchor="middle">p+1</text><text x="380" y="30" text-anchor="middle">p+2</text><text x="432" y="30" text-anchor="middle">p+3</text><text x="484" y="30" text-anchor="middle">p+4</text><text x="536" y="30" text-anchor="middle">p+5</text><text x="588" y="30" text-anchor="middle">p+6</text><text x="640" y="30" text-anchor="middle">p+7</text>
  </g>
  <g font-size="12">
    <text x="10" y="58">bytes at p, in memory</text>
    <text x="10" y="108">v = get_unaligned_le64(p)</text>
    <text x="10" y="122" font-size="11">bit 63 left, bit 0 right</text>
    <text x="10" y="158">v &lt;&lt; 24</text>
    <text x="10" y="208">(v &lt;&lt; 24) * 889523592379</text>
    <text x="10" y="222" font-size="11">modulo 2^64</text>
  </g>
  <g fill="none" stroke="currentColor" stroke-width="1">
    <rect x="510" y="38" width="52" height="28"/><rect x="562" y="38" width="52" height="28"/><rect x="614" y="38" width="52" height="28"/>
    <rect x="250" y="88" width="52" height="28"/><rect x="302" y="88" width="52" height="28"/><rect x="354" y="88" width="52" height="28"/>
    <rect x="510" y="138" width="52" height="28"/><rect x="562" y="138" width="52" height="28"/><rect x="614" y="138" width="52" height="28"/>
    <rect x="250" y="188" width="52" height="28"/><rect x="302" y="188" width="52" height="28"/><rect x="354" y="188" width="52" height="28"/><rect x="406" y="188" width="52" height="28"/><rect x="458" y="188" width="52" height="28"/><rect x="510" y="188" width="52" height="28"/><rect x="562" y="188" width="52" height="28"/><rect x="614" y="188" width="52" height="28"/>
  </g>
  <g class="acc" stroke="currentColor" stroke-width="2">
    <rect class="fill-sheet" x="250" y="38" width="260" height="28"/>
    <rect class="fill-sheet" x="406" y="88" width="260" height="28"/>
    <rect class="fill-sheet" x="250" y="138" width="260" height="28"/>
    <line x1="302" y1="38" x2="302" y2="66"/><line x1="354" y1="38" x2="354" y2="66"/><line x1="406" y1="38" x2="406" y2="66"/><line x1="458" y1="38" x2="458" y2="66"/>
    <line x1="458" y1="88" x2="458" y2="116"/><line x1="510" y1="88" x2="510" y2="116"/><line x1="562" y1="88" x2="562" y2="116"/><line x1="614" y1="88" x2="614" y2="116"/>
    <line x1="302" y1="138" x2="302" y2="166"/><line x1="354" y1="138" x2="354" y2="166"/><line x1="406" y1="138" x2="406" y2="166"/><line x1="458" y1="138" x2="458" y2="166"/>
    <rect class="fill-sheet" x="250" y="188" width="78" height="28"/>
  </g>
  <g font-size="13" text-anchor="middle">
    <text x="276" y="57">10</text><text x="328" y="57">00</text><text x="380" y="57">56</text><text x="432" y="57">34</text><text x="484" y="57">12</text><text x="536" y="57">7f</text><text x="588" y="57">00</text><text x="640" y="57">00</text>
    <text x="276" y="107">00</text><text x="328" y="107">00</text><text x="380" y="107">7f</text><text x="432" y="107">12</text><text x="484" y="107">34</text><text x="536" y="107">56</text><text x="588" y="107">00</text><text x="640" y="107">10</text>
    <text x="276" y="157">12</text><text x="328" y="157">34</text><text x="380" y="157">56</text><text x="432" y="157">00</text><text x="484" y="157">10</text><text x="536" y="157">00</text><text x="588" y="157">00</text><text x="640" y="157">00</text>
    <text x="276" y="207">5f</text><text x="315" y="207">d</text><text x="341" y="207">e</text><text x="380" y="207">9f</text><text x="432" y="207">cb</text><text x="484" y="207">b0</text><text x="536" y="207">00</text><text x="588" y="207">00</text><text x="640" y="207">00</text>
  </g>
  <g class="acc" fill="none" stroke="currentColor" stroke-width="1.5">
    <path d="M 250 224 L 250 230 L 328 230 L 328 224"/>
  </g>
  <g class="acc" font-size="12"><text x="250" y="248">top 12 bits: 0x5fd = slot 1533</text></g>
</svg>
</div>
<figcaption>The 5 bytes 10 00 56 34 12 become slot 1533; the 3 bytes after them do not count.</figcaption>
</figure>

The bytes `20 00 56 34 12`, the start of the next pointer, give the product `0x519a6d9760000000` and
slot `0x519`, 1305. Both numbers come from a few lines of C against `src/page_lz.h`. One byte of
difference moves the slot from 1533 to 1305, and that is what a hash has to do: similar inputs land far
apart, so that the table is used evenly.

**Why the top bits.** In a multiplication, bit `k` of the product depends only on bits 0 to `k` of the
two factors: a carry moves up, never down. The input sits in bits 24 to 63 of `v << 24`, so bits 0 to
23 of the product are always 0 (the low 6 hex digits), and only bits 63 down to 24
can depend on the input at all. The top 12 bits, 52 to 63, depend on every one of the 40 input bits;
any lower 12 bits would ignore the top input bits. That is the reason for both the shift by 24 and
the shift down by 52: the 5 bytes are put at the top so that every one of them reaches the bits that
are kept.

**Why a multiplication.** It is one instruction, and it adds many shifted copies of the input on top of
each other, so every input bit reaches many output bits. The constant has to be odd: multiplying by an
odd number modulo $2^{64}$ can be undone, so no two inputs collide before the shift. `zstd` calls
this constant `prime5bytes` and uses the same formula for its 5-byte hash. 889523592379 is
`0xcf1bbcdcbb`, 40 bits, and it is prime; for the hash, being odd is what matters.

::: why A hash of 5 bytes: fewer, longer matches
A match in seqlz is at least 4 bytes long, and `lz4` and `lzo` hash 4 bytes for that reason. seqlz
hashes 5, so the table finds a candidate only where 5 bytes repeat. Matches of exactly 4 bytes are
still found, but only at the repeat offset, which needs no table (see
[two candidates](#ch-matcher--position)).

When it was introduced on 24th September 2026, the 5-byte hash took 5% fewer cycles to compress a page
(20 425 against 21 440 per page, x86-64, userspace) and cost 0.6 points of memory (27.2% against
26.6% of the page). In the kernel it took `seqlz-fast`'s write p99 from 11 650 to 10 480 ns, twice what
the cycles say: the slowest pages are the ones with many short matches, and the 5-byte hash finds fewer
of them ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression),
"A hash of 5 bytes: C3 in the kernel"). With today's tables and coded literals the 4-byte hash is
not even smaller: `seqlz-fast-lit` gets 18 to 35 bytes per page larger with it. Most likely a match of 4
bytes at a new offset takes more bits than the 4 coded literals it replaces, and the greedy matcher
then misses the longer match behind it
([item 5](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#5-the-matchers-hash-5-bytes-into-4096-slots-stays)).
A hash of 6 bytes went further the same way, fewer cycles for more memory, and was dropped.
:::

## How long a match is: seqlz_match_len() {#ch-matcher--match-len}

Once the matcher knows that 4 bytes at the current position `p` equal 4 bytes at an earlier position
`q`, it needs to know how far the equality goes. The obvious loop compares one byte at a time. This one
compares 8 bytes at a time, and the first 16 with a single branch.

@listing src/page_lz.h 24-52

Line 31 keeps the start, so that the function can return how far `p` moved. `end` is the end of the
page: a match cannot run past it, and no load may read past it.

**One word, XOR, and the count of trailing zeros.** Lines 42 and 43 load 8 bytes from each side, twice,
and XOR them. A XOR is 0 in every bit where both sides agree, so `x1` is 0 exactly when the first 8
bytes are equal, and otherwise its lowest set bit is in the first byte that differs. That last part
holds because the loads are little endian: byte 0 is in bits 0 to 7, byte 1 in bits 8 to 15, and so
on, so the lowest bits belong to the earliest byte. `__builtin_ctzll()` counts the zero bits below the
lowest set bit, one instruction on x86-64 (`tzcnt` or `bsf`) and two on arm64 (`rbit`, `clz`), and
`>> 3` turns bits into bytes.

A worked example. Say `p` holds `ABCDEFGHIJKLMNOP` and `q` holds `ABCDEFGHIJKxMNOP`, so the first
difference is byte 11, `L` against `x`:

| | value |
| --- | --- |
| `x1`, bytes 0 to 7 | `0x0000000000000000`, all equal |
| `x2`, bytes 8 to 15 | `0x0000000034000000`: byte 3 of it is `'L' ^ 'x'` = `0x4c ^ 0x78` = `0x34` |
| `x = x1 ? x1 : x2` | `x2` |
| <code>x &#124; x2</code> | not 0, so the function returns |
| <code>__builtin_ctzll(x &#124; 1ULL &lt;&lt; 63)</code> | 26: `0x34` is `00110100`, its lowest set bit is bit 2 of byte 3, $3 \times 8 + 2$ |
| result | `8 + (26 >> 3)` = 8 + 3 = 11 |

11 bytes are equal, and the program that checked this, compiled against `src/page_lz.h`, returns 11.

**One branch for 16 bytes.** Line 44 picks the first word that differs. `x1 ? x1 : x2` is a select, which
compilers turn into a conditional move (`cmov` on x86-64, `csel` on arm64), not a jump. Line 46 asks
whether anything in the 16 bytes differs at all: `x | x2` is 0 only if both words are 0. If something
differs, line 47 adds 8 when the difference is in the second word, with another select, and the count
of the first differing byte. So a match that ends anywhere in the first 16 bytes costs one branch, and
that branch goes the same way most of the time. The comment on lines 33 to 39 gives the reason: whether a
match ends in the first 8 bytes is the hard question to predict, because most matches do and many do
not. A branch that goes one way most of the time and the other way at random is the worst case for a
branch predictor; [chapter 3](#ch-cpu) explains what a mispredicted branch costs.

::: measured The first 16 bytes without a branch
Comparing the first 16 bytes without a branch between the halves took the compressor from 22 848 to
21 916 cycles per page, and the mispredictions from 293 to 280 per page, x86-64 at a fixed 4.5 GHz, on
20 000 pages of a desktop dump. Of the mispredictions before, 29% were the loop of this function: 76%
of the matches were shorter than 12 bytes and 13% were 20 bytes or longer
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).
The version with one `ctz` of the first differing word, instead of a `ctz` of each word combined, came on
3rd October 2026 (commit `4186196`): 3% fewer cycles per page on x86-64, and on the Mi 9T's big core
the commit reports `seqlz-fast`'s compress p50 against `lz4` as 1107 instead of 1208 ns. A loop that compares 16
bytes per round instead of 8 was 1% slower, on the Mi 9T's little core
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-on-an-in-order-core-34-fewer-compress-cycles-on-the-a55-same-output-kept)).
:::

::: warn Why `| 1ULL << 63`
`__builtin_ctzll(0)` is undefined: GCC's manual says the result is undefined if the argument is 0, and
on x86-64 Intel documents the output of the instruction `bsf` as undefined for 0. Inside the `if` on line
46, `x` is never 0: if `x1` is not 0, `x` is `x1`; if it is, `x` is `x2`, and `x | x2` was not 0. But the
compiler does not have to see that through the select. Setting bit 63 makes the argument non-zero for
any `x`, so the call is defined whatever the compiler can prove. It does not change the result: for an
`x` that is not 0, the lowest set bit is at most bit 63, so OR-ing bit 63 in leaves the count as it was.
The price is one OR. Before commit `4186196` the function took the `ctz` of both words every time, and
there the second word could well be 0; the OR made that defined, and a count of 7 bytes stood for 8.
:::

If all 16 bytes are equal, lines 50 and 51 move both pointers on, and the rest of the function takes
over.

@listing src/page_lz.h 53-67

Lines 53 to 61 are the same idea without the second word: 8 bytes per round, as long as 8 bytes are
left before `end`. At the first word that differs, line 57 returns the bytes compared so far plus the
count in this word. Here `x` is not 0 inside the `if`, and the compiler can see it, so no OR is needed.

Lines 62 to 65 handle the last 0 to 7 bytes before the end of the page one at a time; only a match that
runs to the last bytes of the page gets there. The `>= 16` on line 41 and the `>= 8` on line 53 are what
keep every load inside the page: `p` is never closer than 8 bytes to `end` when it loads 8 bytes, and
`q` is always before `p` in the page, so it is in bounds whenever `p` is.

`end - p` is a difference of two pointers into the same array, a signed `ptrdiff_t`, and it is never
negative here. A match may overlap its own source: in a page of `ab` repeated, the match at offset 2
has `q = p - 2`, and the comparison reads bytes that are part of the match. That is no problem for the
matcher, which only reads the page. The decoder, which writes the match while it copies it, has to care,
see [chapter 14](#ch-decode).

## The callback costs no call {#ch-matcher--emit}

@listing src/page_lz.h 69-74

`emit_fn` is the type of a pointer to a function that takes the sequence: a pointer to its first
literal, `ll`, `ml` and the offset, plus `ctx`, a pointer to whatever state the receiver needs. The last
sequence of a page has `ml` 0 and offset 0, because it has only literals.

A call through a function pointer is an indirect call: the CPU jumps to an address it has to load
first, and the compiler cannot look into the function. Once per sequence that would cost, and it would
force the matcher to keep its state in memory across the call. It does not happen, because
`seqlz_match_page()` is `__always_inline` and its two callers pass a constant function:
`seqlz_find()` passes `find_emit()` and `compress_page()` passes `encode_emit()`, both in
`src/seqlz_compress.c`, both `__always_inline` too. After inlining the matcher into its caller, the
compiler sees `emit` is always the same function, turns the indirect call into a direct one, and
inlines that as well. Compiled with `gcc -O2` for x86-64, `seqlz_compress.o` has no indirect call at
all; the only calls left are `memset()`, `memmove()`, `memcpy()` and `encoder_finish()`, and a jump to
`code_literals()` at the end of `seqlz_compress()`.

So the type is there for the source, not for the machine: one matcher with two receivers. `seqlz_find()`
collects the sequences into an array, for tests and tools such as the table trainer
(`bench/seqlz_train_main.cpp`), which trains the tables on exactly the sequences this matcher finds. The
encoder writes them. The kernel's copy has only the encoder, and there `port.py` removes the type and
calls `encode_emit()` by name.

::: why One pass, not two
"Each sequence goes to the encoder as soon as it is found" (line 92) means that matching and encoding
are one loop. Two passes, the matcher into an array and the encoder over the array, took 24 257 instead
of 22 848 cycles per page, and the mispredictions went from 293 to 319 per page: "the branches of the
encoder lose the history of the matcher's branches, which predicted them"
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions),
x86-64, 20 000 pages of a desktop dump). A branch predictor learns from the recent history of branches,
and a sequence the matcher just found is a good hint for the branches that write it.
:::

## Greedy, two candidates, every position {#ch-matcher--plan}

@listing src/page_lz.h 76-97

The comment on lines 76 to 94 is the whole design in three paragraphs. Here it is in more words.

**Greedy.** At each position the matcher looks for a match, and if it finds one, it takes it, extends it
and jumps over it. It does not check whether a match that starts one byte later would be longer, which
is what a **lazy** matcher does, and it does not search for the longest of all earlier occurrences, which
is what a hash chain does. `lz4`'s fast mode is greedy in the same way.

::: why Lazy matching and hash chains: smaller pages, too slow
One step of lazy matching, taking the next position's candidate if it is longer by 2 bytes or more, took
24 808 instead of 20 400 compress cycles per page, 21% more, and `seqlz-fast` stayed at 27.0% of the
page ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#zram-prefetch-the-compressed-data-before-decompression),
the paragraph "A hash of 5 bytes: C3 in the kernel", September 2026). Searching harder was built in full as levels 3 and 4: a hash chain, priced by the
tables' code lengths, with one or two lazy steps. Level 3 stored pages 2.7% to 4.3% smaller than level
2 for 1.8 to 1.9 times the time to write, and was not kept: "Who values memory that much more than time
uses zstd 3"
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#levels-3-and-4-a-hash-chain-priced-by-the-tables-as-small-as-zstd-3-on-two-of-four-dumps-and-faster-to-write-and-read-not-kept)).
:::

**Two candidates.** At each position there are two places where the next 4 bytes might have been
before. One is the **repeat offset**: the offset of the match before, which costs no lookup and which
the format sends in 0 bits (offset class 0). On memory pages it repeats often, e.g. in an array of
structs, where the same field comes back every 16 or 24 bytes. On three desktop dumps of
`docs/seqlz.md`, 18% to 25% of seqlz's matches use it
([docs/seqlz.md](https://github.com/martinus/quetschn/blob/main/docs/seqlz.md#numbers-the-book-quotes));
[chapter 2](#ch-compression)'s 30.55% is another count, the share of all sequences on the pages the
tables were trained on. The
other is the last position whose 5 bytes had the same hash, from the table.

**Every position.** `lz4` and `lzo` skip ahead faster and faster when they find no match, which saves
time on data that does not compress. seqlz tries every position.

::: measured Without `lz4`'s step: writes 3% faster, incompressible pages slower
The step was `1 + (pos - anchor) >> 6`, as in `lz4`. Without it, in the kernel in a VM (Ryzen 9 7950X,
CPU fixed at 4.5 GHz, 20 000 pages of each of two desktop dumps), `seqlz-fast-lit` wrote a page 0.21 and
0.25 µs faster, 3%, at 3.2 bytes more and 2.4 bytes less per page, and the write p99 went from 12 200 to
11 330 ns and from 12 400 to 11 460 ns. Pages that zram stores raw, because they do not compress, take
2.3 µs longer each: the whole page is searched.

It is not faster because it visits other positions. A step that starts only after 1024 literals visits
the same positions on almost every page and writes the same bytes, and it was slower than the old step.
The step needs `anchor` in the loop for the positions without a match, "and the matcher's loop has no
register to spare"
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept),
commit `055e7b5`, 30th September 2026). The 3% are from boots with only `lzo-rle` next to it; in boots
with all four codecs the writes gained 0.05 µs.
:::

The signature on lines 95 and 96: `table` is the hash table, `src` the page, `emit` and `ctx` the
receiver. `__always_inline` comes from `seqlz_compat.h` and is `inline` plus GCC's `always_inline`
attribute, so the compiler inlines it even where its own cost model would not. Without it, a 90-line
function with two callers might stay a function, and `emit` would stay an indirect call.

The picture to keep in mind for the rest of the chapter, with the numbers of a real step from the trace
[at the end](#ch-matcher--back):

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 270" role="img" aria-label="At position 400 of the page the matcher reads two candidates: position 336, 64 bytes back at the repeat offset, and position 126, which the table's slot 3830 holds for the hash of the 5 bytes at 400.">
  <text x="20" y="40" font-size="12">page</text>
  <rect class="fill-paper" x="20" y="50" width="680" height="34" stroke="currentColor" stroke-width="1"/>
  <text x="20" y="100" font-size="11">0</text>
  <text x="700" y="100" font-size="11" text-anchor="end">4095</text>
  <g stroke="currentColor" stroke-width="1.5">
    <rect class="fill-sheet" x="140" y="50" width="40" height="34"/>
    <rect class="fill-sheet" x="465" y="50" width="40" height="34"/>
  </g>
  <g class="acc" stroke="currentColor" stroke-width="2">
    <rect class="fill-sheet" x="580" y="50" width="40" height="34"/>
  </g>
  <g font-size="12" text-anchor="middle">
    <text x="160" y="72">126</text>
    <text x="485" y="72">336</text>
  </g>
  <g class="acc" font-size="12" text-anchor="middle"><text x="600" y="72">400</text></g>
  <g class="mark" fill="none" stroke="currentColor" stroke-width="1.5">
    <path d="M 600 50 C 590 18, 495 18, 485 46"/>
    <polygon class="fill-mark" points="485,50 481,41 489,42"/>
  </g>
  <g class="mark" font-size="12"><text x="542" y="14" text-anchor="middle">repeat offset: pos - last, last = 64</text></g>
  <g class="acc" fill="none" stroke="currentColor" stroke-width="1.5">
    <line x1="600" y1="84" x2="600" y2="196"/>
    <polygon class="fill-acc" points="600,200 596,190 604,190"/>
  </g>
  <g class="acc" font-size="12">
    <text x="610" y="134">seqlz_hash5()</text>
    <text x="610" y="150">of 5 bytes</text>
    <text x="610" y="166">= 3830</text>
  </g>
  <text x="20" y="194" font-size="12">table: 4096 slots of u16, 8 KiB</text>
  <rect class="fill-paper" x="20" y="200" width="680" height="34" stroke="currentColor" stroke-width="1"/>
  <g stroke="currentColor" stroke-width="1">
    <line x1="60" y1="200" x2="60" y2="234"/><line x1="100" y1="200" x2="100" y2="234"/><line x1="140" y1="200" x2="140" y2="234"/>
    <line x1="540" y1="200" x2="540" y2="234"/><line x1="660" y1="200" x2="660" y2="234"/>
  </g>
  <g font-size="11" text-anchor="middle"><text x="40" y="221">0</text><text x="80" y="221">0</text><text x="120" y="221">0</text><text x="340" y="221">...</text></g>
  <g class="acc" stroke="currentColor" stroke-width="2">
    <rect class="fill-sheet" x="580" y="200" width="40" height="34"/>
  </g>
  <g class="acc" font-size="12" text-anchor="middle"><text x="600" y="222">126</text></g>
  <text x="600" y="252" font-size="11" text-anchor="middle">slot 3830</text>
  <g fill="none" stroke="currentColor" stroke-width="1.5" stroke-dasharray="5 3">
    <line x1="582" y1="200" x2="166" y2="90"/>
  </g>
  <polygon points="160,86 172,87 167,96" class="fill-paper" stroke="currentColor" stroke-width="1"/>
  <text x="40" y="150" font-size="12">the slot's position is a candidate</text>
  <text x="40" y="166" font-size="12">only: 4 bytes are compared</text>
</svg>
</div>
<figcaption>Two candidates at position 400: the repeat offset, 64 bytes back, and the position the table's slot holds.</figcaption>
</figure>

## The state of the loop, and why reading a candidate is always safe {#ch-matcher--state}

@listing src/page_lz.h 98-124

**Positions, not pointers.** Line 103 sets `limit` to 4088, the first position that is not tried. All
the matcher's places are `unsigned int` positions in the page, not pointers: the limit is then a
constant the compiler can put into the compare instruction, and the table stores positions anyway.
Changing the loop from pointers to positions took 21 916 to 21 786 cycles per page, a small step of the
[second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions).

The variables of line 104:

| variable | meaning | starts at |
| --- | --- | --- |
| `pos` | the position being tried | 1 |
| `anchor` | the first byte not yet in any sequence, where the next sequence's literals start | 0 |
| `last` | the repeat offset, the offset of the match before | 1, as the format says |
| `h` | the hash of the 5 bytes at `pos` | |
| `cand` | the table's position for `h` | |

Position 0 is never tried: there is nothing before it to match. `last` starts at 1 because the format
starts every page with a repeat offset of 1 (see [offsets](#ch-format--offsets)), and the encoder can
then send a match at offset 1 in class 0, without offset bits.

**The repeat offset as a negative index.** Line 109: `back` is `-last`, a `long`. The load at the repeat
offset on line 139 is `src + pos + back`, which C reads as `(src + pos) + back`: a pointer plus a
negative number, one addressing operation. With `src + pos - last` the subtraction would be an
instruction of its own on every position. `long` because it is negative and is added to a pointer; an
`unsigned int` would wrap around to a large positive number.

**Loaded one position ahead.** Lines 117 and 118 declare two more values, and the comment on lines 110
to 114 says what they are for: `v` holds the 8 bytes at `pos`, `h` their hash, `cand` the table's
entry and `cand_bytes` the 4 bytes at that entry. "Each of them needs the one before": the hash needs
`v`, the table load needs `h`, the candidate's bytes need `cand`. Lines 121 to 124 compute them for
position 1 before the loop, and the loop always computes them for the next position before it needs
them. Why that helps is in the [next section](#ch-matcher--position).

**The table: 4096 positions of 2 bytes, cleared for each page.** Line 120 sets the table to zeros:
`sizeof(u16) << SEQLZ_HASH_BITS` is $2 \times 4096 = 8192$ bytes. The table lives in `struct seqlz_state`
(see [chapter 6](#ch-header)), which zram allocates once per CPU, and it is reused for every page.

::: why A cache of 8 KiB, not a map
Each slot holds one position, the last one stored there. A new position overwrites the old one, there
is no list, and a collision loses the older position. That costs a match now and then, never a wrong
result, because every candidate is compared before it is used
([one position](#ch-matcher--position)).

- **2 bytes per slot.** A position in a page of 4 KiB needs 12 bits, so `u16` is the smallest type that
  holds it, also for 16 KiB pages. A table of 4096 pointers would take 32 KiB per CPU. `lz4` in the
  kernel also stores positions of 2 bytes for inputs this small, 8192 of them.
- **4096 slots.** "A page has 4096 positions, and a table of 4096 entries already keeps nearly every
  candidate that helps"
  ([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-on-the-phone-time-goes-per-sequence-and-code-layout-moves-reads-by-130-ns)).
  On the Mi 9T, 2048 slots made pages 939.6 instead of 936.9 bytes and were no
  faster; 8192 slots made them 937.1 bytes, cost the in-order A55 1.6 µs more per write, and
  would take 16 KiB per CPU, all of `lz4`'s budget
  ([item 5](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#5-the-matchers-hash-5-bytes-into-4096-slots-stays)).
  With 16 KiB pages `SEQLZ_HASH_BITS` is 13: 8192 slots, 16 KiB.
- **Cleared for each page.** An 8 KiB `memset()` per page took the compressor from 25 064 to 23 327
  cycles per page, 7% less, against a check at every position whether the entry is a position of this
  page ([second round](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).
  `lz4` in the kernel clears its 16 KiB for each page too.
:::

Clearing the table has a second effect that the loop depends on. Every entry is now a position of this
page: 0 if nothing was stored, else a position stored earlier on this page, and both are before `pos`.
So line 124 can load 4 bytes at `src + cand` without checking anything, and so can every later load of
a candidate. A slot that was never written is not "empty"; it says "position 0", a real place, and if
the 4 bytes there match, it is a real match. The page of `ab` repeated finds its only match that way: at
position 2 the slot of `ababa` was never written, holds 0, and the 4 bytes at 0, `abab`, equal the 4
bytes at 2.

The other candidate is safe for a similar reason: `last` starts at 1 and `pos` at 1, so the first load
at the repeat offset reads position 0. After a match, `last` is that match's offset, which is at most
the match's start, and `pos` only grows, so `pos - last` stays at 0 or more. The comment on lines 135 to
135 says this in one sentence: both candidates point into the page, before `pos`.

## One position: two candidates, one branch {#ch-matcher--position}

@listing src/page_lz.h 126-151

This is the loop over the positions, and lines 127 to 150 are the path of a position without a match,
the path taken most often: on the phone's pages, a run without a match is 4 positions long on average
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-a55-again-the-write-as-on-4-october-the-decoders-code-prefetched-for-cold-reads-not-kept)).

Line 126 runs while `pos < limit`. Each position reads 8 bytes at `pos + 1` (line 127), so the last one
tried, 4087, reads up to byte 4095, the last byte of the page. That is the reason for `limit`: "every
try reads 8 bytes". The last 8 bytes of a page are never the start of a match; they end up in the last
sequence's literals unless a match runs over them.

Line 127 loads the next position's 8 bytes and line 128 hashes them, before anything else. Line 130
takes the first 4 bytes at `pos` from `v`, which holds 8 bytes starting there: the cast to `u32` keeps
the low 32 bits, and in a little-endian load those are the first 4 bytes. One load saved per position.

Lines 139 and 141 test both candidates. `rep_hit` compares the 4 bytes at the repeat offset with
`cur`, `cand_hit` the 4 bytes at the table's candidate, loaded during the position before. A compare in
C gives 0 or 1, and line 144 combines them with `|`, the bitwise OR, not `||`. With `||` the C rules
say the second operand is evaluated only if the first is false, and compilers often make that a
second branch. With `|` both are computed and one branch decides. The comment on lines 131 to 138 gives
the reason: with a branch for each candidate, the matcher mispredicted almost twice as often as `lz4`.

::: measured One branch for both candidates
With three branches per position, the first `seqlz-fast` mispredicted 539 times per page. Reading both
candidates and deciding with one branch took that to 329, and the p99 of the cold compress loop from
15.4 to 12.6 µs. With the page in the cache, the extra reads cost 7%
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-seqlz-fast),
x86-64, userspace, September 2026, with an older matcher).
:::

Line 143 stores the current position in the table, before the branch, so that it is stored whether or
not there is a match. The `(u16)` cast is exact, since `pos` is below 4096. It also has to come before
line 148: if the next position has the same hash as this one, `table[h]` for the next position must
already be `pos`, as it was in the loop before the loads moved ahead. The comment on lines 114 and 115
says this: "the entry is still read after the position before it was stored, so the matches are the
same".

Lines 145 to 150 are the miss: move to the next position, take over its 8 bytes and hash computed on
lines 127 and 128, and load its table entry and the 4 bytes at its candidate.

On an in-order core such as the phone's Cortex-A55, which waits at every load whose value is not
there yet ([chapter 3](#ch-cpu--in-order)), a position without a match is one chain: load 8 bytes,
multiply for the hash, load the table entry, load 4 bytes at it, compare, branch. There every link
waited for the one before, at 0.91 instructions per cycle. Today's loop starts the next position's load
and multiplication first, which is software pipelining, so the compares use operands loaded during the
position before.

The loop before commit `996248b` loaded and hashed at the top of each position and compared right away
(its comment left out):

```c
while (pos < limit) {
	u32 cur = load32(src + pos);
	unsigned int h = hash5(load64(src + pos)), cand = table[h], m, len;
	unsigned int rep_hit = load32(src + pos - last) == cur;
	unsigned int cand_hit = load32(src + cand) == cur;

	table[h] = (unsigned short)pos;
	if (!(rep_hit | cand_hit)) {
		pos++;
		continue;
	}
	...
```

Each line waits for the one before it. In today's loop the compares on lines 139 and 141 use `v` and
`cand_bytes`, which were loaded at the end of the previous position; the chain for position `pos + 1`
starts on line 127 and ends on line 149, and its results are needed only at the compares of the next
iteration.

::: measured One position ahead: 3.4% fewer cycles on the A55, the same bytes
On the Mi 9T's little core (Cortex-A55) the compress loop took 42 349 instead of 43 841 cycles per page,
and 39 762 instead of 40 965 instructions; on x86-64 16 148 instead of 16 489 user cycles. The 4 bytes
taken from the 8 of the hash and the negative index for the repeat offset made it 15 instead of 17
instructions per position. Every page's compressed length stayed the same, on 455 239 pages of a
desktop dump and 81 709 of a phone's
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-on-an-in-order-core-34-fewer-compress-cycles-on-the-a55-same-output-kept),
commit `996248b`, 3rd October 2026). Reading the next table entry before the store and choosing the
right one with a compare was 9% slower: more instructions.
:::

## A hit: back, forward, emit {#ch-matcher--hit}

@listing src/page_lz.h 152-167

Line 152 picks the match's source `m`, the earlier position the bytes are copied from. If the repeat
offset hit, it is `pos - last`; else the table's candidate. When both hit, the repeat offset wins: its
offset costs 0 bits. Whether the table's candidate would give a longer match is not checked. Measured
offline on 20 000 pages of two dumps, always taking the longer of the two made pages about 1 byte larger, not
smaller, and taking the table's only when it is longer by more than 2 bytes saved 0.3 to 0.6 bytes per
page; either needs a second call of `seqlz_match_len()`
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept),
"The longer of the two candidates").

**Backwards.** Lines 157 to 160 extend the match to the left while the byte before `pos` equals the byte
before `m`. Three conditions: `pos > anchor`, so the match does not reach into the sequence before, whose
bytes are already written; `m > 0`, so `src[m - 1]` exists; and the bytes are equal. Each step moves
both by one, so the offset `pos - m` stays the same.

Why can a match start before the position where it is found, if every position is tried? Because a
position is found only if its candidate is in the table. The positions inside a match are not
inserted (see [below](#ch-matcher--insert)), and a slot can be overwritten by a collision. When the bytes
of a later place repeat bytes from the inside of an earlier match, the matcher finds them only from the
one inserted position near that match's end, and the backward extension recovers the bytes before it.
[The trace below](#ch-matcher--back) shows such a case.

**Forwards.** Line 161 counts the match length. The first 4 bytes are known to be equal, from the 4-byte
compare, so the count starts 4 bytes in and adds 4. `src + SEQLZ_PAGE` is the end: a match may run to the
last byte of the page.

**Emit.** Line 163 sets the new repeat offset, line 164 its negative index. Line 165 hands the sequence
to the receiver: its literals start at `anchor`, there are `pos - anchor` of them, then the match of
`len` bytes at offset `last`. In the compressor this is where `encode_emit()` copies the literals and
writes the bits. Lines 166 and 167 jump over the match: the next position to try is the first byte after
it, and that is also where the next sequence's literals start.

## One position of a match goes into the table {#ch-matcher--insert}

@listing src/page_lz.h 168-185

The positions inside a match were skipped, so they are not in the table. Lines 173 to 175 store one of
them: the position 2 bytes before the end of the match, under the hash of its 5 bytes. That is where the
next match often starts: data that repeats once tends to repeat again, and the end of one match is often
near the start of the next. `lz4`'s fast mode stores the same position.

::: why One position, not all of them, and not the start
Storing every position of a match would mean a hash, a load and a store per byte of the match, where the
matcher now does nothing at all. Storing the start of each match as well, as `zstd`'s fast mode does, was
measured: 600 and 500 cycles more per page, for 29 bytes per µs on one desktop dump and nothing on the
other, and not kept
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-matcher-without-its-step-writes-1-to-3-faster-kept)).
:::

The `if` on line 173 does two things. It keeps the 8-byte loads inside the page, as `limit` does in the
loop, and it skips the work when the loop is about to end. Lines 176 to 179 restart the one-ahead
state at the new `pos`: its 8 bytes, hash, table entry and candidate bytes, as lines 121 to 124 did before
the loop. Note the order again: the insert on line 174 comes before the load of `table[h]` on line 178,
so if the new position has the same hash as `pos - 2`, it finds `pos - 2`.

When the loop ends, the bytes from `anchor` to the end of the page are not in any sequence yet. Line
180 emits them as the last sequence: literals only, `ml` 0 and offset 0. The format has no count of
sequences, and the last one has no token: the decoder knows it has come when the literals left are as
many as the bytes the page still needs, see [the format chapter](#ch-format--end). That last sequence can
have 0 literals, when a match ran to the end of the page.

How many sequences can there be? Every sequence but the last covers at least 4 bytes, the shortest
match, so a page of 4096 bytes has at most $4096 / 4 + 1 = 1025$, which is `SEQLZ_MAX_SEQUENCES`. The
bound matters for `seqlz_find()`'s array, and the encoder's proof that its output fits uses it too, see
[chapter 10](#ch-encoder--layout).

## The matcher on two made-up pages {#ch-matcher--back}

Below, `seqlz_match_page()` runs on two pages, one statement at a time: the page of 512 pointers of this
section, and the made-up page of [chapter 16](#ch-trace). It is the function of this chapter, recorded
while it searched the pages when the book was built, so every position, slot and match is the real one.
Step with the buttons or the arrow keys, and with shift from one sequence to the next.

@stepper match

The same in prose and tables:

The numbers in this section come from running the code: `seqlz_find()` from `src/` on pages built in a
few lines of C, and for the step-by-step tables a copy of `page_lz.h` with a `printf()` added after line
139, compiled with `gcc -O2 -Isrc`.

**An array of pointers.** The page holds 512 pointers of 8 bytes, `0x7f1234560010 + 16 * i`.
In memory the first two are `10 00 56 34 12 7f 00 00` and `20 00 56 34 12 7f 00 00`. The first positions
the loop tries:

| `pos` | 5 bytes at `pos` | `h` | `cand` | repeat offset | `rep_hit` | `cand_hit` |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `00 56 34 12 7f` | 3974 | 0 | 1 | 0 | 0 |
| 2 | `56 34 12 7f 00` | 4063 | 0 | 1 | 0 | 0 |
| 3 to 7 | ... | ... | 0 | 1 | 0 | 0 |
| 8 | `20 00 56 34 12` | 1305 | 0 | 1 | 0 | 0 |
| 9 | `00 56 34 12 7f` | 3974 | **1** | 1 | 0 | **1** |
| 16 | `30 00 56 34 12` | 1077 | 0 | 8 | 0 | 0 |
| 17 | `00 56 34 12 7f` | 3974 | 9 | **8** | **1** | 1 |
| 24 | `40 00 56 34 12` | 849 | 0 | 8 | 0 | 0 |
| 25 | `00 56 34 12 7f` | 3974 | 17 | **8** | **1** | 1 |

At positions 1 to 8 every slot is still 0, so every `cand` is 0 and its 4 bytes are `10 00 56 34`, which
match nothing. Position 8 is where the 5 bytes `20 00 56 34 12` get slot 1305, the second example of
[the hash](#ch-matcher--hash). (The first example, slot 1533, never comes up here: it is the hash of
position 0, which is never tried.) At position 9 the 5 bytes `00 56 34 12 7f` come again, slot 3974 holds
1 from position 1, and the 4 bytes match: offset 8. Extending backwards fails at once, `20` is not `10`.
Forwards, the match covers `00 56 34 12 7f 00 00` and stops at `30` against `20`: 7 bytes. The first
sequence is 9 literals and a match of 7 at offset 8.

From then on it repeats: position 16 (`30`, the low byte of the third pointer) misses, position 17 hits
at the repeat offset 8, and the table's candidate hits too, at the same place. What `seqlz_find()`
returns:

| sequence | starts at | `ll` | `ml` | offset |
| --- | --- | --- | --- | --- |
| 0 | 0 | 9 | 7 | 8 |
| 1 to 13 | 16, 24, ..., 112 | 1 | 7 | 8 |
| 14 | 120 | 2 | 6 | 8 |
| 15 to 29 | 128, ..., 240 | 1 | 7 | 8 |
| 30 | 248 | 2 | 6 | 8 |
| ... | | | | |
| 509 | 4080 | 1 | 7 | 8 |
| 510 | 4088 | 8 | 0 | 0 |

511 sequences. Every 16th pointer carries into its second byte (`0x...00f0 + 0x10 = 0x...0100`), and
then 2 bytes differ from the pointer before: sequence 14 has the literals `00 01` and a match of 6. The
last sequence shows `limit` at work: the match of sequence 509 ends at 4088, the loop stops there, and the
last pointer's 8 bytes become literals, where one literal and a match of 7 would have done.

**A match found late and extended backwards.** A page of random bytes `R`, with bytes 0 to 63 copied to
64 to 127, and the 14 bytes at 124 to 137 copied to 398 to 411. Bytes 124 to 127 are the end of the copy
of `R`, bytes 128 to 137 are random. The matcher finds the copy at 64 at once, a match of 64 bytes at
offset 64, and inserts position 126 ($= 128 - 2$) into the table. The positions 64 to 125 are not in it.
At the second copy:

| `pos` | 5 bytes at `pos` | `h` | `cand` | `cand_hit` |
| --- | --- | --- | --- | --- |
| 398 | `3c f9 35 fd f3` | 1099 | 0 | 0 |
| 399 | `f9 35 fd f3 ce` | 3419 | 139 | 0 |
| 400 | `35 fd f3 ce b3` | 3830 | 126 | 1 |

The copy starts at 398, but 398 and 399 miss: their 5 bytes were last at 124 and 125, inside the match,
never inserted. Slot 3419 even holds 139, a collision with other bytes. Position 400 has the bytes of
126, the one position the match inserted, and hits. Then lines 157 to 160 step back: `src[399]` is
`f9` and `src[125]` is `f9`, `src[398]` is `3c` and `src[124]` is `3c`, `src[397]` is `fe` and
`src[123]` is not. The sequence `seqlz_find()` returns is 270 literals (128 to 397) and a match of 14 at
offset 274, starting at 398, as if it had been found there. Without the backward extension the 2 bytes
would be literals, and a match of 12 would follow them.

::: try The sequences of the page of pointers
In the repository, save this as `find.c` and build it with `gcc -O2 -Isrc -o find find.c
src/seqlz_compress.c src/seqlz_codes.c src/seqlz_default_tables.c src/seqlz_lit_sets.c`:

```c
#include "seqlz.h"
#include <stdio.h>

static struct seqlz_state st;
static u8 page[SEQLZ_PAGE];
static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];

int main(void)
{
	unsigned int i, n, pos = 0;

	for (i = 0; i < SEQLZ_PAGE / 8; i++)
		put_unaligned_le64(0x7f1234560010ULL + 16 * i, page + 8 * i);
	n = seqlz_find(&st, page, seq);
	printf("%u sequences\n", n);
	for (i = 0; i < n; i++) {
		printf("at %4u: ll %u, ml %u, offset %u\n", pos,
		       seq[i].literals, seq[i].match, seq[i].offset);
		pos += seq[i].literals + seq[i].match;
	}
	return 0;
}
```

It prints `511 sequences` and the sequences of the table above. Fill `page` with your own bytes to see
what the matcher makes of them.
:::

What the matcher misses is older places. The table keeps one position per slot, the newest, so when the
same bytes appeared at several earlier places, it sees only the last one, and a longer match at an older
place is not found. More positions per slot, a hash chain, lazy steps: each was measured, and each costs
about as much time as it saves bytes
([explored-designs.md, "seqlz-fast-lit: one of 8 literal tables per page"](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-one-of-8-literal-tables-per-page)).
`zstd` 3 searches harder, and that is part of why it stores pages smaller, at about twice the time to
write one.
