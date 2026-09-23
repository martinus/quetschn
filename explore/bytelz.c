// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "bytelz.h"

#include "page_lz.h"

_Static_assert(PAGE_LZ_PAGE == BYTELZ_PAGE && sizeof(((struct bytelz_state*)0)->table) == 2U << PAGE_LZ_HASH_BITS,
               "page_lz.h and bytelz.h disagree");

/* ---- encoder ---- */

struct encoder {
    u8* op;
    const u8* src_end; /* the 16-byte literal copies may read up to here */
    unsigned int last, before;
};

/* An extension of v when need is 1: always 3 bytes written, as many kept as v needs. dst has room. */
static ALWAYS_INLINE u8* put_extension(u8* op, unsigned int v, unsigned int need) {
    unsigned int two = v >= 128U, three = v >= 16384U;

    op[0] = (u8)((v & 127U) | two << 7);
    op[1] = (u8)(((v >> 7) & 127U) | three << 7);
    op[2] = (u8)(v >> 14);
    return op + ((1U + two + three) & (0U - need));
}

/* One sequence, without branches except for literals longer than 16 bytes: the extensions and the
 * offset bytes are always written and op moves by as many as count. */
static ALWAYS_INLINE void encode_emit(void* ctx, const u8* in, unsigned int ll, unsigned int ml, unsigned int off) {
    struct encoder* e = ctx;
    u8* op = e->op;
    unsigned int e0 = off == e->last, e1 = off == e->before, small = off <= 256U, is_new = (e0 | e1) ^ 1U;
    unsigned int mode = e1 + is_new * (3U - small), k = 16;
    unsigned int llc = ll < 7U ? ll : 7U, mlc = ml - 4U < 7U ? ml - 4U : 7U;

    if (ml == 0) {
        /* the last sequence: ll and the literals, nothing else */
        mlc = 0;
        mode = 0;
    }
    *op++ = (u8)(llc | mlc << 3 | mode << 6);
    op = put_extension(op, ll - 7U, ll >= 7U);
    if ((unsigned int)(e->src_end - in) >= 16U)
        __builtin_memcpy(op, in, 16);
    else
        k = 0;
    for (; k < ll && (unsigned int)(e->src_end - in) >= k + 16U; k += 16)
        __builtin_memcpy(op + k, in + k, 16);
    for (; k < ll; k++)
        op[k] = in[k];
    op += ll;
    if (ml != 0) {
        /* one byte for offset - 1 up to 256, two for the offset */
        store16(op, off - small);
        op += is_new * (2U - small);
        op = put_extension(op, ml - 11U, ml >= 11U);
        e->before = e0 ? e->before : e->last;
        e->last = off;
    }
    e->op = op;
}

unsigned int bytelz_compress(struct bytelz_state* st, const void* src_v, void* dst, unsigned int dst_cap) {
    const u8* const src = src_v;
    struct encoder e = {dst, src + BYTELZ_PAGE, 1, 4};

    /* at most 3 bytes per 4 bytes of page, or the page as literals with 3 bytes before them, and up to
     * 16 bytes written behind: two pages are always enough */
    if (dst_cap < 2U * BYTELZ_PAGE)
        return 0;
    match_page(st->table, src, encode_emit, &e);
    return (unsigned int)(e.op - (u8*)dst);
}

/* ---- decoder ---- */

/* An extension, 1 to 3 bytes; -1 if the input ends before it or it does not end after 3 bytes. */
static inline int get_extension(const u8** sp, const u8* s_end, unsigned int* v) {
    const u8* s = *sp;
    unsigned int x = 0, shift = 0, b;

    do {
        if (s == s_end || shift > 14U)
            return -1;
        b = *s++;
        x |= (b & 127U) << shift;
        shift += 7;
    } while (b & 128U);
    *sp = s;
    *v = x;
    return 0;
}

int bytelz_decode(const void* src, unsigned int src_len, void* dst) {
    const u8* s = src;
    const u8* const s_end = s + src_len;
    u8* d = dst;
    u8* const d_end = d + BYTELZ_PAGE;
    unsigned int last = 1, before = 4;

    for (;;) {
        unsigned int tok, nl, len, mode, n_off, raw, v, off;

        if (s == s_end)
            return -1;
        tok = *s++;
        nl = tok & 7U;
        if (nl == 7U) {
            if (get_extension(&s, s_end, &v))
                return -1;
            nl += v;
        }
        if (nl > (unsigned int)(s_end - s) || nl > (unsigned int)(d_end - d))
            return -1;
        copy_literals(d, d_end, s, s_end, nl);
        d += nl;
        s += nl;
        if (d == d_end)
            return s == s_end && (tok >> 3) == 0 ? 0 : -1;

        /* the offset without branches: 0, 0, 1 or 2 bytes, the new one read in any case */
        mode = tok >> 6;
        n_off = (0x2100U >> (mode * 4U)) & 3U;
        if (n_off > (unsigned int)(s_end - s))
            return -1;
        raw = (unsigned int)(s_end - s) >= 2U ? load16(s) : n_off != 0 ? s[0] : 0U;
        s += n_off;
        {
            unsigned int one = mode == 2U, reps[4];

            reps[0] = last;
            reps[1] = before;
            reps[2] = (raw & (0xffffU >> (8U * one))) + one;
            reps[3] = reps[2];
            off = reps[mode];
            before = mode == 0 ? before : last;
            last = off;
        }
        len = ((tok >> 3) & 7U) + 4U;
        if (len == 11U) {
            if (get_extension(&s, s_end, &v))
                return -1;
            len += v;
        }
        if (off == 0 || off > (unsigned int)(d - (u8*)dst) || len > (unsigned int)(d_end - d))
            return -1;
        copy_match(d, d_end, off, len);
        d += len;
    }
}
