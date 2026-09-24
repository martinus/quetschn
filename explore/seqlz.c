// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "seqlz.h"

#include "page_lz.h"

_Static_assert(PAGE_LZ_PAGE == SEQLZ_PAGE && PAGE_LZ_HASH_BITS == SEQLZ_HASH_BITS, "page_lz.h and seqlz.h disagree");

/*
 * Decode table entries, 0 for bits that start no code.
 * Length tables: bits 0-3 the code length, bits 4-7 the number of extra bits, bits 8-23 the base
 * value. A value is base + extra bits.
 * Token table: bits 0-3 the code length, bits 4-7 min(ll, 15), bits 8-12 min(ml - 4, 31), bits 13-14
 * the class of the offset.
 */
/*
 * The encoder's arrays have a power of 2 entries and its indices and shifts are masked: the kernel
 * builds with -fsanitize=bounds-strict and -fsanitize=shift, and a check the compiler cannot prove away
 * is a compare and a branch.
 */
#define ENC_LEN_SYMBOLS 32U

struct value_table {
    u32 decode[1U << SEQLZ_MAX_BITS];
    u32 enc[ENC_LEN_SYMBOLS]; /* code | length << 16, see build() */
};

struct token_table {
    u16 decode[1U << SEQLZ_TOKEN_BITS];
    u16 enc[SEQLZ_TOKEN_SYMBOLS + 1]; /* code | length << 12: 4 KiB in L1 next to the hash table */
};

/* The literals' streams are read most significant bit first, so the codes are not reversed and the
 * decode table is indexed by the next SEQLZ_LIT_BITS bits from the top. */
struct lit_table {
    u16 decode[1U << SEQLZ_LIT_BITS]; /* code length | symbol << 8 */
    u32 enc[256];                     /* code length | code << 8 */
};

/* per byte its code lengths in all literal tables, 8 bits each: the bits of the literals in all tables
 * at once are one add per literal, at most 25 literals before a lane could overflow */
#define LIT_COST_WORDS ((SEQLZ_LIT_SETS + 7U) / 8U)
_Static_assert(SEQLZ_LIT_BITS * 25U <= 255U, "lit_cost has lanes of 8 bits");

struct seqlz_tables {
    struct token_table token;
    struct lit_table lit[SEQLZ_LIT_SETS];
    u64 lit_cost[256][LIT_COST_WORDS];
    struct value_table ll, ml;
    u8 all_symbols; /* every symbol has a code, which the encoder needs; the decoder does not */
};

__SIZE_TYPE__ seqlz_tables_size(void) {
    return sizeof(struct seqlz_tables);
}

static unsigned int reverse(unsigned int code, unsigned int len) {
    unsigned int r = 0, i;

    for (i = 0; i < len; i++)
        r |= ((code >> i) & 1U) << (len - 1 - i);
    return r;
}

/* base and extra bits of a length value symbol, see seqlz.h */
static u32 length_entry(unsigned int s) {
    if (s < 16)
        return s << 8;
    return ((1U << (s - 12U)) << 8) | ((s - 12U) << 4);
}

/* ll, ml - 4 and the offset class of a token symbol, bit 15 for the escape */
static u32 token_entry(unsigned int s) {
    if (s == SEQLZ_ESCAPE)
        return 1U << 15;
    return ((s & SEQLZ_LL_CAP) << 4) | (((s >> SEQLZ_LL_BITS) & SEQLZ_ML_CAP) << 8) |
           ((s >> (SEQLZ_LL_BITS + SEQLZ_ML_BITS)) << 13);
}

/*
 * Canonical Huffman codes from the code lengths, bit reversed, and the decode table with 1 << bits
 * entries of entry(symbol) | code length. -1 if a length is longer than bits, or the codes are not a
 * complete prefix code: over-subscribed, with gaps, or none at all.
 */
static int build(const u8* len,
                 unsigned int n,
                 unsigned int bits,
                 u32 (*entry)(unsigned int),
                 u32* enc,
                 u16* enc16,
                 u32* decode32,
                 u16* decode16) {
    unsigned int count[16] = {0}, next[17], c = 0, used = 0, s, l, k;

    if (bits > 15)
        return -1;
    for (s = 0; s < n; s++) {
        if (len[s] > bits)
            return -1;
        count[len[s]]++;
        used += len[s] != 0;
    }
    if (used == 0)
        return -1;
    /* Kraft: the codes must fill the bits bits exactly. Then every bit pattern starts a code, and the
     * decoder does not need to check for one that does not. Huffman codes are complete. */
    for (l = 1, k = 0; l <= bits; l++)
        k += count[l] << (bits - l);
    if (k != (1U << bits))
        return -1;
    count[0] = 0;
    for (l = 1; l <= bits; l++) {
        c = (c + count[l - 1]) << 1;
        next[l] = c;
    }
    for (k = 0; k < (1U << bits); k++) {
        if (decode32)
            decode32[k] = 0;
        else
            decode16[k] = 0;
    }
    for (s = 0; s < n; s++) {
        unsigned int r;

        l = len[s];
        if (enc)
            enc[s] = 0;
        else
            enc16[s] = 0;
        if (l == 0)
            continue;
        r = reverse(next[l]++, l);
        if (enc)
            enc[s] = r | l << 16;
        else
            enc16[s] = (u16)(r | l << 12);
        for (k = r; k < (1U << bits); k += 1U << l) {
            if (decode32)
                decode32[k] = entry(s) | l;
            else
                decode16[k] = (u16)(entry(s) | l);
        }
    }
    return 0;
}

/* canonical codes and the decode table of a literal table, not reversed; -1 as build() */
static int build_lit(const u8* len, struct lit_table* t) {
    unsigned int count[16] = {0}, next[17], c = 0, s, l, k;

    for (s = 0; s < 256; s++) {
        if (len[s] > SEQLZ_LIT_BITS)
            return -1;
        count[len[s]]++;
    }
    for (l = 1, k = 0; l <= SEQLZ_LIT_BITS; l++)
        k += count[l] << (SEQLZ_LIT_BITS - l);
    if (k != (1U << SEQLZ_LIT_BITS))
        return -1;
    count[0] = 0;
    for (l = 1; l <= SEQLZ_LIT_BITS; l++) {
        c = (c + count[l - 1]) << 1;
        next[l] = c;
    }
    for (s = 0; s < 256; s++) {
        l = len[s];
        t->enc[s] = 0;
        if (l == 0)
            continue;
        c = next[l]++;
        t->enc[s] = l | c << 8;
        for (k = c << (SEQLZ_LIT_BITS - l); k < (c + 1U) << (SEQLZ_LIT_BITS - l); k++)
            t->decode[k] = (u16)(l | s << 8);
    }
    return 0;
}

