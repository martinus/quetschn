/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef _LINUX_SEQLZ_H
#define _LINUX_SEQLZ_H

#include "seqlz_compat.h"

/*
 * seqlz: an LZ format for memory pages whose sequences are Huffman coded with static tables. The why
 * of each choice below, with the numbers, is in docs/explored-designs.md.
 *
 * A sequence is a literal length ll, a match length ml and an offset; the last sequence of a page has
 * no match. ll and ml - 4 share one symbol with the class of the offset, the token:
 *   token:              min(ll, 15) + 16 * min(ml - 4, 31) + 512 * class, 3072 symbols, Huffman coded
 *                       with at most SEQLZ_TOKEN_BITS bits; the rare ones have no code and are sent as
 *                       the escape (SEQLZ_ESCAPE) and SEQLZ_ESCAPE_BITS raw bits.
 *   offset class:       0 repeats the last offset (initially 1); 1 to 3 is an offset below 16, 256 or
 *                       the page size; 4 and 5 are multiples of 8 from 16 on, below 256 or the page size.
 *   offset:             SEQLZ_RAW_BITS(class) raw bits right after the token, divided by 8 for classes
 *                       4 and 5, so the decoder needs one table lookup per sequence.
 *   ll >= 15:           ll - 15 follows as a length value; ml - 4 >= 31: ml - 4 - 31
 *   length value v:     v < 16 is the symbol itself; otherwise b = bit_width(v) - 1, the symbol is 12 +
 *                       b, and the b low bits of v follow as extra bits. ll and ml have their own tables
 *                       of at most SEQLZ_MAX_BITS bits.
 * Everything goes into one bitstream, read most significant bit first, as the literals' streams: per
 * sequence the token, the offset, then the length values if any, each symbol followed by its extra bits.
 * The last sequence's token has ml - 4 = 0 and class 0. There is no count of the sequences: the last one
 * is the one whose literals fill the page.
 *
 * Page layout, all little endian, with the literals raw:
 *   u16 number of literals, the literals, the bitstream (the rest)
 * or with the literals Huffman coded, where that saves at least 1/16 of them:
 *   u16 SEQLZ_LIT_CODED | number of literals, u8 which of the SEQLZ_LIT_SETS literal tables | (w - 5) << 3,
 *   the sizes of the literals' 8 bitstreams in w bits each (SEQLZ_LIT_HEADER), those streams, the
 *   sequences' bitstream (the rest). Literal k is in stream k % 8, most significant bit first, with
 *   canonical codes of at most SEQLZ_LIT_BITS bits.
 */

#ifndef QUETSCHN_PAGE_BITS
#    define QUETSCHN_PAGE_BITS 12 /* see page_lz.h */
#endif
#define SEQLZ_PAGE (1U << QUETSCHN_PAGE_BITS)
#define SEQLZ_MAX_BITS 8U /* for length values; tables of 1 KiB each, 9 bits were 80 ns slower when cold */
/* With the token table prefetched only on in-order cores, 10 bits gave the A76 nothing and the A55 0.8 to
 * 1.1 us per page written, for 4.8 bytes per page ("Six choices made on the PC, measured on the phone", 1
 * and 7). */
#define SEQLZ_TOKEN_BITS 11U
/* The bits of ll and ml - 4 in the token, at most 4 and 5: token_entry() in seqlz.c has 4 bits for ll and
 * 6 for ml. With 4 and 5 a match length value follows 8.5% of the matches instead of 18.7% ("Second
 * round: branches, tails and table sizes"); 3 / 5 and 4 / 4 are in "seqlz, third decoder round". */
#define SEQLZ_LL_BITS 4U
#define SEQLZ_ML_BITS 5U
/* the classes of the comment above; two more gave 0.1 points at most in a model ("Offset classes from a
 * histogram") */
#define SEQLZ_OFF_CLASSES 6U
#define SEQLZ_TOKEN_SYMBOLS (SEQLZ_OFF_CLASSES << (SEQLZ_LL_BITS + SEQLZ_ML_BITS))
/* A token without a code is sent as the escape's code and SEQLZ_ESCAPE_BITS raw bits of the token.
 * Codes of at most 11 bits have room for 2048 tokens, there are 3072: only the frequent ones get a code.
 * Already with 1536 tokens a code for each needed 75% of the code space ("seqlz, third decoder round"). */
