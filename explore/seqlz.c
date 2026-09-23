// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "seqlz.h"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

/*
 * Decode table entry, 0 for an unused code, otherwise: bits 0-3 the code length, bits 4-7 the number
 * of extra bits, bits 8-23 the base value, bit 24 set for a repeat offset (the base is its index).
 * A value is then base + the extra bits, in one step.
 */
struct code_table {
    u32 decode[1U << SEQLZ_MAX_BITS];
    u16 code[SEQLZ_LEN_SYMBOLS]; /* bit reversed, so it can be written least significant bit first */
    u8 len[SEQLZ_LEN_SYMBOLS];
};

struct seqlz_tables {
    struct code_table ll, ml, off;
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

/* base and extra bits of a length symbol, see seqlz.h */
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

/* canonical Huffman codes from the lengths, then the decode table */
static int build(struct code_table* t, const u8* len, unsigned int n, u32 (*entry)(unsigned int)) {
    unsigned int count[SEQLZ_MAX_BITS + 1] = {0}, next[SEQLZ_MAX_BITS + 2], code = 0, used = 0, s, l, k;

    __builtin_memset(t, 0, sizeof(*t));
    for (s = 0; s < n; s++) {
        if (len[s] > SEQLZ_MAX_BITS)
            return -1;
        count[len[s]]++;
        used += len[s] != 0;
    }
    if (used == 0)
        return -1;
    /* Kraft: the codes must fit into SEQLZ_MAX_BITS bits */
    for (l = 1, k = 0; l <= SEQLZ_MAX_BITS; l++)
        k += count[l] << (SEQLZ_MAX_BITS - l);
    if (k > (1U << SEQLZ_MAX_BITS))
        return -1;
    count[0] = 0;
    for (l = 1; l <= SEQLZ_MAX_BITS; l++) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    for (s = 0; s < n; s++) {
        unsigned int r;

        l = len[s];
        t->len[s] = (u8)l;
        if (l == 0)
            continue;
        r = reverse(next[l]++, l);
        t->code[s] = (u16)r;
        for (k = r; k < (1U << SEQLZ_MAX_BITS); k += 1U << l)
            t->decode[k] = entry(s) | l;
    }
    return 0;
}

int seqlz_tables_init(struct seqlz_tables* t, const struct seqlz_lengths* lengths) {
    if (build(&t->ll, lengths->ll, SEQLZ_LEN_SYMBOLS, length_entry) ||
        build(&t->ml, lengths->ml, SEQLZ_LEN_SYMBOLS, length_entry) ||
        build(&t->off, lengths->off, SEQLZ_OFF_SYMBOLS, offset_entry))
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

static int put_symbol(struct bit_writer* w, const struct code_table* t, unsigned int s) {
    if (t->len[s] == 0)
        return -1;
    put(w, t->code[s], t->len[s]);
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
    unsigned int rep[3] = {1, 4, 8}, i, ll_bytes, ml_bytes, off_bytes, extra, s;
    u8* start;

    if (dst_cap < SEQLZ_HEADER + n_literals || n > 0xffff)
        return 0;
    store16(d, n);
    store16(d + 2, n_literals);
    __builtin_memcpy(d + SEQLZ_HEADER, literals, n_literals);

    /* ll stream */
    start = d + SEQLZ_HEADER + n_literals;
    w = (struct bit_writer){start, d + dst_cap, 0, 0, 0};
    for (i = 0; i < n; i++) {
        s = seqlz_len_symbol(seq[i].literals, &extra);
        if (put_symbol(&w, &t->ll, s))
            return 0;
        put(&w, seq[i].literals & ((1U << extra) - 1U), extra);
    }
    ll_bytes = finish(&w, start);

    /* ml stream, one symbol less: the last sequence has no match */
    start += ll_bytes;
    w = (struct bit_writer){start, d + dst_cap, 0, 0, w.overflow};
    for (i = 0; i + 1 < n; i++) {
        unsigned int v = seq[i].match - 4U;

        s = seqlz_len_symbol(v, &extra);
        if (put_symbol(&w, &t->ml, s))
            return 0;
        put(&w, v & ((1U << extra) - 1U), extra);
    }
    ml_bytes = finish(&w, start);

    /* offset stream */
    start += ml_bytes;
    w = (struct bit_writer){start, d + dst_cap, 0, 0, w.overflow};
    for (i = 0; i + 1 < n; i++) {
        unsigned int off = seq[i].offset, r;

        for (r = 0; r < 3 && rep[r] != off; r++) {
        }
        if (r < 3) {
            if (put_symbol(&w, &t->off, r))
                return 0;
            for (; r > 0; r--)
                rep[r] = rep[r - 1];
        } else {
            s = seqlz_off_bucket(off, &extra);
            if (put_symbol(&w, &t->off, s))
                return 0;
            put(&w, off & ((1U << extra) - 1U), extra);
            rep[2] = rep[1];
            rep[1] = rep[0];
        }
        rep[0] = off;
    }
    off_bytes = finish(&w, start);
    if (w.overflow || ll_bytes > 0xffff || ml_bytes > 0xffff)
        return 0;
    store16(d + 4, ll_bytes);
    store16(d + 6, ml_bytes);
    return SEQLZ_HEADER + n_literals + ll_bytes + ml_bytes + off_bytes;
}

/* ---- decoder ---- */

/*
 * Least significant bit first. Refill loads 8 bytes at once while at least 8 are left in the stream,
 * and byte by byte at the end, so it never reads past the stream. Past the end it shifts in zeros and
 * notes it; memory safety does not depend on the bits, every length and offset is checked where it
 * is used.
 */
struct bit_reader {
    const u8* p;
    const u8* end;
    u64 bits;
    unsigned int count;
    unsigned int past_end; /* set once a value used bits beyond the end: the page is not valid */
};

static inline void refill(struct bit_reader* r) {
    if (r->end - r->p >= 8) {
        u64 v;

        __builtin_memcpy(&v, r->p, 8);
        r->bits |= v << r->count;
        r->p += (63U - r->count) >> 3;
        r->count |= 56U;
    } else {
        while (r->count <= 56U && r->p < r->end) {
            r->bits |= (u64)*r->p++ << r->count;
            r->count += 8;
        }
    }
}

/* One value: the table entry of the next code, then base + extra bits, in one step. Needs 10 + 12
 * bits, refilled before. The entry is 0 for bits that start no code. */
static inline unsigned int value(struct bit_reader* r, const struct code_table* t, u32* entry) {
    u32 e = t->decode[r->bits & ((1U << SEQLZ_MAX_BITS) - 1U)];
    unsigned int n = e & 15U, x = (e >> 4) & 15U, used = n + x;
    unsigned int v = ((e >> 8) & 0xffffU) + (unsigned int)((r->bits >> n) & ((1ULL << x) - 1ULL));

    *entry = e;
    r->bits >>= used;
    r->past_end |= r->count < used;
    r->count = r->count < used ? 0 : r->count - used;
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
    struct bit_reader ll, ml, of;
    unsigned int n, n_lit, ll_bytes, ml_bytes, i, rep0 = 1, rep1 = 4, rep2 = 8;

    if (src_len < SEQLZ_HEADER)
        return -1;
    n = load16(s);
    n_lit = load16(s + 2);
    ll_bytes = load16(s + 4);
    ml_bytes = load16(s + 6);
    if (n == 0 || (u64)SEQLZ_HEADER + n_lit + ll_bytes + ml_bytes > src_len)
        return -1;
    lit = s + SEQLZ_HEADER;
    lit_end = lit + n_lit;
    ll = (struct bit_reader){lit_end, lit_end + ll_bytes, 0, 0, 0};
    ml = (struct bit_reader){ll.end, ll.end + ml_bytes, 0, 0, 0};
    of = (struct bit_reader){ml.end, s_end, 0, 0, 0};

    for (i = 0;; i++) {
        unsigned int nl, len, off, v, k;
        u32 e_ll, e_ml, e_of;

        refill(&ll);
        nl = value(&ll, &t->ll, &e_ll);
        if (e_ll == 0 || nl > (unsigned int)(lit_end - lit) || nl > (unsigned int)(d_end - d))
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

        refill(&ml);
        refill(&of);
        len = value(&ml, &t->ml, &e_ml) + 4U;
        v = value(&of, &t->off, &e_of);
        if (e_ml == 0 || e_of == 0)
            return -1;
        /* Repeat offsets without branches: the new first is always this offset, the second is the old
         * first unless this was the old first, the third the old second if this was the old second
         * or third or new, the old third otherwise. */
        {
            unsigned int is_rep = (e_of >> 24) & 1U, idx = is_rep ? v : 3U;

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
    if (d != d_end || lit != lit_end || ll.past_end || ml.past_end || of.past_end)
        return -1;
    return 0;
}