int seqlz_all_symbols(const struct seqlz_tables* t) {
    return t->all_symbols;
}

int seqlz_tables_init(struct seqlz_tables* t, const struct seqlz_lengths* lengths) {
    __builtin_memset(t, 0, sizeof(*t)); /* also the encoder's entries behind the last symbol */
    if (build(lengths->token, SEQLZ_TOKEN_SYMBOLS + 1, SEQLZ_TOKEN_BITS, token_entry, 0, t->token.enc, 0, t->token.decode) ||
        build(lengths->ll, SEQLZ_LEN_SYMBOLS, SEQLZ_MAX_BITS, length_entry, t->ll.enc, 0, t->ll.decode, 0) ||
        build(lengths->ml, SEQLZ_LEN_SYMBOLS, SEQLZ_MAX_BITS, length_entry, t->ml.enc, 0, t->ml.decode, 0))
        return -1;
    {
        /* the literal tables are compiled in, seqlz_lit_sets */
        unsigned int k;

        for (k = 0; k < SEQLZ_LIT_SETS; k++)
            if (build_lit(seqlz_lit_sets[k], &t->lit[k]))
                return -1;
        for (k = 0; k < 256U * SEQLZ_LIT_SETS; k++)
            t->lit_cost[k % 256][k / 256 / 8] |= (u64)seqlz_lit_sets[k / 256][k % 256] << (8U * (k / 256 % 8));
    }
    {
        /* every token has a code or the escape has one, every length value has one */
        unsigned int k, all = 1;

        for (k = 0; k < SEQLZ_TOKEN_SYMBOLS; k++)
            all &= lengths->token[k] != 0 || lengths->token[SEQLZ_ESCAPE] != 0;
        /* an escaped token and a 12-bit offset in at most 31 bits, the encoder's bound */
        all &= lengths->token[SEQLZ_ESCAPE] <= SEQLZ_MAX_ESCAPE_LEN;
        for (k = 0; k < SEQLZ_LEN_SYMBOLS; k++)
            all &= lengths->ll[k] != 0 && lengths->ml[k] != 0;
        for (k = 0; k < 256 * SEQLZ_LIT_SETS; k++)
            all &= seqlz_lit_sets[k / 256][k % 256] != 0;
        t->all_symbols = (u8)all;
    }
    return 0;
}

/* ---- shared by matcher, encoder and decoder ---- */

/* ---- matcher, see page_lz.h ---- */

struct find_ctx {
    struct seqlz_sequence* seq;
    unsigned int n;
};

static ALWAYS_INLINE void find_emit(void* ctx, const u8* literals, unsigned int ll, unsigned int ml, unsigned int off) {
    struct find_ctx* f = ctx;

    (void)literals;
    f->seq[f->n].literals = (unsigned short)ll;
    f->seq[f->n].match = (unsigned short)ml;
    f->seq[f->n].offset = (unsigned short)off;
    f->n++;
}

unsigned int seqlz_find(struct seqlz_state* st, const void* src, struct seqlz_sequence* seq) {
    struct find_ctx f = {seq, 0};

    match_page(st->table, src, find_emit, &f);
    return f.n;
}

/* ---- encoder ---- */

/*
 * One encoder for both entry points. Literals go to the front of dst, right after the header; the
 * bitstream goes behind the room of a page of literals and is moved in behind the literals at the end.
 * dst has two pages, and that is always enough: the most bits per page byte are a sequence of 4 bytes
 * without literals, an 11-bit token and a 12-bit offset, so at most 1024 * 23 + 31 bits, 2948 bytes;
 * behind 4 + 4096 + 16 bytes of header, literals and room for their 16-byte copies there are 4076. The
 * code lengths are capped at 11 and 9 bits, so this holds for any tables.
 * The bit writer is a 64-bit accumulator, stored 8 bytes at a time and advanced by the whole bytes. It
 * holds at most 7 bits after a flush, so one flush per sequence is enough: 7 + 11 bits of token, 12 of
 * offset and 20 of a match length value are 50. Only a literal length value, another 20, needs its
 * own flush.
 */
struct encoder {
    const struct seqlz_tables* t;
    u64 acc;
    unsigned int cnt;
    u8* p;             /* bitstream */
    u8* lit;           /* literals */
    const u8* src_end; /* the 16-byte literal copies may read up to here */
    unsigned int last; /* the last offset */
};

/* v has n bits, n < 64 - cnt */
static ALWAYS_INLINE void enc_put(struct encoder* e, u64 v, unsigned int n) {
    e->acc |= v << (e->cnt & 63U);
    e->cnt += n;
}

/* a code from an enc[] entry, followed by the n_extra low bits of extra */
static ALWAYS_INLINE void enc_put_code(struct encoder* e, u32 entry, unsigned int extra, unsigned int n_extra) {
    unsigned int len = entry >> 16 & 15U;

    n_extra &= 31U;
    enc_put(e, (entry & 0xffffU) | (u64)(extra & ((1U << n_extra) - 1U)) << len, len + n_extra);
}

static ALWAYS_INLINE void enc_flush(struct encoder* e) {
    store64(e->p, e->acc);
    e->p += e->cnt >> 3;
    e->acc >>= e->cnt & 56U;
    e->cnt &= 7U;
}

static ALWAYS_INLINE void put_len_value(struct encoder* e, const struct value_table* t, unsigned int v) {
    unsigned int extra, s = seqlz_len_symbol(v, &extra) & (ENC_LEN_SYMBOLS - 1U);

    enc_put_code(e, t->enc[s], v, extra);
}