#define SEQLZ_ESCAPE SEQLZ_TOKEN_SYMBOLS
#define SEQLZ_ESCAPE_BITS 12U /* the fewest bits that hold every token, 3072 < 4096 */
/* an escaped token and the largest offset in at most 31 bits, the encoder's bound for two pages */
#define SEQLZ_MAX_ESCAPE_LEN (31U - QUETSCHN_PAGE_BITS - SEQLZ_ESCAPE_BITS)
#define SEQLZ_LL_CAP ((1U << SEQLZ_LL_BITS) - 1U)    /* in the token, larger literal lengths follow as a value */
#define SEQLZ_ML_CAP ((1U << SEQLZ_ML_BITS) - 1U)    /* the same for ml - 4 */
#define SEQLZ_LEN_DIRECT 16U                         /* length values below it are their own symbol */
#define SEQLZ_LEN_SYMBOLS (13U + QUETSCHN_PAGE_BITS) /* 16 direct values, then buckets 4 to page bits */
#define SEQLZ_HEADER 2U
#define SEQLZ_LIT_CODED 0x8000U /* in the u16 at the start of a page with coded literals */

/* The code lengths of the three tables, 0 for a symbol that never occurs. This is what training
 * produces; the ones compiled in, seqlz_default_own, are part of the format. */
struct seqlz_lengths {
    u8 token[SEQLZ_TOKEN_SYMBOLS + 1]; /* the last one is the escape, see SEQLZ_ESCAPE */
    u8 ll[SEQLZ_LEN_SYMBOLS];
    u8 ml[SEQLZ_LEN_SYMBOLS];
};

/* symbol and extra bits of a length, see above */
static inline unsigned int seqlz_len_symbol(unsigned int v, unsigned int* extra_bits) {
    unsigned int b;

    if (v < SEQLZ_LEN_DIRECT) {
        *extra_bits = 0;
        return v;
    }
    b = 31U - (unsigned int)__builtin_clz(v);
    *extra_bits = b;
    return 12U + b; /* b is at least 4 here, its bucket is symbol 16 */
}

/* The raw bits of an offset class, one nibble per class: 0, 4, 8, the page's bits, 5, the page's bits - 3
 * (docs/format.md, Offsets); 0x95C840 for 4 KiB pages, 0xB5E840 for 16 KiB. From a packed constant: the
 * multiply by (class == 3) made gcc branch on the class for 16 KiB pages. */
#define SEQLZ_RAW_BITS(cls) (((0x50840U | QUETSCHN_PAGE_BITS << 12 | (QUETSCHN_PAGE_BITS - 3U) << 20) >> (4U * (cls))) & 15U)
/* classes 4 and 5 send the offset divided by 8 */
#define SEQLZ_OFF_SHIFT(cls) (((cls) >> 2) * 3U)

/* The class of an offset and its raw bits, see above; offsets are below the page size. Without a
 * branch: the encoder calls it for every match. */
static inline unsigned int seqlz_off_class(unsigned int off, unsigned int last, unsigned int* raw_bits) {
    unsigned int is_new = (off == last) - 1U, aligned = (off >= 16U) & ((off & 7U) == 0);
    /* 1 to 3 by size; a multiple of 8 from 16 on moves class 2 or 3 to 4 or 5 */
    unsigned int cls = (1U + (off >= 16U) + (off >= 256U) + 2U * aligned) & is_new;

    *raw_bits = SEQLZ_RAW_BITS(cls);
    return cls;
}

/* A sequence as the matcher found it. */
struct seqlz_sequence {
    u16 literals;
    u16 match; /* 0 for the last sequence */
    u16 offset;
};

/* Encoder and decoder state for one set of tables. Opaque, seqlz_tables_size() bytes. */
struct seqlz_tables;

size_t seqlz_tables_size(void);

/* true if every symbol has a code, which the encoder needs; complete prefix codes can leave symbols out. */
bool seqlz_all_symbols(const struct seqlz_tables* t);

/* Builds encode codes and decode tables from code lengths. -EINVAL if the lengths are not a valid prefix
 * code: longer than SEQLZ_MAX_BITS, over-subscribed, or no symbol at all. */
int seqlz_tables_init(struct seqlz_tables* t, const struct seqlz_lengths* lengths);

/* The literal tables, in seqlz_lit_sets_4k.inc and _16k.inc, trained as seqlz_default_tables.c says.
 * 9 bits were no faster on the phone and up to 64 bytes per page larger ("Six choices made on the PC,
 * measured on the phone", 2). 16 tables saved 2 and 8 bytes per page and read cold 0.26 and 0.28 us
 * slower in the kernel ("seqlz-fast-lit by the score: no budget, offsets in steps of 8"). */
#define SEQLZ_LIT_BITS 10U
#define SEQLZ_LIT_SETS 8U
/* Literal k is in stream k % 8, so that the decoder has 8 chains side by side. 4 streams made pages 2.7
 * bytes smaller and were no faster on the phone ("Six choices made on the PC, measured on the phone",
 * 3). */
