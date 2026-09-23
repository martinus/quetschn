/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_SEQLZ_H
#define QUETSCHN_EXPLORE_SEQLZ_H

/*
 * seqlz: an LZ format for 4 KiB pages whose sequences are Huffman coded with static tables, and whose
 * literals are raw. docs/explored-designs.md, "Where the ratio of zstd comes from", estimated about 25%
 * Σ zsmalloc cost for this; the prototype measures what it really gets and how fast it decodes.
 *
 * A sequence is a literal length ll, a match length ml and an offset; the last sequence of a page has
 * no match. Like lz4's token, ll and ml - 4 share one symbol, capped at 15 and 31:
 *   token:              min(ll, 15) + 16 * min(ml - 4, 31), 512 symbols, Huffman coded with at most
 *                       SEQLZ_TOKEN_BITS bits. 31 and not 15 for the match length, because with 15
 *                       every fifth match needed a length value, and that branch mispredicted. 11 and
 *                       not 12 bits, because the decode table of 12 bits, 8 KiB, made the cold decode
 *                       slower: the tables of the decoder have to stay in L1.
 *   ll >= 15:           ll - 15 follows as a length value; ml - 4 >= 31: ml - 4 - 31
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
#define SEQLZ_MAX_BITS 9U /* for length values and offsets; tables of 2 KiB each */
#define SEQLZ_TOKEN_BITS 11U
#define SEQLZ_TOKEN_SYMBOLS 512U
#define SEQLZ_LL_CAP 15U      /* in the token, larger literal lengths follow as a value */
#define SEQLZ_ML_CAP 31U      /* the same for ml - 4 */
#define SEQLZ_LEN_SYMBOLS 25U /* 16 direct values, then buckets 4 to 12 */
#define SEQLZ_OFF_SYMBOLS 15U /* 3 repeats, then buckets 0 to 11 */
#define SEQLZ_HEADER 4U

/* The code lengths of the four tables, 0 for a symbol that never occurs. This is what training
 * produces and what zram's dictionary parameter can carry: 577 bytes. */
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

/* 1 if every symbol has a code, which the encoder needs; complete prefix codes can leave symbols out. */
int seqlz_all_symbols(const struct seqlz_tables* t);

/* Builds encode codes and decode tables from code lengths. -1 if the lengths are not a valid prefix
 * code: longer than SEQLZ_MAX_BITS, over-subscribed, or no symbol at all. */
int seqlz_tables_init(struct seqlz_tables* t, const struct seqlz_lengths* lengths);

/* Writes the page for these sequences and literals, the last sequence with match 0. dst_cap must be at
 * least two pages. Returns the length, or 0 if dst_cap is smaller, the tables lack a code for some
 * symbol, or the sequences do not fit a page. */
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
    unsigned int a = ll < SEQLZ_LL_CAP ? ll : SEQLZ_LL_CAP;
    unsigned int b = ml == 0 ? 0 : ml - 4 < SEQLZ_ML_CAP ? ml - 4 : SEQLZ_ML_CAP;

    return a + 16U * b;
}

/*
 * The compressor: its own matcher, and the sequences coded straight into dst, the literals copied from
 * the page. The state is per CPU, the hash table in it is never cleared: an entry from an earlier page
 * only costs a comparison that fails, every match is checked against the bytes.
 */
#define SEQLZ_HASH_BITS 12U
#define SEQLZ_MAX_SEQUENCES (SEQLZ_PAGE / 4U + 1U)

struct seqlz_state {
    unsigned short table[1U << SEQLZ_HASH_BITS];
};

/* The sequences of a page as seqlz's matcher finds them, the last one without a match. Returns their
 * number. The literals are the bytes of the page that no match covers. */
unsigned int seqlz_find(struct seqlz_state* st, const void* src, struct seqlz_sequence* seq);

/* Compresses one page with seqlz_find's matcher and the tables. dst_cap must be at least two pages, as
 * zram's buffer is, which is always enough (see the encoder in seqlz.c). Returns the length, or 0 if
 * dst_cap is smaller or the tables lack a code for some symbol. The same bytes as seqlz_encode() for
 * seqlz_find's sequences. */
unsigned int
seqlz_compress(const struct seqlz_tables* t, struct seqlz_state* st, const void* src, void* dst, unsigned int dst_cap);

/* The tables compiled in, trained on resident pages (explore/seqlz_default_tables.c). */
extern const struct seqlz_lengths seqlz_default_lz4;
extern const struct seqlz_lengths seqlz_default_lz4hc;
extern const struct seqlz_lengths seqlz_default_own; /* for seqlz_compress, its own matcher */

#ifdef __cplusplus
}
#endif

#endif
