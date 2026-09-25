/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_SEQLZ_H
#define QUETSCHN_EXPLORE_SEQLZ_H

/*
 * seqlz: an LZ format for 4 KiB pages whose sequences are Huffman coded with static tables, and whose
 * literals are raw. docs/explored-designs.md, "Where the ratio of zstd comes from", estimated about 25%
 * Σ zsmalloc cost for this; the prototype measures what it really gets and how fast it decodes.
 *
 * A sequence is a literal length ll, a match length ml and an offset; the last sequence of a page has
 * no match. Like lz4's token, ll and ml - 4 share one symbol, capped at 15 and 31, and with them the
 * class of the offset:
 *   token:              min(ll, 15) + 16 * min(ml - 4, 31) + 512 * class, 3072 symbols, Huffman coded
 *                       with at most SEQLZ_TOKEN_BITS bits, rare ones escaped (SEQLZ_ESCAPE). Class 0
 *                       repeats the last offset (initially 1), class c from 1 to 3 is an offset below
 *                       1 << 4c, sent in 4c raw bits: below 16, 256 or 4096. Classes 4 and 5 are the
 *                       offsets from 16 that are multiples of 8, below 256 and below 4096, sent divided
 *                       by 8 in 5 and 9 bits: memory pages are full of 8-byte aligned data, and 72% and
 *                       56% of the offsets from 16 to 255 are multiples of 8 (docs/explored-designs.md,
 *                       LZX's aligned offsets). 31 and not 15 for
 *                       the match length, because with 15 every fifth match needed a length value, and
 *                       that branch mispredicted. 11 and not 12 bits, because the decode table of 12
 *                       bits, 8 KiB, made the cold decode slower: the tables of the decoder have to stay
 *                       in L1.
 *   offset:             SEQLZ_RAW_BITS(class) raw bits, right after the token. So the decoder
 *                       needs one table lookup per sequence and not two: a Huffman coded offset
 *                       bucket cost 0.3 points less memory, but its lookup was a second step on the
 *                       chain from one sequence to the next. One repeat offset and not three: the other
 *                       two were 11% of seqlz-fast's matches, 0.2 points of memory, and their move to
 *                       front made decoding 17% slower.
 *   ll >= 15:           ll - 15 follows as a length value; ml - 4 >= 31: ml - 4 - 31
 *   length value v:     v < 16 is the symbol itself; otherwise b = bit_width(v) - 1, the symbol is 12 +
 *                       b, and the b low bits of v follow as extra bits
 * The length values of ll and of ml have Huffman tables of at most SEQLZ_MAX_BITS bits. Everything goes
 * into one bitstream, read least significant bit first: per sequence the token, the offset, then the
 * length values if any, each symbol followed by its extra bits. One stream and not three, because the
 * state of three bit readers does not fit into the registers of x86-64 (docs/explored-designs.md). The
 * last sequence's token has ml - 4 = 0 and class 0.
 *
 * Page layout, all little endian:
 *   u16 literal bytes (on 4 KiB pages bits 13 and 14 the token table, see SEQLZ_TOKEN_SETS), literals,
 *   the bitstream (the rest)
 * No count of the sequences: the last one is the one whose literals fill the page, every other one
 * has a match behind its literals.
 *
 * The prototype does not find matches itself: it takes them from the kernel's lz4 or lz4hc. So its
 * compression time says nothing yet; its size and its decode time do.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef QUETSCHN_PAGE_BITS
#    define QUETSCHN_PAGE_BITS 12 /* see page_lz.h */
#endif
#define SEQLZ_PAGE (1U << QUETSCHN_PAGE_BITS)
#define SEQLZ_MAX_BITS 8U /* for length values; tables of 1 KiB each, 9 bits were 80 ns slower when cold */
#define SEQLZ_TOKEN_BITS 11U
#define SEQLZ_LL_BITS 4U /* of the token for ll, at most 4 */
#define SEQLZ_ML_BITS 5U /* for ml - 4, at most 5 */
#define SEQLZ_OFF_CLASSES 6U
#define SEQLZ_TOKEN_SYMBOLS (SEQLZ_OFF_CLASSES << (SEQLZ_LL_BITS + SEQLZ_ML_BITS))
/* A token without a code is sent as the escape's code and SEQLZ_ESCAPE_BITS raw bits of the token.
 * With 1536 tokens and codes of at most 11 bits, a code for each needed 75% of the code space for the
 * shortest codes alone, and 2048 do not fit at all: only the frequent ones get a code. */