/* the code of a token and its length; the escape and the token for a token without a code */
static ALWAYS_INLINE u32 token_code(const struct seqlz_tables* t, unsigned int tok, unsigned int* len) {
    u32 te = t->token.enc[tok], ee;

    if (te != 0) {
        *len = te >> 12;
        return te & 0xfffU;
    }
    ee = t->token.enc[SEQLZ_ESCAPE];
    *len = (ee >> 12) + SEQLZ_ESCAPE_BITS;
    return (ee & 0xfffU) | tok << (ee >> 12);
}

/* one sequence: its literals from in, ml 0 for the last one */
static ALWAYS_INLINE void encode_emit(void* ctx, const u8* in, unsigned int ll, unsigned int ml, unsigned int off) {
    struct encoder* e = ctx;
    const struct seqlz_tables* t = e->t;
    unsigned int k = 0;

    /* the literals, the first 16 bytes without a loop to mispredict; dst has room behind them */
    if ((unsigned int)(e->src_end - in) >= 16U) {
        __builtin_memcpy(e->lit, in, 16);
        k = 16;
    }
    for (; k < ll && (unsigned int)(e->src_end - in) >= k + 16U; k += 16)
        __builtin_memcpy(e->lit + k, in + k, 16);
    for (; k < ll; k++)
        e->lit[k] = in[k];
    e->lit += ll;

    if (ml == 0) {
        /* the last sequence */
        unsigned int tlen;
        u32 code = token_code(t, seqlz_token(ll, 0, 0), &tlen);

        enc_put(e, code, tlen);
        if (ll >= SEQLZ_LL_CAP)
            put_len_value(e, &t->ll, ll - SEQLZ_LL_CAP);
        enc_flush(e);
        return;
    }
    {
        /* The class of the offset without a branch: 0 the last offset, 1 below 256 in 8 bits, 2 in 12.
         * Token and offset in one put, the length values after them. */
        unsigned int is_new = (off == e->last) - 1U, big = off >= 256U;
        unsigned int cls = (1U + (off >= 16U) + big) & is_new;
        unsigned int raw_bits = SEQLZ_RAW_BITS(cls);
        unsigned int tlen;
        u32 code = token_code(t, seqlz_token(ll, ml, cls), &tlen);

        enc_put(e, code | (u64)(off & ((1U << raw_bits) - 1U)) << tlen, tlen + raw_bits);
        if (ll >= SEQLZ_LL_CAP) {
            put_len_value(e, &t->ll, ll - SEQLZ_LL_CAP);
            enc_flush(e);
        }
        if (ml - 4 >= SEQLZ_ML_CAP)
            put_len_value(e, &t->ml, ml - 4 - SEQLZ_ML_CAP);
        enc_flush(e);
        e->last = off;
    }
}

static ALWAYS_INLINE void encoder_init(struct encoder* e, const struct seqlz_tables* t, u8* d, const u8* src_end) {
    *e = (struct encoder){t, 0, 0, d + SEQLZ_HEADER + SEQLZ_PAGE + 16U, d + SEQLZ_HEADER, src_end, 1};
}

/* the last bits, the header, and the bitstream moved in behind the literals */
static unsigned int encoder_finish(struct encoder* e, u8* d) {
    u8* bits = d + SEQLZ_HEADER + SEQLZ_PAGE + 16U;
    unsigned int n_lit = (unsigned int)(e->lit - (d + SEQLZ_HEADER)), bytes;

    if (e->cnt > 0) {
        store64(e->p, e->acc);
        e->p++;
    }
    bytes = (unsigned int)(e->p - bits);
    store16(d, n_lit);
    __builtin_memmove(e->lit, bits, bytes);
    return SEQLZ_HEADER + n_lit + bytes;
}

unsigned int seqlz_encode(const struct seqlz_tables* t,
                          const struct seqlz_sequence* seq,
                          unsigned int n,
                          const unsigned char* literals,
                          unsigned int n_literals,
                          void* dst,
                          unsigned int dst_cap) {
    const u8* in = literals;
    struct encoder e;
    unsigned int i;

    if (dst_cap < 2U * SEQLZ_PAGE || !t->all_symbols || n == 0 || n > SEQLZ_MAX_SEQUENCES || n_literals > SEQLZ_PAGE)
        return 0;
    encoder_init(&e, t, dst, literals + n_literals);
    for (i = 0; i < n; i++) {
        unsigned int ll = seq[i].literals;

        if (ll > (unsigned int)(literals + n_literals - in))
            return 0;
        encode_emit(&e, in, ll, i + 1 == n ? 0U : seq[i].match, seq[i].offset);
        in += ll;
    }
    return encoder_finish(&e, dst);
}

/* One literal into a stream: the code shifted in from the right, the shift takes the code length from
 * the low 6 bits of the entry. The entries of a stream's literals are summed: the low byte of the sum
 * is their bits, at most 4 * 10 between two flushes. */
#define ENC_LIT(acc, sum, b)                   \
    do {                                       \
        u32 e_ = enc[b];                       \
                                               \
        (acc) = (acc) << (e_ & 63U) | e_ >> 8; \
        (sum) += e_;                           \
    } while (0)

/* The whole bytes of a stream's accumulator out, and the byte of the bits left over, which the next
 * flush writes again. cnt is the number of bits in acc; the bits above it are old ones, shifted out
 * here. 8 bytes at once while the stream has room for them, it ends where the next begins. */
#define ENC_FLUSH(p, end, acc, cnt, sum)                         \
    do {                                                         \
        unsigned int n_ = (cnt) + ((sum) & 255U);                \
        u64 w_ = __builtin_bswap64((acc) << ((64U - n_) & 63U)); \
                                                                 \
        if ((end) - (p) >= 8)                                    \
            store64((p), w_);                                    \
        else                                                     \
            store_tail((p), w_, (n_ + 7U) >> 3);                 \
        (p) += n_ >> 3;                                          \
        (cnt) = n_ & 7U;                                         \
        (sum) = 0;                                               \
    } while (0)

/* the first n bytes of w as store64() would write them, n < 8 */
static void store_tail(u8* p, u64 w, unsigned int n) {
    u8 b[8];
    unsigned int k;

    store64(b, w);
    for (k = 0; k < n; k++)
        p[k] = b[k];
}

