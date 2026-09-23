/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_SEQLZ_H
#define QUETSCHN_EXPLORE_SEQLZ_H

/*
 * seqlz: an LZ format for 4 KiB pages whose sequences are Huffman coded with static tables, and whose
 * literals are raw. docs/explored-designs.md, "Where the ratio of zstd comes from", estimated about 25%
 * Σ zsmalloc cost for this; the prototype measures what it really gets and how fast it decodes.
 *
 * A sequence is a literal length ll, a match length ml and an offset; the last sequence of a page has
 * no match. Like lz4's token, ll and ml - 4 share one symbol, each capped at 15:
 *   token:              min(ll, 15) + 16 * min(ml - 4, 15), 256 symbols, Huffman coded with at most
 *                       SEQLZ_TOKEN_BITS bits
 *   ll >= 15:           ll - 15 follows as a length value; the same for ml - 4 >= 15
 *   length value v:     v < 16 is the symbol itself; otherwise b = bit_width(v) - 1, the symbol is 12 +
 *                       b, and the b low bits of v follow as extra bits
 *   offset:             symbols 0 to 2 repeat the first, second or third of the last three offsets
 *                       (initially 1, 4, 8), which moves it to the front; otherwise b = bit_width(off)
 *                       - 1, the symbol is 3 + b, and the b low bits of off follow
 * The length values of ll and of ml and the offsets have Huffman tables of at most SEQLZ_MAX_BITS
 * bits. Everything goes into one bitstream, read least significant bit first: per sequence the token,
 * the length values if any, then the offset, each symbol followed by its extra bits. One token and not
 * two length symbols, because each symbol is a table lookup on the chain of dependent steps of the
 * decoder; one stream and not three, because the state of three bit readers does not fit into the
 * registers of x86-64 (docs/explored-designs.md). The last sequence's token has ml - 4 = 0.
 *
 * Page layout, all little endian:
 *   u16 sequences, u16 literal bytes, literals, the bitstream (the rest)
 *
 * The prototype does not find matches itself: it takes them from the kernel's lz4 or lz4hc. So its
 * compression time says nothing yet; its size and its decode time do.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define SEQLZ_PAGE 4096U
#define SEQLZ_MAX_BITS 10U
#define SEQLZ_TOKEN_BITS 11U
#define SEQLZ_TOKEN_SYMBOLS 256U
#define SEQLZ_LEN_SYMBOLS 25U /* 16 direct values, then buckets 4 to 12 */
#define SEQLZ_OFF_SYMBOLS 15U /* 3 repeats, then buckets 0 to 11 */
#define SEQLZ_HEADER 4U

/* The code lengths of the four tables, 0 for a symbol that never occurs. This is what training
 * produces and what zram's dictionary parameter can carry: 321 bytes. */
struct seqlz_lengths {
    unsigned char token[SEQLZ_TOKEN_SYMBOLS];
    unsigned char ll[SEQLZ_LEN_SYMBOLS];
    unsigned char ml[SEQLZ_LEN_SYMBOLS];
    unsigned char off[SEQLZ_OFF_SYMBOLS];
};

/* symbol and extra bits of a length, see above */
static inline unsigned int seqlz_len_symbol(unsigned int v, unsigned int* extra_bits) {
    unsigned int b;

    if (v < 16) {
        *extra_bits = 0;
        return v;
    }
    b = 31U - (unsigned int)__builtin_clz(v);
    *extra_bits = b;
    return 12U + b;
}

static inline unsigned int seqlz_off_bucket(unsigned int off, unsigned int* extra_bits) {
    unsigned int b = 31U - (unsigned int)__builtin_clz(off);

    *extra_bits = b;
    return 3U + b;
}

/* A sequence as the matcher found it. */
struct seqlz_sequence {
    unsigned short literals;
    unsigned short match; /* 0 for the last sequence */
    unsigned short offset;
};

/* Encoder and decoder state for one set of tables. Opaque, seqlz_tables_size() bytes. */
struct seqlz_tables;

__SIZE_TYPE__ seqlz_tables_size(void);

/* Builds encode codes and decode tables from code lengths. -1 if the lengths are not a valid prefix
 * code: longer than SEQLZ_MAX_BITS, over-subscribed, or no symbol at all. */
int seqlz_tables_init(struct seqlz_tables* t, const struct seqlz_lengths* lengths);

/* Writes the page for these sequences and literals. Returns the length, or 0 if dst_cap is too small
 * or a symbol has no code in these tables. */
unsigned int seqlz_encode(const struct seqlz_tables* t,
                          const struct seqlz_sequence* seq,
                          unsigned int n,
                          const unsigned char* literals,
                          unsigned int n_literals,
                          void* dst,
                          unsigned int dst_cap);

/* 0 on success, -1 if src is not a valid page for these tables. Never reads outside
 * [src, src + src_len) and never writes outside [dst, dst + SEQLZ_PAGE). */
int seqlz_decode(const struct seqlz_tables* t, const void* src, unsigned int src_len, void* dst);

/* the token of a sequence, see above */
static inline unsigned int seqlz_token(unsigned int ll, unsigned int ml) {
    unsigned int a = ll < 15 ? ll : 15, b = ml < 19 ? ml - 4 : 15;

    return a + 16U * (ml == 0 ? 0 : b);
}

/* The tables compiled in, trained on resident pages (explore/seqlz_default_tables.c). */
extern const struct seqlz_lengths seqlz_default_lz4;
extern const struct seqlz_lengths seqlz_default_lz4hc;

#ifdef __cplusplus
}
#endif

#endif
