/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_SEQLZ_H
#define QUETSCHN_EXPLORE_SEQLZ_H

/*
 * seqlz: an LZ format for 4 KiB pages whose sequences are Huffman coded with static tables, and whose
 * literals are raw. docs/explored-designs.md, "Where the ratio of zstd comes from", estimated about 25%
 * Σ zsmalloc cost for this; the prototype measures what it really gets and how fast it decodes.
 *
 * A sequence is a literal length, a match length and an offset; the last sequence of a page has no
 * match. Each of the three becomes a symbol and extra bits:
 *   literal length ll:  ll < 16 is the symbol itself; otherwise b = bit_width(ll) - 1, the symbol is
 *                       12 + b, and the b low bits of ll follow as extra bits
 *   match length ml:    the same for ml - 4
 *   offset:             symbols 0 to 2 repeat the first, second or third of the last three offsets
 *                       (initially 1, 4, 8), which moves it to the front; otherwise b = bit_width(off)
 *                       - 1, the symbol is 3 + b, and the b low bits of off follow
 * Each kind has its own Huffman table of at most SEQLZ_MAX_BITS bits per code, and its own bitstream,
 * read least significant bit first, the extra bits right after their symbol.
 *
 * Page layout, all little endian:
 *   u16 sequences, u16 literal bytes, u16 bytes of the ll stream, u16 bytes of the ml stream
 *   literals, ll stream, ml stream, offset stream (the rest)
 *
 * The prototype does not find matches itself: it takes them from the kernel's lz4 or lz4hc. So its
 * compression time says nothing yet; its size and its decode time do.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define SEQLZ_PAGE 4096U
#define SEQLZ_MAX_BITS 10U
#define SEQLZ_LEN_SYMBOLS 25U /* 16 direct values, then buckets 4 to 12 */
#define SEQLZ_OFF_SYMBOLS 15U /* 3 repeats, then buckets 0 to 11 */
#define SEQLZ_HEADER 8U

/* The code lengths of the three tables, 0 for a symbol that never occurs. This is what training
 * produces and what zram's dictionary parameter can carry: 65 bytes. */
struct seqlz_lengths {
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

/* The tables compiled in, trained on resident pages (explore/seqlz_default_tables.h). */
extern const struct seqlz_lengths seqlz_default_lz4;
extern const struct seqlz_lengths seqlz_default_lz4hc;

#ifdef __cplusplus
}
#endif

#endif