#define SEQLZ_ESCAPE SEQLZ_TOKEN_SYMBOLS
#define SEQLZ_ESCAPE_BITS 12U
/* an escaped token and the largest offset in at most 31 bits, the encoder's bound for two pages */
#define SEQLZ_MAX_ESCAPE_LEN (31U - QUETSCHN_PAGE_BITS - SEQLZ_ESCAPE_BITS)
#define SEQLZ_LL_CAP ((1U << SEQLZ_LL_BITS) - 1U)    /* in the token, larger literal lengths follow as a value */
#define SEQLZ_ML_CAP ((1U << SEQLZ_ML_BITS) - 1U)    /* the same for ml - 4 */
#define SEQLZ_LEN_SYMBOLS (13U + QUETSCHN_PAGE_BITS) /* 16 direct values, then buckets 4 to page bits */
#define SEQLZ_HEADER 2U

/* The code lengths of the three tables, 0 for a symbol that never occurs. This is what training
 * produces and what zram's dictionary parameter can carry: 2099 bytes. */
struct seqlz_lengths {
    unsigned char token[SEQLZ_TOKEN_SYMBOLS + 1]; /* the last one is the escape, see SEQLZ_ESCAPE */
    unsigned char ll[SEQLZ_LEN_SYMBOLS];
    unsigned char ml[SEQLZ_LEN_SYMBOLS];
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

/* The raw bits of an offset class: 4 * class, and for class 3 as many as the page size has. From a
 * packed constant: the multiply by (class == 3) made gcc branch on the class for 16 KiB pages. */
#if QUETSCHN_PAGE_BITS == 12
#    define SEQLZ_RAW_BITS(cls) ((0x95C840U >> (4U * (cls))) & 15U)
#else
#    define SEQLZ_RAW_BITS(cls) ((0xB5E840U >> (4U * (cls))) & 15U)
#endif
/* classes 4 and 5 send the offset divided by 8 */
#define SEQLZ_OFF_SHIFT(cls) (((cls) >> 2) * 3U)

/* the class of an offset and its raw bits, see above; offsets are below the page size */
static inline unsigned int seqlz_off_class(unsigned int off, unsigned int last, unsigned int* raw_bits) {
    unsigned int cls = off == last ? 0U : off < 16 ? 1U : (off & 7U) == 0 ? (off < 256 ? 4U : 5U) : off < 256 ? 2U : 3U;

    *raw_bits = SEQLZ_RAW_BITS(cls);
    return cls;
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
 * [src, src + src_len) and never writes outside [dst, dst + SEQLZ_PAGE). A page with coded literals is
 * not valid here, see seqlz_decode_scratch(). */
int seqlz_decode(const struct seqlz_tables* t, const void* src, unsigned int src_len, void* dst);

/*
 * EXPERIMENT: the literals Huffman coded too, with one of SEQLZ_LIT_SETS static tables, the one that
 * codes them in the fewest bits, if that is smaller than the raw bytes by 1/16. Such a page starts
 * with SEQLZ_LIT_HEADER bytes: u16 0x8000 | the token table << 13 | literal bytes, u8 the table (with
 * SEQLZ_LIT_OWN for a page's own), 8 u16 bytes of the literals'
 * eight bitstreams (literal k in stream k % 8, most significant bit first, canonical codes of at most
 * SEQLZ_LIT_BITS bits); then those streams, then the sequences' bitstream. seqlz_decode_scratch()
 * decodes the literals into scratch first, SEQLZ_SCRATCH bytes; for any other page it is
 * seqlz_decode().
 */
#define SEQLZ_LIT_BITS 10U
#define SEQLZ_LIT_SETS 8U
/* the literal tables, trained on resident pages (seqlz_lit_sets.c) */
extern const unsigned char seqlz_lit_sets[SEQLZ_LIT_SETS][256];
/* literals per stream and refill: a refill leaves 56 bits */
#define SEQLZ_LIT_ROUNDS (56U / SEQLZ_LIT_BITS)
#define SEQLZ_LIT_HEADER 19U
/* One of SEQLZ_TOKEN_SETS token tables per page. On 4 KiB pages bits 13 and 14 of the u16
 * at the start of a page name it: 0 the table of the lengths, 1 to 3 seqlz_token_sets. The literal count
 * is the bits of SEQLZ_NLIT_MASK. */
#if QUETSCHN_PAGE_BITS == 12
#    define SEQLZ_TOKEN_SETS 4U
#    define SEQLZ_NLIT_MASK 0x1fffU
extern const unsigned char seqlz_token_sets[SEQLZ_TOKEN_SETS - 1][SEQLZ_TOKEN_SYMBOLS + 1];
#else
#    define SEQLZ_TOKEN_SETS 1U
#    define SEQLZ_NLIT_MASK 0x7fffU
#endif
/* The table number has this bit when the page has its own literal table. The number then names the fixed table
 * whose lengths are the context for the page's lengths. Behind the 19 bytes of the header: u16 bytes that follow for the
 * lengths; u8 bytes of each of the first 3 of 4 streams; the 4 streams, stream j with the lengths of bytes j, j + 4, j + 8,
 * ... (0 for a byte without a code), each coded with seqlz_lit_hdr[context], least significant bit first. Then the streams of
 * the literals. */
#define SEQLZ_LIT_OWN 0x40U
extern const unsigned char seqlz_lit_hdr[SEQLZ_LIT_BITS + 1][SEQLZ_LIT_BITS + 1];
/* 16 for the literal copies, 8 * rounds - 1 decoded past the end; then the lengths and the table of a
 * page with its own, so the scratch needs 4-byte alignment */
#define SEQLZ_SCRATCH (SEQLZ_PAGE + 48U + 256U + 3072U)
unsigned int seqlz_encode_coded(const struct seqlz_tables* t,
                                const struct seqlz_sequence* seq,
                                unsigned int n,
                                const unsigned char* literals,
                                unsigned int n_literals,
                                void* dst,
                                unsigned int dst_cap);
int seqlz_decode_scratch(const struct seqlz_tables* t, const void* src, unsigned int src_len, void* dst, void* scratch);
/* seqlz_compress() with the token table that codes the page's tokens in the fewest bits
 * (SEQLZ_TOKEN_SETS), then the literals coded as in seqlz_encode_coded(), or with the page's own table
 * where that saves 16 bytes (SEQLZ_LIT_OWN). scratch: SEQLZ_SCRATCH bytes, 4-byte aligned, the same the
 * decoder uses, for the encoder's work. */
struct seqlz_state;
unsigned int seqlz_compress_coded(
    const struct seqlz_tables* t, struct seqlz_state* st, const void* src, void* dst, unsigned int dst_cap, void* scratch);

/* the token of a sequence, see above */
static inline unsigned int seqlz_token(unsigned int ll, unsigned int ml, unsigned int cls) {
    unsigned int a = ll < SEQLZ_LL_CAP ? ll : SEQLZ_LL_CAP;
    unsigned int b = ml == 0 ? 0 : ml - 4 < SEQLZ_ML_CAP ? ml - 4 : SEQLZ_ML_CAP;

    return a + (b << SEQLZ_LL_BITS) + (cls << (SEQLZ_LL_BITS + SEQLZ_ML_BITS));
}

/*
 * The compressor: its own matcher, and the sequences coded straight into dst, the literals copied from
 * the page. The state is per CPU, 8 KiB of hash table, cleared for each page: that was 7% faster than
 * keeping it and checking each entry for whether it is before the current position.
 */
#define SEQLZ_HASH_BITS (QUETSCHN_PAGE_BITS == 12 ? 12U : 13U)
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