/* A raw page of len bytes in d turned into one with coded literals, if that pays; literals are its
 * literals somewhere that the coded ones do not overwrite. Returns the new length. */
static unsigned int
code_literals(const struct seqlz_tables* t, u8* d, unsigned int len, const u8* literals, unsigned int n_literals) {
    const u64 lanes = 0x00ff00ff00ff00ffULL;
    unsigned int bits = ~0U, k, j, coded, seq_bytes, set = 0, sizes[8];
    /* per stream the bits in all tables, 16-bit lanes: tables 0, 2, 4, 6 and 1, 3, 5, 7 of each word */
    u64 even[8][LIT_COST_WORDS] = {{0}}, odd[8][LIT_COST_WORDS] = {{0}};
    unsigned int w;
    u8* q[9];
    const struct lit_table* lt;

    /* the bits of each stream in each table, 25 literals of a stream per sum of 8-bit lanes */
    for (k = 0; k < n_literals;) {
        u64 x[8][LIT_COST_WORDS] = {{0}};
        unsigned int end = n_literals - k < 200U ? n_literals : k + 200U;

        for (; k + 8U <= end; k += 8)
            for (j = 0; j < 8U; j++)
                for (w = 0; w < LIT_COST_WORDS; w++)
                    x[j][w] += t->lit_cost[literals[k + j]][w];
        for (; k < end; k++)
            for (w = 0; w < LIT_COST_WORDS; w++)
                x[k & 7U][w] += t->lit_cost[literals[k]][w];
        for (j = 0; j < 8U; j++)
            for (w = 0; w < LIT_COST_WORDS; w++) {
                even[j][w] += x[j][w] & lanes;
                odd[j][w] += x[j][w] >> 8 & lanes;
            }
    }
    /* the table with the fewest bits */
    for (k = 0; k < SEQLZ_LIT_SETS; k++) {
        unsigned int b = 0;

        for (j = 0; j < 8U; j++)
            b += (unsigned int)(((k & 1U ? odd : even)[j][k / 8] >> (16U * (k % 8 / 2))) & 0xffffU);
        if (b < bits) {
            bits = b;
            set = k;
        }
    }
    lt = &t->lit[set];
    for (j = 0, coded = 0; j < 8U; j++) {
        sizes[j] = (((unsigned int)((set & 1U ? odd : even)[j][set / 8] >> (16U * (set % 8 / 2))) & 0xffffU) + 7U) / 8U;
        coded += sizes[j];
    }
    /* only if it saves at least 1/16: decoding coded literals costs time per byte, and coding them
     * whenever they save anything saved less than 0.1 points more (docs/explored-designs.md) */
    if (coded + SEQLZ_LIT_HEADER >= n_literals - n_literals / 16U)
        return len;
    seq_bytes = len - SEQLZ_HEADER - n_literals;
    /* the sequences' bitstream out of the way, behind where it would ever be */
    __builtin_memmove(d + SEQLZ_HEADER + SEQLZ_PAGE + 16U, d + SEQLZ_HEADER + n_literals, seq_bytes);
    q[0] = d + SEQLZ_LIT_HEADER;
    for (j = 0; j < 8U; j++)
        q[j + 1] = q[j] + sizes[j];
    /* Eight streams, literal k in stream k % 8, so that the decoder has eight chains side by side; most
     * significant bit first, see decode_literals(). Four at a time, each straight to its place: one
     * stream after the other waited for its accumulator, 3.5 cycles per literal. */
    {
        const u32* const enc = lt->enc;
        unsigned int half;

        for (half = 0; half < 8U; half += 4) {
            u8 *p0 = q[half], *p1 = q[half + 1], *p2 = q[half + 2], *p3 = q[half + 3];
            u8 *e0 = p1, *e1 = p2, *e2 = p3, *e3 = q[half + 4];
            u64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
            unsigned int c0 = 0, c1 = 0, c2 = 0, c3 = 0, s0 = 0, s1 = 0, s2 = 0, s3 = 0;

            for (k = half; k + 28U < n_literals; k += 32) {
                for (j = 0; j < 32U; j += 8) {
                    ENC_LIT(a0, s0, literals[k + j]);
                    ENC_LIT(a1, s1, literals[k + j + 1]);
                    ENC_LIT(a2, s2, literals[k + j + 2]);
                    ENC_LIT(a3, s3, literals[k + j + 3]);
                }
                ENC_FLUSH(p0, e0, a0, c0, s0);
                ENC_FLUSH(p1, e1, a1, c1, s1);
                ENC_FLUSH(p2, e2, a2, c2, s2);
                ENC_FLUSH(p3, e3, a3, c3, s3);
            }
            /* the rest, fewer than 4 literals per stream */
            for (; k < n_literals; k++) {
                switch (k & 7U) {
                case 0:
                case 4:
                    ENC_LIT(a0, s0, literals[k]);
                    break;
                case 1:
                case 5:
                    ENC_LIT(a1, s1, literals[k]);
                    break;
                case 2:
                case 6:
                    ENC_LIT(a2, s2, literals[k]);
                    break;
                default:
                    ENC_LIT(a3, s3, literals[k]);
                }
                if ((k & 3U) == 3U)
                    k += 4;
            }
            ENC_FLUSH(p0, e0, a0, c0, s0);
            ENC_FLUSH(p1, e1, a1, c1, s1);
            ENC_FLUSH(p2, e2, a2, c2, s2);
            ENC_FLUSH(p3, e3, a3, c3, s3);
        }
    }
    store16(d, 0x8000U | n_literals);
    d[2] = (u8)set;
    for (j = 0; j < 8U; j++)
        store16(d + 3 + 2 * j, sizes[j]);
    coded += SEQLZ_LIT_HEADER;
    __builtin_memmove(d + coded, d + SEQLZ_HEADER + SEQLZ_PAGE + 16U, seq_bytes);
    return coded + seq_bytes;
}

unsigned int seqlz_encode_coded(const struct seqlz_tables* t,
                                const struct seqlz_sequence* seq,
                                unsigned int n,
                                const unsigned char* literals,
                                unsigned int n_literals,
                                void* dst,
                                unsigned int dst_cap) {
    unsigned int len = seqlz_encode(t, seq, n, literals, n_literals, dst, dst_cap);

    return len == 0 ? 0 : code_literals(t, dst, len, literals, n_literals);
}

