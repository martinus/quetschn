# One page, through every function {#ch-trace}

Part II explained each function on its own. This chapter follows one page through all of them, in
the order they run: the tables are built, the page is compressed with `seqlz-fast-lit`, and the
compressed page is decoded again. Every number in the chapter was printed by a small program that
calls the real code of `src/` on that page, or by a few `printf()` lines added to it or to a copy of
the decoder. The program is at the end, with the command that builds it, so you can change the page
and watch the numbers move.

The page is made up. Real memory pages from a zram dump would be more honest, but they hold keys and
passwords, so the page here is built to look like one: a small header, an array of records with a
pointer, a few small integers and a name each, a stretch of English text, and zeros up to the end.
It compresses to 796 bytes, 19.4% of the page, and with raw literals to 947.

## The page {#ch-trace--page}

The 4096 bytes, in four parts:

- **Bytes 0 to 63, a header.** `"TASKSLAB"`, then the record count 48 and the record size 32 as
  little-endian 32-bit integers, then zeros.
- **Bytes 64 to 1599, 48 records of 32 bytes.** Each has a `next` pointer, a kernel address
  `0xffff888104a3c040` plus 32 times another record's number, `(5i + 1) mod 48`, so 8-aligned and with the same top 6 bytes in
  every record; a pid, `1000 + 3 * i`; a state `i % 3` and a priority 120 as 16-bit integers; and a
  name of 16 bytes, one of 6 names in turn (`kworker/0:1`, `bash`, `systemd`, `sshd`, `kworker/1:2`,
  `cron`), padded with zeros.
- **Bytes 1600 to 2224, 625 bytes of text,** ten lines about zram.
- **Bytes 2225 to 4095, 1871 zero bytes.**

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 120" role="img" aria-label="The 4096 bytes of the made-up page: a 64-byte header, 48 records up to byte 1600, text up to byte 2225, zeros to the end">
  <g font-size="12">
    <rect x="20" y="30" width="10.6" height="40" class="fill-acc" stroke="currentColor"/>
    <rect x="30.6" y="30" width="255" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="285.6" y="30" width="103.8" height="40" class="fill-mark" stroke="currentColor"/>
    <rect x="389.4" y="30" width="310.6" height="40" fill="none" stroke="currentColor"/>
    <text x="20" y="22">header</text>
    <text x="158" y="55" text-anchor="middle">48 records of 32 bytes</text>
    <text x="337.5" y="55" text-anchor="middle">text</text>
    <text x="544.7" y="55" text-anchor="middle">zeros</text>
    <text x="20" y="88" text-anchor="middle">0</text>
    <text x="34" y="104" text-anchor="middle">64</text>
    <text x="285.6" y="88" text-anchor="middle">1600</text>
    <text x="389.4" y="88" text-anchor="middle">2225</text>
    <text x="700" y="88" text-anchor="middle">4096</text>
  </g>
</svg>
</div>
<figcaption>The made-up page, drawn to scale: header, records, text and zeros.</figcaption>
</figure>

Each part tests something else. The zeros after the header and at the end give long matches at a
small offset, which need a length value. The records repeat with a period of 32 bytes, so their
matches have offsets that are multiples of 8: classes 4 and 5, and the repeat offset. The text has
few repeats and many literals, and its letters are the kind of bytes the trained literal tables code
in 4 to 6 bits, so the page passes the rule for coded literals.

## The tables: seqlz_tables_init() {#ch-trace--tables}

Before any page, the tables are built: in the kernel once at boot by `seqlz_init()`, in the harness
once per device, in our program with one call:

```c
struct seqlz_tables *t = malloc(seqlz_tables_size());   /* 43 280 bytes */
seqlz_tables_init(t, &seqlz_default_own);
seqlz_all_symbols(t);                                    /* true */
```

