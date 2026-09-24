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

struct lit_table {
    u16 decode[1U << SEQLZ_LIT_BITS]; /* symbol | code length << 8 */
    u16 enc[256];                     /* code | length << 12 */
};

struct seqlz_tables {
    struct token_table token;
    struct lit_table lit;
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

/* a literal byte */
static u32 lit_entry(unsigned int s) {
    return s << 8;
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

int seqlz_all_symbols(const struct seqlz_tables* t) {
    return t->all_symbols;
}

int seqlz_tables_init(struct seqlz_tables* t, const struct seqlz_lengths* lengths) {
    __builtin_memset(t, 0, sizeof(*t)); /* also the encoder's entries behind the last symbol */
    if (build(lengths->token, SEQLZ_TOKEN_SYMBOLS + 1, SEQLZ_TOKEN_BITS, token_entry, 0, t->token.enc, 0, t->token.decode) ||
        build(lengths->ll, SEQLZ_LEN_SYMBOLS, SEQLZ_MAX_BITS, length_entry, t->ll.enc, 0, t->ll.decode, 0) ||
        build(lengths->ml, SEQLZ_LEN_SYMBOLS, SEQLZ_MAX_BITS, length_entry, t->ml.enc, 0, t->ml.decode, 0) ||
        build(lengths->lit, 256, SEQLZ_LIT_BITS, lit_entry, 0, t->lit.enc, 0, t->lit.decode))
        return -1;
    {
        /* every token has a code or the escape has one, every length value has one */
        unsigned int k, all = 1;

        for (k = 0; k < SEQLZ_TOKEN_SYMBOLS; k++)
            all &= lengths->token[k] != 0 || lengths->token[SEQLZ_ESCAPE] != 0;
        /* an escaped token and a 12-bit offset in at most 31 bits, the encoder's bound */
        all &= lengths->token[SEQLZ_ESCAPE] <= SEQLZ_MAX_ESCAPE_LEN;
        for (k = 0; k < SEQLZ_LEN_SYMBOLS; k++)
            all &= lengths->ll[k] != 0 && lengths->ml[k] != 0;
        for (k = 0; k < 256; k++)
            all &= lengths->lit[k] != 0;
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
        unsigned int raw_bits = 4U * cls + (QUETSCHN_PAGE_BITS - 12U) * (cls == 3U);
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

unsigned int seqlz_encode_coded(const struct seqlz_tables* t,
                                const struct seqlz_sequence* seq,
                                unsigned int n,
                                const unsigned char* literals,
                                unsigned int n_literals,
                                void* dst_v,
                                unsigned int dst_cap) {
    u8* const d = dst_v;
    unsigned int len = seqlz_encode(t, seq, n, literals, n_literals, dst_v, dst_cap), bits = 0, k, coded, seq_bytes;
    struct encoder e;

    if (len == 0)
        return 0;
    for (k = 0; k < n_literals; k++)
        bits += t->lit.enc[literals[k]] >> 12;
    coded = (bits + 7U) / 8U;
    /* EXPERIMENT: only if it saves at least 1/16: decoding coded literals costs time per byte (1/8:
     * 24.9% and cold p99 3840 ns, any saving: 24.3% and 4280). The coded page has 10 bytes of header
     * and up to 4 more for the ends of its streams. */
    if (coded + 14U >= n_literals - n_literals / 16U)
        return len;
    seq_bytes = len - SEQLZ_HEADER - n_literals;
    /* the sequences' bitstream out of the way, behind where it would ever be */
    __builtin_memmove(d + SEQLZ_HEADER + SEQLZ_PAGE + 16U, d + SEQLZ_HEADER + n_literals, seq_bytes);
    /* four streams, literal k in stream k % 4, so that the decoder has four chains side by side */
    {
        u8* p = d + 10;
        unsigned int st, sizes[4];

        for (st = 0; st < 4U; st++) {
            u8* begin = p;

            e = (struct encoder){t, 0, 0, p, 0, 0, 0};
            for (k = st; k < n_literals; k += 4) {
                u32 le = t->lit.enc[literals[k]];

                enc_put(&e, le & 0xfffU, le >> 12);
                if (((k >> 2) & 3U) == 3U)
                    enc_flush(&e);
            }
            enc_flush(&e);
            if (e.cnt > 0)
                e.p++;
            p = e.p;
            sizes[st] = (unsigned int)(p - begin);
        }
        coded = (unsigned int)(p - (d + 10));
        store16(d, 0x8000U | n_literals);
        for (st = 0; st < 4U; st++)
            store16(d + 2 + 2 * st, sizes[st]);
    }
    __builtin_memmove(d + 10 + coded, d + SEQLZ_HEADER + SEQLZ_PAGE + 16U, seq_bytes);
    return 10U + coded + seq_bytes;
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
        /* coded literals: four streams, decoded into scratch first, 5 rounds of 4 per refill */
        unsigned int sz0, sz1, sz2, sz3, k;
        struct bit_reader r0, r1, r2, r3;
        u8* out = scratch;
        const u8* q = s + 10;

        n_lit &= 0x7fffU;
        if (!scratch || src_len < 10U || n_lit > SEQLZ_PAGE)
            return -1;
        sz0 = load16(s + 2);
        sz1 = load16(s + 4);
        sz2 = load16(s + 6);
        sz3 = load16(s + 8);
        if ((u64)10U + sz0 + sz1 + sz2 + sz3 > src_len)
            return -1;
        r0 = (struct bit_reader){q, q + sz0, 0, 0};
        r1 = (struct bit_reader){q + sz0, q + sz0 + sz1, 0, 0};
        r2 = (struct bit_reader){q + sz0 + sz1, q + sz0 + sz1 + sz2, 0, 0};
        r3 = (struct bit_reader){q + sz0 + sz1 + sz2, q + sz0 + sz1 + sz2 + sz3, 0, 0};
        for (k = 0; k < n_lit;) {
            unsigned int j;

            refill(&r0);
            refill(&r1);
            refill(&r2);
            refill(&r3);
            /* scratch has room for 4 * 5 bytes past n_lit */
            for (j = 0; j < 5U; j++, k += 4) {
                unsigned int e0 = t->lit.decode[r0.bits & ((1U << SEQLZ_LIT_BITS) - 1U)];
                unsigned int e1 = t->lit.decode[r1.bits & ((1U << SEQLZ_LIT_BITS) - 1U)];
                unsigned int e2 = t->lit.decode[r2.bits & ((1U << SEQLZ_LIT_BITS) - 1U)];
                unsigned int e3 = t->lit.decode[r3.bits & ((1U << SEQLZ_LIT_BITS) - 1U)];

                out[k] = (u8)(e0 >> 8);
                out[k + 1] = (u8)(e1 >> 8);
                out[k + 2] = (u8)(e2 >> 8);
                out[k + 3] = (u8)(e3 >> 8);
                drop(&r0, e0 & 15U);
                drop(&r1, e1 & 15U);
                drop(&r2, e2 & 15U);
                drop(&r3, e3 & 15U);
            }
        }
        /* each stream may be read past its end only for the symbols behind n_lit */
        if (r0.count < -44 || r1.count < -44 || r2.count < -44 || r3.count < -44)
            return -1;
        br = (struct bit_reader){q + sz0 + sz1 + sz2 + sz3, s_end, 0, 0};
        lit = out;
        lit_end = out + n_lit;
        lit_bound = out + SEQLZ_SCRATCH;
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

        /* One refill per sequence: token, offset and literal length value need at most 11 + 12 + 21 =
         * 44 of the at least 56 bits a refill leaves. Only a match length value, up to 21 more bits,
         * needs another one. The token's entry has the class of the offset, so its raw bits are
         * known without a second lookup, and the next token's lookup waits for one load, not two. */
        refill(&br);
        tok = t->token.decode[br.bits & ((1U << SEQLZ_TOKEN_BITS) - 1U)];
        if (tok >> 15) {
            /* the escape: the token follows in SEQLZ_ESCAPE_BITS bits */
            unsigned int idx;

            drop(&br, tok & 15U);
            idx = (unsigned int)br.bits & ((1U << SEQLZ_ESCAPE_BITS) - 1U);
            drop(&br, SEQLZ_ESCAPE_BITS);
            if (idx >= SEQLZ_TOKEN_SYMBOLS)
                return -1;
            tok = token_entry(idx);
        }
        {
            unsigned int cls = tok >> 13, raw_bits = (cls << 2) + (QUETSCHN_PAGE_BITS - 12U) * (cls == 3U);
            unsigned int n_tok = tok & 15U;
            unsigned int is_new = 0U - (cls != 0);
            unsigned int raw = (unsigned int)(br.bits >> n_tok) & ((1U << raw_bits) - 1U);

            drop(&br, n_tok + raw_bits);
            off = (raw & is_new) | (last & ~is_new);
        }
        nl = (tok >> 4) & 15U;
        len = ((tok >> 8) & 31U) + 4U;
        if (nl == SEQLZ_LL_CAP) {
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