unsigned int seqlz_compress_coded(
    const struct seqlz_tables* t, struct seqlz_state* st, const void* src, void* dst_v, unsigned int dst_cap) {
    u8* const d = dst_v;
    unsigned int len = seqlz_compress(t, st, src, dst_v, dst_cap), n_lit;
    u8* keep;

    if (len == 0)
        return 0;
    n_lit = load16(d);
    /* the literals to the end of dst, behind where code_literals() puts the sequences' bitstream */
    if (len - SEQLZ_HEADER + SEQLZ_LIT_HEADER + 8U > SEQLZ_PAGE)
        return len;
    keep = d + 2U * SEQLZ_PAGE - n_lit;
    __builtin_memcpy(keep, d + SEQLZ_HEADER, n_lit);
    return code_literals(t, d, len, keep, n_lit);
}

unsigned int
seqlz_compress(const struct seqlz_tables* t, struct seqlz_state* st, const void* src_v, void* dst, unsigned int dst_cap) {
    const u8* const src = src_v;
    struct encoder e;

    if (dst_cap < 2U * SEQLZ_PAGE || !t->all_symbols)
        return 0;
    encoder_init(&e, t, dst, src + SEQLZ_PAGE);
    match_page(st->table, src, encode_emit, &e);
    return encoder_finish(&e, dst);
}

/* ---- decoder ---- */

/*
 * Least significant bit first. Refill loads 8 bytes at once while at least 8 are left in the stream,
 * and byte by byte at the end, so it never reads past the stream. Past the end it shifts in zeros and
 * count goes negative; memory safety does not depend on the bits, every length and offset is checked
 * where it is used.
 */
struct bit_reader {
    const u8* p;
    const u8* end;
    u64 bits;
    int count; /* negative once more bits were used than the stream has: the page is not valid */
};

static inline void refill(struct bit_reader* r) {
    if (r->end - r->p >= 8) {
        u64 v;

        /* count is at least 0 here: it only goes negative at the end of the stream */
        __builtin_memcpy(&v, r->p, 8);
        r->bits |= v << r->count;
        r->p += (63 - r->count) >> 3;
        r->count |= 56;
    } else {
        while (r->count >= 0 && r->count <= 56 && r->p < r->end) {
            r->bits |= (u64)*r->p++ << r->count;
            r->count += 8;
        }
    }
}

static inline void drop(struct bit_reader* r, unsigned int n) {
    r->bits >>= n;
    r->count -= (int)n;
}

/* One value: the table entry of the next code, then base + extra bits, in one step. Needs 9 + 12
 * bits, refilled before. The entry is 0 for bits that start no code. */
static inline unsigned int value(struct bit_reader* r, const struct value_table* t, u32* entry) {
    u32 e = t->decode[r->bits & ((1U << SEQLZ_MAX_BITS) - 1U)];
    unsigned int n = e & 15U, x = (e >> 4) & 15U;
    unsigned int v = ((e >> 8) & 0xffffU) + (unsigned int)((r->bits >> n) & ((1ULL << x) - 1ULL));

    *entry = e;
    drop(r, n + x);
    return v;
}

/*
 * The literals' streams, as zstd reads its Huffman coded literals: most significant bit first, 64 bits
 * from ip with the lowest one replaced by a 1, shifted left by the bits of ip's byte already used. Each
 * code shifts the container further left, so the marker's position is the number of bits used since
 * ip: a refill needs no counter, and a stream is two registers, ip and the container. Streams with a
 * pointer, a container and a counter each did not fit into the registers of x86-64. A refill leaves
 * at least 56 bits, the marker is never reached.
 */
static inline u64 load_be64(const u8* p) {
    return __builtin_bswap64(load64(p));
}

/* the 8 bytes at ip, zeros behind end */
static __attribute__((__noinline__, __cold__)) u64 lit_load_tail(const u8* ip, const u8* end) {
    u8 b[8] = {0};
    unsigned int k;

    for (k = 0; k < 8U && ip + k < end; k++)
        b[k] = ip[k];
    return load_be64(b);
}

/* 8 bytes at ip, which may be those of the next stream or of the sequences: they are only used for the
 * symbols behind n_lit */
#define LIT_REFILL(ip, bits)                                                                          \
    do {                                                                                              \
        unsigned int used_ = (unsigned int)__builtin_ctzll(bits);                                     \
                                                                                                      \
        (ip) += used_ >> 3;                                                                           \
        (bits) = ((end - (ip) >= 8 ? load_be64(ip) : lit_load_tail((ip), end)) | 1U) << (used_ & 7U); \
    } while (0)

#define LIT_DECODE(out, bits)                                                 \
    do {                                                                      \
        unsigned int e_ = lt[(bits) >> (64U - SEQLZ_LIT_BITS)];               \
                                                                              \
        (out) = (u8)(e_ >> 8);                                                \
        (bits) <<= e_ & 63U; /* the code length; the symbol is above bit 6 */ \
    } while (0)

/* The coded literals of a page into out, see seqlz_encode_coded(). Returns where the sequences'
 * bitstream starts, 0 if the page is not valid. Not inlined: in the loop over the sequences its
 * registers made seqlz-fast's pages 12% slower. */
