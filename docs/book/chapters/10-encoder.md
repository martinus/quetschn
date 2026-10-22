# The encoder writes the page from both ends {#ch-encoder}

The matcher of [chapter 9](#ch-matcher) finds a page's sequences; the encoder in
`src/seqlz_compress.c` writes them in the format of [chapter 4](#ch-format): the literals as they are,
and for `ll`, `ml` and the offset the token's code, the offset bits and the length values. The two run
as one loop, so the encoder gets each sequence the moment it is found and never sees the page as a
whole. That is a problem, because the bitstream starts after all literals, and how many there are is
known only after the last sequence. The encoder writes from both ends of its output buffer: the
literals from the front, the bitstream backwards from the back, and at the end it copies the
bitstream, turned around, to right after the literals. This chapter is all of `seqlz_compress.c`;
level 3's deltas of the literals and the coding of the literals are in `seqlz_compress_literals.c`,
[chapter 11](#ch-literals-enc).

## The file's head, and seqlz_find() {#ch-encoder--find}

@listing src/seqlz_compress.c 1-41

`find_emit()` is the simple receiver of the matcher's sequences: it stores the three numbers into the
next element and ignores the literals. A level that is not one gives no sequences, as
`seqlz_compress()` and `seqlz_encode()` give no page for it. It is inlined into the matcher's loop like `encode_emit()`
([chapter 9](#ch-matcher--emit)). `seqlz_find()` is not for zram: the table trainer counts the symbols
of exactly these sequences, and the round trip fuzzer checks that `seqlz_encode()` of them gives the
same bytes as `seqlz_compress()`. `port.py` cuts it from the kernel's copy. The two calls that differ
only in `true` and `false` could be one call with `level >= SEQLZ_LEVEL_OWN`; they are two on
purpose. With the constant in each call, the compiler inlines two copies of the matcher, and the copy
for levels 1 and 2 has no trace of level 3's code, not even a test of `lazy` per match.

## Literals from the front, bits from the back {#ch-encoder--layout}

@listing src/seqlz_compress.c 43-77

<figure class="diagram">
<div class="scroll">
<svg viewBox="0 0 720 222" role="img" aria-label="While encoding, the literals grow from the front of dst after 2 bytes left for the u16, and the bitstream grows backwards from the end of dst; at the end the bitstream is copied, turned around, to right after the literals.">
  <text x="20" y="26" font-size="12">while encoding</text>
  <rect x="20" y="34" width="40" height="36" fill="none" stroke="currentColor" stroke-width="1" stroke-dasharray="4 3"/>
  <rect class="fill-sheet" x="60" y="34" width="240" height="36" stroke="currentColor" stroke-width="1"/>
  <rect x="300" y="34" width="170" height="36" fill="none" stroke="currentColor" stroke-width="1"/>
  <g class="mark"><rect class="fill-sheet" x="470" y="34" width="230" height="36" stroke="currentColor" stroke-width="1.5"/></g>
  <g font-size="12" text-anchor="middle">
    <text x="40" y="56">u16</text>
    <text x="180" y="56">literals, in order →</text>
    <text x="385" y="56">free, ≥ ll + 32</text>
  </g>
  <g class="mark" font-size="12" text-anchor="middle"><text x="585" y="56">← bitstream, byte 0 last</text></g>
  <g font-size="11">
    <text x="20" y="86">d</text>
    <text x="60" y="86" text-anchor="middle">d+2</text>
    <text x="300" y="86" text-anchor="middle">lit</text>
    <text x="470" y="86" text-anchor="middle">p</text>
    <text x="700" y="86" text-anchor="end">d+dst_cap</text>
  </g>
  <text x="20" y="136" font-size="12">after encoder_finish()</text>
  <g class="acc"><rect class="fill-sheet" x="20" y="144" width="40" height="36" stroke="currentColor" stroke-width="1.5"/></g>
  <rect class="fill-sheet" x="60" y="144" width="240" height="36" stroke="currentColor" stroke-width="1"/>
  <g class="mark"><rect class="fill-sheet" x="300" y="144" width="230" height="36" stroke="currentColor" stroke-width="1.5"/></g>
  <rect x="530" y="144" width="170" height="36" fill="none" stroke="currentColor" stroke-width="1" stroke-dasharray="4 3"/>
  <g font-size="12" text-anchor="middle">
    <g class="acc"><text x="40" y="166" font-size="10.5">n_lit</text></g>
    <text x="180" y="166">literals</text>
    <text x="615" y="166">not part of the page</text>
  </g>
  <g class="mark" font-size="12" text-anchor="middle"><text x="415" y="166">bitstream, byte 0 first →</text></g>
  <g font-size="11">
    <text x="20" y="196">d</text>
    <text x="300" y="196" text-anchor="middle">d+2+n_lit</text>
    <text x="530" y="196" text-anchor="middle">d+len</text>
  </g>
  <g class="mark" fill="none" stroke="currentColor" stroke-width="1.5">
    <path d="M 585 72 C 585 112, 430 104, 418 140"/>
    <polygon class="fill-mark" points="417,144 413,134 422,136"/>
  </g>
  <g class="mark" font-size="11"><text x="716" y="106" text-anchor="end">copied, byte order</text><text x="716" y="120" text-anchor="end">turned around</text></g>
</svg>
</div>
<figcaption>The buffer while encoding, and after encoder_finish(): the bitstream moves from the back to right after the literals.</figcaption>
</figure>

Byte 0 of the bitstream, the first the decoder reads, is the last byte of `dst`, and the two ends grow
toward each other. The same with a real page, the slab of task records from [chapter 16](#ch-trace),
one sequence per step; the buttons give `dst` the two pages of zram, the smallest size the page fits
into, and 1 byte less.

@anim ends wide The buffer dst while seqlz compresses chapter 16's page with raw literals, after each sequence, then encoder_finish(). The numbers come from the real encoder, through anim/ends.c; the close-ups show the 16 bytes at each end.

::: why One pass, any buffer
Two passes, the matcher into an array and then the encoder, were 6% slower
([chapter 9](#ch-matcher--emit)). A second buffer for the bitstream needs more work memory per CPU.
Until 8th October 2026 the encoder wrote the bitstream forwards, behind room for a page of literals at
`d + 2 + 4096`, and moved it down at the end, which needed a `dst` of two pages. zram gives two pages,
zswap, the kernel's compressed cache in front of another swap device, gives one, and `lz4`, `lzo` and
`zstd` take any buffer and fail when the page does not fit
([#122](https://github.com/martinus/quetschn/issues/122)). The layout from both ends takes any `dst`,
writes the same bytes, and made writes in the kernel VM 0.20 µs faster per page; on the Mi 9T they
stayed within 0.22 µs
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-into-a-buffer-of-any-size-the-bitstream-from-the-back-the-same-bytes-in-zram-writes-2-faster-in-the-vm-kept),
commit `66aab11`).
:::

**The room check.** Before each sequence the literals and `ENC_ROOM` bytes more must fit between `lit`
and `p`. The literal copies write up to 16 bytes past the last literal, so the literals reach at most
`lit + ll + 16`. A sequence flushes at most twice; the first moves `p` down by at most 7 bytes, and the
second stores 8 bytes below that, so the bits reach no lower than `p - 15`. With `p - lit >= ll + 32`
the two never meet, with 1 byte spare. Until 11th October the comment counted this as 40 bytes, not 32
([#180](https://github.com/martinus/quetschn/issues/180)). As the check is not exact, a page can fail
although it would have fit with up to 31 bytes to spare: the page of pointers from chapter 9
compresses to {{pointers.len}} bytes and needs a `dst` of {{ch10.ptr.min_dst}}.
@assert ch10.ptr.min_dst - pointers.len < ch10.enc_room and ch10.slab.min_dst - slab.len < ch10.enc_room

**Why two pages are always enough.** Every sequence but the last covers at least 4 bytes, and its
token and offset take at most 31 bits: 11 bits of code and 12 of offset, or an escaped token, whose
escape code `SEQLZ_MAX_ESCAPE_LEN` holds to 7 bits ([chapter 6](#ch-header--escape)); with today's
{{tok.len.3072}}-bit escape the real maximum is {{ch10.max_tok_off}}. Length values come only with long
sequences: an `ll` value, at most $8 + {{ch10.len_extra_max}} = {{ch10.len_bits_max}}$ bits, only with
15 literals or more, which alone are worth $15 / 4 \times 31 = 116$ bits of the budget, and an `ml`
value only with a match of 35 bytes or more, worth 271. The last sequence has no bits. So:

| | 4 KiB pages | 16 KiB pages |
| --- | --- | --- |
| bits at most, `SEQLZ_BLOCK_SIZE / 4 * 31` | ${{ch10.bound.seqs.tex}} \times 31 = {{ch10.bound.bits.tex}}$ | ${{k16.ch10.bound.seqs.tex}} \times 31 = {{k16.ch10.bound.bits.tex}}$ |
| bytes of bitstream at most | {{ch10.bound.bytes|grp}} | {{k16.ch10.bound.bytes|grp}} |
| the u16 + every byte a literal + that bitstream | $2 + {{ch10.bound.page.tex}} + {{ch10.bound.bytes.tex}} = {{ch10.bound.total.tex}}$ | $2 + {{k16.ch10.bound.page.tex}} + {{k16.ch10.bound.bytes.tex}} = {{k16.ch10.bound.total.tex}}$ |
| two pages | {{ch10.bound.two|grp}} | {{k16.ch10.bound.two|grp}} |
| left over, must be ≥ `ENC_ROOM` | {{ch10.bound.left}} | {{k16.ch10.bound.left}} |
@assert min(F("ch10.bound.left"), F("k16.ch10.bound.left")) >= ch10.enc_room

The bound adds the most literals and the most bits, which no page has at once.

**The accumulator.** After a flush at most 7 bits are left, so token, offset and a match length value
fit before the next one: $7 + 31 + 20 = 58$ bits, 60 on 16 KiB pages, where the comment counts 22 per
value. The 20 has a bit to spare: a value is at most 8 bits of code and {{ch10.len_extra_max}} extra
bits, {{ch10.len_bits_max}}, {{k16.ch10.len_bits_max}} on 16 KiB pages; the tables had one more symbol
with one more extra bit, which no page needs, until 11th October 2026
([#182](https://github.com/martinus/quetschn/issues/182)). A literal length value would add as many
bits again, so it gets a flush of its own.

## The encoder's state {#ch-encoder--state}

@listing src/seqlz_compress.c 79-91

`cnt` bits of `acc` count, the newest lowest; bits above may be old ones already written, and are
shifted out before they matter. `src_end` is how far the 16-byte literal copies may read: the end of
the page in the compressor, the end of the caller's literals in `seqlz_encode()`. `last` is the repeat
offset as the encoder sees it, the matcher's `last` again, both starting at 1. The matcher's loop and
all of `encode_seq()` are inlined into `compress_block()`, and the matcher leaves the encoder no
registers, so this struct lives on the stack
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).
`rec` and `pos` are level 3's, for the records below; `pos` is where the next sequence starts in the
page, which the encoder does not need for anything else.

## Level 3: a record in front of each sequence's literals {#ch-encoder--records}

@listing src/seqlz_internal.h 102-111

Level 3 stores the literals as deltas to earlier bytes of the page ([chapter 6](#ch-header--literals)),
one choice for the whole page, and which one is cheapest is known only when all literals are there.
For a literal's delta, the encoder needs where it is in the page and the offset of the sequence before it,
and neither is in the literals. So at level 3 the encoder writes that next to them, a record of
{{delta.rec}} bytes in front of each sequence's literals, and `seqlz_delta_literals()` reads the records once
the page is matched ([chapter 11](#ch-literals-enc--deltas)). `dst` is where the room is: the matcher's
state is its hash table until the last sequence, and the literals' copy goes there only afterwards.
Each field fits a `u16`, as a page of 16 KiB has positions, lengths and offsets below $2^{16}$. The
cost is room in `dst`: on the page of pointers of [chapter 9](#ch-matcher--back), level 3 finds
{{delta.ptr.n_lit}} literals in {{delta.ptr.recs}} sequences, and with the records they take
{{delta.ptr.with_rec|grp}} bytes. A page that does not fit with its records is matched a second time
without them, and stored without deltas ([below](#ch-encoder--entry)).
@assert delta.ptr.with_rec == delta.ptr.n_lit + delta.rec * delta.ptr.recs

## Bits into a 64-bit accumulator {#ch-encoder--put}

@listing src/seqlz_compress.c 93-116

`v` must have no bits above `n`, and the comment's `n < 64 - cnt` keeps bits from falling out of the
top. The masks `& 63U` and `& 31U` change no value the encoder uses; they let the compiler drop the
checks that `CONFIG_UBSAN_SHIFT` adds before a shift it cannot prove in range
([chapter 7](#ch-internal--decode-tables)). `enc_put_code()` puts a code and then its extra bits in one
put, the code in the higher bits, as the decoder reads the most significant bit first.

The page of `ab` repeated has a match of {{ch10.ab.s1.ml}} bytes, so `ml - 4` is {{ch10.ab.s1.ml4}} and
the length value ${{ch10.ab.s1.ml4}} - 31 = {{ch10.ab.s1.mlv.v}}$: symbol {{ch10.ab.s1.mlv.sym}} with
{{ch10.ab.s1.mlv.extra}} extra bits, whose {{ch10.ab.s1.mlv.len}}-bit code in the `ml` table is
`{{ch10.ab.s1.mlv.code}}`, entry `{{ch10.ab.s1.mlv.entry}}`:
@assert ch10.ab.s1.mlv.v == 4059 and ch10.ab.s1.mlv.code == ml.code[23]

| | value |
| --- | --- |
| `extra & ((1U << 11) - 1U)` | {{ch10.ab.s1.mlv.v}} & 2047 = {{ch10.ab.s1.mlv.x}} = `{{ch10.ab.s1.mlv.xbits}}` |
| `(u64){{ch10.ab.s1.mlv.codehex}} << 11` | `{{ch10.ab.s1.mlv.shifted}}` |
| value put | <code>{{ch10.ab.s1.mlv.shifted}} &#124; {{ch10.ab.s1.mlv.x}}</code> = `{{ch10.ab.s1.mlv.put}}`, {{ch10.ab.s1.mlv.n}} bits: `{{ch10.ab.s1.mlv.bits}}` |

The top bit of 4059, the 2048, is not sent: the symbol says it.

## enc_flush(): whole bytes go below p {#ch-encoder--flush}

@listing src/seqlz_compress.c 118-130

The shift puts the `cnt` bits that count at the top of the word, the oldest in bit 63. A little-endian
store writes the top byte last, at `p - 1`, so the oldest bit lands in bit 7 of the byte right below
`p`, and the bits after it go downwards: read from the end of `dst` backwards, the bytes hold the bits
in the order the decoder wants. `p` moves down by the whole bytes, and the at most 7 bits left over
stay in `acc`; the next flush writes their byte again, with the bits after them. The first two
sequences of the page of pointers:

- Sequence 1, {{ch10.ptr.s1.ll}} literals and {{ch10.ptr.s1.ml}} bytes at offset
  {{ch10.ptr.s1.off}}, is token {{ch10.ptr.s1.tok}}, code `{{ch10.ptr.s1.code}}`, and the offset
  `{{ch10.ptr.s1.offbits}}` in {{ch10.ptr.s1.raw}} bits: `cnt = {{ch10.ptr.s1.cnt}}`. The flush
  stores `acc << {{ch10.ptr.f1.shift}}` = `{{ch10.ptr.f1.word}}` at `p - 8`, which puts
  `{{ch10.ptr.f1.e1}}`, the first 8 bits, at `p - 1` and `{{ch10.ptr.f1.e2}}`, the 6 bits left and two
  0s, at `p - 2`. `p` moves down by {{ch10.ptr.f1.moved}}, `cnt` is {{ch10.ptr.f1.cnt1}}.
- Sequence 2, {{ch10.ptr.s2.ll}} literal and {{ch10.ptr.s2.ml}} bytes at the repeat offset, is token
  {{ch10.ptr.s2.tok}}, code `{{ch10.ptr.s2.code}}`, no offset bits. The flush writes
  `{{ch10.ptr.f2.e2}}` at the new `p - 1`, the same byte as before, now with all its bits, and
  `{{ch10.ptr.f2.e3}}` below it. `p` moves down by {{ch10.ptr.f2.moved}}, `cnt` is
  {{ch10.ptr.f2.cnt1}}.
@assert ch10.ptr.s1.code + ch10.ptr.s1.offbits == "10101011" + "000001" and ch10.ptr.f2.e3 == "00" and ch10.ptr.s2.code == tok.code[49] and ch10.ptr.f1.cnt1 == 6 and ch10.ptr.f2.cnt1 == 2

The same, put by put and flush by flush, for the first sequences of chapter 16's page:

@anim accum wide The accumulator acc, the value each enc_put() appends or the word enc_flush() stores, and the last bytes of dst, for the first 4 sequences of chapter 16's page. The values come from the encoder's own helpers, through anim/accum.c, which checks them against encode_emit().

For `cnt` 0 the shift would be by 64, undefined, and the mask makes it 0. That happens: a sequence with
an `ll` value and no `ml` value flushes twice, the second time possibly with no bits. Then the old
word goes below `p`, `p` does not move, and the next flush overwrites it. Before commit `66aab11` the
bitstream went forwards and the flush swapped the bytes of each word; now the swap is in
`encoder_finish()`, once per page.

## Codes for length values and tokens {#ch-encoder--codes}

@listing src/seqlz_compress.c 132-158

`put_len_value()` masks the symbol to the 32 entries of `enc[]`, for UBSan again, and passes `v`
whole, since `enc_put_code()` masks it to its extra bits. `token_code()` tests for the common case
first: an entry of 0 means no code, because a code has a length of at least 1 in the top bits
([chapter 7](#ch-internal--token-table)). A token without a code is **escaped**: the escape's code
shifted left by 12 and ORed with the token, one code of the escape's length plus 12 bits.
{{ch10.tok_codes}} of the {{const.TOKEN_SYMBOLS}} tokens have a code of their own; token 10, `ll` 10
and a match of 4 bytes at the repeat offset, has none:
@assert ch10.esc10.enc == 0 and tok.len[10] == 0

| | value |
| --- | --- |
| `ee = seqlz_tables.token.enc[3072]` | `{{ch10.esc.entry}}`: length {{ch10.esc.len}}, code `{{ch10.esc.codehex}}` |
| `*len` | {{ch10.esc.len}} + 12 = {{ch10.esc10.len}} |
| returned | <code>{{ch10.esc.codehex}} &lt;&lt; 12 &#124; 10</code> = `{{ch10.esc10.ret}}`: `{{ch10.esc10.bits}}` |

## encode_seq(), one sequence {#ch-encoder--emit}

`encode_seq()` writes one sequence. The matcher calls it through `encode_emit()` or
`encode_emit_rec()` ([below](#ch-encoder--wrappers)), inlined into its loop, and `encode_raw()` calls
it in a plain loop.

@listing src/seqlz_compress.c 160-182

`n_rec` is the record's size, {{delta.rec}} bytes, or 0. Three things have to hold for a record: `rec`,
a constant at every call, `true` from the callers that write records, `encode_emit_rec()` and
`encode_raw()`, and `false` from `encode_emit()`, so that the copy of the matcher's loop for levels 1
and 2 has none of this code; `e->rec`, which is false in the second pass without records; and `ll`
not 0, because a sequence without literals has no deltas. The room check counts the record too. The cast to `size_t` makes
the compare unsigned; `p` never goes below `lit`, because both callers
reject a `dst_cap` below {{ch10.dst_min}}. A failed check sets `p` to `lit`, and that sticks: the
room is 0, every later sequence fails the same check, and `encoder_finish()` sees `p == lit`. The
matcher runs to the end of the page anyway, which costs nothing with zram's two pages, where it never
happens.

@listing src/seqlz_compress.c 183-190

The record is the position of the sequence's first literal, `e->pos`, the number of literals, and
`e->last`, the offset of the sequence before, which the end of this function sets to `off` only after
the record is written. Then `lit` moves on, so the literals come right after their record. `pos` moves
on by the whole sequence, literals and match, whenever `rec` is the constant `true`, also without
records, where nobody reads it. The first two records of the page of pointers, as the bytes in `dst`:

| record | bytes | `pos` | `ll` | `last` |
| --- | --- | --- | --- | --- |
| 0 | `{{delta.ptr.rec.0.hex}}` | {{delta.ptr.rec.0.pos}} | {{delta.ptr.rec.0.ll}} | {{delta.ptr.rec.0.last}} |
| 1 | `{{delta.ptr.rec.1.hex}}` | {{delta.ptr.rec.1.pos}} | {{delta.ptr.rec.1.ll}} | {{delta.ptr.rec.1.last}} |

The first sequence takes the first pointer and the low byte of the second as literals, and copies 7
bytes from 8 back, so the second starts at {{delta.ptr.rec.1.pos}} with `last` 8.
@assert delta.ptr.rec[0].pos == 0 and delta.ptr.rec[0].last == 1 and delta.ptr.rec[1].pos == delta.ptr.rec[0].ll + 7 and delta.ptr.rec[1].last == 8

@listing src/seqlz_compress.c 191-211

Most sequences have few literals, 1 in the page of pointers, so the first 16 bytes are one fixed copy
without a loop, whose exit branch would depend on `ll` and could mispredict. The bytes past `ll` fit
into `ENC_ROOM`, and the next sequence's literals overwrite them. The byte loop runs only near
`src_end`, where a 16-byte read would go past it. `__builtin_memcpy()` with a constant 16 is always
inlined, also with `CONFIG_FORTIFY_SOURCE` ([chapter 6](#ch-header--summary)). The last sequence, `ml`
0, has only its literals and no bits; until 11th October 2026 it had a token and possibly an `ll`
value, about 9 bits per page ([chapter 4](#ch-format--end)).

@listing src/seqlz_compress.c 212-232

The first sequence of the page of `ab` is {{ch10.ab.s1.ll}} literals and a match at offset
{{ch10.ab.s1.off}}, with `last` 1: class {{ch10.ab.s1.cls}}, {{ch10.ab.s1.raw}} bit, token
$2 + 16 \times 31 + 512 \times 1 = {{ch10.ab.s1.tok}}$.
@assert ch10.ab.s1.ll == 2 and ch10.ab.s1.cls == 1 and ch10.ab.s1.raw == 1 and ch10.ab.s1.tok == 2 + 16 * 31 + 512

**Token and offset in one put.** The offset is shifted right by `SEQLZ_OFF_SHIFT(cls)` and ANDed with
`0U - (cls != 0)`, all ones for classes 1 to 5 and 0 for class 0, which sends no bits and would else OR
the offset into the low bits of the token's code. The other classes need no mask: `seqlz_off_class()`
picks a class whose bits hold the offset. Masking only class 0, instead of every offset to `raw_bits`
bits, came with commit `996248b`, with no number of its own. One put instead of two took 21 786 to
21 233 cycles per page on x86-64
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#the-compressor-second-round-fewer-instructions)).
For the page of `ab`: `{{ch10.ab.s1.codehex}} << {{ch10.ab.s1.raw}} | ({{ch10.ab.s1.off}} >> 1) = {{ch10.ab.s1.put}}`,
{{ch10.ab.s1.putn}} bits.
@assert ch10.ab.s1.off >> 1 == int(ch10.ab.s1.offbits, 2)

**When the flushes come.** An `ll` value gets a flush of its own, and every sequence ends with one, so
the budget holds at every put:

| put | `cnt` before at most | bits at most | `cnt` after at most |
| --- | --- | --- | --- |
| token and offset | 7 | 31 | 38 |
| `ll` value | 38 | 20 | 58, then a flush leaves 7 |
| `ml` value | 38 | 20 | 58 |

Without the flush between them, an `ll` and an `ml` value after a long escaped token could reach
$7 + 31 + 20 + 20 = 78$ bits.

## Two receivers for one encoder {#ch-encoder--wrappers}

@listing src/seqlz_compress.c 234-247

The matcher calls its receiver with the signature of `emit_fn` ([chapter 9](#ch-matcher--emit)): a
`void *` and the sequence. `rec` has to be a constant at the call, so there are two receivers, one
per constant; `compress_block()` passes `encode_emit()` to the greedy matcher, `compress_own()` passes
`encode_emit_rec()` to the lazy one. Once inlined, the code of the records exists only in level 3's
copy of the loop.

::: why Level 3's records in a copy of the loop of their own
The first version, with the XOR that came before the deltas, checked `e->rec` for every sequence in
the loop all levels share, and the decoder its XOR the same way. Levels 1 and 2 then wrote pages about 6% slower on the Mi 9T's Cortex-A76 and
read them slower too, userspace, 21st October 2026
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#level-3-xors-its-literals-with-an-earlier-byte-of-the-page-38-fewer-bytes-than-seqlz-3-of-15-october-kept)).
With the records only in the lazy matcher's copy, and the decoder's loop copied for pages with deltas
([chapter 14](#ch-decode--deltas)), levels 1 and 2 write and read as fast as before, within the
run's spread.
:::

## encoder_init() {#ch-encoder--init}

@listing src/seqlz_compress.c 249-257

The compound literal zeroes every field it does not name, so `acc` and `cnt` start at 0, `rec` is
false and `pos` 0. `p` starts at the end of `dst`, `lit` after the 2 bytes of the u16, which is
written last. Level 3 sets `rec` after the call; a second `encoder_init()` for a page that did not fit
with its records clears it again.

## encoder_finish(): the bitstream turned around {#ch-encoder--finish}

@listing src/seqlz_compress.c 259-283

`p == lit` only after a failed room check, since a successful one leaves at least 16 bytes between
them. The bits left over, at most 7, are in memory already: the sequence's last flush wrote them on
top of the byte below `p`, with zeros below them, which is exactly the end the format wants
([chapter 4](#ch-format--validity)). So `p` only moves down by one to count that byte. Until 11th
October a store here wrote that byte a second time; without it the bytes were the same on the 600
pages of `test/seqlz_endian.c` and on both desktop dumps (commit `c9db4e5`,
[#180](https://github.com/martinus/quetschn/issues/180)). The u16 for the page of pointers is
{{pointers.lits}} literals, `{{ch10.ptr.u16}}`, with `SEQLZ_LIT_CODED` 0; `seqlz_code_literals()` rewrites it
if the literals get coded.

@listing src/seqlz_compress.c 284-307

Byte `k` of the bitstream is at `end - 1 - k`. A little-endian load of the 8 bytes at `end - 8 - k`
puts the byte at the highest address on top, and a big-endian store writes the top byte to the lowest
address, so the 8 bytes come out reversed: with `gcc -O2` for x86-64 a load, a `bswap` and a store,
with `clang -O2` for arm64 a `rev`. For the page of pointers the last 8 bytes of `dst` are
`{{ch10.ptr.last8}}`, read as `{{ch10.ptr.le64}}` and written as `{{ch10.ptr.head8}}`, the bitstream's
first 8 bytes in order. Only a page that nearly fills `dst` has the two places overlap; then the
bitstream is reversed in place and moved down with `memmove()`. The [animation above](#ch-encoder--layout)
does all of this for chapter 16's page, whose smallest `dst` takes that path.
@assert ch10.slab.min_dst - slab.len < slab.b.bytes

## encode_raw(): sequences from outside, checked first {#ch-encoder--raw}

@listing src/seqlz_compress.c 309-341

`seqlz_encode()` writes a page from sequences the caller made, for tests and tools. A `dst` below
{{ch10.dst_min}} bytes would start `p` below `lit + 32`, or below `lit`, where the room check's cast
would turn a negative room into a huge one. The sequences are checked by the rules a decoder checks
([chapter 14](#ch-decode)) before a byte is written.

::: warn Why a match below 4 bytes is not only invalid but dangerous
`encode_seq()` computes `ml - SEQLZ_MIN_MATCH` unsigned. For `ml` 3 that wraps to {{ch10.wrap.ml4|grp}}, so a
length value follows with {{ch10.wrap.extra}} extra bits, which breaks the accumulator's budget and
the 31 bits per 4 bytes of the room proof. Before commit `256768e` the encoder checked only that the
literals were there, and 1025 such sequences wrote past the two pages of `dst`; AddressSanitizer
reported it in `enc_flush()` ([#121](https://github.com/martinus/quetschn/issues/121)). None of the
fuzzers had found it, because each reached `seqlz_encode()` with valid sequences only.
`fuzz/seqlz_encode_fuzz.c` gives it raw sequences now, and failed within seconds against the old
encoder. `seqlz_compress()` was never affected: its matcher makes only valid sequences.
:::

@listing src/seqlz_compress.c 342-364

`src_end` is the end of the caller's literals, so the 16-byte copies never read past them, and the
last sequence gets `ml` 0 whatever its `match` field says. With a `page`, level 3, the encoder writes
the records as `compress_own()` does: `encode_seq()` is called with `rec` the constant `true`, and
`e.rec` decides; speed does not matter here. If the page does not fit with its records, the loop
starts over with `encoder_init()`, which clears `e.rec`, and as the comment above the test says, the
pass without records is the last, whether the page fits or not.
`seqlz_delta_literals()` then takes the records out again and writes the literals' deltas to `deltas`, the
matcher's state that `seqlz_encode()` passes; `*delta` is the delta it picked, or 0.

## The entry points {#ch-encoder--entry}

The file ends with the public functions. They hand the literals to `seqlz_code_literals()` or
`seqlz_code_own()`, the two ways into `seqlz_compress_literals.c` ([chapter 11](#ch-literals-enc)),
and one helper of that file belongs here:

@listing src/seqlz_compress_literals.c 789-793

For a table of the page's own, the coder needs `struct own_work`, {{own.enc.work_size|grp}} bytes of
counts, lengths, codes and the table's description ([chapter 11](#ch-literals-enc--own)), and
`own_work()` puts it into the matcher's state at byte {{own.enc.work_at}}: `st->work` counts in
`u64`, so `OWN_LITERALS_MAX / 8U` is that byte. `seqlz_code_own()`, level 3's way in, which tries the
deltas first, comes right after it in that file ([chapter 11](#ch-literals-enc--code-own)).

@listing src/seqlz_compress.c 366-389

`seqlz_code_literals()` codes the literals with one of the 8 fixed literal tables if that saves
enough, and returns the new length or `len` unchanged; `seqlz_code_own()` does the same at level 3,
also with a table of the page's own. Both read the literals from a copy while they write the coded
ones over the raw ones in `dst`: `seqlz_compress()` gives them the matcher's state for it, and
`seqlz_encode()`, whose state may be `NULL`, a block on its stack. The kernel has only the first.
At level 3, `encode_raw()` gets the page and the matcher's state, writes the literals' deltas there, and
`seqlz_code_own()` codes them. The level is checked before anything is written, so a level outside 1 to
`SEQLZ_LEVELS`, or level 3 without the state, returns 0 as the header's kernel-doc says
([chapter 6](#ch-header--compressor)).

@listing src/seqlz_compress.c 391-401

`compress_block()` is `encode_raw()` without the checks, since the matcher's sequences are valid by
construction, and its call of `seqlz_match_block()` with `encode_emit` is the whole compression loop of
levels 1 and 2 once inlined, the greedy matcher with `lazy` the constant `false`.

@listing src/seqlz_compress.c 403-438

`compress_own()` is the same for level 3: the lazy matcher, with `encode_emit_rec`, so the records go
in front of the literals. If the page does not fit, `e.p == e.lit`, the loop calls `encoder_init()`
again, which clears `e.rec`, and matches the page once more, now without records; the comment above
the test says it: that pass ends the loop whether the page fits or not. With the records still on,
`seqlz_delta_literals()` picks the delta, moves the literals down over the records and writes their deltas to `st->literals`, and `encoder_finish()`
then finds `dst` as it would without records. It is `noinline`: inlined into `seqlz_compress()` next
to `compress_block()`, levels 1 and 2 wrote their pages slower, as the comment says, without a number
of its own.

@listing src/seqlz_compress.c 440-455

`seqlz_compress()` is what zram calls ([chapter 15](#ch-zram-glue)), with the level of the device:
`seqlz-1`, `seqlz-2` and `seqlz-3` in the harness. Level 1 stops after `compress_block()`. Level 2
passes `st->literals`, the matcher's 8 KiB table as bytes through the union in `struct seqlz_state`,
for the copy of the raw literals; the matcher is done with it, so `seqlz_code_literals()` needs no memory of
its own ([chapter 11](#ch-literals-enc--in-place)). Level 3 hands the delta that `compress_own()` picked
to `seqlz_code_own()`, which also uses `own_work(st)`, the upper half of the same 8 KiB. The text page of
[chapter 4](#ch-format--own) shows what the levels do to one page: {{words.len}} bytes at level 1,
{{words.coded.len}} at level 2 and {{words.l3.len}} at level 3, without deltas; the page of pointers
{{pointers.len}} bytes at level 1, {{delta.ptr.l2.len}} at level 2 and {{delta.ptr.l3.len}} at level 3,
with them.
@assert words.l3.delta == 0 and pointers.l3.delta == 1
The kernel's copy keeps `compress_block()`, `compress_own()` and `seqlz_compress()`, renamed to
`seqlz_compress_block()`, as the kernel's interface function is called `seqlz_compress()`, which tests
the level instead. It drops the `!seqlz_tables.all_symbols` tests, because the kernel's tables are
checked when they are generated ([chapter 15](#ch-zram-glue--init)).
@assert pointers.len == delta.ptr.l2.len

## The page of ab, bit by bit {#ch-encoder--example}

The format chapter takes the {{ab.len}} bytes `{{ab.hex}}` of a page of `ab` 2048 times apart
([chapter 4](#ch-format--example)); here they come into being. The matcher finds {{ab.seqs}}
sequences: at position 2 the slot of `ababa` was never written and holds 0, and the 4 bytes at 0
match, so {{ch10.ab.s1.ll}} literals and a match at offset {{ch10.ab.s1.off}} to the end of the page,
{{ch10.ab.s1.ml}} bytes; the last sequence has 0 literals.
@assert ab.seqs == 2 and ab.lits == ch10.ab.s1.ll == 2 and ch10.ab.s1.ml == 4094

| step | what happens | `acc` after | `cnt` |
| --- | --- | --- | --- |
| literals | 16 bytes `abab...` copied to `d + 2`, `lit` moves on by 2 | | 0 |
| token and offset | token {{ch10.ab.s1.tok}} has no code: the escape's code `{{ch10.ab.s1.esccode}}` and the token's 12 bits `{{ch10.ab.s1.tokbits}}`, then the offset `{{ch10.ab.s1.offbits}}` | `{{ch10.ab.s1.acc}}` | {{ch10.ab.s1.cnt}} |
| `ml` value | code `{{ch10.ab.s1.mlv.code}}`, extra `{{ch10.ab.s1.mlv.xbits}}` | `{{ch10.ab.s1.mlv.acc}}` | {{ch10.ab.s1.mlv.cnt}} |
| flush | `{{ch10.ab.f1.word}}` stored at `end - 8`: `{{ch10.ab.f1.stored}}`; `p` = `end - {{ch10.ab.f1.p}}` | | {{ch10.ab.f1.cnt1}} |
| last sequence | 0 literals, no bits | | {{ch10.ab.f1.cnt1}} |
| finish | the byte below `p` is the last of the bitstream; {{ab.b.bytes}} bytes of bitstream; u16 = {{ab.lits}} | | |

`encoder_finish()` copies the {{ab.b.bytes}} bytes one at a time from `end - 1` down, `{{ab.b.hex}}`,
right after `61 62`: {{ab.len}} bytes. The 14 more bytes of `abab` the literal copy wrote are
overwritten by the bitstream or lie beyond the {{ab.len}} bytes returned. In the order the decoder
reads them, the {{ab.b.bits}} bits are `{{ch10.ab.s1.esccode}}` (the escape) `{{ch10.ab.s1.tokbits}}`
(token {{ch10.ab.s1.tok}}) `{{ch10.ab.s1.offbits}}` (offset {{ch10.ab.s1.off}}, sent as 1)
`{{ch10.ab.s1.mlv.code}}` (symbol {{ch10.ab.s1.mlv.sym}} of the `ml` table)
`{{ch10.ab.s1.mlv.xbits}}` ({{ch10.ab.s1.mlv.x}}).
@assert ab.b.bytes == ch10.ab.f1.moved + 1 and ab.b.pad == 8 - ch10.ab.f1.cnt1
@assert ch10.ab.s1.putbits + ch10.ab.s1.mlv.code + ch10.ab.s1.mlv.xbits == ab.b.bin

::: try Compress a page yourself
In the repository, save this as `ab.c` and build it with `gcc -O2 -Isrc -o ab ab.c src/seqlz_compress.c
src/seqlz_compress_literals.c src/seqlz_codes.c src/seqlz_default_tables.c src/seqlz_lit_sets.c`:

```c
#include "seqlz.h"
#include <stdio.h>
#include <stdlib.h>

static struct seqlz_state st;
static u8 page[SEQLZ_BLOCK_SIZE], dst[2 * SEQLZ_BLOCK_SIZE];

int main(void)
{
	unsigned int i, len;

	for (i = 0; i < SEQLZ_BLOCK_SIZE; i++)
		page[i] = i & 1 ? 'b' : 'a';
	len = seqlz_compress(&st, page, dst, sizeof(dst), 1);
	for (i = 0; i < len; i++)
		printf("%02x ", dst[i]);
	printf("\n");
	return 0;
}
```
@program ch10-ab

It prints:

@output ch10-ab

With level 2 or 3 instead of 1 the result is the same: {{ab.lits}} literals are too few to save the
{{ch11.coded_min}} bytes coded literals must save.
@assert ab.coded.len == ab.len == ab.l3.len
:::

What this design gives up is the buffer of one page. The page is written with raw literals first, so a
page whose raw form does not fit fails, also where its coded form would. With a `dst` of exactly one
page, as zswap gives, that costs 0.2% more memory on desktop and phone pages
([explored-designs.md](https://github.com/martinus/quetschn/blob/main/docs/explored-designs.md#coded-literals-from-the-matchers-table-a-buffer-of-one-page-costs-01-to-05-instead-of-16-to-29-kept)).
zram gives two pages, and there it costs nothing.