`seqlz_tables_init()` ([chapter 8](#ch-codes)) turns the code lengths compiled into
`seqlz_default_own` into canonical codes for the encoder and decode tables for the decoder. A few
values we will meet on this page:

| table | symbol | code length | code |
| --- | --- | ---: | --- |
| token | escape (3072) | 4 | `0011` |
| token | 34: `ll` 2, `ml` 6, class 0 | 4 | |
| token | 0: `ll` 0, `ml` 4, class 0, also the last sequence without literals | 8 | |
| literal table 0 | `e` | 4 | `0001` |
| literal table 0 | space | 4 | |
| literal table 0 | `t`, `a`, `o`, `s` | 5 | |
| literal table 0 | `0x00` | 8 | |
| literal table 0 | `0x40`, `0x80`, `0xc0`, `0xff` | 10 | |

576 of the 3072 tokens have a code of their own; the others are sent as the escape's code followed by
the token in 12 bits ([chapter 4](#ch-format--token)). Literal table 0 is the one that codes text
best; we will see the encoder pick it.

## seqlz_compress(), and the matcher's first sequence {#ch-trace--matcher}

zram calls the backend, and the backend calls `seqlz_compress(t, st, page, dst, 8192, 1)`: the tables,
the per-CPU `struct seqlz_state` of 8192 bytes, the page, an output buffer of 2 pages, and `coded = 1`
for `seqlz-fast-lit`. `seqlz_compress()` first calls `compress_page()`
([chapter 10](#ch-encoder)), which checks that `dst_cap` is at least `SEQLZ_HEADER + ENC_ROOM`, 34
bytes, and that the tables have every symbol, and then sets up the encoder with `encoder_init()`:

- `e.p = dst + 8192`, the end of the buffer: the bitstream grows downward from there;
- `e.lit = dst + 2`, right after the u16: the literals grow upward from there;
- `e.src_end = page + 4096`, so that 16-byte copies of literals never read past the page;
- `e.last = 1`, the repeat offset at the start of every page.

Then `seqlz_match_page()` ([chapter 9](#ch-matcher)) runs over the page, with `encode_emit()` as the
function it calls for every sequence. It clears its hash table, 4096 entries of 2 bytes, with one
`memset()` of 8192 bytes, and starts at position 1 with `anchor = 0`, `last = 1` and `back = -1`.

At positions 1 to 13 nothing matches. Each position compares 4 bytes against two candidates: the
bytes one position back, for the repeat offset 1, and the bytes at the position the hash table holds
for the hash of the next 5 bytes. The table starts as all zeros, so the second candidate is position
0, `"TASK"`, until a position with the same hash was stored. Each position is stored in the table
before the test.

At position 14 the repeat offset hits: bytes 13 to 16 and bytes 14 to 17 are all zero. Byte 12 is
`0x20`, the low byte of the record size, so the backward extension stops at once: `src[13]` is `0x00`
and `src[12]` is not. `seqlz_match_len()` then counts how far the match goes: the zeros reach byte 63,
and byte 64 is `0x60`, the low byte of the first record's pointer. So the matcher emits its first
sequence: 14 literals (`TASKSLAB`, `30 00 00 00`, `20 00`), then a match of 50 bytes at offset 1. It
moves `pos` to 64, sets `anchor` to 64, and stores position 62, 2 bytes before the match's end, in
the hash table.

## encode_emit(): the first sequence in bits {#ch-trace--emit}

`encode_emit()` ([chapter 10](#ch-encoder)) gets `ll = 14`, `ml = 50`, `off = 1`.

1. **Room.** `e.p - e.lit` is 8190 bytes, more than `14 + ENC_ROOM`, so the page still fits.
2. **Literals.** The page has more than 16 bytes from `in` on, so one 16-byte copy writes the 14
   literals and 2 bytes more to `dst + 2`; the next sequence's literals overwrite those 2. `e.lit`
   moves to `dst + 16`.
3. **Offset class.** `seqlz_off_class(1, 1, &raw_bits)`: `off == last`, so `is_new` is
   `(1 == 1) - 1 = 0`, and the class, masked with it, is 0, with 0 offset bits
   ([chapter 6](#ch-header)).
4. **Token.** `seqlz_token(14, 50, 0)` is `14 + 16 * min(46, 31) + 512 * 0 = 510`. Token 510 has no
   code (`enc[510]` is 0), so `token_code()` returns the escape: code `0011` shifted left by 12, or'ed
   with 510, `0x31fe`, 16 bits.
5. **Token and offset in one put.** `enc_put(e, 0x31fe << 0 | (1 & 0), 16)`. For class 0 the offset is
   masked to 0 by `0U - (cls != 0)`, which is 0 here.
6. **Length value.** `ml - 4 = 46` is at least 31, so `put_len_value()` writes `46 - 31 = 15` with the
   `ml` table: values below 16 are their own symbol, and symbol 15 has the code `1111110`, 7 bits.
7. **Flush.** The accumulator holds 23 bits, `0011 000111111110 1111110`. `enc_flush()` shifts them to
   the top of a 64-bit word and stores it little endian at `e.p - 8`, so the top byte, `0x31`, lands
   at `dst + 8191`, the next, `0xfe`, at `dst + 8190`. `e.p` moves down by 2 whole bytes, and the 7
   bits left over stay in the accumulator for the next sequence.
8. `e.last = 1`.

The next two sequences are the first record and the start of the second. The trace program prints
the bits of all three:

```text
seq 0: escape code 0011, token 000111111110, ml value 15: symbol 15, code 1111110
seq 1: token code 1100000100, offset bits 01001101, ll value 12: symbol 12, code 111010
seq 2: token code 101100110, offset bits 00100
```

Sequence 1 starts the first record: 27 literals (the pointer, pid, state, priority, and the 11
letters of `kworker/0:1`), then 6 zero bytes, the rest of the name's padding and the next record's
first byte, at offset 77, which points into the header's zeros. 77 is not a multiple of 8 and below
256, so class 2 with 8 offset bits, `01001101`. `ll = 27` is above the token's cap of 15, so the
token holds 15 and a length value of 12 follows, symbol 12 in the `ll` table, 6 bits. The token is
`15 + 16 * 2 + 512 * 2 = 1071`, with a code of 10 bits. Sequence 2 is one literal and 6 bytes at
offset 32, the record before: 32 is a multiple of 8 from 16 on, so class 4, and the offset goes as
`32 / 8 = 4` in 5 bits, `00100`.

## All 180 sequences {#ch-trace--sequences}

The matcher finds 180 sequences. The first 14, and the last 3, with the bits each takes in the
bitstream: the token's code (16 with `e` for an escaped one), the offset bits, the length values'
bits, and the sum.

| # | page position | `ll` | `ml` | offset | class | token | code | offset bits | length values | bits |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 14 | 50 | 1 | 0 | 510 | 16 e | 0 | 7 | 23 |
| 1 | 64 | 27 | 6 | 77 | 2 | 1071 | 10 | 8 | 6 | 24 |
| 2 | 97 | 1 | 6 | 32 | 4 | 2081 | 9 | 5 | 0 | 14 |
| 3 | 104 | 12 | 6 | 25 | 2 | 1068 | 16 e | 8 | 0 | 24 |
| 4 | 122 | 0 | 6 | 2 | 1 | 544 | 9 | 4 | 0 | 13 |
| 5 | 128 | 1 | 7 | 32 | 4 | 2097 | 4 | 5 | 0 | 9 |
| 6 | 136 | 15 | 9 | 32 | 0 | 95 | 11 | 0 | 4 | 15 |
| 7 | 160 | 2 | 6 | 32 | 0 | 34 | 4 | 0 | 0 | 4 |
| 8 | 168 | 1 | 7 | 96 | 4 | 2097 | 4 | 5 | 0 | 9 |
| 9 | 176 | 3 | 10 | 29 | 2 | 1123 | 16 e | 8 | 0 | 24 |
| 10 | 189 | 4 | 7 | 32 | 4 | 2100 | 11 | 5 | 0 | 16 |
| 11 | 200 | 1 | 7 | 96 | 4 | 2097 | 4 | 5 | 0 | 9 |
| 12 | 208 | 0 | 8 | 128 | 4 | 2112 | 5 | 5 | 0 | 10 |
| 13 | 216 | 3 | 5 | 128 | 0 | 19 | 7 | 0 | 0 | 7 |
| ... | | | | | | | | | | |
| 177 | 2224 | 1 | 7 | 1976 | 5 | 2609 | 6 | 9 | 0 | 15 |
| 178 | 2232 | 0 | 1864 | 2 | 1 | 1008 | 7 | 4 | 16 | 27 |
| 179 | 4096 | 0 | 0 | | | 0 | 8 | 0 | 0 | 8 |

Some rows worth reading closely:

- **Sequence 7 is the cheapest kind**: 2 literals, a match of 6 at the same offset as the sequence
  before, 32. Class 0 needs no offset bits, and token 34 has a code of 4 bits. 8 bytes of the page in
  4 bits of bitstream and 2 literals.
- **Sequence 6 is class 0 with a length value**: `ll = 15` is exactly the cap, so a value of 0 follows,
  4 bits in the `ll` table. A token holds `ll` 0 to 14 in full; 15 means "15 or more".
- **Sequences 5, 8 and 11 are the same token, 2097**: `ll` 1, `ml` 7, class 4, a code of 4 bits plus
  5 offset bits. One literal byte that differs, then 7 bytes that repeat, at a multiple of 8.
  Sequence 5 is record 2's pointer: its low byte goes as a literal, and the 7 bytes above it match
  record 1's pointer, 32 bytes back. Pointers into the same few KiB of memory share their top bytes,
  and that is common in real pages too. Sequences 8 and 11 are a pid: its low byte goes as a literal,
  and its 3 high bytes, the state and the priority match the record 3 places back, 96 bytes, which
  has the same state `i % 3`.
- **Sequences 12 and 13** show the greedy matcher at work. Record 4's name is `kworker/1:2`. The hash
  table's candidate for `kwork` is record 0, 128 bytes back, whose name is `kworker/0:1`, so the
  match covers `kworker/`, 8 bytes, and ends at the `1` that differs. Then 3 literals and the zeros
  of the padding, again at offset 128, now as the repeat offset.
- **Sequence 177** is class 5: the 7 zero bytes after the text's last newline match a record's
  padding 1976 bytes back. `1976 = 8 * 247`, so it goes as 247 in 9 bits.
- **Sequence 178** covers the 1864 zeros up to the end of the page, at offset 2. Offset 2 and not 1,
  because offset 1 is not a candidate: the repeat offset is now 1976, from sequence 177, and the 4
  bytes 1976 back are a record's pointer, not zeros. The hash table's candidate is position 2230,
  stored as "2 bytes before the end" of sequence 177's match, and its 5 bytes are zeros like those at
  2232. Offset 2 is class 1, 4 bits. The length value is `1864 - 4 - 31 = 1829`. That is above 15,
  so it goes as its highest bit's position: 1829 is 11 bits long, its top bit is bit 10, so the
  symbol is `12 + 10 = 22`, with a 6-bit code in the `ml` table, followed by the 10 bits below the
  top one, `1829 - 1024 = 805`. 16 bits for the length, 27 for the whole sequence, for 1864 bytes of
  the page.
- **Sequence 179** is the last one. The match before ends exactly at byte 4096, so it has no literals
  and no match: token 0, `ll` 0, `ml - 4 = 0`, class 0, 8 bits. The decoder knows it is the last
  because the page is full after its literals.

The whole page, summed by the program:

```text
literals 674, bitstream 2168 bits = 271 bytes
matches per class: 0:27 1:3 2:21 3:2 4:96 5:30, escapes 4, length values 16
```

179 matches cover 3422 bytes, the 674 literals the other 674. 96 of the 179 matches are class 4, a
multiple of 8 below 256: the records' period of 32 shows up as offsets 32, 64, 96, 128 and 192. 30
more are class 5, multiples of 8 from 256 on, mostly 288, 9 records back; 25 of them are the 7 high
bytes of a pointer. Only 2 matches needed the 12 offset bits of class 3. Of the 2168 bits, 1109 are
token codes, 954 offset bits and 105 length values. By part of the page:

| part | sequences | literals | bits |
| --- | ---: | ---: | ---: |
| header, bytes 0 to 63 | 1 | 14 | 23 |
| records, 64 to 1599 | 155 | 167 | 1635 |
| text, 1600 to 2224 | 22 | 493 | 475 |
| zeros, 2225 to 4095 | 2 | 0 | 35 |

The records take three quarters of the bitstream, about 10.5 bits per sequence, and leave few
literals. The text is the other way round: 22 sequences, but 493 of the 674 literals.

::: why One position per hash, on this page
The names `kworker/0:1` and `kworker/1:2` share their first 8 bytes. Record 30, at byte 1024, is named
`kworker/0:1`, like record 24 six places before it, so a match at offset 192 would cover the whole
16-byte name in one sequence: token 2240 and 5 offset bits, 12 bits, as sequence 24 does for `bash`.
But the hash table keeps one position per hash, the newest, and for `kwork` that is record 28,
`kworker/1:2`, 64 bytes back. So the matcher emits 8 bytes at offset 64 (sequence 98) and then 8 at
offset 192 (sequence 99), 20 bits. The project measured a table that keeps the 2 newest positions per
hash in the same 8 KiB and takes the longer match: 11 bytes less per page on its two dumps in the
zsmalloc model, for 4400 to 4700 more compression cycles, about 1 µs per page written. That is about
11 bytes per µs, at the edge of what the score accepts over both dumps, and it was dropped, without a
run in the kernel VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)).
:::

## encoder_finish(): 947 bytes with raw literals {#ch-trace--finish}

When the matcher is done, `compress_page()` calls `encoder_finish()` ([chapter 10](#ch-encoder)).
At this point the literals fill `dst + 2` to `dst + 676`, and the bitstream fills the last 271 bytes
of the 8192-byte buffer, backwards. 2168 is a multiple of 8, so after the last sequence's flush no
bits are left in the accumulator, and `e.cnt` is 0: no last partial byte to write.

`encoder_finish()` writes the literal count, 674, as the u16 at the start, `a2 02`. Then it copies the
bitstream from the end of the buffer to right after the literals. The gap between them is
`(8192 - 271) - 676 = 7245` bytes, much more than 271, so the copy cannot overlap: 33 copies of 8
bytes, each a little-endian load from the back and a big-endian store to the front, which reverses
the byte order, and 7 single bytes. The bitstream now starts at byte 676 with `31 fe fd 82`, and the
function returns `2 + 674 + 271 = 947`.

That is the page as `seqlz-fast` stores it, 23.1% of 4096 bytes. `seqlz_compress()` was called with
`coded = 1`, so it now hands the 947 bytes to `code_literals()`.

## code_literals(): 151 bytes less {#ch-trace--literals}

`code_literals()` ([chapter 11](#ch-literals-enc)) reads the count, 674, from the u16 and computes the
bitstream's length, `947 - 2 - 674 = 271`. Then `lit_cost()` sums, for every stream and every one of
the 8 literal tables, the code lengths of that stream's literals. It works on the literals in chunks
of `8 * 25 = 200`, three full ones and one of 74, each chunk summed in 8-bit lanes of one u64 per
stream and then moved into 16-bit lanes before a lane can overflow. The results, as the trace program
recomputes them one literal at a time:

| literal table | bits | bytes in 8 streams |
| ---: | ---: | ---: |
| 0 | 4089 | 515 |
| 1 | 5512 | 691 |
| 2 | 5975 | 751 |
| 3 | 4328 | 545 |
| 4 | 5538 | 694 |
| 5 | 6340 | 797 |
| 6 | 5896 | 740 |
| 7 | 6080 | 763 |

Table 0 wins, at 4089 bits, 6.07 bits per literal. The text's 493 literals take 5.26 bits each in it,
the records' 167 take 8.34: pointer and pid bytes are close to random, and table 0 gives every byte
value above `0x7f` 10 bits. Each stream's bits are rounded up to whole bytes: the 8 streams take 66,
59, 65, 64, 64, 65, 66 and 66 bytes, 515 together.

The width of the stream sizes is the bits the largest needs, at least 5: the OR of the 8 sizes and 16
is `123`, `0b1111011`, so `32 - __builtin_clz(123) = 7` bits each, and the header is `3 + 7 = 10`
bytes.

Then the rule. Coded literals are kept only if `coded + 51 < n - n / 16`:
`515 + 51 = 566` against `674 - 42 = 632`. 566 is less, so the literals get coded. Had the page
carried only the records and no text, they would not have been: the trace program, run without the
text, finds 181 literals, which table 3 codes best, in 162 bytes, and `162 + 51 = 213` is not below
`181 - 11 = 170`.

What follows is the write. `code_literals()` copies the 674 raw literals into the matcher's hash
table, `st->literals`, which nobody needs any more, because the coded streams are about to be written
over the raw literals in `dst`. It computes where each stream starts, `dst + 10`, `+ 76`, `+ 135`,
and so on, and `lit_streams()` writes literal `k` into stream `k % 8`, 4 streams at a time with an
accumulator each, flushing every 32 literals. Stream 0 and 1 get 85 literals, the other six 84. Then:

- the u16 becomes `0x8000 | 674 = 0x82a2`, bytes `a2 82`;
- byte 2 becomes `0 | (7 - 5) << 3 = 0x10`: table 0, width 7;
- `lit_sizes()` writes the 8 sizes of 7 bits each, lowest bit first, into bytes 3 to 9:
  `c2 5d 10 08 0c 0a 85`;
- `memmove()` moves the 271 bytes of bitstream from byte 676 down to byte 525.

The function returns `525 + 271 = 796`. `seqlz_compress()` returns that, and the backend tells zram
the page is 796 bytes.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 230" role="img" aria-label="The 796 bytes of the compressed page: a 10-byte header, 8 literal streams from byte 10 to 525, the bitstream of the sequences from 525 to 796, with the header's bytes shown enlarged below">
  <g font-size="11">
    <rect x="20" y="40" width="8.5" height="40" class="fill-acc" stroke="currentColor"/>
    <rect x="28.5" y="40" width="56.4" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="84.9" y="40" width="50.4" height="40" class="fill-paper" stroke="currentColor"/>
    <rect x="135.3" y="40" width="55.5" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="190.9" y="40" width="54.7" height="40" class="fill-paper" stroke="currentColor"/>
    <rect x="245.5" y="40" width="54.7" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="300.2" y="40" width="55.5" height="40" class="fill-paper" stroke="currentColor"/>
    <rect x="355.7" y="40" width="56.4" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="412.1" y="40" width="56.4" height="40" class="fill-paper" stroke="currentColor"/>
    <rect x="468.5" y="40" width="231.5" height="40" class="fill-mark" stroke="currentColor"/>
    <text x="56.7" y="64" text-anchor="middle">s0</text>
    <text x="110.1" y="64" text-anchor="middle">s1</text>
    <text x="163.1" y="64" text-anchor="middle">s2</text>
    <text x="218.2" y="64" text-anchor="middle">s3</text>
    <text x="272.9" y="64" text-anchor="middle">s4</text>
    <text x="328" y="64" text-anchor="middle">s5</text>
    <text x="383.9" y="64" text-anchor="middle">s6</text>
    <text x="440.3" y="64" text-anchor="middle">s7</text>
    <text x="584.3" y="58" text-anchor="middle">bitstream of the sequences</text>
    <text x="584.3" y="72" text-anchor="middle">271 bytes, MSB first</text>
    <text x="248" y="30" text-anchor="middle">8 literal streams, 515 bytes, table 0</text>
    <text x="20" y="96" text-anchor="middle">0</text>
    <text x="30" y="110" text-anchor="middle">10</text>
    <text x="84.9" y="96" text-anchor="middle">76</text>
    <text x="135.3" y="110" text-anchor="middle">135</text>
    <text x="190.9" y="96" text-anchor="middle">200</text>
    <text x="245.5" y="110" text-anchor="middle">264</text>
    <text x="300.2" y="96" text-anchor="middle">328</text>
    <text x="355.7" y="110" text-anchor="middle">393</text>
    <text x="412.1" y="96" text-anchor="middle">459</text>
    <text x="468.5" y="110" text-anchor="middle">525</text>
    <text x="700" y="96" text-anchor="middle">796</text>
    <line x1="20" y1="80" x2="20" y2="160" stroke="currentColor" stroke-dasharray="3 3"/>
    <line x1="28.5" y1="80" x2="420" y2="160" stroke="currentColor" stroke-dasharray="3 3"/>
    <rect x="20" y="160" width="80" height="34" class="fill-acc" stroke="currentColor"/>
    <rect x="100" y="160" width="40" height="34" class="fill-acc" stroke="currentColor"/>
    <rect x="140" y="160" width="280" height="34" class="fill-acc" stroke="currentColor"/>
    <text x="60" y="181" text-anchor="middle">a2 82</text>
    <text x="120" y="181" text-anchor="middle">10</text>
    <text x="280" y="181" text-anchor="middle">c2 5d 10 08 0c 0a 85</text>
    <text x="60" y="212" text-anchor="middle">u16: coded, 674</text>
    <text x="120" y="212" text-anchor="middle">table 0, w 7</text>
    <text x="280" y="212" text-anchor="middle">sizes 66 59 65 64 64 65 66 66, 7 bits each</text>
    <text x="20" y="154">bytes 0 to 9, the header</text>
  </g>
</svg>
</div>
<figcaption>The compressed page, 796 bytes, drawn to scale; the 10-byte header enlarged below.</figcaption>
</figure>

## How much memory the page takes in zram {#ch-trace--zsmalloc}

zram does not store 796 bytes. It asks zsmalloc for an object of that size, and zsmalloc rounds it
up to a size class ([chapter 1](#ch-zram)). The project models this in `bench/zsmalloc_cost.cpp`,
a port of the kernel's class computation; summed over a new device's pages it matched zram's own
`mem_used_total` to the byte in the kernel VM. For 4 KiB pages:

1. zsmalloc adds a handle of 8 bytes: `796 + 8 = 804`.
2. Classes are 16 bytes apart (`4096 >> 8`) from 32 bytes on, so the class index is
   `ceil((804 - 32) / 16) = ceil(48.25) = 49`, and the class size `32 + 49 * 16 = 816` bytes.
3. A class keeps its objects in a "zspage" of 1 to 8 pages, the number with the least waste at the end.
   For 816 that is 1 page with 5 objects, since `4096 / 816 = 5.02`. Classes with the same pages and
   objects per zspage are merged into the larger: class 800 also has 1 page and 5 objects, so it is
   merged into 816, and no larger class shares 816's.
4. So each object costs `4096 / 5 = 819.2` bytes of memory.

The same page with raw literals, `seqlz-fast`'s 947 bytes, lands in class 960 (`955` rounded up), 4
pages holding 17 objects, `16 384 / 17 = 963.8` bytes per page. Coding the literals saved 151 bytes of
compressed size and 144.6 bytes of zsmalloc memory. The page could grow by 12 bytes and stay in class
816 (up to `808 + 8 = 816`); it would have to shrink by 20 bytes, to 776, to reach the next smaller
class, 784, because class 800 is part of 816.

## seqlz_decode(): the literals first {#ch-trace--decode}

Some time later the page is read. zram calls the backend's decompress with the 796 bytes, and the
backend calls `seqlz_decode(t, src, 796, dst, scratch)` with the per-CPU scratch of 4112 bytes
([chapter 14](#ch-decode)).

`src_len` is at least 2, so the decoder first prefetches its tables: the token table's 8192 bytes in
16 rounds of 8 cache lines, and the 1024 bytes of each length table in 2 rounds. Then it reads the
u16, `0x82a2`. The top bit is set, so the literals are coded: `n_lit = 0x02a2 = 674`, the scratch is
not `NULL`, and `decode_literals()` runs ([chapter 13](#ch-literals-dec)).

`decode_literals()` reads byte 2, `0x10`: width `5 + ((0x10 >> 3) & 7) = 7`, table `0x10 & 7 = 0`,
and bits 6 and 7 are zero as they must be. The streams start at byte `3 + 7 = 10`. It prefetches
literal table 0's 2048 bytes of decode table, 32 lines, and `lit_stream_sizes()` copies the 7 bytes of
sizes out and takes 7 bits at a time: 66, 59, 65, 64, 64, 65, 66, 66, starts 10, 76, 135, 200, 264,
328, 393, 459, total 515. `10 + 515 = 525` is within the 796 bytes.

The 8 streams are then decoded side by side, `SEQLZ_LIT_ROUNDS = 56 / 10 = 5` literals per stream
after each refill, 40 literals per round. 674 literals are 16 full rounds, 640 literals. The rest, 34,
is `34 / 8 = 4` steps of all 8 streams and then one more step in which only `34 % 8 = 2` streams,
0 and 1, take their literal. Streams 2 to 7 decode a literal in that step too, but with a mask of 0
they do not move on; what they decode lands in `scratch[674]` to `scratch[679]`, after the last
literal, where nothing reads it. On a page of 4096 literals those bytes are past the page, which is
one reason the scratch has 16 bytes more than a page. Finally `lit_stream_ends()` checks for each stream that it ended in
its last byte and that the bits after its last code are 0. All 8 do, and `decode_literals()` returns
`src + 525`, where the bitstream starts.

The decoder sets up the bit reader at byte 525, `lit` at the scratch, `lit_end` at `scratch + 674`,
`lit_bound` at `scratch + 4112`, and the limits of the fast path: `d_fast` 64 bytes before the end
of the page, `lit_fast` 16 bytes before `lit_bound`, `in_fast` 8 bytes before the end of the input.

## The loop over the sequences {#ch-trace--loop}

**The first token.** `next_token()` finds `count = 0`, less than `11 + 12`, and refills: the 8 bytes
at 525 loaded big endian, `p` moved on by 7 bytes, `count` set to 56. The top 11 bits of `31 fe fd
82 ...` are `00110001111`, 399. Entry 399 of the token table is `0x80010000`: bit 31 says escape, and
bits 14 to 18 hold 4, the escape code's length. All 128 entries whose index starts with `0011` hold
this value, which is how one lookup finds a 4-bit code in an 11-bit table.

**The escape.** The loop drops the 4 bits, refills, reads the next 12 bits, `000111111110`, 510, and
drops them. 510 is below 3072, so it is a valid token, and `token_entry(510, 0)` builds its entry on
the spot, `0x51f00000`: `ll` 14, `ml` 35, the length-value bit set, class 0 so `back` 0, and 0 bits
to drop, since the token's bits are already gone.

**Sequence 0, slow path.** `back` is 0, so `off = last = 1`. The entry has the length-value bit, so
the fast path is out. `nl = 14` is not 15, so no `ll` value. The checks pass: `lit` is not past
`lit_end`, 14 literals are left and fit in the page. `copy_literals()` copies them with one 16-byte
copy, and `d` and `lit` move on by 14. The page is not full yet. `len` is 35, `SEQLZ_ML_CAP + 4`, so a
length value follows: refill, look up the next 8 bits in the `ml` table, `1111110` is symbol 15, base
15, no extra bits, and `len` becomes `35 + 15 = 50`. `off - 1 = 0` is less than the 14 bytes written,
and 50 bytes fit. `copy_match()` with offset 1 builds the 8-byte pattern from `dst[13]`, a zero byte
repeated 8 times by `repeat_pattern()`, and stores it every `repeat_step[1] = 8` bytes until 50 are
covered, 56 bytes written in 7 stores. `d` is now at 64, and `next_token()` loads sequence 1's entry.

**Sequence 1, slow path again.** The token's code is 10 bits, so its entry fills the 2 indices
`1100000100x`. It says `ll` 15 with a value to follow, `ml` 6, 8 offset bits. The offset is cut out of
the bits right after the code with two shifts, `(bits << 10) >> 56`, which is 77. The `ll` value adds
12, so 27 literals are copied, with two 16-byte copies, and the 6 bytes at offset 77 with two 8-byte
copies.

**Sequence 2, fast path.** Token 2081 has no length value, `d` at 97 is far from the end, the scratch
has room, and the input has more than 8 bytes left. So the fast path runs: `next_token_fast()` refills
without checking `count`, because `skip` is 0 after a slow sequence, and loads sequence 3's entry
before anything is copied. Then one 16-byte copy for the 1 literal, and `copy_match_fast()` with
offset `4 << 3 = 32`, two 8-byte copies for the 6 bytes. No compare of `nl` against the literals left,
no check of `len` against the room: the conditions of the fast path cover both.

The rest of the page goes the same way. The program counts the paths with a copy of the decoder that
has two counters added:

```text
round trip ok, 162 sequences on the fast path, 18 on the slow path
```

The 18 slow ones are the 16 sequences with a length value (14 for `ll`, 2 for `ml`); sequence 177,
whose bit reader has reached the last 8 bytes of the input, so that a refill without a check could
read past it; and sequence 179, the last, whose literals end the page.

**The end.** Sequence 179's token is 0: `ll` 0, `ml` 4, class 0. There are no literals left to copy,
`d` is at the end of the page, so the loop checks that this is a valid last sequence, `len == 4` and
`TOK_BACK(tok) == 0`, and leaves. Then the checks that make a page valid only if it ends where its
bits end ([chapter 4](#ch-format--validity)): `d` is at the end of the page, `lit` at `lit_end`, all
674 literals used; the bit reader has read every byte, `p == end`; and `count`, the bits left, is 0,
because 2168 bits are exactly 271 bytes. `seqlz_decode()` returns 0, and the 4096 bytes in `dst` are
the page we started with.

Without the scratch, the same 796 bytes are invalid: `seqlz_decode(t, src, 796, dst, NULL)` returns
`-EINVAL` as soon as it sees the top bit of the u16. That is why `seqlz-fast-lit` has a decompression
context and `seqlz-fast` does not ([chapter 15](#ch-zram-glue)).

## The program {#ch-trace--program}

`trace.c` builds the page, runs `seqlz_find()` for the sequences (the same ones the compressor finds;
a test in `test/seqlz_test.cpp` checks that its output fed to `seqlz_encode()` gives the compressor's
bytes), computes each sequence's bits from the tables' `enc[]` arrays, compresses the page with raw
and with coded literals, and decodes it. It includes `seqlz_internal.h` to read the tables' codes, so
it depends on the layout at commit `a2d57dd`.

Its whole output comes first, with gcc 16.2.1 on x86-64; the compressed bytes are the same on every CPU,
so the output is too:

```text
180 sequences
  # |  pos |  ll |   ml |  off | cl | token | code | off | values | bits
seq 0: escape code 0011, token 000111111110, ml value 15: symbol 15, code 1111110
  0 |    0 |  14 |   50 |    1 |  0 |   510 | 16 e |   0 |      7 |   23
seq 1: token code 1100000100, offset bits 01001101, ll value 12: symbol 12, code 111010
  1 |   64 |  27 |    6 |   77 |  2 |  1071 | 10   |   8 |      6 |   24
seq 2: token code 101100110, offset bits 00100
  2 |   97 |   1 |    6 |   32 |  4 |  2081 |  9   |   5 |      0 |   14
  3 |  104 |  12 |    6 |   25 |  2 |  1068 | 16 e |   8 |      0 |   24
  4 |  122 |   0 |    6 |    2 |  1 |   544 |  9   |   4 |      0 |   13
  5 |  128 |   1 |    7 |   32 |  4 |  2097 |  4   |   5 |      0 |    9
  6 |  136 |  15 |    9 |   32 |  0 |    95 | 11   |   0 |      4 |   15
  7 |  160 |   2 |    6 |   32 |  0 |    34 |  4   |   0 |      0 |    4
  8 |  168 |   1 |    7 |   96 |  4 |  2097 |  4   |   5 |      0 |    9
  9 |  176 |   3 |   10 |   29 |  2 |  1123 | 16 e |   8 |      0 |   24
 10 |  189 |   4 |    7 |   32 |  4 |  2100 | 11   |   5 |      0 |   16
 11 |  200 |   1 |    7 |   96 |  4 |  2097 |  4   |   5 |      0 |    9
 12 |  208 |   0 |    8 |  128 |  4 |  2112 |  5   |   5 |      0 |   10
 13 |  216 |   3 |    5 |  128 |  0 |    19 |  7   |   0 |      0 |    7
...
177 | 2224 |   1 |    7 | 1976 |  5 |  2609 |  6   |   9 |      0 |   15
178 | 2232 |   0 | 1864 |    2 |  1 |  1008 |  7   |   4 |     16 |   27
179 | 4096 |   0 |    0 |    0 |  - |     0 |  8   |   0 |      0 |    8
literals 674, bitstream 2168 bits = 271 bytes
matches per class: 0:27 1:3 2:21 3:2 4:96 5:30, escapes 4, length values 16
raw literals: 947 bytes = 2 + 674 + 271
literal table 0: 4089 bits, 515 bytes in 8 streams
literal table 1: 5512 bits, 691 bytes in 8 streams
literal table 2: 5975 bits, 751 bytes in 8 streams
literal table 3: 4328 bits, 545 bytes in 8 streams
literal table 4: 5538 bits, 694 bytes in 8 streams
literal table 5: 6340 bits, 797 bytes in 8 streams
literal table 6: 5896 bits, 740 bytes in 8 streams
literal table 7: 6080 bits, 763 bytes in 8 streams
fewest bits: table 0
coded literals: 796 bytes, u16 0x82a2
byte 2 = 0x10: table 0, w 7, header 10 bytes, sizes 66 59 65 64 64 65 66 66 = 515
bitstream from byte 525, 271 bytes
first 16 bytes: a2 82 10 c2 5d 10 08 0c 0a 85 ad 1f a3 bc 44 8c
first 4 bytes of the bitstream: 31 fe fd 82
round trip ok, 162 sequences on the fast path, 18 on the slow path
```

The program itself:

```c
// trace.c: one made-up page through seqlz, with the numbers of each step
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz_internal.h"

#define N_REC 48 /* records of 32 bytes from byte 64 on */

/* from dec_counted.c, seqlz_decompress.c with two counters */
extern unsigned int n_fast, n_slow;
int seqlz_decode_counted(const struct seqlz_tables *t, const void *src,
			 unsigned int src_len, void *dst, void *scratch);

/* n bytes of v, little endian */
static void le(u8 *p, unsigned long long v, unsigned int n)
{
	while (n--) {
		*p++ = (u8)v;
		v >>= 8;
	}
}

/* a slab of 48 task records, a text after them, zeros up to the end */
static void make_page(u8 *p)
{
	static const char *const names[] = { "kworker/0:1", "bash", "systemd",
					     "sshd", "kworker/1:2", "cron" };
	static const char *const text =
		"zram stores each page in memory, compressed on its own. A page\n"
		"that comes back is decompressed while the program waits for it,\n"
		"so reading must be quick. Writing happens when memory runs low.\n"
		"Most pages hold pointers, small numbers and short strings, with\n"
		"many zero bytes between them. The compressor finds the bytes\n"
		"that repeat and writes a copy instead; the other bytes stay as\n"
		"literals. Text like this has few repeats but uses few letters,\n"
		"and a Huffman code gives the frequent ones short codes: e, t,\n"
		"a, o and the space take four or five bits, rare ones up to ten.\n"
		"Every table is fixed, so a page carries none of its own.\n";
	unsigned int i;

	memset(p, 0, SEQLZ_PAGE);
	memcpy(p, "TASKSLAB", 8);
	le(p + 8, N_REC, 4);
	le(p + 12, 32, 4);
	for (i = 0; i < N_REC; i++) {
		u8 *r = p + 64 + 32 * i;

		/* next: a kernel pointer to another record, 8-aligned */
		le(r, 0xffff888104a3c040ULL + 32 * ((i * 5 + 1) % N_REC), 8);
		le(r + 8, 1000 + 3 * i, 4); /* pid */
		le(r + 12, i % 3, 2); /* state */
		le(r + 14, 120, 2); /* priority */
		strcpy((char *)r + 16, names[i % 6]); /* name, zero padded */
	}
	memcpy(p + 64 + 32 * N_REC, text, strlen(text));
}

/* the n low bits of v as 0 and 1 */
static const char *binary(unsigned int v, unsigned int n)
{
	static char buf[4][40];
	static unsigned int k;
	char *b = buf[k++ % 4];
	unsigned int i;

	for (i = 0; i < n; i++)
		b[i] = (char)('0' + (v >> (n - 1 - i) & 1U));
	b[n] = 0;
	return b;
}

/* the bits of a length value: its symbol's code and the extra bits */
static unsigned int value_bits(const struct value_table *vt, unsigned int v,
			       int show)
{
	unsigned int x, s = seqlz_len_symbol(v, &x);
	unsigned int e = vt->enc[s];

	if (show)
		printf(" value %u: symbol %u, code %s", v, s,
		       binary(e & 0xffff, e >> 16 & 15));
	if (show && x)
		printf(", extra bits %s", binary(v, x));
	return (e >> 16 & 15) + x;
}

int main(void)
{
	static u8 page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE], out[SEQLZ_PAGE],
		scratch[SEQLZ_SCRATCH];
	static struct seqlz_state st;
	static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
	struct seqlz_tables *t = malloc(seqlz_tables_size());
	unsigned int n, i, k, pos = 0, last = 1, total = 0, n_lit = 0;
	unsigned int per_class[SEQLZ_OFF_CLASSES] = { 0 }, escapes = 0;
	unsigned int values = 0, len0, len1, set = 0, best = ~0U;

	make_page(page);
	if (!t || seqlz_tables_init(t, &seqlz_default_own) ||
	    !seqlz_all_symbols(t))
		return 1;

	/* the sequences, and the bits each one takes in the bitstream */
	n = seqlz_find(&st, page, seq);
	printf("%u sequences\n", n);
	printf("  # |  pos |  ll |   ml |  off | cl | token | code | off | values | bits\n");
	for (i = 0; i < n; i++) {
		unsigned int ll = seq[i].literals, ml = seq[i].match;
		unsigned int off = seq[i].offset, cls = 0, raw = 0, vbits = 0;
		unsigned int tok, clen, te, b, show = i < 3;

		if (ml)
			cls = seqlz_off_class(off, last, &raw);
		tok = seqlz_token(ll, ml, cls);
		te = t->token.enc[tok];
		if (te) {
			clen = te >> TOK_ENC_LEN_AT;
			if (show)
				printf("seq %u: token code %s", i,
				       binary(te & TOK_ENC_CODE_MASK, clen));
		} else {
			unsigned int ee = t->token.enc[SEQLZ_ESCAPE];

			clen = (ee >> TOK_ENC_LEN_AT) + SEQLZ_ESCAPE_BITS;
			escapes++;
			if (show)
				printf("seq %u: escape code %s, token %s", i,
				       binary(ee & TOK_ENC_CODE_MASK,
					      ee >> TOK_ENC_LEN_AT),
				       binary(tok, SEQLZ_ESCAPE_BITS));
		}
		if (show && raw)
			printf(", offset bits %s",
			       binary(off >> SEQLZ_OFF_SHIFT(cls), raw));
		if (ll >= SEQLZ_LL_CAP) {
			if (show)
				printf(", ll");
			vbits += value_bits(&t->ll, ll - SEQLZ_LL_CAP, show);
			values++;
		}
		if (ml && ml - 4 >= SEQLZ_ML_CAP) {
			if (show)
				printf(", ml");
			vbits += value_bits(&t->ml, ml - 4 - SEQLZ_ML_CAP, show);
			values++;
		}
		if (show)
			printf("\n");
		b = clen + raw + vbits;
		total += b;
		n_lit += ll;
		if (ml)
			per_class[cls]++;
		if (i < 14 || i + 3 >= n)
			printf("%3u | %4u | %3u | %4u | %4u | %2c | %5u | %2u%s | %3u | %6u | %4u\n",
			       i, pos, ll, ml, ml ? off : 0,
			       ml ? (char)('0' + cls) : '-', tok, clen,
			       te ? "  " : " e", raw, vbits, b);
		else if (i == 14)
			printf("...\n");
		pos += ll + ml;
		if (ml)
			last = off;
	}
	printf("literals %u, bitstream %u bits = %u bytes\n", n_lit, total,
	       (total + 7) / 8);
	printf("matches per class:");
	for (i = 0; i < SEQLZ_OFF_CLASSES; i++)
		printf(" %u:%u", i, per_class[i]);
	printf(", escapes %u, length values %u\n", escapes, values);

	/* seqlz-fast: raw literals */
	len0 = seqlz_compress(t, &st, page, dst, sizeof dst, 0);
	printf("raw literals: %u bytes = 2 + %u + %u\n", len0, n_lit,
	       len0 - 2 - n_lit);
	if (seqlz_decode(t, dst, len0, out, NULL) ||
	    memcmp(out, page, SEQLZ_PAGE))
		return 2;

	/* the 8 literal tables as code_literals() weighs them */
	for (k = 0; k < SEQLZ_LIT_SETS; k++) {
		unsigned int bits = 0, bytes = 0, j, m;

		for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
			unsigned int sb = 0;

			for (m = j; m < n_lit; m += SEQLZ_LIT_STREAMS)
				sb += t->lit[k].enc[dst[SEQLZ_HEADER + m]] & 255U;
			bits += sb;
			bytes += (sb + 7) / 8;
		}
		printf("literal table %u: %u bits, %u bytes in 8 streams\n", k,
		       bits, bytes);
		if (bits < best) {
			best = bits;
			set = k;
		}
	}
	printf("fewest bits: table %u\n", set);

	/* seqlz-fast-lit: coded literals where that saves enough */
	len1 = seqlz_compress(t, &st, page, dst, sizeof dst, 1);
	printf("coded literals: %u bytes, u16 0x%04x\n", len1,
	       get_unaligned_le16(dst));
	if (get_unaligned_le16(dst) & SEQLZ_LIT_CODED) {
		unsigned int w = SEQLZ_SIZE_BITS_MIN + (dst[2] >> 3 & 7);
		unsigned int sum = 0, cnt = 0, at = 3;
		unsigned long long acc = 0;

		printf("byte 2 = 0x%02x: table %u, w %u, header %u bytes, sizes",
		       dst[2], dst[2] & 7, w, SEQLZ_LIT_HEADER(w));
		for (k = 0; k < SEQLZ_LIT_STREAMS; k++) {
			while (cnt < w) {
				acc |= (unsigned long long)dst[at++] << cnt;
				cnt += 8;
			}
			printf(" %llu", acc & ((1ULL << w) - 1));
			sum += (unsigned int)(acc & ((1ULL << w) - 1));
			acc >>= w;
			cnt -= w;
		}
		printf(" = %u\nbitstream from byte %u, %u bytes\n", sum,
		       SEQLZ_LIT_HEADER(w) + sum, len1 - SEQLZ_LIT_HEADER(w) - sum);
	}
	printf("first 16 bytes:");
	for (i = 0; i < 16; i++)
		printf(" %02x", dst[i]);
	printf("\nfirst 4 bytes of the bitstream:");
	for (i = len1 - (len0 - 2 - n_lit); i < len1 - (len0 - 2 - n_lit) + 4; i++)
		printf(" %02x", dst[i]);
	printf("\n");

	/* back again: not without the scratch, the same page with it */
	if (seqlz_decode(t, dst, len1, out, NULL) != -EINVAL)
		return 3;
	memset(out, 0, sizeof out);
	if (seqlz_decode_counted(t, dst, len1, out, scratch) ||
	    memcmp(out, page, SEQLZ_PAGE))
		return 4;
	printf("round trip ok, %u sequences on the fast path, %u on the slow path\n",
	       n_fast, n_slow);
	free(t);
	return 0;
}
```

`dec_counted.c` is `src/seqlz_decompress.c` with two counters and `seqlz_decode()` renamed, so that
it can be linked next to the original. `sed` makes it. From the root of the repository, with GNU sed
and gcc, `trace.c` in the current directory:

```text
R=/path/to/quetschn
sed -e 's/^#include "seqlz_internal.h"$/&\nunsigned int n_fast, n_slow;\n#define seqlz_decode seqlz_decode_counted/' \
    -e 's/^\t\t\ttok = next_token_fast(/\t\t\tn_fast++;\n&/' \
    -e 's|^\t\t/\* the slow path: length values, and every check \*/$|&\n\t\tn_slow++;|' \
    $R/src/seqlz_decompress.c > dec_counted.c
gcc -O2 -Wall -Wextra -I$R/src -o trace trace.c dec_counted.c \
    $R/src/seqlz_{codes,compress,decompress,default_tables,lit_sets}.c
./trace
```

::: try Change the page
Replace the `memcpy()` of the text in `make_page()` with `(void)text;`, and the page has 181
literals, a bitstream of 212 bytes, and stays at 395 bytes with raw literals: `code_literals()` finds
no table that saves enough. Change `N_REC`, the names or the pointer step, and watch the table of
sequences, the classes and the size class move.
:::

This page was built for the walk and is not typical. On the first desktop dump the project measures,
in the kernel VM with the tables of 7th October 2026, `seqlz-fast-lit` stores a page in 1037.5 bytes
of zsmalloc memory on average and `seqlz-fast` in 1123.7, so coded literals save 86 bytes per page
there, against 144.6 on this page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-score-with-the-swap-times-seqlz-fast-lits-range-ends-at-155-and-211-bytes-per-µs-instead-of-200-and-279)).
How much a page gains depends on its literals: without the text, this page gains nothing.