static __attribute__((__noinline__, __aligned__(64))) const u8*
decode_literals(const struct seqlz_tables* t, const u8* s, unsigned int src_len, unsigned int n_lit, u8* out) {
    const u8* const end = s + src_len;
    const u8* q = s + SEQLZ_LIT_HEADER;
    const u8* ip[8];
    const u8* start[8];
    unsigned int sz[8];
    u64 b0 = 1, b1 = 1, b2 = 1, b3 = 1, b4 = 1, b5 = 1, b6 = 1, b7 = 1;
    unsigned int k;
    u64 total = 0;
    const u16* lt;

    if (src_len < SEQLZ_LIT_HEADER || n_lit > SEQLZ_PAGE || s[2] >= SEQLZ_LIT_SETS)
        return 0;
    lt = t->lit[s[2]].decode;
    for (k = 0; k < sizeof(t->lit[0].decode); k += 64)
        __builtin_prefetch((const u8*)lt + k);
    for (k = 0; k < 8U; k++) {
        sz[k] = load16(s + 3 + 2 * k);
        start[k] = q + total;
        ip[k] = start[k];
        total += sz[k];
    }
    if (SEQLZ_LIT_HEADER + total > src_len)
        return 0;
    for (k = 0; k < n_lit; k += 8U * SEQLZ_LIT_ROUNDS) {
        unsigned int j;
        const u8 *i0 = ip[0], *i1 = ip[1], *i2 = ip[2], *i3 = ip[3], *i4 = ip[4], *i5 = ip[5], *i6 = ip[6], *i7 = ip[7];

        LIT_REFILL(i0, b0);
        LIT_REFILL(i1, b1);
        LIT_REFILL(i2, b2);
        LIT_REFILL(i3, b3);
        LIT_REFILL(i4, b4);
        LIT_REFILL(i5, b5);
        LIT_REFILL(i6, b6);
        LIT_REFILL(i7, b7);
        ip[0] = i0;
        ip[1] = i1;
        ip[2] = i2;
        ip[3] = i3;
        ip[4] = i4;
        ip[5] = i5;
        ip[6] = i6;
        ip[7] = i7;
        for (j = 0; j < SEQLZ_LIT_ROUNDS; j++) {
            LIT_DECODE(out[k + 8 * j], b0);
            LIT_DECODE(out[k + 8 * j + 1], b1);
            LIT_DECODE(out[k + 8 * j + 2], b2);
            LIT_DECODE(out[k + 8 * j + 3], b3);
            LIT_DECODE(out[k + 8 * j + 4], b4);
            LIT_DECODE(out[k + 8 * j + 5], b5);
            LIT_DECODE(out[k + 8 * j + 6], b6);
            LIT_DECODE(out[k + 8 * j + 7], b7);
        }
    }
    {
        const long slack = (long)(SEQLZ_LIT_ROUNDS * SEQLZ_LIT_BITS);
        u64 bb[8] = {b0, b1, b2, b3, b4, b5, b6, b7};

        for (k = 0; k < 8U; k++)
            if (8L * (ip[k] - start[k]) + __builtin_ctzll(bb[k]) > 8L * sz[k] + slack)
                return 0;
    }
    return q + total;
}

int seqlz_decode(const struct seqlz_tables* t, const void* src, unsigned int src_len, void* dst) {
    return seqlz_decode_scratch(t, src, src_len, dst, 0);
}

int seqlz_decode_scratch(const struct seqlz_tables* t, const void* src, unsigned int src_len, void* dst, void* scratch) {
    const u8* s = src;
    const u8* const s_end = s + src_len;
    u8* d = dst;
    u8* const d_end = d + SEQLZ_PAGE;
    const u8 *lit, *lit_end, *lit_bound;
    struct bit_reader br;
    unsigned int n_lit, last = 1;

    if (src_len < SEQLZ_HEADER)
        return -1;
    {
        /* the decoder's tables, so that their misses overlap when they are cold */
        const u8* q;

        for (q = (const u8*)t->token.decode; q < (const u8*)(t->token.decode + (1U << SEQLZ_TOKEN_BITS)); q += 64)
            __builtin_prefetch(q);
        for (q = (const u8*)t->ll.decode; q < (const u8*)(t->ll.decode + (1U << SEQLZ_MAX_BITS)); q += 64)
            __builtin_prefetch(q);
        for (q = (const u8*)t->ml.decode; q < (const u8*)(t->ml.decode + (1U << SEQLZ_MAX_BITS)); q += 64)
            __builtin_prefetch(q);
    }
    n_lit = load16(s);
    if (n_lit & 0x8000U) {
        const u8* q;

        n_lit &= 0x7fffU;
        if (!scratch || !(q = decode_literals(t, s, src_len, n_lit, scratch)))
            return -1;
        br = (struct bit_reader){q, s_end, 0, 0};
        lit = scratch;
        lit_end = lit + n_lit;
        lit_bound = lit + SEQLZ_SCRATCH;
    } else {
        if ((u64)SEQLZ_HEADER + n_lit > src_len)
            return -1;
        lit = s + SEQLZ_HEADER;
        lit_end = lit + n_lit;
        lit_bound = s_end;
        br = (struct bit_reader){lit_end, s_end, 0, 0};
    }

    for (;;) {
        unsigned int tok, nl, len, off;
        u32 e;

        /* A refill only when token and offset might not fit, 11 + 12 bits: the next token's lookup then
         * does not wait for the refill's load, and a refill leaves 56 bits for two or three sequences.
         * The escape and the length values refill before they read. The token's entry has the class
         * of the offset, so its raw bits are known without a second lookup. */
        if (br.count < (int)(SEQLZ_TOKEN_BITS + QUETSCHN_PAGE_BITS))
            refill(&br);
        tok = t->token.decode[br.bits & ((1U << SEQLZ_TOKEN_BITS) - 1U)];
        if (tok >> 15) {
            /* the escape: the token follows in SEQLZ_ESCAPE_BITS bits */
            unsigned int idx;

            drop(&br, tok & 15U);
            refill(&br);
            idx = (unsigned int)br.bits & ((1U << SEQLZ_ESCAPE_BITS) - 1U);
            drop(&br, SEQLZ_ESCAPE_BITS);
            if (idx >= SEQLZ_TOKEN_SYMBOLS)
                return -1;
            tok = token_entry(idx);
        }
        {
            unsigned int cls = tok >> 13, raw_bits = SEQLZ_RAW_BITS(cls);
            unsigned int n_tok = tok & 15U;
            unsigned int is_new = 0U - (cls != 0);
            unsigned int raw = (unsigned int)(br.bits >> n_tok) & ((1U << raw_bits) - 1U);

            drop(&br, n_tok + raw_bits);
            off = (raw & is_new) | (last & ~is_new);
        }
        nl = (tok >> 4) & 15U;
        len = ((tok >> 8) & 31U) + 4U;
        /* Far from the end of the page and of the literals, and no length value: 16 literal bytes
         * and 16 or 32 bytes of match copied without checking the room behind them; nl + len is at
         * most 14 + 34, and the last sequence always has its literals up to the end of the page. */
        if (nl < SEQLZ_LL_CAP && len < SEQLZ_ML_CAP + 4U && (unsigned int)(d_end - d) >= 64U &&
            (unsigned int)(lit_bound - lit) >= 16U) {
            u64 a, b;

            if (nl > (unsigned int)(lit_end - lit))
                return -1;
            __builtin_memcpy(&a, lit, 8);
            __builtin_memcpy(&b, lit + 8, 8);
            __builtin_memcpy(d, &a, 8);
            __builtin_memcpy(d + 8, &b, 8);
            d += nl;
            lit += nl;
            last = off;
            if (off - 1U >= (unsigned int)(d - (u8*)dst))
                return -1;
            if (off >= 8U) {
                /* for 8 <= off < 16 each load reads what the stores before it wrote, which is right */
                __builtin_memcpy(&a, d - off, 8);
                __builtin_memcpy(d, &a, 8);
                __builtin_memcpy(&b, d + 8 - off, 8);
                __builtin_memcpy(d + 8, &b, 8);
                if (len > 16U) {
                    __builtin_memcpy(&a, d + 16 - off, 8);
                    __builtin_memcpy(d + 16, &a, 8);
                    __builtin_memcpy(&b, d + 24 - off, 8);
                    __builtin_memcpy(d + 24, &b, 8);
                    if (len > 32U) {
                        __builtin_memcpy(&a, d + 32 - off, 8);
                        __builtin_memcpy(d + 32, &a, 8);
                    }
                }
            } else {
                /* The off bytes before d repeated to 8 bytes; step, the largest multiple of off up to
                 * 8, apart it is the same 8 bytes again, so the same register is stored at each step,
                 * without loads. Five stores cover at least 28 bytes. */
                static const u8 step_for[8] = {0, 8, 8, 6, 8, 5, 6, 7};
                static const u64 repeat[8] = {0,
                                              0x0101010101010101ULL,
                                              0x0001000100010001ULL,
                                              0x0001000001000001ULL,
                                              0x0000000100000001ULL,
                                              0x0000010000000001ULL,
                                              0x0001000000000001ULL,
                                              0x0100000000000001ULL};
                unsigned int step = step_for[off], k;

                __builtin_memcpy(&a, d - off, 8);
                a = (a & (~0ULL >> (64U - 8U * off))) * repeat[off];
                __builtin_memcpy(d, &a, 8);
                __builtin_memcpy(d + step, &a, 8);
                __builtin_memcpy(d + 2U * step, &a, 8);
                __builtin_memcpy(d + 3U * step, &a, 8);
                __builtin_memcpy(d + 4U * step, &a, 8);
                for (k = 5U * step; k < len; k += step)
                    __builtin_memcpy(d + k, &a, 8);
            }
            d += len;
            continue;
        }
        if (nl == SEQLZ_LL_CAP) {
            refill(&br);
            nl += value(&br, &t->ll, &e);
        }
        if (nl > (unsigned int)(lit_end - lit) || nl > (unsigned int)(d_end - d))
            return -1;
        copy_literals(d, d_end, lit, lit_bound, nl);
        d += nl;
        lit += nl;
        if (d == d_end)
            break; /* the last sequence */

        if (len == SEQLZ_ML_CAP + 4U) {
            refill(&br);
            len += value(&br, &t->ml, &e);
        }
        last = off;
        /* off - 1 wraps for 0 */
        if (off - 1U >= (unsigned int)(d - (u8*)dst) || len > (unsigned int)(d_end - d))
            return -1;
        copy_match(d, d_end, off, len);
        d += len;
    }
    if (d != d_end || lit != lit_end || br.count < 0)
        return -1;
    return 0;
}