#define SEQLZ_LIT_STREAMS 8U
extern const u8 seqlz_lit_sets[SEQLZ_LIT_SETS][256];
#define SEQLZ_LIT_ROUNDS (56U / SEQLZ_LIT_BITS) /* literals per stream and refill: a refill leaves 56 bits */
/* The header of a page with coded literals: the 2 bytes of every page, a byte with the literal table in
 * bits 0 to 2, w - SEQLZ_SIZE_BITS_MIN in bits 3 to 5 and bits 6 and 7 zero, then the 8 stream sizes of
 * w bits each, lowest bit first, in w bytes. w is 5 to 12: a stream has at most every 8th literal, of at
 * most SEQLZ_LIT_BITS bits, 640 bytes in a 4 KiB page and 2560 in a 16 KiB page. The encoder takes the
 * smallest w from 5 on: sizes of 2 bytes each made pages 5.2 and 5.5 bytes larger on the two dumps, and
 * widths of 3 and 4 bits were 0.1% to 3% of the pages. */
#define SEQLZ_LIT_HEADER(w) (3U + (w))
#define SEQLZ_SIZE_BITS_MIN 5U
#define SEQLZ_SIZE_BITS_MAX 12U
/* Where the fields of byte 2 start: the literal table at bit 0, w - SEQLZ_SIZE_BITS_MIN at bit 3, the
 * bits that must be zero at bit 6. The two fields have 3 bits each, masked with SEQLZ_LIT_SETS - 1 and
 * SEQLZ_SIZE_BITS_MAX - SEQLZ_SIZE_BITS_MIN. */
#define SEQLZ_LIT_WIDTH_AT 3
#define SEQLZ_LIT_ZERO_AT 6
/* The encoder codes literals only where that saves 1/16 of them and 51 bytes. A page with coded literals
 * costs a fixed time to read, its literal table and the buffer they are decoded into: on phone pages 51
 * instead of 19 bytes made a page 7.9 bytes larger and the time per page written 2.7 us shorter on the
 * A55, 1.1 us on the A76; on the PC 11 bytes for 0.1 us. */
#define SEQLZ_LIT_CODED_MIN 51U
/* where the decoder decodes coded literals: 16 bytes behind them for the literal copies, which read 16
 * bytes at a time */
#define SEQLZ_SCRATCH (SEQLZ_PAGE + 16U)

/* Writes the page for these sequences and literals, the last sequence with match 0; with coded set,
 * the literals coded where that pays. dst_cap must be at least two pages. Returns the length, or 0 if
 * dst_cap is smaller, the tables lack a code for some symbol, or the sequences do not fit a page. */
unsigned int seqlz_encode(const struct seqlz_tables* t,
                          const struct seqlz_sequence* seq,
                          unsigned int n,
                          const u8* literals,
                          unsigned int n_literals,
                          void* dst,
                          unsigned int dst_cap,
                          bool coded);

/* 0 on success, -EINVAL if src is not a valid page for these tables. Never reads outside
 * [src, src + src_len) and never writes outside [dst, dst + SEQLZ_PAGE) and the scratch. scratch has
 * SEQLZ_SCRATCH bytes; without one (NULL), pages with coded literals are not valid. */
int seqlz_decode(const struct seqlz_tables* t, const void* src, unsigned int src_len, void* dst, void* scratch);

/* the token of a sequence, see above */
static inline unsigned int seqlz_token(unsigned int ll, unsigned int ml, unsigned int cls) {
    unsigned int a = min(ll, SEQLZ_LL_CAP);
    unsigned int b = ml ? min(ml - 4, SEQLZ_ML_CAP) : 0;

    return a + (b << SEQLZ_LL_BITS) + (cls << (SEQLZ_LL_BITS + SEQLZ_ML_BITS));
}

/*
 * The compressor: its own matcher (src/page_lz.h), and the sequences coded straight into dst. The
 * state is per CPU, the matcher's hash table, cleared for each page.
 */
#define SEQLZ_HASH_BITS (QUETSCHN_PAGE_BITS == 12 ? 12U : 13U) /* PAGE_LZ_HASH_BITS of page_lz.h, seqlz.c checks it */
/* every sequence but the last has a match of at least 4 bytes */
#define SEQLZ_MAX_SEQUENCES (SEQLZ_PAGE / 4U + 1U)

struct seqlz_state {
    u16 table[1U << SEQLZ_HASH_BITS];
};

/* The sequences of a page as the matcher finds them, the last one without a match. Returns their
 * number. The literals are the bytes of the page that no match covers. */
unsigned int seqlz_find(struct seqlz_state* st, const void* src, struct seqlz_sequence* seq);

/* Compresses one page, the same bytes as seqlz_encode() for seqlz_find()'s sequences. dst_cap must be
 * at least two pages, as zram's buffer is, which is always enough (see the encoder in seqlz.c). Returns
 * the length, or 0 if dst_cap is smaller or the tables lack a code for some symbol. */
unsigned int seqlz_compress(
    const struct seqlz_tables* t, struct seqlz_state* st, const void* src, void* dst, unsigned int dst_cap, bool coded);

/* the tables compiled in, see src/seqlz_default_tables.c for the pages they are trained on */
extern const struct seqlz_lengths seqlz_default_own;

#endif
