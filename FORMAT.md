# The seqlz format

seqlz compresses one memory page into one compressed page. This file describes the bytes, so that a
decoder can be written from it alone, and it says which compressed pages are valid. It describes the
format as `explore/seqlz.c` writes and reads it today; where the code decides something that a format
should decide on purpose, [Open points](#open-points-before-the-format-is-frozen) lists it. Nothing
here is frozen yet.

`seqlz-fast` and `seqlz-fast-lit` are the same format: `seqlz-fast` always stores the literals raw,
`seqlz-fast-lit` codes them where that pays. A decoder for one decodes both.

`tools/seqlz_ref.py` is a decoder written from this file, slow on purpose. On 7570 inputs, the queues
of AFL++'s decode target and the pages of its roundtrip target compressed with raw and coded literals,
it agreed with `seqlz_decode()` on every one: 5343 valid with the same page, 2227 invalid. Without the
check of the bitstream's end it accepted 5 of them that `seqlz_decode()` rejects.

## Parameters

| name | 4 KiB pages | 16 KiB pages | what it is |
| --- | --- | --- | --- |
| `P` | 12 | 14 | log2 of the page size |
| `PAGE` | 4096 | 16384 | bytes of the uncompressed page |
| `LEN_SYMBOLS` | 25 | 27 | symbols of a length value, `13 + P` |
| `RAW_BITS(c)` for class `c` = 0..5 | 0, 4, 8, 12, 5, 9 | 0, 4, 8, 14, 5, 11 | raw bits of an offset |
| `SHIFT(c)` | 0, 0, 0, 0, 3, 3 | the same | the raw value is shifted left by this |

The other numbers are the same for both page sizes: tokens are 3072 symbols plus the escape, token codes
have at most 11 bits, length value codes at most 8, literal codes at most 10, there are 8 literal
tables, and an escaped token has 12 raw bits.

A compressed page is a byte string of length `len` >= 0. Its length is not in it: zram stores it, and the
decoder gets it. All multi-byte numbers are little endian.

## Layout of a compressed page

The first two bytes are a `u16`, `h`.

**Raw literals**, bit 15 of `h` clear:

| offset | size | content |
| --- | --- | --- |
| 0 | 2 | `h` = `n`, the number of literals |
| 2 | `n` | the literals |
| `2 + n` | the rest | the sequences' bitstream |

The page is invalid if `len < 2` or `2 + n > len`.

**Coded literals**, bit 15 of `h` set:

| offset | size | content |
| --- | --- | --- |
| 0 | 2 | `h` = `0x8000` or `n`, the number of literals |
| 2 | 1 | `t`, which literal table, 0 to 7 |
| 3 | 16 | `s[0..7]`, 8 `u16`: the size in bytes of each literal stream |
| 19 | `s[0]` + ... + `s[7]` | the 8 literal streams, one after the other |
| `19 + s[0] + ... + s[7]` | the rest | the sequences' bitstream |

The page is invalid if `len < 19`, `n > PAGE`, `t > 7`, or `19 + s[0] + ... + s[7] > len`. Stream `j`
starts at `19 + s[0] + ... + s[j-1]`. How the literals come out of the streams is in
[Coded literals](#coded-literals).

The literals are the bytes of the page that no match covers, in page order. The sequences' bitstream
says how many of them come next, and which match follows.

## Prefix codes

Tokens, length values and literals are prefix codes, each defined by a table of code lengths: one per
symbol, 0 for a symbol without a code. The codes are canonical, as in deflate (RFC 1951, 3.2.2): count
the symbols of each length, the first code of length `l` is `next[l] = (next[l-1] + count[l-1]) << 1`
with `count[0] = 0` and `next[0] = 0`, and the symbols get the codes of their length in the order of
their number, each the next one.

Every table must be a complete prefix code: the sum of `2^-l` over its codes is exactly 1, and no
length is above the table's maximum. Then every string of bits that is long enough starts with exactly
one code. A decoder may rely on that, a set of tables that is not complete is not seqlz.

A code is always sent with its first bit, the most significant one of the number, first. What "first"
means depends on the stream: in the sequences' bitstream the bits are read least significant first
within each byte, in the literal streams most significant first.

## The sequences' bitstream

Bit `i` of the bitstream `B` is bit `i mod 8` of byte `i div 8`, bit 0 the least significant one. A
decoder keeps a position `pos`, starting at 0:

- `bit()`: bit `pos` of `B`, 0 if `pos >= 8 * |B|`, then `pos += 1`.
- `read(n)`: `n` bits as a number, the first one the least significant: the sum of `bit() << k` for
  `k = 0 .. n-1`. `read(0)` is 0.
- `symbol(table)`: the bits one by one until they are a code of the table, the first bit the most
  significant of the code; the symbol of that code.

Bits past the end read as 0, and a page that uses them is invalid: at the end, `pos <= 8 * |B|` must
hold.

A **length value** of the literal length table `LL` or the match length table `ML`:

    s = symbol(table)
    if s < 16:  v = s
    else:       b = s - 12;  v = 2^b + read(b)

**Tokens.** The token table `TOK` has 3073 symbols: 0 to 3071 are tokens, 3072 is the escape. A token
`t` holds three fields:

    ll  = t & 15            the literal length, 15 means at least 15
    mlf = (t >> 4) & 31     the match length - 4, 31 means at least 31
    c   = t >> 9            the class of the offset, 0 to 5

`next_token()` reads one:

    t = symbol(TOK)
    if t == 3072:
        t = read(12)
        if t >= 3072: invalid

An offset class says where the offset comes from: class 0 is the last offset again, the others send
`RAW_BITS(c)` raw bits, shifted left by `SHIFT(c)`.

## Decoding

`lits` are the literals from the layout, `n` of them, `out` the page being written. `last` starts at 1.

    loop:
        t = next_token(); ll, mlf, c = the fields of t
        raw = read(RAW_BITS(c))
        off = (c == 0) ? last : raw << SHIFT(c)
        if ll == 15: ll = 15 + value(LL)
        if ll > literals left or ll > PAGE - |out|: invalid
        append the next ll literals to out
        if |out| == PAGE: stop
        ml = mlf + 4
        if mlf == 31: ml = 35 + value(ML)
        last = off
        if off == 0 or off > |out| or ml > PAGE - |out|: invalid
        append ml bytes to out, each the byte off positions before it    (off < ml repeats)

    at the stop: all n literals used and pos <= 8 * |B|, else invalid

The bits of a sequence are, in this order: the token (with the escape and its 12 bits), the raw bits of
the offset, the literal length value if `ll` was 15, the match length value if `mlf` was 31.

The page ends with the sequence whose literals fill it. That sequence still has a token, and its class
and raw offset bits are read; its `mlf` and its offset are not used. A page whose last match ends at the
end of the page has one more token with `ll = 0`. A page always ends after literals, never after a
match.

## Coded literals

Literal `k`, for `k = 0 .. n-1`, is in stream `k mod 8`, as its symbol number `k div 8` there. A stream
is read most significant bit first: bit `i` of stream `j` is bit `7 - (i mod 8)` of the page's byte
`start[j] + i div 8`. The stream does not stop at `s[j]`: past it the bits are those of the next bytes of
the page, the following streams and the sequences' bitstream, and past the end of the page they are 0.

Each stream is decoded with literal table `t` (`seqlz_lit_sets[t]`, 256 symbols, codes of at most 10
bits) for `5 * R` symbols, where `R = ceil(n / 40)`. The symbols numbered `n div 8` and up in a stream
that has no literal `k < n` there are decoded and thrown away. The page is invalid if, for some stream
`j`, those `5 * R` symbols together have more than `8 * s[j] + 50` bits.

This rule comes from the decoder, which decodes 8 streams side by side in rounds of 5 symbols each,
see [Open points](#open-points-before-the-format-is-frozen).

## Tables

The tables are not in the compressed page. Compressor and decompressor have to use the same ones; seqlz
in zram uses the ones compiled in:

| table | symbols | max bits | 4 KiB pages | 16 KiB pages |
| --- | --- | --- | --- | --- |
| tokens and escape `TOK` | 3073 | 11 | `explore/seqlz_default_tables.c`, `.token` | `explore/seqlz_default_tables_16k.inc`, `.token` |
| literal length `LL` | `LEN_SYMBOLS` | 8 | the same, `.ll` | the same, `.ll` |
| match length `ML` | `LEN_SYMBOLS` | 8 | the same, `.ml` | the same, `.ml` |
| literal tables 0 to 7 | 256 each | 10 | `explore/seqlz_lit_sets.c` | the same |

`TOK`, `LL` and `ML` can be replaced: zram's dictionary parameter, if it is exactly a `struct
seqlz_lengths` (3123 bytes for 4 KiB pages, 3127 for 16 KiB), carries other code lengths. The literal tables are fixed.

## What the compressor writes

Not part of the format, any page that decodes is fine. For reference, `seqlz_compress()` writes:

- the matches of a greedy matcher (`explore/page_lz.h`), each with class 0 when the offset is the last
  one, else class 4 or 5 for a multiple of 8 from 16 on, else the smallest of 1 to 3 that holds it;
- tokens without a code as the escape, so `TOK` needs a code for the escape or for every token, and
  `LL` and `ML` a code for every symbol;
- for the last sequence a token with `mlf = 0` and class 0;
- zero bits to fill the last byte of the bitstream and of each literal stream, and nothing after it;
- coded literals only when they save at least `n / 16` bytes against raw ones, with the table that
  codes them in the fewest bits.

The compressed page is at most `2 * PAGE` bytes, which is the buffer zram gives the compressor. zram
stores a page that does not compress well enough uncompressed, so it never reaches the decoder.

## Open points before the format is frozen

Things the format allows today because of how the decoder is built, not because they were chosen:

1. **Literal streams borrow bits.** The rule above lets a stream's real literals use up to 50 bits past
   `s[j]`, as long as the symbols decoded and thrown away leave room, and the symbols thrown away
   depend on the bytes that follow. A rule like "the codes of the real literals fit into `8 * s[j]`
   bits" is simpler to state and to check in a second decoder, if the fast decoder can check it as
   cheaply.
2. **Bytes after the bitstream are ignored.** A page with any number of extra bytes at the end
   decodes. zram knows the length, so nothing needs them.
3. **The last sequence's unused fields.** Its class and raw offset bits are read but not used, and its
   `mlf` can be anything. The compressor always writes class 0 and `mlf = 0`.
4. **One page, several encodings.** A token that has a code can also be sent escaped, class 1 can send
   an offset class 2 could, class 4 can send an offset of 8, and a coded page can have 0 literals and
   streams of any size. All of these decode.
5. **No version in the page.** The page does not say which tables it was written with or which version
   of the format it is. zram stores the algorithm's name per device, so a new format can be a new name.
6. **The literal tables are an experiment.** `seqlz_lit_sets.c` says so; they are trained on one
   machine's pages.
