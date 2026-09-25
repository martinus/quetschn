// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "lz4page.h"

#include "page_lz.h"

#define MFLIMIT 12U     /* the last match starts at least this before the end */
#define LASTLITERALS 5U /* and ends at least this before it */

struct out {
    u8* op;
};

static ALWAYS_INLINE void put_length(struct out* o, unsigned int v) {
    while (v >= 255U) {
        *o->op++ = 255U;
        v -= 255U;
    }
    *o->op++ = (u8)v;
}

/* one sequence in lz4's format: the token, the literal length, the literals, and for ml > 0 the
 * offset and the rest of the match length */
static ALWAYS_INLINE void put_sequence(struct out* o, const u8* lit, unsigned int ll, unsigned int ml, unsigned int off) {
    unsigned int m = ml ? ml - 4U : 0U;

    *o->op++ = (u8)(((ll < 15U ? ll : 15U) << 4) | (m < 15U ? m : 15U));
    if (ll >= 15U)
        put_length(o, ll - 15U);
    __builtin_memcpy(o->op, lit, ll);
    o->op += ll;
    if (!ml)
        return;
    store16(o->op, off);
    o->op += 2;
    if (m >= 15U)
        put_length(o, m - 15U);
}

/* the longest match at pos from the last offset or the candidates, 0 if none; its start in *m */
static ALWAYS_INLINE unsigned int
best_at(unsigned short* table, const u8* src, unsigned int pos, unsigned int last, unsigned int* m, unsigned int flags) {
    const u8* const end = src + PAGE_LZ_PAGE - LASTLITERALS;
    const u32 cur = load32(src + pos);
    const unsigned int h = hash5(load64(src + pos)) & ((flags & LZ4PAGE_TWO_WAY) ? ~1U : ~0U);
    unsigned int best = 0, c;

    if (load32(src + pos - last) == cur) {
        best = 4U + count(src + pos + 4, src + pos - last + 4, end);
        *m = pos - last;
    }
    c = table[h];
    if (c < pos && load32(src + c) == cur) {
        unsigned int l = 4U + count(src + pos + 4, src + c + 4, end);

        if (l > best) {
            best = l;
            *m = c;
        }
    }
    if (flags & LZ4PAGE_TWO_WAY) {
        c = table[h + 1];
        if (c < pos && load32(src + c) == cur) {
            unsigned int l = 4U + count(src + pos + 4, src + c + 4, end);

            if (l > best) {
                best = l;
                *m = c;
            }
        }
        table[h + 1] = table[h];
    }
    table[h] = (unsigned short)pos;
    return best;
}

static ALWAYS_INLINE unsigned int compress_page(unsigned short* table, const u8* src, u8* dst, const unsigned int flags) {
    const unsigned int limit = PAGE_LZ_PAGE - MFLIMIT; /* a match starts at most here */
    unsigned int pos = 1, anchor = 0, last = 1;
    struct out o = {dst};

    __builtin_memset(table, 0, sizeof(unsigned short) << PAGE_LZ_HASH_BITS);
    while (pos <= limit) {
        unsigned int m = 0, len = best_at(table, src, pos, last, &m, flags);

        if (len < 4U) {
            pos += 1U + ((pos - anchor) >> 6);
            continue;
        }
        if ((flags & LZ4PAGE_LAZY) && pos + 1U <= limit) {
            /* one step: a longer match at the next position costs one literal more */
            unsigned int m2 = 0, len2 = best_at(table, src, pos + 1U, last, &m2, flags);

            if (len2 > len + 1U) {
                pos++;
                len = len2;
                m = m2;
            }
        }
        /* backwards into the literals */
        while (pos > anchor && m > 0 && src[pos - 1] == src[m - 1]) {
            pos--;
            m--;
            len++;
        }
        last = pos - m;
        put_sequence(&o, src + anchor, pos - anchor, len, last);
        pos += len;
        anchor = pos;
        if (pos <= limit) {
            unsigned int h2 = hash5(load64(src + pos - 2)) & ((flags & LZ4PAGE_TWO_WAY) ? ~1U : ~0U);

            if (flags & LZ4PAGE_TWO_WAY)
                table[h2 + 1] = table[h2];
            table[h2] = (unsigned short)(pos - 2);
        }
    }
    put_sequence(&o, src + anchor, PAGE_LZ_PAGE - anchor, 0, 0);
    return (unsigned int)(o.op - dst);
}

unsigned int lz4page_compress(unsigned short* table, const void* src, void* dst, unsigned int dst_cap, unsigned int flags) {
    if (dst_cap < 2U * PAGE_LZ_PAGE)
        return 0;
    switch (flags & 3U) {
    case 0:
        return compress_page(table, src, dst, 0);
    case LZ4PAGE_TWO_WAY:
        return compress_page(table, src, dst, LZ4PAGE_TWO_WAY);
    case LZ4PAGE_LAZY:
        return compress_page(table, src, dst, LZ4PAGE_LAZY);
    default:
        return compress_page(table, src, dst, LZ4PAGE_TWO_WAY | LZ4PAGE_LAZY);
    }
}
