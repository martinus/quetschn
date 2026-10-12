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
It compresses to {{ch16.coded.len}} bytes, {{ch16.coded.pct}}% of the page, and with raw literals to {{ch16.raw.len}}.

Four figures of the book show this page sequence by sequence, each from the real code: the matcher
finding it ([chapter 9](#stepper-match)), the encoder writing it into `dst` ([chapter 10](#anim-ends)),
the cache line of its token's lookup ([chapter 3](#anim-lines)), and the decoder reading it again
([chapter 14](#stepper-decode)). Under each one's sentence a row of links jumps the others to the same
sequence, so one sequence can be followed from the matcher to the page it decodes to.

## The page {#ch-trace--page}

The 4096 bytes, in four parts:

- **Bytes 0 to 63, a header.** `"TASKSLAB"`, then the record count 48 and the record size 32 as
  little-endian 32-bit integers, then zeros.
- **Bytes 64 to {{ch16.page.records_last}}, 48 records of 32 bytes.** Each has a `next` pointer, a kernel address
  `0xffff888104a3c040` plus 32 times another record's number, `(5i + 1) mod 48`, so 8-aligned and with the same top 6 bytes in
  every record; a pid, `1000 + 3 * i`; a state `i % 3` and a priority 120 as 16-bit integers; and a
  name of 16 bytes, one of 6 names in turn (`kworker/0:1`, `bash`, `systemd`, `sshd`, `kworker/1:2`,
  `cron`), padded with zeros.
- **Bytes {{ch16.page.records_end}} to {{ch16.page.text_last}}, {{ch16.page.text_len}} bytes of text,** ten lines about zram.
- **Bytes {{ch16.page.text_end}} to 4095, {{ch16.page.zeros}} zero bytes.**

@assert ch16.page.text_lines == 10

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 120" role="img" aria-label="The 4096 bytes of the made-up page: a 64-byte header, 48 records up to byte {{ch16.page.records_end}}, text up to byte {{ch16.page.text_end}}, zeros to the end">
  <g font-size="12">
    <rect x="20" y="30" width="{{ch16.fig.p.w0}}" height="40" class="fill-acc" stroke="currentColor"/>
    <rect x="{{ch16.fig.p.64}}" y="30" width="{{ch16.fig.p.w1}}" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="{{ch16.fig.p.x2}}" y="30" width="{{ch16.fig.p.w2}}" height="40" class="fill-mark" stroke="currentColor"/>
    <rect x="{{ch16.fig.p.x3}}" y="30" width="{{ch16.fig.p.w3}}" height="40" fill="none" stroke="currentColor"/>
    <text x="20" y="22">header</text>
    <text x="{{ch16.fig.p.c1}}" y="55" text-anchor="middle">48 records of 32 bytes</text>
    <text x="{{ch16.fig.p.c2}}" y="55" text-anchor="middle" class="on-fill">text</text>
    <text x="{{ch16.fig.p.c3}}" y="55" text-anchor="middle">zeros</text>
    <text x="20" y="88" text-anchor="middle">0</text>
    <text x="34" y="104" text-anchor="middle">64</text>
    <text x="{{ch16.fig.p.x2}}" y="88" text-anchor="middle">{{ch16.page.records_end}}</text>
    <text x="{{ch16.fig.p.x3}}" y="88" text-anchor="middle">{{ch16.page.text_end}}</text>
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
struct seqlz_tables *t = malloc(seqlz_tables_size());   /* {{size.tables|grp}} bytes */
seqlz_tables_init(t, &seqlz_default_own);
seqlz_all_symbols(t);                                    /* true */
```

`seqlz_tables_init()` ([chapter 8](#ch-codes)) turns the code lengths compiled into
`seqlz_default_own` into canonical codes for the encoder and decode tables for the decoder. A few
values we will meet on this page:

| table | symbol | code length | code |
| --- | --- | ---: | --- |
| token | escape (3072) | {{tok.len.3072}} | `{{tok.code.3072}}` |
| token | 34: `ll` 2, `ml` 6, class 0 | {{tok.len.34}} | |
| literal table 0 | `e` | {{lit.0.len.101}} | `{{lit.0.code.101}}` |
| literal table 0 | space | {{lit.0.len.32}} | |
| literal table 0 | `t`, `a`, `o`, `s` | {{lit.0.len.116}} | |
| literal table 0 | `0x00` | {{lit.0.len.0}} | |
| literal table 0 | `0x40`, `0x80`, `0xc0`, `0xff` | {{lit.0.len.64}} | |

@assert lit[0].len[116] == lit[0].len[97] == lit[0].len[111] == lit[0].len[115] and lit[0].len[64] == lit[0].len[128] == lit[0].len[192] == lit[0].len[255]

{{ch14.tokens_coded}} of the {{const.TOKEN_SYMBOLS}} tokens have a code of their own; the others are sent as the escape's code followed by
the token in 12 bits ([chapter 4](#ch-format--token)). Literal table 0 is the one that codes text
best; we will see the encoder pick it.

@assert ch16.lt.set == 0

## seqlz_compress(), and the matcher's first sequence {#ch-trace--matcher}

zram calls the backend, and the backend calls `seqlz_compress(t, st, page, dst, 8192, 1)`: the tables,
the per-CPU `struct seqlz_state` of {{size.state}} bytes, the page, an output buffer of 2 pages, and `coded = 1`
for `seqlz-fast-lit`. `seqlz_compress()` first calls `compress_page()`
([chapter 10](#ch-encoder)), which checks that `dst_cap` is at least `SEQLZ_HEADER + ENC_ROOM`, {{ch16.enc_min}}
bytes, and that the tables have every symbol, and then sets up the encoder with `encoder_init()`:

- `e.p = dst + 8192`, the end of the buffer: the bitstream grows downward from there;
- `e.lit = dst + 2`, right after the u16: the literals grow upward from there;
- `e.src_end = page + 4096`, so that 16-byte copies of literals never read past the page;
- `e.last = 1`, the repeat offset at the start of every page.

Then `seqlz_match_page()` ([chapter 9](#ch-matcher)) runs over the page, with `encode_emit()` as the
function it calls for every sequence. It clears its hash table, {{ch16.hash_entries}} entries of 2 bytes, with one
`memset()` of {{ch16.hash_bytes}} bytes, and starts at position 1 with `anchor = 0`, `last = 1` and `back = -1`.

At positions 1 to 13 nothing matches. Each position compares 4 bytes against two candidates: the
bytes one position back, for the repeat offset 1, and the bytes at the position the hash table holds
for the hash of the next 5 bytes. The table starts as all zeros, so the second candidate is position
0, `"TASK"`, until a position with the same hash was stored. Each position is stored in the table
before the test.

At position {{ch16.s.0.ll}} the repeat offset hits: bytes 13 to 16 and bytes 14 to 17 are all zero. Byte 12 is
`{{ch16.byte12_hex}}`, the low byte of the record size, so the backward extension stops at once: `src[13]` is `0x00`
and `src[12]` is not. `seqlz_match_len()` then counts how far the match goes: the zeros reach byte {{ch16.e0.zeros_to}},
and byte {{ch16.s.0.end}} is `{{ch16.byte64_hex}}`, the low byte of the first record's pointer. So the matcher emits its first
sequence: {{ch16.s.0.ll}} literals (`TASKSLAB`, `30 00 00 00`, `20 00`), then a match of {{ch16.s.0.ml}} bytes at offset {{ch16.s.0.off}}. It
moves `pos` to {{ch16.s.0.end}}, sets `anchor` to {{ch16.s.0.end}}, and stores position {{ch16.e0.store}}, 2 bytes before the match's end, in
the hash table.

@assert ch16.s[0].ll == 14 and ch16.s[0].off == 1 and ch16.byte12 == 32

## encode_emit(): the first sequence in bits {#ch-trace--emit}

`encode_emit()` ([chapter 10](#ch-encoder)) gets `ll = {{ch16.s.0.ll}}`, `ml = {{ch16.s.0.ml}}`, `off = {{ch16.s.0.off}}`.

1. **Room.** `e.p - e.lit` is {{ch16.e0.room}} bytes, more than `14 + ENC_ROOM`, so the page still fits.
2. **Literals.** The page has more than 16 bytes from `in` on, so one 16-byte copy writes the 14
   literals and 2 bytes more to `dst + 2`; the next sequence's literals overwrite those 2. `e.lit`
   moves to `dst + {{ch16.e0.lit_end}}`.
3. **Offset class.** `seqlz_off_class(1, 1, &raw_bits)`: `off == last`, so `is_new` is
   `(1 == 1) - 1 = 0`, and the class, masked with it, is 0, with 0 offset bits
   ([chapter 6](#ch-header)).
4. **Token.** `seqlz_token(14, 50, 0)` is `14 + 16 * min(46, 31) + 512 * 0 = {{ch16.s.0.tok}}`. Token 510 has no
   code (`enc[510]` is 0), so `token_code()` returns the escape: code `{{tok.code.3072}}` shifted left by 12, or'ed
   with 510, `{{ch16.e0.word}}`, {{ch16.e0.word_bits}} bits.
5. **Token and offset in one put.** `enc_put(e, {{ch16.e0.word}} << 0 | (1 & 0), {{ch16.e0.word_bits}})`. For class 0 the offset is
   masked to 0 by `0U - (cls != 0)`, which is 0 here.
6. **Length value.** `ml - 4 = {{ch16.e0.ml4}}` is at least 31, so `put_len_value()` writes `{{ch16.e0.ml4}} - 31 = {{ch16.e0.value}}` with the
   `ml` table: values below 16 are their own symbol, and symbol {{ch16.e0.sym}} has the code `{{ml.code.15}}`, {{ml.len.15}} bits.
7. **Flush.** The accumulator holds {{ch16.s.0.bits}} bits, `{{ch16.e0.acc}}`. `enc_flush()` shifts them to
   the top of a 64-bit word and stores it little endian at `e.p - 8`, so the top byte, `{{ch16.e0.top}}`, lands
   at `dst + 8191`, the next, `{{ch16.e0.next}}`, at `dst + 8190`. `e.p` moves down by {{ch16.e0.full}} whole bytes, and the {{ch16.e0.left}}
   bits left over stay in the accumulator for the next sequence.
8. `e.last = 1`.

@assert ch16.s[0].tok == 510 and ch16.s[0].esc == 1 and ch16.e0.sym == 15

The next two sequences are the first record and the start of the second. The trace program prints
the bits of all three:

@output ch16-trace 3,5,7

Sequence 1 starts the first record: {{ch16.s.1.ll}} literals (the pointer, pid, state, priority, and the 11
letters of `kworker/0:1`), then {{ch16.s.1.ml}} zero bytes, the rest of the name's padding and the next record's
first byte, at offset {{ch16.s.1.off}}, which points into the header's zeros. {{ch16.s.1.off}} is not a multiple of 8 and below
256, so class {{ch16.s.1.cls}} with {{ch16.s.1.raw}} offset bits, `{{ch16.s.1.ob_bin}}`. `ll = {{ch16.s.1.ll}}` is above the token's cap of 15, so the
token holds 15 and a length value of 12 follows, symbol 12 in the `ll` table, {{ll.len.12}} bits. The token is
$15 + 16 \times 2 + 512 \times 2 = {{ch16.s.1.tok}}$, with a code of {{ch16.s.1.clen}} bits. Sequence 2 is one literal and {{ch16.s.2.ml}} bytes at
offset {{ch16.s.2.off}}, the record before: 32 is a multiple of 8 from 16 on, so class {{ch16.s.2.cls}}, and the offset goes as
$32 / 8 = {{ch16.s.2.ob}}$ in {{ch16.s.2.raw}} bits, `{{ch16.s.2.ob_bin}}`.

@assert ch16.s[1].ll == 27 and ch16.s[1].ml == 6 and ch16.s[2].ll == 1 and ch16.s[2].off == 32

## All {{ch16.seqs}} sequences {#ch-trace--sequences}

The matcher finds {{ch16.seqs}} sequences. The first 14, and the last 3, with the bits each takes in the
bitstream: the token's code (16 with `e` for an escaped one), the offset bits, the length values'
bits, and the sum.

@output ch16-trace 2,4,6,8-23

Some rows worth reading closely:

- **Sequence 7 is the cheapest kind**: {{ch16.s.7.ll}} literals, a match of {{ch16.s.7.ml}} at the same offset as the sequence
  before, {{ch16.s.7.off}}. Class 0 needs no offset bits, and token {{ch16.s.7.tok}} has a code of {{ch16.s.7.clen}} bits. 8 bytes of the page in
  {{ch16.s.7.bits}} bits of bitstream and 2 literals.
- **Sequence 6 is class 0 with a length value**: `ll = {{ch16.s.6.ll}}` is exactly the cap, so a value of 0 follows,
  {{ll.len.0}} bits in the `ll` table. A token holds `ll` 0 to 14 in full; 15 means "15 or more".
- **Sequences 5, 8 and 11 are the same token, {{ch16.s.5.tok}}**: `ll` 1, `ml` 7, class 4, a code of {{ch16.s.5.clen}} bits plus
  {{ch16.s.5.raw}} offset bits. One literal byte that differs, then 7 bytes that repeat, at a multiple of 8.
  Sequence 5 is record 2's pointer: its low byte goes as a literal, and the 7 bytes above it match
  record 1's pointer, 32 bytes back. Pointers into the same few KiB of memory share their top bytes,
  and that is common in real pages too. Sequences 8 and 11 are a pid: its low byte goes as a literal,
  and its 3 high bytes, the state and the priority match the record 3 places back, {{ch16.s.8.off}} bytes, which
  has the same state `i % 3`.
- **Sequences 12 and 13** show the greedy matcher at work. Record 4's name is `kworker/1:2`. The hash
  table's candidate for `kwork` is record 0, {{ch16.s.12.off}} bytes back, whose name is `kworker/0:1`, so the
  match covers `kworker/`, {{ch16.s.12.ml}} bytes, and ends at the `1` that differs. Then {{ch16.s.13.ll}} literals and the zeros
  of the padding, again at offset {{ch16.s.13.off}}, now as the repeat offset.
- **Sequence 177** is class {{ch16.s.177.cls}}: the {{ch16.s.177.ml}} zero bytes after the text's last newline match a record's
  padding {{ch16.s.177.off}} bytes back. ${{ch16.s.177.off}} = 8 \times {{ch16.s.177.ob}}$, so it goes as {{ch16.s.177.ob}} in {{ch16.s.177.raw}} bits.
- **Sequence 178** covers the {{ch16.s.178.ml}} zeros up to the end of the page, at offset {{ch16.s.178.off}}. Offset 2 and not 1,
  because offset 1 is not a candidate: the repeat offset is now {{ch16.s.177.off}}, from sequence 177, and the 4
  bytes {{ch16.s.177.off}} back are a record's pointer, not zeros. The hash table's candidate is position {{ch16.s.177.cand}},
  stored as "2 bytes before the end" of sequence 177's match, and its 5 bytes are zeros like those at
  {{ch16.s.178.pos}}. Offset 2 is class {{ch16.s.178.cls}}, {{ch16.s.178.raw}} bits. The length value is ${{ch16.s.178.ml}} - 4 - 31 = {{ch16.v178.value}}$. That is above 15,
  so it goes as its highest bit's position: {{ch16.v178.value}} is {{ch16.v178.len}} bits long, its top bit is bit {{ch16.v178.top}}, so the
  symbol is $12 + {{ch16.v178.top}} = {{ch16.v178.sym}}$, with a {{ch16.v178.code_len}}-bit code in the `ml` table, followed by the {{ch16.v178.top}} bits below the
  top one, ${{ch16.v178.value}} - {{ch16.v178.top_val}} = {{ch16.v178.extra}}$. {{ch16.s.178.vbits}} bits for the length, {{ch16.s.178.bits}} for the whole sequence, for {{ch16.s.178.ml}} bytes of
  the page.
- **Sequence {{ch16.matches}}** is the last one. The match before ends exactly at byte 4096, so it has no literals
  and no match, and it takes no bits: the decoder knows it is the last because 0 literals are left
  for 0 bytes of the page. Until 11th October 2026 it was token 0, 8 bits.

@assert ch16.s[7].ll == 2 and ch16.s[7].ml == 6 and ch16.s[7].cls == 0 and ch16.s[7].off == ch16.s.prev_off_7 and ch16.s[6].ll == 15 and ch16.s[6].cls == 0 and ch16.s[5].tok == ch16.s[8].tok == ch16.s[11].tok == 2097 and ch16.s[5].off == 32 and ch16.s[11].off == 96
@assert ch16.s[12].pos == 208 and ch16.s[12].ml == 8 and ch16.s[13].cls == 0 and ch16.s[177].cls == 5 and ch16.s[178].end == 4096 and ch16.s[178].off == 2 and ch16.s[179].ml == 0 and ch16.s[179].ll == 0 and ch16.matches == 179

The whole page, summed by the program:

@output ch16-trace 24-25

{{ch16.matches}} matches cover {{ch16.matched}} bytes, the {{ch16.lits}} literals the other {{ch16.lits}}. {{ch16.class.4}} of the {{ch16.matches}} matches are class 4, a
multiple of 8 below 256: the records' period of 32 shows up as offsets {{ch16.c4_offs}}. {{ch16.class.5}}
more are class 5, multiples of 8 from 256 on, mostly {{ch16.c5_mode}}, {{ch16.c5_mode_records}} records back; {{ch16.c5_ptr}} of them are the 7 high
bytes of a pointer. Only {{ch16.class.3}} matches needed the 12 offset bits of class 3. Of the {{ch16.bits}} bits, {{ch16.tok_bits}} are
token codes, {{ch16.off_bits}} offset bits and {{ch16.v_bits}} length values. By part of the page:

@assert 2 * ch16.c5_mode_n > F("ch16.class.5") and ch16.matched + ch16.lits == 4096

| part | sequences | literals | bits |
| --- | ---: | ---: | ---: |
| header, bytes 0 to 63 | {{ch16.part.0.seqs}} | {{ch16.part.0.lits}} | {{ch16.part.0.bits}} |
| records, 64 to {{ch16.page.records_last}} | {{ch16.part.1.seqs}} | {{ch16.part.1.lits}} | {{ch16.part.1.bits}} |
| text, {{ch16.page.records_end}} to {{ch16.page.text_last}} | {{ch16.part.2.seqs}} | {{ch16.part.2.lits}} | {{ch16.part.2.bits}} |
| zeros, {{ch16.page.text_end}} to 4095 | {{ch16.part.3.seqs}} | {{ch16.part.3.lits}} | {{ch16.part.3.bits}} |

The records take three quarters of the bitstream, about {{ch16.part.records_per_seq}} bits per sequence, and leave few
literals. The text is the other way round: {{ch16.part.2.seqs}} sequences, but {{ch16.part.2.lits}} of the {{ch16.lits}} literals.

@assert 70 <= ch16.part.records_pct <= 80 and ch16.part[2].lits * 2 > ch16.lits

::: why One position per hash, on this page
The names `kworker/0:1` and `kworker/1:2` share their first 8 bytes. Record 30, at byte 1024, is named
`kworker/0:1`, like record 24 six places before it, so a match at offset 192 would cover the whole
16-byte name in one sequence: token {{ch16.s.24.tok}} and {{ch16.s.24.raw}} offset bits, {{ch16.s.24.bits}} bits, as sequence 24 does for `bash`.
But the hash table keeps one position per hash, the newest, and for `kwork` that is record 28,
`kworker/1:2`, 64 bytes back. So the matcher emits {{ch16.s.98.ml}} bytes at offset {{ch16.s.98.off}} (sequence 98) and then {{ch16.s.99.ml}} at
offset {{ch16.s.99.off}} (sequence 99), {{ch16.s.98_99_bits}} bits. The project measured a table that keeps the 2 newest positions per
hash in the same 8 KiB and takes the longer match: 11 bytes less per page in the zsmalloc model, for
4400 to 4700 more compression cycles, about 1 µs per page written. That is about 11 bytes per µs, at
the edge of what the score accepts, and it was dropped, without a run in the kernel VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)).
:::

@assert ch16.s[24].ll == 0 and ch16.s[24].ml == 16 and ch16.s[24].off == 192 and ch16.s[98].pos == 1040 and ch16.s[98].off == 64 and ch16.s[99].off == 192

## encoder_finish(): {{ch16.raw.len}} bytes with raw literals {#ch-trace--finish}

When the matcher is done, `compress_page()` calls `encoder_finish()` ([chapter 10](#ch-encoder)).
At this point the literals fill `dst + 2` to `dst + {{ch16.raw.lit_end}}`, and the bitstream fills the last {{ch16.bytes}} bytes
of the 8192-byte buffer, backwards, the last of them in part: {{ch16.bits}} bits are {{ch16.full_bytes}} bytes and {{ch16.pad_bits}} bits. After
the flush of sequence 178, the last with bits, `e.cnt` is {{ch16.pad_bits}}. That flush already wrote the {{ch16.pad_bits}} bits on top
of the byte below `p`, zeros below them, so `encoder_finish()` moves `p` down by one to count that
byte.

`encoder_finish()` writes the literal count, {{ch16.lits}}, as the u16 at the start, `{{ch16.raw.u16}}`. Then it copies the
bitstream from the end of the buffer to right after the literals. The gap between them is
$(8192 - {{ch16.bytes}}) - {{ch16.raw.lit_end}} = {{ch16.raw.gap}}$ bytes, much more than {{ch16.bytes}}, so the copy cannot overlap: {{ch16.raw.copies}} copies of 8
bytes, each a little-endian load from the back and a big-endian store to the front, which reverses
the byte order, and {{ch16.raw.singles}} single bytes. The bitstream now starts at byte {{ch16.raw.lit_end}} with `{{ch16.raw.b4}}`, and the
function returns $2 + {{ch16.lits}} + {{ch16.bytes}} = {{ch16.raw.len}}$.

@assert ch16.pad_bits > 0 and ch16.raw.gap >= ch16.bytes

That is the page as `seqlz-fast` stores it, {{ch16.raw.pct}}% of 4096 bytes. `seqlz_compress()` was called with
`coded = 1`, so it now hands the {{ch16.raw.len}} bytes to `code_literals()`.

## code_literals(): {{ch16.coded.saved}} bytes less {#ch-trace--literals}

`code_literals()` ([chapter 11](#ch-literals-enc)) reads the count, {{ch16.lits}}, from the u16 and computes the
bitstream's length, ${{ch16.raw.len}} - 2 - {{ch16.lits}} = {{ch16.bytes}}$. Then `lit_cost()` sums, for every stream and every one of
the 8 literal tables, the code lengths of that stream's literals. It works on the literals in chunks
of $8 \times 25 = {{ch16.lt.chunk}}$, {{ch16.lt.chunks}} full ones and one of {{ch16.lt.chunk_rest}}, each chunk summed in 8-bit lanes of one u64 per
stream and then moved into 16-bit lanes before a lane can overflow. The results, as the trace program
recomputes them one literal at a time:

@output ch16-trace 27-35

Table {{ch16.lt.set}} wins, at {{ch16.lt.bits}} bits, {{ch16.lt.per_lit}} bits per literal. The text's {{ch16.part.2.lits}} literals take {{ch16.lit0.text}} bits each in it,
the records' {{ch16.part.1.lits}} take {{ch16.lit0.records}}: pointer and pid bytes are close to random, and table 0 gives every byte
value above `0x7f` 10 bits. Each stream's bits are rounded up to whole bytes: the 8 streams take {{ch16.lt.sizes}} bytes, {{ch16.lt.bytes}} together.

@assert ch16.lit0.high_all10 == 1

The width of the stream sizes is the bits the largest needs, at least 5: the OR of the 8 sizes and {{ch16.lt.or_min}}
is `{{ch16.lt.or}}`, `0b{{ch16.lt.or_bin}}`, so `32 - __builtin_clz({{ch16.lt.or}}) = {{ch16.lt.width}}` bits each, and the header is $3 + {{ch16.lt.width}} = {{ch16.lt.header}}$
bytes.

Then the rule. Coded literals are kept only if `coded + 51 < n - n / 16`:
`{{ch16.lt.bytes}} + 51 = {{ch16.lt.rule_l}}` against `{{ch16.lits}} - {{ch16.lt.n16}} = {{ch16.lt.rule_r}}`. {{ch16.lt.rule_l}} is less, so the literals get coded. Had the page
carried only the records and no text, they would not have been: the page without the
text has {{ch16.nt.lits}} literals, which table {{ch16.nt.set}} codes best, in {{ch16.nt.coded}} bytes, and `{{ch16.nt.coded}} + 51 = {{ch16.nt.rule_l}}` is not below
`{{ch16.nt.lits}} - {{ch16.nt.n16}} = {{ch16.nt.rule_r}}`.

@assert ch16.lt.rule_l < ch16.lt.rule_r and ch16.nt.rule_l >= ch16.nt.rule_r and ch16.nt.kept_raw == 1

What follows is the write. `code_literals()` copies the 674 raw literals into the matcher's hash
table, `st->literals`, which nobody needs any more, because the coded streams are about to be written
over the raw literals in `dst`. It computes where each stream starts, `dst + {{ch16.lt.start.0}}`, `+ {{ch16.lt.start.1}}`, `+ {{ch16.lt.start.2}}`,
and so on, and `lit_streams()` writes literal `k` into stream `k % 8`, 4 streams at a time with an
accumulator each, flushing every 32 literals. Stream 0 and 1 get {{ch16.lt.more}} literals, the other six {{ch16.lt.less}}. Then:

- the u16 becomes `0x8000 | {{ch16.lits}} = {{ch16.coded.u16}}`, bytes `{{ch16.coded.u16_bytes}}`;
- byte 2 becomes `{{ch16.lt.set}} | ({{ch16.lt.width}} - 5) << 3 = {{ch16.coded.byte2}}`: table {{ch16.lt.set}}, width {{ch16.lt.width}};
- `lit_sizes()` writes the 8 sizes of {{ch16.lt.width}} bits each, lowest bit first, into bytes 3 to 9:
  `{{ch16.coded.sizes_hex}}`;
- `memmove()` moves the {{ch16.bytes}} bytes of bitstream from byte {{ch16.raw.lit_end}} down to byte {{ch16.lt.end}}.

@assert ch16.lt.n_more == 2 and ch16.lt.width == 7

The function returns ${{ch16.lt.end}} + {{ch16.bytes}} = {{ch16.coded.len}}$. `seqlz_compress()` returns that, and the backend tells zram
the page is {{ch16.coded.len}} bytes.

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 240" role="img" aria-label="The {{ch16.coded.len}} bytes of the compressed page: a {{ch16.lt.header}}-byte header, 8 literal streams from byte {{ch16.lt.header}} to {{ch16.lt.end}}, the bitstream of the sequences from {{ch16.lt.end}} to {{ch16.coded.len}}, with the header's bytes shown enlarged below">
  <g font-size="11">
    <rect x="20" y="40" width="{{ch16.fig.c.w0}}" height="40" class="fill-acc" stroke="currentColor"/>
    <rect x="{{ch16.fig.c.x1}}" y="40" width="{{ch16.fig.c.w1}}" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="{{ch16.fig.c.x2}}" y="40" width="{{ch16.fig.c.w2}}" height="40" class="fill-paper" stroke="currentColor"/>
    <rect x="{{ch16.fig.c.x3}}" y="40" width="{{ch16.fig.c.w3}}" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="{{ch16.fig.c.x4}}" y="40" width="{{ch16.fig.c.w4}}" height="40" class="fill-paper" stroke="currentColor"/>
    <rect x="{{ch16.fig.c.x5}}" y="40" width="{{ch16.fig.c.w5}}" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="{{ch16.fig.c.x6}}" y="40" width="{{ch16.fig.c.w6}}" height="40" class="fill-paper" stroke="currentColor"/>
    <rect x="{{ch16.fig.c.x7}}" y="40" width="{{ch16.fig.c.w7}}" height="40" class="fill-sheet" stroke="currentColor"/>
    <rect x="{{ch16.fig.c.x8}}" y="40" width="{{ch16.fig.c.w8}}" height="40" class="fill-paper" stroke="currentColor"/>
    <rect x="{{ch16.fig.c.x9}}" y="40" width="{{ch16.fig.c.w9}}" height="40" class="fill-mark" stroke="currentColor"/>
    <text x="{{ch16.fig.c.m1}}" y="64" text-anchor="middle">s0</text>
    <text x="{{ch16.fig.c.m2}}" y="64" text-anchor="middle">s1</text>
    <text x="{{ch16.fig.c.m3}}" y="64" text-anchor="middle">s2</text>
    <text x="{{ch16.fig.c.m4}}" y="64" text-anchor="middle">s3</text>
    <text x="{{ch16.fig.c.m5}}" y="64" text-anchor="middle">s4</text>
    <text x="{{ch16.fig.c.m6}}" y="64" text-anchor="middle">s5</text>
    <text x="{{ch16.fig.c.m7}}" y="64" text-anchor="middle">s6</text>
    <text x="{{ch16.fig.c.m8}}" y="64" text-anchor="middle">s7</text>
    <text x="{{ch16.fig.c.m9}}" y="58" text-anchor="middle" class="on-fill">bitstream of the sequences</text>
    <text x="{{ch16.fig.c.m9}}" y="72" text-anchor="middle" class="on-fill">{{ch16.bytes}} bytes, MSB first</text>
    <text x="{{ch16.fig.c.lits_mid}}" y="30" text-anchor="middle">8 literal streams, {{ch16.lt.bytes}} bytes, table {{ch16.lt.set}}</text>
    <text x="20" y="96" text-anchor="middle" class="halo">0</text>
    <text x="30" y="110" text-anchor="middle">{{ch16.lt.start.0}}</text>
    <text x="{{ch16.fig.c.x2}}" y="96" text-anchor="middle" class="halo">{{ch16.lt.start.1}}</text>
    <text x="{{ch16.fig.c.x3}}" y="110" text-anchor="middle">{{ch16.lt.start.2}}</text>
    <text x="{{ch16.fig.c.x4}}" y="96" text-anchor="middle">{{ch16.lt.start.3}}</text>
    <text x="{{ch16.fig.c.x5}}" y="110" text-anchor="middle">{{ch16.lt.start.4}}</text>
    <text x="{{ch16.fig.c.x6}}" y="96" text-anchor="middle">{{ch16.lt.start.5}}</text>
    <text x="{{ch16.fig.c.x7}}" y="110" text-anchor="middle">{{ch16.lt.start.6}}</text>
    <text x="{{ch16.fig.c.x8}}" y="96" text-anchor="middle">{{ch16.lt.start.7}}</text>
    <text x="{{ch16.fig.c.x9}}" y="110" text-anchor="middle">{{ch16.lt.end}}</text>
    <text x="700" y="96" text-anchor="middle">{{ch16.coded.len}}</text>
    <line x1="20" y1="80" x2="20" y2="160" stroke="currentColor" stroke-dasharray="3 3"/>
    <line x1="{{ch16.fig.c.x1}}" y1="80" x2="520" y2="160" stroke="currentColor" stroke-dasharray="3 3"/>
    <rect x="20" y="160" width="100" height="34" class="fill-acc" stroke="currentColor"/>
    <rect x="120" y="160" width="50" height="34" class="fill-acc" stroke="currentColor"/>
    <rect x="170" y="160" width="350" height="34" class="fill-acc" stroke="currentColor"/>
    <text x="70" y="181" text-anchor="middle" class="on-fill">{{ch16.coded.u16_bytes}}</text>
    <text x="145" y="181" text-anchor="middle" class="on-fill">{{ch16.coded.byte2}}</text>
    <text x="345" y="181" text-anchor="middle" class="on-fill">{{ch16.coded.sizes_hex}}</text>
    <text x="70" y="212" text-anchor="middle">u16: coded, {{ch16.lits}}</text>
    <line x1="145" y1="196" x2="145" y2="216" stroke="currentColor"/>
    <text x="145" y="230" text-anchor="middle">table {{ch16.lt.set}}, w {{ch16.lt.width}}</text>
    <text x="345" y="212" text-anchor="middle">sizes {{ch16.lt.sizes_plain}}, {{ch16.lt.width}} bits each</text>
    <text x="536" y="181">bytes 0 to 9, the header</text>
  </g>
</svg>
</div>
<figcaption>The compressed page, {{ch16.coded.len}} bytes, drawn to scale; the {{ch16.lt.header}}-byte header enlarged below.</figcaption>
</figure>

## How much memory the page takes in zram {#ch-trace--zsmalloc}

zram does not store {{ch16.coded.len}} bytes. It asks zsmalloc for an object of that size, and zsmalloc rounds it
up to a size class ([chapter 1](#ch-zram)). The project models this in `bench/zsmalloc_cost.cpp`,
a port of the kernel's class computation; summed over a new device's pages it matched zram's own
`mem_used_total` to the byte in the kernel VM. For 4 KiB pages:

1. zsmalloc adds a handle of 8 bytes: ${{ch16.coded.len}} + 8 = {{ch16.zs.size}}$.
2. Classes are 16 bytes apart (`4096 >> 8`) from 32 bytes on, so the class index is
   $\lceil ({{ch16.zs.size}} - 32) / 16 \rceil = \lceil {{ch16.zs.frac}} \rceil = {{ch16.zs.idx}}$, and the class size $32 + {{ch16.zs.idx}} \times 16 = {{ch16.zs.nominal}}$ bytes.
3. A class keeps its objects in a "zspage" of 1 to 8 pages, the number with the least waste at the end.
   For {{ch16.zs.class}} that is {{ch16.zs.pages}} page with {{ch16.zs.objs}} objects, since $4096 / {{ch16.zs.class}} = {{ch16.zs.div}}$. Classes with the same pages and
   objects per zspage are merged into the larger: class {{ch16.zs.merged}} also has 1 page and 5 objects, so it is
   merged into {{ch16.zs.class}}, and no larger class shares {{ch16.zs.class}}'s.
4. So each object costs $4096 / {{ch16.zs.objs}} = {{ch16.zs.cost}}$ bytes of memory.

@assert F("ch16.zs.class") == ch16.zs.nominal and ch16.zs.pages == 1 and ch16.zs.merged == F("ch16.zs.class") - 16

The same page with raw literals, `seqlz-fast`'s {{ch16.raw.len}} bytes, lands in class {{ch16.zs.raw_class}} (`{{ch16.zs.raw_size}}` rounded up), {{ch16.zs.raw_pages}}
pages holding {{ch16.zs.raw_objs}} objects, ${{ch16.zs.raw_pages_bytes|grp}} / {{ch16.zs.raw_objs}} = {{ch16.zs.raw_cost}}$ bytes per page. Coding the literals saved {{ch16.coded.saved}} bytes of
compressed size and {{ch16.zs.saved}} bytes of zsmalloc memory. The page could grow by {{ch16.zs.grow}} bytes and stay in class
{{ch16.zs.class}} (up to ${{ch16.zs.hi}} + 8 = {{ch16.zs.class}}$); it would have to shrink by {{ch16.zs.shrink}} bytes, to {{ch16.zs.lo}}, to reach the next smaller
class, {{ch16.zs.lower}}, because class {{ch16.zs.merged}} is part of {{ch16.zs.class}}.

## seqlz_decode(): the literals first {#ch-trace--decode}

Some time later the page is read. zram calls the backend's decompress with the 795 bytes, and the
backend calls `seqlz_decode(t, src, {{ch16.coded.len}}, dst, scratch)` with the per-CPU scratch of {{const.SCRATCH}} bytes
([chapter 14](#ch-decode)).

`src_len` is at least 2, so the decoder first prefetches its tables: the token table's {{ch14.token_bytes}} bytes in
16 rounds of 8 cache lines, and the 1024 bytes of each length table in 2 rounds, each table with one
more prefetch for the line of its last byte. Then it reads the
u16, `{{ch16.coded.u16}}`. The top bit is set, so the literals are coded: `n_lit = {{ch16.coded.nlit_hex}} = {{ch16.lits}}`, the scratch is
not `NULL`, and `decode_literals()` runs ([chapter 13](#ch-literals-dec)).

`decode_literals()` reads byte 2, `{{ch16.coded.byte2}}`: width `5 + (({{ch16.coded.byte2}} >> 3) & 7) = {{ch16.lt.width}}`, table `{{ch16.coded.byte2}} & 7 = {{ch16.lt.set}}`,
and bits 6 and 7 are zero as they must be. The streams start at byte $3 + {{ch16.lt.width}} = {{ch16.lt.header}}$. It prefetches
literal table {{ch16.lt.set}}'s 2048 bytes of decode table, {{ch13.lt.loop_prefetches}} lines and the line of its last byte, and `lit_stream_sizes()` copies the {{ch16.lt.width}} bytes of
sizes out and takes {{ch16.lt.width}} bits at a time: {{ch16.lt.sizes_sp}}, starts {{ch16.lt.starts}}, total {{ch16.lt.bytes}}. ${{ch16.lt.header}} + {{ch16.lt.bytes}} = {{ch16.lt.end}}$ is within the {{ch16.coded.len}} bytes.

The 8 streams are then decoded side by side, `SEQLZ_LIT_ROUNDS = 56 / 10 = 5` literals per stream
after each refill, 40 literals per round. {{ch16.lits}} literals are {{ch16.d.rounds}} full rounds, {{ch16.d.round_lits}} literals. The rest, {{ch16.d.rest}},
is `{{ch16.d.rest}} / 8 = {{ch16.d.steps}}` steps of all 8 streams and then one more step in which only `{{ch16.d.rest}} % 8 = {{ch16.d.r}}` streams,
0 and {{ch16.d.r_last}}, take their literal. Streams {{ch16.d.first_quiet}} to 7 decode a literal in that step too, but with a mask of 0
they do not move on; what they decode lands in `scratch[{{ch16.d.extra_lo}}]` to `scratch[{{ch16.d.extra_hi}}]`, after the last
literal, where nothing reads it. On a page of 4096 literals those bytes are past the page, which is
one reason the scratch has 16 bytes more than a page. Finally `lit_stream_ends()` checks for each stream that it ended in
its last byte and that the bits after its last code are 0. All 8 do, and `decode_literals()` returns
`src + {{ch16.lt.end}}`, where the bitstream starts.

@assert ch16.d.r == 2 and ch14.pf.token == 129

The decoder sets up the bit reader at byte {{ch16.lt.end}}, `lit` at the scratch, `lit_end` at `scratch + {{ch16.lits}}`,
`lit_bound` at `scratch + {{const.SCRATCH}}`, and the limits of the fast path: `d_fast` 64 bytes before the end
of the page, `lit_fast` 16 bytes before `lit_bound`, `in_fast` 8 bytes before the end of the input.

## The loop over the sequences {#ch-trace--loop}

**The first token.** `next_token()` finds `count = 0`, less than `11 + 12`, and refills: the 8 bytes
at {{ch16.lt.end}} loaded big endian, `p` moved on by {{ch16.d.p_moved}} bytes, `count` set to {{ch16.d.count}}. The top 11 bits of `{{ch16.raw.b4}}
...` are `{{ch16.d.idx_bin}}`, {{ch16.d.idx}}. Entry {{ch16.d.idx}} of the token table is `{{ch16.d.entry}}`: bit 31 says escape, and
bits 14 to 18 hold {{ch16.d.esc_len}}, the escape code's length. All {{ch16.d.esc_fill}} entries whose index starts with `{{tok.code.3072}}` hold
this value, which is how one lookup finds a {{ch16.d.esc_len}}-bit code in an 11-bit table.

**The escape.** The loop drops the {{ch16.d.esc_len}} bits, refills, reads the next 12 bits, `{{ch16.d.esc_bin}}`, {{ch16.d.esc_idx}}, and
drops them. {{ch16.d.esc_idx}} is below 3072, so it is a valid token, and `token_entry({{ch16.d.esc_idx}}, 0)` builds its entry on
the spot, `{{ch16.d.esc_entry}}`: `ll` {{ch16.d.esc_ll}}, `ml` {{ch16.d.esc_ml}}, the length-value bit set, class 0 so `back` {{ch16.d.esc_back}}, and {{ch16.d.esc_drop}} bits
to drop, since the token's bits are already gone.

**Sequence 0, slow path.** `back` is 0, so `off = last = 1`. The entry has the length-value bit, so
the fast path is out. `nl = 14` is not 15, so no `ll` value. The checks pass: `lit` is not past
`lit_end`, 14 literals are left and fit in the page. `copy_literals()` copies them with one 16-byte
copy, and `d` and `lit` move on by 14. The page is not full yet. `len` is 35, `SEQLZ_ML_CAP + 4`, so a
length value follows: refill, look up the next 8 bits in the `ml` table, `{{ml.code.15}}` is symbol 15, base
15, no extra bits, and `len` becomes `35 + 15 = {{ch16.s.0.ml}}`. `off - 1 = 0` is less than the {{ch16.s.0.ll}} bytes written,
and {{ch16.s.0.ml}} bytes fit. `copy_match()` with offset 1 builds the 8-byte pattern from `dst[13]`, a zero byte
repeated 8 times by `repeat_pattern()`, and stores it every `repeat_step[1] = 8` bytes until {{ch16.s.0.ml}} are
covered, {{ch16.d.m0_written}} bytes written in {{ch16.d.m0_stores}} stores. `d` is now at {{ch16.s.0.end}}, and `next_token()` loads sequence 1's entry.

**Sequence 1, slow path again.** The token's code is {{ch16.s.1.clen}} bits, so its entry fills the {{ch16.d.t1_fill}} indices
`{{ch16.d.t1_code}}x`. It says `ll` 15 with a value to follow, `ml` {{ch16.s.1.ml}}, {{ch16.s.1.raw}} offset bits. The offset is cut out of
the bits right after the code with two shifts, `(bits << {{ch16.s.1.clen}}) >> {{ch16.d.t1_back}}`, which is {{ch16.s.1.off}}. The `ll` value adds
12, so {{ch16.s.1.ll}} literals are copied, with two 16-byte copies, and the {{ch16.s.1.ml}} bytes at offset {{ch16.s.1.off}} with two 8-byte
copies.

@assert ch16.s[1].clen == 10 and 16 < ch16.s[1].ll <= 32 and ch16.e0.sym == 15

**Sequence 2, fast path.** Token {{ch16.s.2.tok}} has no length value, `d` at {{ch16.s.2.pos}} is far from the end, the scratch
has room, and the input has more than 8 bytes left. So the fast path runs: `next_token_fast()` refills
without checking `count`, because `skip` is 0 after a slow sequence, and loads sequence 3's entry
before anything is copied. Then one 16-byte copy for the 1 literal, and `copy_match_fast()` with
offset `{{ch16.s.2.ob}} << 3 = {{ch16.s.2.off}}`, two 8-byte copies for the {{ch16.s.2.ml}} bytes. No compare of `nl` against the literals left,
no check of `len` against the room: the conditions of the fast path cover both.

The rest of the page goes the same way. The program counts the paths with a copy of the decoder that
has two counters added:

@output ch16-trace 41

{{ch14.seqs}} sequences have a token. The {{ch14.slow}} slow ones are the {{ch16.slow_value}} sequences with a length value ({{ch16.slow_ll}} for `ll`, {{ch16.slow_ml}}
for `ml`), and sequences {{ch16.slow_novalue}}, whose bit reader has reached the last 8 bytes of the input, so
that a refill without a check could read past it.

@assert ch16.slow_novalue_n == 2 and ch16.slow_novalue_past_in == 1 and ch16.slow_value + ch16.slow_novalue_n == ch14.slow

**The end.** After sequence 178's match `d` is at the end of the page and `lit` at `lit_end`, all {{ch16.lits}}
literals used: `d - lit` is `end_gap`, 0 literals left for 0 bytes of the page. That is sequence 179,
the last, which has no token, and the loop leaves without reading one; `copy_literals()` copies 0 bytes. Then
the checks that make a page valid only if it ends where its bits end
([chapter 4](#ch-format--validity)): the bit reader has read every byte, `p == end`, and `count`, the
bits left, is {{ch16.pad}}, fewer than 8, and that bit is 0: {{ch16.bits}} bits are {{ch16.full_bytes}} bytes and {{ch16.pad_bits}} bits, and the last
byte's lowest bit is padding. `seqlz_decode()` returns 0, and the 4096 bytes in `dst` are
the page we started with.

@assert ch16.pad == 1

Without the scratch, the same {{ch16.coded.len}} bytes are invalid: `seqlz_decode(t, src, {{ch16.coded.len}}, dst, NULL)` returns
`-EINVAL` as soon as it sees the top bit of the u16. That is why `seqlz-fast-lit` has a decompression
context and `seqlz-fast` does not ([chapter 15](#ch-zram-glue)).

## The program {#ch-trace--program}

`trace.c` builds the page, runs `seqlz_find()` for the sequences (the same ones the compressor finds;
a test in `test/seqlz_test.cpp` checks that its output fed to `seqlz_encode()` gives the compressor's
bytes), computes each sequence's bits from the tables' `enc[]` arrays, compresses the page with raw
and with coded literals, and decodes it. It includes `seqlz_internal.h` to read the tables' codes, so
it depends on the layout at commit `a2d57dd`.

Its whole output comes first, as the build of this book printed it; the compressed bytes are the same
on every CPU, so the output is too:

@output ch16-trace

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

		if (!ml) {
			/* the last sequence: only its literals, no bits */
			printf("%3u | %4u | %3u |    0 |    - |  - |     - |    - |   - |      - |    0\n",
			       i, pos, ll);
			n_lit += ll;
			break;
		}
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
		if (ml - 4 >= SEQLZ_ML_CAP) {
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
		per_class[cls]++;
		if (i < 14 || i + 3 >= n)
			printf("%3u | %4u | %3u | %4u | %4u | %2c | %5u | %2u%s | %3u | %6u | %4u\n",
			       i, pos, ll, ml, off, (char)('0' + cls), tok, clen,
			       te ? "  " : " e", raw, vbits, b);
		else if (i == 14)
			printf("...\n");
		pos += ll + ml;
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
@program ch16-trace with=dec_counted

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
Replace the `memcpy()` of the text in `make_page()` with `(void)text;`, and the page has {{ch16.nt.lits}}
literals, a bitstream of {{ch16.nt.bytes}} bytes, and stays at {{ch16.nt.len}} bytes with raw literals: `code_literals()` finds
no table that saves enough. Change `N_REC`, the names or the pointer step, and watch the table of
sequences, the classes and the size class move.
:::

This page was built for the walk and is not typical. On a desktop's zram pages, in the kernel VM with
the tables of 7th October 2026, `seqlz-fast-lit` stores a page in 1161.0 bytes of zsmalloc memory on
average and `seqlz-fast` in 1298.1, so coded literals save 137 bytes per page there, against
{{ch16.zs.saved}} on this page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-score-with-the-swap-times-seqlz-fast-lits-range-ends-at-155-and-211-bytes-per-µs-instead-of-200-and-279)).
How much a page gains depends on its literals: without the text, this page gains nothing.