/* ---- EXPERIMENT: a parser that knows seqlz's costs, for recompression ---- */

/* bits of a length value, see seqlz.h */
static unsigned int value_bits(const struct value_table* t, unsigned int v) {
    unsigned int extra, s = seqlz_len_symbol(v, &extra) & (ENC_LEN_SYMBOLS - 1U);

    return (t->enc[s] >> 16 & 15U) + extra;
}

/* bits of token and raw offset bits by min(ll, 15), ml - 4 up to 31 and the class, per parse */
struct opt_prices {
    u8 bits[SEQLZ_LL_CAP + 1][SEQLZ_ML_CAP + 1][4];
};

static void opt_prices_init(const struct seqlz_tables* t, struct opt_prices* p) {
    unsigned int ll, m, cls, len;

    for (ll = 0; ll <= SEQLZ_LL_CAP; ll++)
        for (m = 0; m <= SEQLZ_ML_CAP; m++)
            for (cls = 0; cls < 4U; cls++) {
                (void)token_code(t, seqlz_token(ll, m + 4U, cls), &len);
                p->bits[ll][m][cls] = (u8)(len + SEQLZ_RAW_BITS(cls));
            }
}

struct opt_node {
    u32 price;    /* bits to get here */
    u16 ll, last; /* literals since the last match, the offset of the last match */
    u16 len, off; /* how we got here: a match of len at offset off, or len 0 for a literal */
};

#ifndef OPT_ENOUGH
#    define OPT_ENOUGH 256U
#endif
#ifndef OPT_CHAIN
#    define OPT_CHAIN 16U
#endif
#define OPT_HASH_BITS 14U

/*
 * The cheapest parse by the code lengths of the tables, forward: at each position the literal and the
 * matches from a hash chain of 4 bytes and the last offset, each length from 4 to the longest, priced
 * with the token for the literals since the last match. One state per position, the cheapest.
 * work: seqlz_opt_work_size() bytes. Returns the length of the page, coded as seqlz_encode_coded().
 */
