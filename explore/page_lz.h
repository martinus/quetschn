/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_PAGE_LZ_H
#define QUETSCHN_EXPLORE_PAGE_LZ_H

/*
 * What the LZ formats of explore/ share: seqlz_find's matcher, and the literal and match copies of the
 * decoder. Everything static inline, so each codec gets its own copy, inlined into its loop.
 */

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

/* 12 for 4 KiB pages, 14 for 16 KiB, set for the whole build (CMake's QUETSCHN_PAGE_BITS) */
#ifndef QUETSCHN_PAGE_BITS
#    define QUETSCHN_PAGE_BITS 12
#endif
#define PAGE_LZ_PAGE (1U << QUETSCHN_PAGE_BITS)
/* the matcher's table has 1 << PAGE_LZ_HASH_BITS unsigned shorts: 8 KiB for 4 KiB pages, 16 KiB for
 * larger ones, lz4's size */
#define PAGE_LZ_HASH_BITS (QUETSCHN_PAGE_BITS == 12 ? 12U : 13U)

#define ALWAYS_INLINE inline __attribute__((always_inline))

static inline void store16(u8* p, unsigned int v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static inline unsigned int load16(const u8* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

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
    return (v * 2654435761U) >> (32U - PAGE_LZ_HASH_BITS);
}

/* number of equal bytes at p and q, p after q, up to end */
static inline unsigned int count(const u8* p, const u8* q, const u8* end) {
    const u8* start = p;

    /* The first 16 bytes without a branch: whether the first 8 are equal mispredicted, 24% of the
     * matches are longer than 11 bytes. With the top bit set, a ctz of 7 bytes stands for 8. */
    if (end - p >= 16) {
        u64 x1 = load64(p) ^ load64(q), x2 = load64(p + 8) ^ load64(q + 8);
        unsigned int z1 = x1 == 0, c1 = (unsigned int)__builtin_ctzll(x1 | 1ULL << 63) >> 3;
        unsigned int c2 = (unsigned int)__builtin_ctzll(x2 | 1ULL << 63) >> 3;

        if (!(z1 & (x2 == 0)))
            return c1 + z1 * (1U + c2);
        p += 16;
        q += 16;
    }
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
static ALWAYS_INLINE void match_page(unsigned short* table, const u8* src, emit_fn emit, void* ctx) {
    /* positions, not pointers: the end is a constant, and the position for the table is at hand */
    const unsigned int limit = PAGE_LZ_PAGE - 8U; /* 8 bytes readable for the first comparison */
    unsigned int pos = 1, anchor = 0, last = 1;

    __builtin_memset(table, 0, sizeof(unsigned short) << PAGE_LZ_HASH_BITS);

    while (pos < limit) {
        u32 cur = load32(src + pos);
        unsigned int h = hash4(cur), cand = table[h], m, len;
        /* One branch for both candidates, not three: the last offset always points into the page (it
         * starts at 1, the search at position 1), and so does a table entry, so both can be read before
         * it is known whether they count. Three branches mispredicted almost twice as often as lz4's
         * one. The table is cleared for each page, so every entry is before pos. */
        unsigned int rep_hit = load32(src + pos - last) == cur;
        unsigned int cand_hit = load32(src + cand) == cur;

        table[h] = (unsigned short)pos;
        if (!(rep_hit | cand_hit)) {
            pos += 1U + ((pos - anchor) >> 6);
            continue;
        }
        m = rep_hit ? pos - last : cand;
        /* backwards into the literals, then forwards */
        while (pos > anchor && m > 0 && src[pos - 1] == src[m - 1]) {
            pos--;
            m--;
        }
        len = 4U + count(src + pos + 4, src + m + 4, src + PAGE_LZ_PAGE);
        last = pos - m;
        emit(ctx, src + anchor, pos - anchor, len, last);
        pos += len;
        anchor = pos;
        /* a position near the end of the match, for the next matches */
        if (pos < limit)
            table[hash4(load32(src + pos - 2))] = (unsigned short)(pos - 2);
    }
    emit(ctx, src + anchor, PAGE_LZ_PAGE - anchor, 0, 0);
}

/*
 * The literals of a sequence, nl bytes from lit to d; the caller has checked that they fit in both.
 * 16 bytes at a time while there are 16 bytes of room behind, in the page and in the input; may write
 * and read past nl, which is overwritten or ignored. The rest one by one, at most 15 bytes at the end
 * of the page or of the input.
 */
static ALWAYS_INLINE void copy_literals(u8* d, const u8* d_end, const u8* lit, const u8* s_end, unsigned int nl) {
    unsigned int k = 0;

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
}

/*
 * A match of len bytes, off back from d; the caller has checked that 0 < off <= d - start of the page
 * and that len fits.
 * 8 bytes at a time while there are 8 bytes of room behind in the page; may write past len, which the
 * next sequence overwrites. The rest one by one, at most 7 bytes at the end of the page; a run to the
 * end of the page byte by byte made the slowest pages 10 times slower than lz4.
 * For an offset below 8 the first 8 bytes of the match are built in a register: the off bytes before
 * the match, repeated with shifts, then one 8-byte store. From there each step copies from step bytes
 * back, the largest multiple of off up to 8, which is exactly what the previous store wrote, so the
 * load gets it from that store. Writing the first bytes one by one made the load wait for four stores,
 * and a loop over them mispredicted its exit.
 */
static ALWAYS_INLINE void copy_match(u8* d, const u8* d_end, unsigned int off, unsigned int len) {
    static const u8 step_for[8] = {0, 8, 8, 6, 8, 5, 6, 7};
    unsigned int step = off >= 8 ? 8U : step_for[off & 7U], back = off >= 8 ? off : step, k = 0;

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
        /* 79% of the matches are at most 16 bytes: two unconditional copies, the second reads only
         * bytes the first wrote or that were there before, because off >= 8. Four copies, for 91% of
         * them, were faster at p50 and slower at p99: the slowest pages have many short matches, and
         * copied 32 bytes for each. */
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

#endif
