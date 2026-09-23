// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "seqlz.h"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

/*
 * Decode table entries, 0 for bits that start no code.
 * Length and offset tables: bits 0-3 the code length, bits 4-7 the number of extra bits, bits 8-23 the
 * base value, bit 24 set for a repeat offset (the base is its index). A value is base + extra bits.
 * Token table: bits 0-3 the code length, bits 4-7 min(ll, 15), bits 8-12 min(ml - 4, 31).
 */
struct value_table {
    u32 decode[1U << SEQLZ_MAX_BITS];
    u16 code[SEQLZ_LEN_SYMBOLS]; /* bit reversed, so it can be written least significant bit first */
    u8 len[SEQLZ_LEN_SYMBOLS];
};

struct token_table {
    u16 decode[1U << SEQLZ_TOKEN_BITS];
    u16 code[SEQLZ_TOKEN_SYMBOLS];
    u8 len[SEQLZ_TOKEN_SYMBOLS];
};

struct seqlz_tables {
    struct token_table token;
    struct value_table ll, ml, off;
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

/* the same for an offset symbol */
static u32 offset_entry(unsigned int s) {
    if (s < 3)
        return (s << 8) | (1U << 24);
    return ((1U << (s - 3U)) << 8) | ((s - 3U) << 4);
}

/* ll and ml - 4 of a token symbol */
static u32 token_entry(unsigned int s) {
    return ((s & 15U) << 4) | ((s >> 4) << 8);
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
                 u16* code,
                 u8* out_len,
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
        out_len[s] = (u8)l;
        code[s] = 0;
        if (l == 0)
            continue;
        r = reverse(next[l]++, l);
        code[s] = (u16)r;
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
    if (build(lengths->token,
              SEQLZ_TOKEN_SYMBOLS,
              SEQLZ_TOKEN_BITS,
              token_entry,
              t->token.code,
              t->token.len,
              0,
              t->token.decode) ||
        build(lengths->ll, SEQLZ_LEN_SYMBOLS, SEQLZ_MAX_BITS, length_entry, t->ll.code, t->ll.len, t->ll.decode, 0) ||
        build(lengths->ml, SEQLZ_LEN_SYMBOLS, SEQLZ_MAX_BITS, length_entry, t->ml.code, t->ml.len, t->ml.decode, 0) ||
        build(lengths->off, SEQLZ_OFF_SYMBOLS, SEQLZ_MAX_BITS, offset_entry, t->off.code, t->off.len, t->off.decode, 0))
        return -1;
    {
        const u8* l = (const u8*)lengths;
        unsigned int k, all = 1;

        for (k = 0; k < sizeof(*lengths); k++)
            all &= l[k] != 0;
        t->all_symbols = (u8)all;
    }
    return 0;
}

/* ---- shared ---- */

#define ALWAYS_INLINE inline __attribute__((always_inline))

static inline void store16(u8* p, unsigned int v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

/*
 * The move to front of the repeat offsets, the same for encoder and decoder: idx 0 to 2 picked a repeat
 * offset, 3 is a new one. The old first becomes second unless it was picked, the old second becomes
 * third if the second, third or a new one was picked. Two loads with computed indices, no branch: as
 * ?: the compiler made branches of it, 28% of all mispredictions of the decoder.
 */
static ALWAYS_INLINE void rep_update(unsigned int* rep, unsigned int idx, unsigned int off) {
    unsigned int r1 = rep[idx == 0], r2 = rep[2U - (idx >= 2)];

    rep[0] = off;
    rep[1] = r1;
    rep[2] = r2;
}

/* ---- matcher ---- */

static inline u32 load32(const u8* p) {
    u32 v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

static inline u64 load64(const u8* p) {
    u64 v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static inline void store64(u8* p, u64 v) {
    __builtin_memcpy(p, &v, 8);
}

static inline unsigned int hash4(u32 v) {
    return (v * 2654435761U) >> (32U - SEQLZ_HASH_BITS);
}

/* number of equal bytes at p and q, p after q, up to end */
static inline unsigned int count(const u8* p, const u8* q, const u8* end) {
    const u8* start = p;

    while (end - p >= 8) {
        u64 x = load64(p) ^ load64(q);

        if (x)
            return (unsigned int)(p - start) + ((unsigned int)__builtin_ctzll(x) >> 3);
        p += 8;
        q += 8;
    }
    while (p < end && *p == *q) {
        p++;
        q++;
    }
    return (unsigned int)(p - start);
}

/* What the matcher hands on per sequence: the literals before the match, the match length, 0 for the
 * last sequence, and the offset. */
typedef void (*emit_fn)(void* ctx, const u8* literals, unsigned int ll, unsigned int ml, unsigned int off);

/*
 * Greedy, like lz4's fast mode: at every position the last offset and one candidate from a hash of 4
 * bytes. Without a match the step grows with the distance to the last match, like lz4's acceleration,
 * so incompressible pages go by fast. Each sequence goes to emit as soon as it is found; inlined with
 * the encoder that is one pass over the page, and the same matcher feeds seqlz_find.
 */
static ALWAYS_INLINE void match_page(struct seqlz_state* st, const u8* src, emit_fn emit, void* ctx) {
    const u8* const end = src + SEQLZ_PAGE;
    const u8* const limit = end - 8; /* 8 bytes readable for the first comparison */
    const u8* ip = src + 1;
    const u8* anchor = src;
    unsigned int last = 1;
    unsigned short* table = st->table;

    while (ip < limit) {
        u32 cur = load32(ip);
        unsigned int h = hash4(cur), pos = (unsigned int)(ip - src), cand = table[h], len;
        /* One branch for both candidates, not three: the last offset always points into the page
         * (it starts at 1, the search at position 1), and so does a table entry from an earlier page,
         * so both can be read before it is known whether they count. Three branches mispredicted
         * almost twice as often as lz4's one. */
        unsigned int rep_hit = load32(ip - last) == cur;
        unsigned int cand_hit = (cand < pos) & (load32(src + cand) == cur);
        const u8* m;

        table[h] = (unsigned short)pos;
        if (!(rep_hit | cand_hit)) {
            ip += 1U + ((unsigned int)(ip - anchor) >> 6);
            continue;
        }
        m = rep_hit ? ip - last : src + cand;
        /* backwards into the literals, then forwards */
        while (ip > anchor && m > src && ip[-1] == m[-1]) {
            ip--;
            m--;
        }
        len = 4U + count(ip + 4, m + 4, end);
        last = (unsigned int)(ip - m);
        emit(ctx, anchor, (unsigned int)(ip - anchor), len, last);
        ip += len;
        anchor = ip;
        /* a position near the end of the match, for the next matches */
        if (ip < limit)
            table[hash4(load32(ip - 2))] = (unsigned short)(ip - 2 - src);
    }
    emit(ctx, anchor, (unsigned int)(end - anchor), 0, 0);
}

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

    match_page(st, src, find_emit, &f);
    return f.n;
}

/* ---- encoder ---- */

/*
 * One encoder for both entry points. Literals go to the front of dst, right after the header; the
 * bitstream goes behind the room of a page of literals and is moved in behind the literals at the end.
 * dst has two pages, and that is always enough: the most bits per page byte are a sequence of 4 bytes
 * without literals, an 11-bit token and a 20-bit offset, so at most 1024 * 31 + 31 bits, 3972 bytes;
 * behind 4 + 4096 + 16 bytes of header, literals and room for their 16-byte copies there are 4076. The
 * code lengths are capped at 11 and 9 bits, so this holds for any tables.
 * The bit writer is a 64-bit accumulator, stored 8 bytes at a time and advanced by the whole bytes; a
 * sequence has at most 11 + 20 + 20 + 20 bits, so it flushes after the token and literal length and
 * after the offset.
 */
struct encoder {
    const struct seqlz_tables* t;
    u64 acc;
    unsigned int cnt;
    u8* p;             /* bitstream */
    u8* lit;           /* literals */
    const u8* src_end; /* the 16-byte literal copies may read up to here */
    unsigned int rep[3];
    unsigned int n;
};

static ALWAYS_INLINE void enc_put(struct encoder* e, u64 v, unsigned int n) {
    e->acc |= v << e->cnt;
    e->cnt += n;
}

static ALWAYS_INLINE void enc_flush(struct encoder* e) {
    store64(e->p, e->acc);
    e->p += e->cnt >> 3;
    e->acc >>= e->cnt & ~7U;
    e->cnt &= 7U;
}

static ALWAYS_INLINE void put_len_value(struct encoder* e, const struct value_table* t, unsigned int v) {
    unsigned int extra, s = seqlz_len_symbol(v, &extra);

    enc_put(e, t->code[s], t->len[s]);
    enc_put(e, v & ((1U << extra) - 1U), extra);
}

/* one sequence: its literals from in, ml 0 for the last one */
static ALWAYS_INLINE void encode_emit(void* ctx, const u8* in, unsigned int ll, unsigned int ml, unsigned int off) {
    struct encoder* e = ctx;
    const struct seqlz_tables* t = e->t;
    unsigned int tok = seqlz_token(ll, ml), k = 0;

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
    e->n++;

    enc_put(e, t->token.code[tok], t->token.len[tok]);
    if (ll >= SEQLZ_LL_CAP)
        put_len_value(e, &t->ll, ll - SEQLZ_LL_CAP);
    enc_flush(e);
    if (ml == 0)
        return;
    if (ml - 4 >= SEQLZ_ML_CAP)
        put_len_value(e, &t->ml, ml - 4 - SEQLZ_ML_CAP);
    {
        /* which repeat offset, without branches: the ?: chain was 11% of the encoder's mispredictions.
         * The symbol and the extra bits of a new offset are computed anyway and used with a mask. */
        unsigned int e0 = off == e->rep[0], e1 = (off == e->rep[1]) & (e0 ^ 1U);
        unsigned int e2 = (off == e->rep[2]) & ((e0 | e1) ^ 1U), is_rep = e0 | e1 | e2;
        unsigned int idx = 3U - 3U * e0 - 2U * e1 - e2, extra, sym = seqlz_off_bucket(off, &extra);
        unsigned int mask = 0U - (is_rep ^ 1U);

        sym = (sym & mask) | (idx & ~mask);
        extra &= mask;
        enc_put(e, t->off.code[sym], t->off.len[sym]);
        enc_put(e, off & ((1U << extra) - 1U), extra);
        rep_update(e->rep, idx, off);
    }
    enc_flush(e);
}

static ALWAYS_INLINE void encoder_init(struct encoder* e, const struct seqlz_tables* t, u8* d, const u8* src_end) {
    *e = (struct encoder){t, 0, 0, d + SEQLZ_HEADER + SEQLZ_PAGE + 16U, d + SEQLZ_HEADER, src_end, {1, 4, 8}, 0};
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
    store16(d, e->n);
    store16(d + 2, n_lit);
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

unsigned int
seqlz_compress(const struct seqlz_tables* t, struct seqlz_state* st, const void* src_v, void* dst, unsigned int dst_cap) {
    const u8* const src = src_v;
    struct encoder e;

    if (dst_cap < 2U * SEQLZ_PAGE || !t->all_symbols)
        return 0;
    encoder_init(&e, t, dst, src + SEQLZ_PAGE);
    match_page(st, src, encode_emit, &e);
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

static inline unsigned int load16(const u8* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

int seqlz_decode(const struct seqlz_tables* t, const void* src, unsigned int src_len, void* dst) {
    static const u8 step_for[8] = {0, 8, 8, 6, 8, 5, 6, 7};
    const u8* s = src;
    const u8* const s_end = s + src_len;
    u8* d = dst;
    u8* const d_end = d + SEQLZ_PAGE;
    const u8 *lit, *lit_end;
    struct bit_reader br;
    unsigned int n, n_lit, i, rep[4] = {1, 4, 8, 0};

    if (src_len < SEQLZ_HEADER)
        return -1;
    n = load16(s);
    n_lit = load16(s + 2);
    if (n == 0 || (u64)SEQLZ_HEADER + n_lit > src_len)
        return -1;
    lit = s + SEQLZ_HEADER;
    lit_end = lit + n_lit;
    br = (struct bit_reader){lit_end, s_end, 0, 0};

    for (i = 0;; i++) {
        unsigned int tok, nl, len, off, v, k;
        u32 e;

        /* One refill per sequence: token, literal length value and offset need at most 11 + 21 + 20 =
         * 52 of the at least 56 bits a refill leaves. Only after a match length value, up to 21 more
         * bits, the offset needs another one. */
        refill(&br);
        tok = t->token.decode[br.bits & ((1U << SEQLZ_TOKEN_BITS) - 1U)];
        drop(&br, tok & 15U);
        nl = (tok >> 4) & 15U;
        len = ((tok >> 8) & 31U) + 4U;
        if (nl == SEQLZ_LL_CAP) {
            nl += value(&br, &t->ll, &e);
        }
        if (nl > (unsigned int)(lit_end - lit) || nl > (unsigned int)(d_end - d))
            return -1;
        /* 16 bytes at a time while there are 16 bytes of room behind, in the page and in the input;
         * may write and read past nl, which is overwritten or ignored. The rest one by one, at most 15
         * bytes at the end of the page or of the input. */
        k = 0;
        /* most literal runs are shorter than 16 bytes: one unconditional copy, no loop to mispredict */
        if ((unsigned int)(d_end - d) >= 16U && (unsigned int)(s_end - lit) >= 16U) {
            u64 a, b;

            __builtin_memcpy(&a, lit, 8);
            __builtin_memcpy(&b, lit + 8, 8);
            __builtin_memcpy(d, &a, 8);
            __builtin_memcpy(d + 8, &b, 8);
            k = 16;
        }
        while (k < nl && (unsigned int)(d_end - d) >= k + 16U && (unsigned int)(s_end - lit) >= k + 16U) {
            u64 a, b;

            __builtin_memcpy(&a, lit + k, 8);
            __builtin_memcpy(&b, lit + k + 8, 8);
            __builtin_memcpy(d + k, &a, 8);
            __builtin_memcpy(d + k + 8, &b, 8);
            k += 16;
        }
        for (; k < nl; k++)
            d[k] = lit[k];
        d += nl;
        lit += nl;
        if (i + 1 == n)
            break;

        if (len == SEQLZ_ML_CAP + 4U) {
            /* no refill before: token and literal length value used at most 32 of the 56 bits */
            len += value(&br, &t->ml, &e);
            refill(&br);
        }
        v = value(&br, &t->off, &e);
        /* Repeat offsets without branches, through an array: rep[3] is the new offset, idx picks the
         * offset. */
        {
            unsigned int is_rep = (e >> 24) & 1U, idx = v & (0U - is_rep);

            idx |= 3U & (0U - (is_rep ^ 1U));
            rep[3] = v;
            off = rep[idx];
            rep_update(rep, idx, off);
        }
        if (off == 0 || off > (unsigned int)(d - (u8*)dst) || len > (unsigned int)(d_end - d))
            return -1;
        /* 8 bytes at a time while there are 8 bytes of room behind in the page; may write past len,
         * which the next sequence overwrites. The rest one by one, at most 7 bytes at the end of the
         * page; a run to the end of the page byte by byte made the slowest pages 10 times slower
         * than lz4.
         * For an offset below 8 the first 8 bytes of the match are built in a register: the off bytes
         * before the match, repeated with shifts, then one 8-byte store. From there each step copies
         * from step bytes back, the largest multiple of off up to 8, which is exactly what the
         * previous store wrote, so the load gets it from that store. Writing the first bytes one by one
         * made the load wait for four stores, and a loop over them mispredicted its exit. */
        {
            unsigned int step = off >= 8 ? 8U : step_for[off], back = off >= 8 ? off : step;

            k = 0;
            if (off < 8) {
                if ((unsigned int)(d_end - d) >= 8U) {
                    u64 w, pat;
                    unsigned int bits = 8U * off;

                    /* d - off + 7 < d + 8 <= d_end: inside the page */
                    __builtin_memcpy(&w, d - off, 8);
                    pat = w & ((1ULL << bits) - 1ULL);
                    pat |= pat << bits;
                    pat |= (pat << ((2U * bits) & 63U)) & (0ULL - (u64)(2U * bits < 64U));
                    pat |= (pat << ((4U * bits) & 63U)) & (0ULL - (u64)(4U * bits < 64U));
                    __builtin_memcpy(d, &pat, 8);
                    k = step;
                } else {
                    back = 0; /* at the end of the page: one by one below */
                }
            } else if ((unsigned int)(d_end - d) >= 16U) {
                /* 79% of the matches are at most 16 bytes: two unconditional copies, the second reads
                 * only bytes the first wrote or that were there before, because off >= 8. Four copies,
                 * for 91% of them, were faster at p50 and slower at p99: the slowest pages have many
                 * short matches, and copied 32 bytes for each. */
                u64 a, b;

                __builtin_memcpy(&a, d - off, 8);
                __builtin_memcpy(d, &a, 8);
                __builtin_memcpy(&b, d + 8 - off, 8);
                __builtin_memcpy(d + 8, &b, 8);
                k = 16;
            }
            if (back != 0) {
                while (k < len && (unsigned int)(d_end - d) >= k + 8U) {
                    u64 w;

                    __builtin_memcpy(&w, d + k - back, 8);
                    __builtin_memcpy(d + k, &w, 8);
                    k += step;
                }
            }
            for (; k < len; k++)
                d[k] = *(d + k - off);
        }
        d += len;
    }
    if (d != d_end || lit != lit_end || br.count < 0)
        return -1;
    return 0;
}
