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
 * Token table: bits 0-3 the code length, bits 4-7 min(ll, 15), bits 8-11 min(ml - 4, 15).
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
 * entries of entry(symbol) | code length. -1 if a length is longer than bits, the codes are
 * over-subscribed, or there is no code at all.
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
    /* Kraft: the codes must fit into bits bits */
    for (l = 1, k = 0; l <= bits; l++)
        k += count[l] << (bits - l);
    if (k > (1U << bits))
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
    return 0;
}

/* ---- encoder ---- */

struct bit_writer {
    u8* p;
    u8* end;
    u64 bits;
    unsigned int count;
    int overflow;
};

static void put(struct bit_writer* w, u64 value, unsigned int n) {
    w->bits |= value << w->count;
    w->count += n;
    while (w->count >= 8) {
        if (w->p == w->end) {
            w->overflow = 1;
            return;
        }
        *w->p++ = (u8)w->bits;
        w->bits >>= 8;
        w->count -= 8;
    }
}

static unsigned int finish(struct bit_writer* w, u8* start) {
    if (w->count > 0) {
        if (w->p == w->end)
            w->overflow = 1;
        else
            *w->p++ = (u8)w->bits;
    }
    return (unsigned int)(w->p - start);
}

/* a symbol with its code from code/len, -1 if the tables have no code for it */
static int put_code(struct bit_writer* w, const u16* code, const u8* len, unsigned int s) {
    if (len[s] == 0)
        return -1;
    put(w, code[s], len[s]);
    return 0;
}

static int put_length(struct bit_writer* w, const struct value_table* t, unsigned int v) {
    unsigned int extra, s = seqlz_len_symbol(v, &extra);

    if (put_code(w, t->code, t->len, s))
        return -1;
    put(w, v & ((1U << extra) - 1U), extra);
    return 0;
}

static inline void store16(u8* p, unsigned int v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

unsigned int seqlz_encode(const struct seqlz_tables* t,
                          const struct seqlz_sequence* seq,
                          unsigned int n,
                          const unsigned char* literals,
                          unsigned int n_literals,
                          void* dst,
                          unsigned int dst_cap) {
    u8* d = dst;
    struct bit_writer w;
    unsigned int rep[3] = {1, 4, 8}, i, bytes, extra, s;
    u8* start;

    if (dst_cap < SEQLZ_HEADER + n_literals || n > 0xffff)
        return 0;
    store16(d, n);
    store16(d + 2, n_literals);
    __builtin_memcpy(d + SEQLZ_HEADER, literals, n_literals);

    start = d + SEQLZ_HEADER + n_literals;
    w = (struct bit_writer){start, d + dst_cap, 0, 0, 0};
    for (i = 0; i < n; i++) {
        unsigned int ll = seq[i].literals, last = i + 1 == n, ml = last ? 0 : seq[i].match, off, r;

        if (put_code(&w, t->token.code, t->token.len, seqlz_token(ll, ml)))
            return 0;
        if (ll >= 15 && put_length(&w, &t->ll, ll - 15))
            return 0;
        if (last)
            break;
        if (ml - 4 >= 15 && put_length(&w, &t->ml, ml - 4 - 15))
            return 0;

        off = seq[i].offset;
        for (r = 0; r < 3 && rep[r] != off; r++) {
        }
        if (r < 3) {
            if (put_code(&w, t->off.code, t->off.len, r))
                return 0;
            for (; r > 0; r--)
                rep[r] = rep[r - 1];
        } else {
            s = seqlz_off_bucket(off, &extra);
            if (put_code(&w, t->off.code, t->off.len, s))
                return 0;
            put(&w, off & ((1U << extra) - 1U), extra);
            rep[2] = rep[1];
            rep[1] = rep[0];
        }
        rep[0] = off;
    }
    bytes = finish(&w, start);
    if (w.overflow)
        return 0;
    return SEQLZ_HEADER + n_literals + bytes;
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

/* One value: the table entry of the next code, then base + extra bits, in one step. Needs 10 + 12
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
    unsigned int n, n_lit, i, rep0 = 1, rep1 = 4, rep2 = 8, bad = 0;

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

        /* One refill per sequence: token, literal length value and offset need at most 11 + 22 + 21 =
         * 54 of the at least 56 bits a refill leaves. Only after a match length value, up to 22 more
         * bits, the offset needs another one. */
        refill(&br);
        tok = t->token.decode[br.bits & ((1U << SEQLZ_TOKEN_BITS) - 1U)];
        bad |= tok == 0;
        drop(&br, tok & 15U);
        nl = (tok >> 4) & 15U;
        len = ((tok >> 8) & 15U) + 4U;
        if (nl == 15) {
            nl += value(&br, &t->ll, &e);
            bad |= e == 0;
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

        if (len == 19) {
            /* no refill before: token and literal length value used at most 33 of the 56 bits */
            len += value(&br, &t->ml, &e);
            bad |= e == 0;
            refill(&br);
        }
        v = value(&br, &t->off, &e);
        bad |= e == 0;
        /* Repeat offsets without branches: the new first is always this offset, the second is the old
         * first unless this was the old first, the third the old second if this was the old second
         * or third or new, the old third otherwise. */
        {
            unsigned int is_rep = (e >> 24) & 1U, idx = is_rep ? v : 3U;

            off = idx == 0 ? rep0 : idx == 1 ? rep1 : idx == 2 ? rep2 : v;
            rep2 = idx >= 2 ? rep1 : rep2;
            rep1 = idx >= 1 ? rep0 : rep1;
            rep0 = off;
        }
        if (off == 0 || off > (unsigned int)(d - (u8*)dst) || len > (unsigned int)(d_end - d))
            return -1;
        /* 8 bytes at a time while there are 8 bytes of room behind in the page; may write past len,
         * which the next sequence overwrites. For an offset below 8 the first bytes are written one
         * by one, up to the largest multiple of the offset that fits into 8; then each step copies
         * from that far back inside the match, so the first step bytes of every store repeat the
         * pattern. The rest one by one, at most 7 bytes at the end of the page. A run to the end of
         * the page byte by byte was what made the slowest pages 10 times slower than lz4. */
        {
            unsigned int step = off >= 8 ? 8U : step_for[off], back = off >= 8 ? off : step;

            k = 0;
            if (off < 8) {
                for (; k < step && k < len; k++)
                    d[k] = *(d + k - off);
            } else if ((unsigned int)(d_end - d) >= 16U) {
                /* most matches are shorter than 16 bytes: two unconditional copies, the second reads
                 * only bytes the first wrote or that were there before, because off >= 8 */
                u64 a, b;

                __builtin_memcpy(&a, d - off, 8);
                __builtin_memcpy(d, &a, 8);
                __builtin_memcpy(&b, d + 8 - off, 8);
                __builtin_memcpy(d + 8, &b, 8);
                k = 16;
            }
            while (k < len && (unsigned int)(d_end - d) >= k + 8U) {
                u64 w;

                __builtin_memcpy(&w, d + k - back, 8);
                __builtin_memcpy(d + k, &w, 8);
                k += step;
            }
            for (; k < len; k++)
                d[k] = *(d + k - off);
        }
        d += len;
    }
    /* An invalid code gives length 0 and uses no bits, and every copy above is checked, so it is safe
     * to find out only here that the page was not valid. */
    if (bad || d != d_end || lit != lit_end || br.count < 0)
        return -1;
    return 0;
}
