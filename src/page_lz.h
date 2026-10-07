/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_PAGE_LZ_H
#define QUETSCHN_EXPLORE_PAGE_LZ_H

/*
 * seqlz's matcher, seqlz_find, and the literal and match copies of its decoder. Everything static
 * inline, so that it is inlined into the loops of seqlz.c.
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

/* Prefetch for reading. Without SSE, as the kernel builds x86-64, clang drops __builtin_prefetch and
 * the kernel's prefetch() with it, gcc does not. Every x86-64 CPU has prefetcht0. */
#if defined(__x86_64__) && !defined(__SSE__)
#    define PAGE_LZ_PREFETCH(p) __asm__("prefetcht0 %0" : : "m"(*(const char*)(p)))
#else
#    define PAGE_LZ_PREFETCH(p) __builtin_prefetch(p)
#endif

/* Every cache line of [p, p + size), size a multiple of 512: 8 lines per iteration. A loop of one line
 * per iteration was 4 instructions per line, about 500 per page for seqlz's tables, 3% of a page's
 * decode on a Cortex-A55. */
static inline void prefetch_lines(const void* p, unsigned long size) {
    const u8* q = p;
    const u8* const end = q + size;

    for (; q < end; q += 512) {
        PAGE_LZ_PREFETCH(q);
        PAGE_LZ_PREFETCH(q + 64);
        PAGE_LZ_PREFETCH(q + 128);
        PAGE_LZ_PREFETCH(q + 192);
        PAGE_LZ_PREFETCH(q + 256);
        PAGE_LZ_PREFETCH(q + 320);
        PAGE_LZ_PREFETCH(q + 384);
        PAGE_LZ_PREFETCH(q + 448);
    }
}

static inline void store16(u8* p, unsigned int v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static inline unsigned int load16(const u8* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Numbers of 4 and 8 bytes, little endian as in the format, whatever the CPU: the first byte is the
 * lowest. On a big-endian CPU the bytes are swapped; on x86-64 and arm64 these are plain loads. */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#    define PAGE_LZ_LE32(x) __builtin_bswap32(x)
#    define PAGE_LZ_LE64(x) __builtin_bswap64(x)
#else
#    define PAGE_LZ_LE32(x) (x)
#    define PAGE_LZ_LE64(x) (x)
#endif

static inline u32 load32(const u8* p) {
    u32 v;
    __builtin_memcpy(&v, p, 4);
    return PAGE_LZ_LE32(v);
}

static inline u64 load64(const u8* p) {
    u64 v;
    __builtin_memcpy(&v, p, 8);
    return PAGE_LZ_LE64(v);
}

static inline void store64(u8* p, u64 v) {
    v = PAGE_LZ_LE64(v);
    __builtin_memcpy(p, &v, 8);
}

/* a hash of the low 5 bytes of v, as zstd's */
static inline unsigned int hash5(u64 v) {
    return (unsigned int)(((v << 24) * 889523592379ULL) >> (64U - PAGE_LZ_HASH_BITS));
}

/* number of equal bytes at p and q, p after q, up to end */
static inline unsigned int count(const u8* p, const u8* q, const u8* end) {
    const u8* start = p;

    /* The first 16 bytes without a branch: whether the first 8 are equal mispredicted, 24% of the
     * matches are longer than 11 bytes. One ctz, of the first word that differs: two, combined, took
     * more instructions, and 74% of the matches end in the first 8 bytes. With the top bit set, a ctz
     * of 7 bytes stands for 8. */
    if (end - p >= 16) {
        u64 x1 = load64(p) ^ load64(q), x2 = load64(p + 8) ^ load64(q + 8);
        u64 x = x1 ? x1 : x2;

        if (x | x2)
            return (x1 ? 0U : 8U) + ((unsigned int)__builtin_ctzll(x | 1ULL << 63) >> 3);
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
 * Greedy, like lz4's fast mode: at every position the last offset and one candidate from a hash of 5
 * bytes. With 5 instead of 4 the matcher finds fewer sequences: 5% fewer compress cycles for 0.6 points
 * of memory, and in the kernel 10% less write time at p99 (docs/explored-designs.md). Matches of 4
 * bytes still come from the last offset. Every position is tried, without lz4's growing step after a
 * long run without a match: the step needed the start of the literals in the loop, and without it the
 * pages that compress were 5% faster, the ones zram stores raw 2.3 us slower (docs/explored-designs.md,
 * "The matcher without its step"). Each sequence goes to emit as soon as it is found; inlined with the
 * encoder that is one pass over the page, and the same matcher feeds seqlz_find.
 */
static ALWAYS_INLINE void match_page(unsigned short* table, const u8* src, emit_fn emit, void* ctx) {
    /* positions, not pointers: the end is a constant, and the position for the table is at hand */
    const unsigned int limit = PAGE_LZ_PAGE - 8U; /* 8 bytes readable for the hash and the comparison */
    unsigned int pos = 1, anchor = 0, last = 1, h, cand;
    /* -last as an index, so the load at the last offset needs no subtraction of its own */
    long back = -1;
    /* This position's 8 bytes, hash, table entry and the 4 bytes there, all loaded one position ahead:
     * on the in-order Cortex-A55 each step waited for the one before, 3.4% of the compressor's cycles.
     * The entry is read after the position before was stored, as before, so the matches are the same. */
    u64 v;
    u32 cand_bytes;

    __builtin_memset(table, 0, sizeof(unsigned short) << PAGE_LZ_HASH_BITS);
    v = load64(src + pos);
    h = hash5(v);
    cand = table[h];
    cand_bytes = load32(src + cand);

    while (pos < limit) {
        u64 v_next = load64(src + pos + 1);
        unsigned int h_next = hash5(v_next), m, len;
        /* the first 4 bytes of the 8 for the hash, without a second load */
        u32 cur = (u32)v;
        /* One branch for both candidates, not three: the last offset always points into the page (it
         * starts at 1, the search at position 1), and so does a table entry, so both can be read before
         * it is known whether they count. Three branches mispredicted almost twice as often as lz4's
         * one. The table is cleared for each page, so every entry is before pos. */
        unsigned int rep_hit = load32(src + pos + back) == cur;
        unsigned int cand_hit = cand_bytes == cur;

        table[h] = (unsigned short)pos;
        if (!(rep_hit | cand_hit)) {
            pos++;
            v = v_next;
            h = h_next;
            cand = table[h];
            cand_bytes = load32(src + cand);
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
        back = -(long)last;
        emit(ctx, src + anchor, pos - anchor, len, last);
        pos += len;
        anchor = pos;
        /* a position near the end of the match, for the next matches */
        if (pos < limit) {
            table[hash5(load64(src + pos - 2))] = (unsigned short)(pos - 2);
            v = load64(src + pos);
            h = hash5(v);
            cand = table[h];
            cand_bytes = load32(src + cand);
        }
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
            w = load64(d - off);
            pat = w & ((1ULL << bits) - 1ULL);
            pat |= pat << bits;
            pat |= (pat << ((2U * bits) & 63U)) & (0ULL - (u64)(2U * bits < 64U));
            pat |= (pat << ((4U * bits) & 63U)) & (0ULL - (u64)(4U * bits < 64U));
            store64(d, pat);
            /* step bytes on, a multiple of off, it is the same 8 bytes again: stores only, without
             * a load that waits for the store before it */
            for (k = step; k < len && (unsigned int)(d_end - d) >= k + 8U; k += step)
                store64(d + k, pat);
            back = 0;
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