unsigned int seqlz_parse_opt(
    const struct seqlz_tables* t, const void* src_v, void* work, unsigned int lit_set, struct seqlz_sequence* seq) {
    const u8* const src = src_v;
    struct opt_node* const node = work;
    u16* const head = (u16*)(node + SEQLZ_PAGE + 1);
    u16* const prev = head + (1U << OPT_HASH_BITS);
    u16* const back = prev + SEQLZ_PAGE; /* the ends of the matches, from the end of the page */
    struct opt_prices* const prices = (struct opt_prices*)(back + SEQLZ_MAX_SEQUENCES + 1U); /* not on the stack */
    const u8* const lit_len = seqlz_lit_sets[lit_set % SEQLZ_LIT_SETS];
    unsigned int i, n, k;

    opt_prices_init(t, prices);
    for (i = 0; i <= SEQLZ_PAGE; i++)
        node[i].price = ~0U;
    node[0] = (struct opt_node){0, 0, 1, 0, 0};
    for (i = 0; i < (1U << OPT_HASH_BITS); i++)
        head[i] = 0xffffU;
    for (i = 0; i < SEQLZ_PAGE; i++) {
        const struct opt_node here = node[i];
        unsigned int c, cand[OPT_CHAIN + 1], nc = 0, best_len = 3;

        /* the literal */
        c = here.price + lit_len[src[i]];
        if (c < node[i + 1].price)
            node[i + 1] = (struct opt_node){c, (u16)(here.ll + 1U), here.last, 0, 0};
        if (i + 4U > SEQLZ_PAGE)
            continue;
        /* candidates: the last offset first, then the chain, only ones that are longer */
        if (here.last <= i)
            cand[nc++] = here.last;
        {
            u32 h = (load32(src + i) * 2654435761U) >> (32U - OPT_HASH_BITS);
            unsigned int p = head[h], steps = 0;

            while (p != 0xffffU && steps++ < OPT_CHAIN) {
                cand[nc++] = i - p;
                p = prev[p];
            }
            prev[i] = head[h];
            head[h] = (u16)i;
        }
        for (k = 0; k < nc; k++) {
            unsigned int off = cand[k], len, l, cls;

            if (off == 0 || load32(src + i) != load32(src + i - off))
                continue;
            len = 4U + count(src + i + 4, src + i - off + 4, src + SEQLZ_PAGE);
            if (len <= best_len && off != here.last)
                continue;
            if (len > best_len)
                best_len = len;
            cls = off == here.last ? 0U : off < 16U ? 1U : off < 256U ? 2U : 3U;
            {
                /* each length up to the token's cap from the table, above it only the longest */
                unsigned int llc = here.ll < SEQLZ_LL_CAP ? here.ll : SEQLZ_LL_CAP;
                unsigned int base = here.price + (here.ll >= SEQLZ_LL_CAP ? value_bits(&t->ll, here.ll - SEQLZ_LL_CAP) : 0U);
                unsigned int top = len < SEQLZ_ML_CAP + 4U ? len : SEQLZ_ML_CAP + 3U;

                for (l = 4; l <= top; l++) {
                    c = base + prices->bits[llc][l - 4U][cls];
                    if (c < node[i + l].price)
                        node[i + l] = (struct opt_node){c, 0, (u16)off, (u16)l, (u16)off};
                }
                if (len >= SEQLZ_ML_CAP + 4U) {
                    c = base + prices->bits[llc][SEQLZ_ML_CAP][cls] + value_bits(&t->ml, len - 4U - SEQLZ_ML_CAP);
                    if (c < node[i + len].price)
                        node[i + len] = (struct opt_node){c, 0, (u16)off, (u16)len, (u16)off};
                }
            }
        }
        /* A match this long is taken as it is: the positions inside it are only put into the chains,
         * not searched. Without that, runs cost milliseconds, each position counting the run again
         * for every candidate. */
        if (best_len >= OPT_ENOUGH) {
            unsigned int j, end = i + best_len;

            for (j = i + 1; j < end && j + 4U <= SEQLZ_PAGE; j++) {
                u32 h = (load32(src + j) * 2654435761U) >> (32U - OPT_HASH_BITS);

                prev[j] = head[h];
                head[h] = (u16)j;
            }
            i = end - 1U;
        }
    }
    /* back from the end: the last sequence is the literals up to the end */
    n = 0;
    {
        unsigned int pos = SEQLZ_PAGE;
        unsigned int nb = 0;

        while (pos > 0) {
            if (node[pos].len == 0) {
                pos--;
                continue;
            }
            back[nb++] = (u16)pos;
            pos -= node[pos].len;
        }
        /* forward: literals between matches */
        pos = 0;
        for (k = nb; k-- > 0;) {
            unsigned int end = back[k], ml = node[end].len, start = end - ml;

            seq[n].literals = (u16)(start - pos);
            seq[n].match = (u16)ml;
            seq[n].offset = node[end].off;
            n++;
            pos = end;
        }
        seq[n].literals = (u16)(SEQLZ_PAGE - pos);
        seq[n].match = 0;
        seq[n].offset = 0;
        n++;
    }
    return n;
}

unsigned int seqlz_compress_opt(const struct seqlz_tables* t,
                                const void* src_v,
                                void* dst,
                                unsigned int dst_cap,
                                void* scratch,
                                void* work,
                                unsigned int lit_set) {
    const u8* const src = src_v;
    struct seqlz_sequence* const seq =
        (struct seqlz_sequence*)((u8*)work + sizeof(struct opt_node) * (SEQLZ_PAGE + 1) +
                                 sizeof(u16) * ((1U << OPT_HASH_BITS) + SEQLZ_PAGE + SEQLZ_MAX_SEQUENCES + 1U) +
                                 sizeof(struct opt_prices));
    u8* const lits = (u8*)(seq + SEQLZ_MAX_SEQUENCES);
    unsigned int n = seqlz_parse_opt(t, src, work, lit_set, seq), i, pos = 0, n_lit = 0;

    for (i = 0; i < n; i++) {
        __builtin_memcpy(lits + n_lit, src + pos, seq[i].literals);
        n_lit += seq[i].literals;
        pos += seq[i].literals + seq[i].match;
    }
    (void)scratch;
    return seqlz_encode_coded(t, seq, n, lits, n_lit, dst, dst_cap);
}

__SIZE_TYPE__ seqlz_opt_work_size(void) {
    return sizeof(struct opt_node) * (SEQLZ_PAGE + 1) +
           sizeof(u16) * ((1U << OPT_HASH_BITS) + SEQLZ_PAGE + SEQLZ_MAX_SEQUENCES + 1U) + sizeof(struct opt_prices) +
           sizeof(struct seqlz_sequence) * SEQLZ_MAX_SEQUENCES + SEQLZ_PAGE;
}
