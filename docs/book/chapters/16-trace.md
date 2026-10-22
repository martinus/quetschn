# One page, through every function {#ch-trace}

Part II explained each function on its own. This chapter follows one page through all of them in the
order they run: the tables are built, the page is compressed with `seqlz-2`, and decoded again.
Every number was printed by a small program that calls the real code of `src/`, or by a copy of the
decoder with two counters added. The program is at the end, so you can change the page and watch the
numbers move.

The page is made up, since real pages from a zram dump hold keys and passwords. It is built to look
like one: a small header, an array of records with a pointer, a few small integers and a name each, a
stretch of English text, and zeros to the end. It compresses to {{ch16.coded.len}} bytes,
{{ch16.coded.pct}}% of the page, and with raw literals to {{ch16.raw.len}}. Four figures of the book show
it sequence by sequence: the matcher finding it ([chapter 9](#stepper-match)), the encoder writing it
([chapter 10](#anim-ends)), the cache line of each token's lookup ([chapter 3](#anim-lines)) and the
decoder reading it ([chapter 14](#stepper-decode)). A row of links under each one's sentence jumps the
others to the same sequence.

## The page {#ch-trace--page}

- **Bytes 0 to 63, a header.** `"TASKSLAB"`, the record count 48 and the record size 32 as
  little-endian 32-bit integers, then zeros.
- **Bytes 64 to {{ch16.page.records_last}}, 48 records of 32 bytes.** A `next` pointer,
  `0xffff888104a3c040` plus 32 times another record's number, `(5i + 1) mod 48`, so 8-aligned and with
  the same top 6 bytes in every record; a pid, `1000 + 3 * i`; a state `i % 3` and a priority 120 as
  16-bit integers; and a zero-padded name of 16 bytes, one of 6 in turn (`kworker/0:1`, `bash`,
  `systemd`, `sshd`, `kworker/1:2`, `cron`).
- **Bytes {{ch16.page.records_end}} to {{ch16.page.text_last}}, {{ch16.page.text_len}} bytes of text,**
  ten lines about zram.
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

The zeros give long matches at a small offset, which need a length value. The records repeat every 32
bytes, so their offsets are multiples of 8, classes 4 and 5, or the repeat offset. The text has few
repeats and many literals, letters that the trained literal tables code in 4 to 6 bits, so the page
passes the rule for coded literals.

## The tables: seqlz_tables_init() {#ch-trace--tables}

```c
seqlz_tables_init(&seqlz_default_lengths);  /* seqlz_tables, {{size.tables|grp}} bytes */
seqlz_all_symbols();                        /* true */
```

`seqlz_tables_init()` ([chapter 8](#ch-codes)) builds the codes and decode tables. Some we meet below:
the escape has the {{tok.len.3072}}-bit code `{{tok.code.3072}}`, and only {{ch14.tokens_coded}} of the
{{const.TOKEN_SYMBOLS}} tokens have a code of their own. Literal table 0, the one for text, gives `e`
{{lit.0.len.101}} bits, the space {{lit.0.len.32}}, `0x00` {{lit.0.len.0}}; the encoder will pick table 3
for this page, which gives `0x00` {{lit.3.len.0}} bits.
@assert ch16.lt.set == 3 and lit[3].len[0] < lit[0].len[0]

## seqlz_compress(), and the matcher's first sequence {#ch-trace--matcher}

The backend calls `seqlz_compress(st, page, dst, 8192, 2)`: the per-CPU state of {{size.state}}
bytes, an output buffer of 2 pages, and level 2, the default. `compress_block()` checks that `dst_cap` is at
least {{ch16.enc_min}} bytes, sets up the encoder with the bitstream growing down from `dst + 8192` and
the literals up from `dst + 2` ([chapter 10](#ch-encoder--layout)), and runs `seqlz_match_block()`
([chapter 9](#ch-matcher)) with `encode_emit()` as its callback. The matcher clears its
{{ch16.hash_entries}} entries of hash table and starts at position 1.

Up to position 13 nothing matches: neither the repeat offset 1 nor the hash table's candidate, at first
position 0, `"TASK"`. Byte 12 is `{{ch16.byte12_hex}}`, the low byte of the record size, so at 13 the
repeat offset misses: the 4 bytes one before start with it. At position {{ch16.s.0.ll}} it hits on
zeros, and the zeros reach byte {{ch16.e0.zeros_to}}. The first sequence is {{ch16.s.0.ll}} literals (`TASKSLAB`,
`30 00 00 00`, `20 00`) and a match of {{ch16.s.0.ml}} bytes at offset {{ch16.s.0.off}}.
@assert ch16.s[0].ll == 14 and ch16.s[0].off == 1 and ch16.byte12 == 32

## encode_emit(): the first sequence in bits {#ch-trace--emit}

`encode_emit()` ([chapter 10](#ch-encoder--emit)) copies the 14 literals with one 16-byte copy. `off`
is the repeat offset, so class 0, without offset bits. The token, `14 + 16 * min(46, 31) + 512 * 0`, is
{{ch16.s.0.tok}}, which has no code, so it goes as the escape and 12 bits, `{{ch16.e0.word}}`,
{{ch16.e0.word_bits}} bits in one `enc_put()`. `ml - 4 = {{ch16.e0.ml4}}` is at least 31, so a length
value of {{ch16.e0.value}} follows, symbol {{ch16.e0.sym}} of the `ml` table with the code
`{{ml.code.15}}`. The accumulator then holds {{ch16.s.0.bits}} bits, `enc_flush()` stores the top byte,
`{{ch16.e0.top}}`, at `dst + 8191`, the next, `{{ch16.e0.next}}`, at `dst + 8190`, and keeps
{{ch16.e0.left}} bits for the next sequence. The trace program prints the bits of the first three
sequences:
@assert ch16.s[0].tok == 510 and ch16.s[0].esc == 1 and ch16.e0.sym == 15

@output ch16-trace 3,5,7

Sequence 1 is the first record: {{ch16.s.1.ll}} literals, the pointer, pid, state, priority and
`kworker/0:1`, then {{ch16.s.1.ml}} zero bytes at offset {{ch16.s.1.off}}, into the header's zeros. That
offset is below 256 and not a multiple of 8, class {{ch16.s.1.cls}} with {{ch16.s.1.raw}} offset bits.
`ll` is above the token's cap, so the token holds 15 and a length value of 12 follows. Sequence 2 is one
literal and {{ch16.s.2.ml}} bytes at offset {{ch16.s.2.off}}, the record before: class
{{ch16.s.2.cls}}, sent as $32 / 8 = {{ch16.s.2.ob}}$ in {{ch16.s.2.raw}} bits.
@assert ch16.s[1].ll == 27 and ch16.s[1].ml == 6 and ch16.s[2].ll == 1 and ch16.s[2].off == 32

## All {{ch16.seqs}} sequences {#ch-trace--sequences}

The first 14 and the last 3, with the bits each takes: the token's code (16 with `e` for an escaped
one), the offset bits, the length values' bits, and the sum.

@output ch16-trace 2,4,6,8-23

- **Sequence 7 is the cheapest kind**: {{ch16.s.7.ll}} literals and {{ch16.s.7.ml}} bytes at the offset
  of the sequence before, class 0, a token of {{ch16.s.7.clen}} bits. 8 bytes of the page in
  {{ch16.s.7.bits}} bits and 2 literals.
- **Sequence 6** has `ll = {{ch16.s.6.ll}}`, exactly the cap, so a value of 0 follows, {{ll.len.0}}
  bits: the token's 15 means "15 or more".
- **Sequences 5, 8 and 11 are token {{ch16.s.5.tok}}**: `ll` 1, `ml` 7, class 4. Sequence 5 is record
  2's pointer: the low byte differs, the 7 above it match record 1's, 32 bytes back. Pointers into the
  same few KiB share their top bytes, in real pages too. Sequences 8 and 11 are a pid's low byte and
  then its high bytes, the state and the priority, matched {{ch16.s.8.off}} bytes back, 3 records, which
  have the same state.
- **Sequences 12 and 13** are the greedy matcher: record 4's `kworker/1:2` matches record 0's
  `kworker/0:1`, {{ch16.s.12.off}} bytes back, for {{ch16.s.12.ml}} bytes up to the `1`, then
  {{ch16.s.13.ll}} literals and the padding at the same offset, now the repeat offset.
- **Sequence 177** is class {{ch16.s.177.cls}}: {{ch16.s.177.ml}} zeros after the text match a record's
  padding {{ch16.s.177.off}} bytes back, sent as {{ch16.s.177.ob}} in {{ch16.s.177.raw}} bits.
- **Sequence 178** covers the {{ch16.s.178.ml}} zeros to the end at offset {{ch16.s.178.off}}, not 1:
  the repeat offset is now {{ch16.s.177.off}}, whose 4 bytes are a pointer, and the hash table's
  candidate is position {{ch16.s.177.cand}}, stored 2 bytes before the end of sequence 177's match.
  Offset 2 is class {{ch16.s.178.cls}}, {{ch16.s.178.raw}} bit. The length value
  ${{ch16.s.178.ml}} - 4 - 31 = {{ch16.v178.value}}$ goes as symbol $12 + {{ch16.v178.top}} = {{ch16.v178.sym}}$,
  its top bit's position, and the {{ch16.v178.top}} bits below the top one: {{ch16.s.178.vbits}} bits
  for the length, {{ch16.s.178.bits}} for {{ch16.s.178.ml}} bytes of the page.
- **Sequence {{ch16.matches}}** is the last: the match before ends at byte 4096, so it has no literals,
  no match and no bits. Until 11th October 2026 it was token 0, 8 bits.
@assert ch16.s[7].ll == 2 and ch16.s[7].ml == 6 and ch16.s[7].cls == 0 and ch16.s[7].off == ch16.s.prev_off_7 and ch16.s[6].ll == 15 and ch16.s[6].cls == 0 and ch16.s[5].tok == ch16.s[8].tok == ch16.s[11].tok == 2097 and ch16.s[5].off == 32 and ch16.s[11].off == 96
@assert ch16.s[12].pos == 208 and ch16.s[12].ml == 8 and ch16.s[13].cls == 0 and ch16.s[177].cls == 5 and ch16.s[178].end == 4096 and ch16.s[178].off == 2 and ch16.s[179].ml == 0 and ch16.s[179].ll == 0 and ch16.matches == 179

@output ch16-trace 24-25

{{ch16.class.4}} of the {{ch16.matches}} matches are class 4, multiples of 8 below 256: the records'
period shows up as offsets {{ch16.c4_offs}}. {{ch16.class.5}} more are class 5, multiples of 8 from 256 to
2040, mostly {{ch16.c5_mode}}, {{ch16.c5_mode_records}} records back, and {{ch16.c5_ptr}} of them are a
pointer's 7 high bytes. Only {{ch16.class.3}} needed the 12 bits of class 3. Of the {{ch16.bits}} bits,
{{ch16.tok_bits}} are token codes, {{ch16.off_bits}} offset bits and {{ch16.v_bits}} length values.
@assert 2 * ch16.c5_mode_n > F("ch16.class.5") and ch16.matched + ch16.lits == 4096

| part | sequences | literals | bits |
| --- | ---: | ---: | ---: |
| header, bytes 0 to 63 | {{ch16.part.0.seqs}} | {{ch16.part.0.lits}} | {{ch16.part.0.bits}} |
| records, 64 to {{ch16.page.records_last}} | {{ch16.part.1.seqs}} | {{ch16.part.1.lits}} | {{ch16.part.1.bits}} |
| text, {{ch16.page.records_end}} to {{ch16.page.text_last}} | {{ch16.part.2.seqs}} | {{ch16.part.2.lits}} | {{ch16.part.2.bits}} |
| zeros, {{ch16.page.text_end}} to 4095 | {{ch16.part.3.seqs}} | {{ch16.part.3.lits}} | {{ch16.part.3.bits}} |

The records take three quarters of the bitstream, about {{ch16.part.records_per_seq}} bits per sequence,
and leave few literals; the text has {{ch16.part.2.lits}} of the {{ch16.lits}} literals.
@assert 70 <= ch16.part.records_pct <= 80 and ch16.part[2].lits * 2 > ch16.lits

::: why One position per hash, on this page
Record 30, at byte 1024, is named `kworker/0:1` like record 24, so a match at offset 192 would cover the
whole 16-byte name, {{ch16.s.24.bits}} bits, as sequence 24 does for `bash`. But the hash table keeps
the newest position per hash, and for `kwork` that is record 28, `kworker/1:2`, 64 bytes back. So the
matcher emits {{ch16.s.98.ml}} bytes at offset {{ch16.s.98.off}} and then {{ch16.s.99.ml}} at offset
{{ch16.s.99.off}}, {{ch16.s.98_99_bits}} bits. A table with the 2 newest positions per hash in the same
8 KiB saved 11 bytes per page in the zsmalloc model, for 4400 to 4700 more compression cycles, about
1 µs per page written: at the edge of what the score accepts, and dropped without a run in the kernel VM
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#seqlz-fast-lit-by-the-score-no-budget-offsets-in-steps-of-8)).
:::
@assert ch16.s[24].ll == 0 and ch16.s[24].ml == 16 and ch16.s[24].off == 192 and ch16.s[98].pos == 1040 and ch16.s[98].off == 64 and ch16.s[99].off == 192

## encoder_finish(): {{ch16.raw.len}} bytes with raw literals {#ch-trace--finish}

The literals fill `dst + 2` to `dst + {{ch16.raw.lit_end}}`, the bitstream the last {{ch16.bytes}}
bytes of the buffer, {{ch16.bits}} bits, so {{ch16.pad_bits}} bits are left in the last flush, already
written on top of the byte below `p`. `encoder_finish()` ([chapter 10](#ch-encoder--finish)) writes the
count, `{{ch16.raw.u16}}`, and moves the bitstream to right after the literals, turned around in
{{ch16.raw.copies}} copies of 8 bytes and {{ch16.raw.singles}} single bytes; the gap of
{{ch16.raw.gap}} bytes is far more than the bitstream, so they cannot overlap. That is
$2 + {{ch16.lits}} + {{ch16.bytes}} = {{ch16.raw.len}}$ bytes, the page as `seqlz-1` stores it.
@assert ch16.pad_bits > 0 and ch16.raw.gap >= ch16.bytes

## seqlz_code_literals(): {{ch16.coded.saved}} bytes less {#ch-trace--literals}

`seqlz_code_literals()` ([chapter 11](#ch-literals-enc)) prices the {{ch16.lits}} literals in all 8 tables,
in chunks of {{ch16.lt.chunk}}. The trace program recomputes it one literal at a time:

@output ch16-trace 27-35

Table {{ch16.lt.set}} wins, at {{ch16.lt.per_lit}} bits per literal. The text's literals take
{{ch16.lit0.text}} bits each in it, the records' {{ch16.lit0.records}}: pointer and pid bytes are close to
random, and table {{ch16.lt.set}} gives every byte above `0x7f` 10 bits. The 8 streams take
{{ch16.lt.sizes}} bytes, {{ch16.lt.bytes}} together, and the OR of the sizes with {{ch16.lt.or_min}}
needs {{ch16.lt.width}} bits, a header of {{ch16.lt.header}} bytes.
@assert ch16.lit0.high_all10 == 1 and ch16.lt.width == 7

The rule `coded + {{ch11.coded_min}} < n - n / 16` is `{{ch16.lt.rule_l}} < {{ch16.lt.rule_r}}`, so the
literals get coded. The page without the text would not: its {{ch16.nt.lits}} literals take
{{ch16.nt.coded}} bytes in table {{ch16.nt.set}}, and `{{ch16.nt.rule_l}}` is not below
`{{ch16.nt.rule_r}}`. Then the raw literals go into the matcher's table, the streams are written over
them in `dst`, the u16 becomes `{{ch16.coded.u16}}`, byte 2 `{{ch16.coded.byte2}}` for code
{{ch16.lt.set}}, the fixed table {{ch16.lt.set}}, in bits 0 to 3 and width {{ch16.lt.width}} in bits 4 to 6, bytes 3 to 9 the sizes, `{{ch16.coded.sizes_hex}}`, and the
bitstream moves down to byte {{ch16.lt.end}}: {{ch16.coded.len}} bytes.
@assert ch16.lt.rule_l < ch16.lt.rule_r and ch16.nt.rule_l >= ch16.nt.rule_r and ch16.nt.kept_raw == 1

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

zsmalloc stores the {{ch16.coded.len}} bytes in a size class ([chapter 1](#ch-zram--size-classes)).
`bench/zsmalloc_cost.cpp` ports the kernel's computation of the classes; summed over a new device's
pages it matched zram's `mem_used_total` to the byte in the kernel VM. With 4 KiB pages:

1. A handle of 8 bytes: ${{ch16.coded.len}} + 8 = {{ch16.zs.size}}$.
2. Classes are 16 bytes apart from 32 on: $32 + \lceil ({{ch16.zs.size}} - 32) / 16 \rceil \times 16 = {{ch16.zs.nominal}}$.
3. A class keeps its objects in a zspage of 1 to 8 pages, the number with the least waste, and classes
   with the same pages and objects per zspage are merged into the larger. {{ch16.zs.nominal}} has
   {{ch16.zs.objs}} objects in {{ch16.zs.pages}} page, and so does {{ch16.zs.class}}, since
   $4096 / {{ch16.zs.class}} = {{ch16.zs.div}}$: the page is stored in class {{ch16.zs.class}}.
4. Each object costs $4096 / {{ch16.zs.objs}} = {{ch16.zs.cost}}$ bytes.
@assert F("ch16.zs.class") == ch16.zs.nominal + 16 and ch16.zs.pages == 1 and ch16.zs.merged == ch16.zs.nominal and ch16.zs.objs == 5

With raw literals the page lands in class {{ch16.zs.raw_class}}, {{ch16.zs.raw_objs}} objects in
{{ch16.zs.raw_pages}} pages, {{ch16.zs.raw_cost}} bytes. Coding the literals saved {{ch16.coded.saved}}
bytes of compressed size and {{ch16.zs.saved}} bytes of memory. The page could grow by {{ch16.zs.grow}}
bytes and stay in its class, and would have to shrink by {{ch16.zs.shrink}} to reach the next smaller
one, {{ch16.zs.lower}}.

## seqlz_decode(): the literals first {#ch-trace--decode}

The backend calls `seqlz_decode(src, {{ch16.coded.len}}, dst, scratch)` with the per-CPU scratch of
{{const.SCRATCH}} bytes ([chapter 14](#ch-decode)). Bit 7 of byte 2 is 0, so it runs the copy of
`decode_block()` without deltas. That prefetches the token and length tables, finds the
top bit of `{{ch16.coded.u16}}` set and `n_lit = {{ch16.lits}}`, and `seqlz_decode_literals()`
([chapter 13](#ch-literals-dec)) reads table {{ch16.lt.set}} and width {{ch16.lt.width}} from byte 2,
prefetches the literal table, and reads the sizes, {{ch16.lt.sizes_sp}}. {{ch16.lits}} literals are
{{ch16.d.rounds}} full rounds of 40 and a rest of {{ch16.d.rest}}: {{ch16.d.steps}} steps of all 8 streams
and one in which streams 0 to {{ch16.d.r_last}} take their literal, while streams
{{ch16.d.first_quiet}} to 7 decode one into `scratch[{{ch16.d.extra_lo}}]` to
`scratch[{{ch16.d.extra_hi}}]` and do not move on. All 8 streams end in their last byte with zero bits
after their codes, and the bitstream starts at byte {{ch16.lt.end}}.
@assert 2 <= ch16.d.r <= 6 and ch14.pf.token == 129 and int(ch16.coded.byte2, 16) < 128

## The loop over the sequences {#ch-trace--loop}

**The first token.** `next_token()` refills: 8 bytes, `p` moved by {{ch16.d.p_moved}}, `count`
{{ch16.d.count}}. The top 11 bits are {{ch16.d.idx}}, and entry {{ch16.d.idx}} is `{{ch16.d.entry}}`: bit
31 for the escape, the code's length {{ch16.d.esc_len}} in bits 14 to 18. All {{ch16.d.esc_fill}} entries
whose index starts with `{{tok.code.3072}}` hold this value. The next 12 bits are {{ch16.d.esc_idx}}, and
`token_entry({{ch16.d.esc_idx}}, 0)` is `{{ch16.d.esc_entry}}`: `ll` {{ch16.d.esc_ll}}, `ml`
{{ch16.d.esc_ml}} with a length value, class 0.

**Sequence 0, slow path,** because of the length value. `off = last = 1`, one 16-byte copy of the 14
literals, the `ml` code `{{ml.code.15}}` adds 15, and `copy_match()` stores the zero pattern every 8 bytes,
{{ch16.d.m0_written}} bytes in {{ch16.d.m0_stores}} stores, for the {{ch16.s.0.ml}} of the match.

**Sequence 1, slow path again**, for its `ll` value: a token code of {{ch16.s.1.clen}} bits, which fills
{{ch16.d.t1_fill}} entries, and the offset `(bits << {{ch16.s.1.clen}}) >> {{ch16.d.t1_back}}`,
{{ch16.s.1.off}}.
@assert ch16.s[1].clen == 9 and 16 < ch16.s[1].ll <= 32

**Sequence 2, fast path.** No length value and room everywhere: `next_token_fast()` refills, since `skip`
is 0 after a slow sequence, and loads sequence 3's entry before one 16-byte copy for the literal and two
8-byte copies for the {{ch16.s.2.ml}} bytes at offset {{ch16.s.2.off}}.

The decoder with two counters:

@output ch16-trace 41

The {{ch14.slow}} slow sequences are the {{ch16.slow_value}} with a length value ({{ch16.slow_ll}} for
`ll`, {{ch16.slow_ml}} for `ml`), and sequences {{ch16.slow_novalue}}, in the last 8 bytes of the input,
where a refill without a check could read past it.
@assert ch16.slow_novalue_n == 2 and ch16.slow_novalue_past_in == 1 and ch16.slow_value + ch16.slow_novalue_n == ch14.slow

**The end.** After sequence 178 the page is full and all literals are used: `d - lit` is `end_gap`, and
the loop leaves without a token. The bit reader has read every byte, and the {{ch16.pad}} bits left are
the zero padding of the last byte ([chapter 4](#ch-format--validity)). `seqlz_decode()` returns 0, and
`dst` holds the page we started with. Without the scratch the same bytes are `-EINVAL`, which is why
`seqlz-2` has a decompression context and `seqlz-1` does not
([chapter 15](#ch-zram-glue--fast-lit)).
@assert ch16.pad == 3 and ch16.pad_bits == 5

## The program {#ch-trace--program}

`trace.c` builds the page, runs `seqlz_find()` for the sequences, the same ones the compressor finds (a
test in `test/seqlz_test.cpp` checks that `seqlz_encode()` of them gives the compressor's bytes),
computes each sequence's bits from the tables' `enc[]` arrays, compresses with raw and with coded
literals, and decodes. It reads the codes from `seqlz_tables`, whose layout `seqlz.h` brings along
through `seqlz_tables.h`, so it depends on the tables' layout. Its whole output, as the build of this book printed it; the compressed bytes are the same on
every CPU, so the output is too:

@output ch16-trace

```c
// trace.c: one made-up page through seqlz, with the numbers of each step
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

#define N_REC 48 /* records of 32 bytes from byte 64 on */

/* from dec_counted.c, seqlz_decompress.c with two counters */
extern unsigned int n_fast, n_slow;
int seqlz_decode_counted(const void *src, unsigned int src_len, void *dst,
			 void *scratch);

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

	memset(p, 0, SEQLZ_BLOCK_SIZE);
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
	static u8 page[SEQLZ_BLOCK_SIZE], dst[2 * SEQLZ_BLOCK_SIZE], out[SEQLZ_BLOCK_SIZE],
		scratch[SEQLZ_SCRATCH];
	static struct seqlz_state st;
	static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
	unsigned int n, i, k, pos = 0, last = 1, total = 0, n_lit = 0;
	unsigned int per_class[SEQLZ_OFF_CLASSES] = { 0 }, escapes = 0;
	unsigned int values = 0, len0, len1, set = 0, best = ~0U;

	make_page(page);
	if (seqlz_tables_init(&seqlz_default_lengths) ||
	    !seqlz_all_symbols())
		return 1;

	/* the sequences, and the bits each one takes in the bitstream */
	n = seqlz_find(&st, page, seq, 2);
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
		te = seqlz_tables.token.enc[tok];
		if (te) {
			clen = te >> TOK_ENC_LEN_AT;
			if (show)
				printf("seq %u: token code %s", i,
				       binary(te & TOK_ENC_CODE_MASK, clen));
		} else {
			unsigned int ee = seqlz_tables.token.enc[SEQLZ_ESCAPE];

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
			vbits += value_bits(&seqlz_tables.ll, ll - SEQLZ_LL_CAP, show);
			values++;
		}
		if (ml - 4 >= SEQLZ_ML_CAP) {
			if (show)
				printf(", ml");
			vbits += value_bits(&seqlz_tables.ml, ml - 4 - SEQLZ_ML_CAP, show);
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

	/* seqlz-1: raw literals */
	len0 = seqlz_compress(&st, page, dst, sizeof dst, 1);
	printf("raw literals: %u bytes = 2 + %u + %u\n", len0, n_lit,
	       len0 - 2 - n_lit);
	if (seqlz_decode(dst, len0, out, NULL) ||
	    memcmp(out, page, SEQLZ_BLOCK_SIZE))
		return 2;

	/* the 8 literal tables as seqlz_code_literals() weighs them */
	for (k = 0; k < SEQLZ_LIT_SETS; k++) {
		unsigned int bits = 0, bytes = 0, j, m;

		for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
			unsigned int sb = 0;

			for (m = j; m < n_lit; m += SEQLZ_LIT_STREAMS)
				sb += seqlz_tables.lit[k].enc[dst[SEQLZ_HEADER + m]] & 255U;
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

	/* seqlz-2: coded literals where that saves enough */
	len1 = seqlz_compress(&st, page, dst, sizeof dst, 2);
	printf("coded literals: %u bytes, u16 0x%04x\n", len1,
	       get_unaligned_le16(dst));
	if (get_unaligned_le16(dst) & SEQLZ_LIT_CODED) {
		unsigned int w = SEQLZ_SIZE_BITS_MIN +
				 (dst[2] >> SEQLZ_LIT_WIDTH_AT & 7);
		unsigned int sum = 0, cnt = 0, at = 3;
		unsigned long long acc = 0;

		printf("byte 2 = 0x%02x: code %u, w %u, header %u bytes, sizes",
		       dst[2], dst[2] & SEQLZ_LIT_CODE_MASK, w,
		       SEQLZ_LIT_HEADER(w));
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
	if (seqlz_decode(dst, len1, out, NULL) != -EINVAL)
		return 3;
	memset(out, 0, sizeof out);
	if (seqlz_decode_counted(dst, len1, out, scratch) ||
	    memcmp(out, page, SEQLZ_BLOCK_SIZE))
		return 4;
	printf("round trip ok, %u sequences on the fast path, %u on the slow path\n",
	       n_fast, n_slow);
	return 0;
}
```
@program ch16-trace with=dec_counted

`dec_counted.c` is `src/seqlz_decompress.c` with two counters and `seqlz_decode()` renamed, so that
it can be linked next to the original. `sed` makes it. From the root of the repository, with GNU sed
and gcc, `trace.c` in the current directory:

```text
R=/path/to/quetschn
sed -e 's/^#include "seqlz.h"$/&\nunsigned int n_fast, n_slow;\n#define seqlz_decode seqlz_decode_counted/' \
    -e 's/^\t\t\ttok = next_token_fast(/\t\t\tn_fast++;\n&/' \
    -e 's|^\t\t/\* the slow path: length values, and every check \*/$|&\n\t\tn_slow++;|' \
    $R/src/seqlz_decompress.c > dec_counted.c
gcc -O2 -Wall -Wextra -I$R/src -o trace trace.c dec_counted.c \
    $R/src/seqlz_{codes,compress,compress_literals,decompress,decompress_literals,default_tables,lit_sets}.c
./trace
```

::: try Change the page
Replace the `memcpy()` of the text in `make_page()` with `(void)text;`, and the page has
{{ch16.nt.lits}} literals, a bitstream of {{ch16.nt.bytes}} bytes, and stays at {{ch16.nt.len}} bytes with
raw literals: no table saves enough. Change `N_REC`, the names or the pointer step, and watch the
sequences, the classes and the size class move.
:::

The page was built for the walk and is not typical. On a desktop's zram pages, in the kernel VM with the
tables of 7th October 2026, `seqlz-2` stored a page in 1161.0 bytes of zsmalloc memory on average
and `seqlz-1` in 1298.1, so coded literals saved 137 bytes per page, against {{ch16.zs.saved}} on this
page
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-score-with-the-swap-times-seqlz-fast-lits-range-ends-at-155-and-211-bytes-per-µs-instead-of-200-and-279)).
Without the text, this page gains nothing.

At level 3, `seqlz-3`, the same page is {{slab.l3.len}} bytes, {{slab.coded.len}} at level 2. The
matcher's lazy step and the extension backwards ([chapter 9](#ch-matcher--lazy)) make the difference;
the literals keep table {{ch16.lt.set}}, as a table of the page's own would not save
{{own.enc.saves_min}} bytes on them, and they are not stored as deltas, so the text's last sentence holds for this
page at both levels.
@assert slab.l3.own == 0 and slab.l3.delta == 0 and slab.l3.len < slab.coded.len
