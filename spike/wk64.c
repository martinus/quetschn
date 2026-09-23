// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "wk64.h"

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;

enum { WK_ZERO = 0, WK_EXACT = 1, WK_PARTIAL = 2, WK_MISS = 3 };

#define HIGH32 0xffffffff00000000ULL

/* Little endian hosts only, which is all the spike runs on (x86-64, the arm64 phone). */
static inline u64 load64(const u8* p) {
    u64 v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static inline u32 load32(const u8* p) {
    u32 v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

static inline void store64(u8* p, u64 v) {
    __builtin_memcpy(p, &v, 8);
}

static inline void store32(u8* p, u32 v) {
    __builtin_memcpy(p, &v, 4);
}

/* table slot: the top 4 bits of a multiplicative hash of the high 32 bits */
static inline unsigned int slot_of(u64 w) {
    return ((u32)(w >> 32) * 0x9e3779b1U) >> 28;
}

static inline unsigned int tag_of(u64 w, const u64* table) {
    u64 t;

    if (w == 0)
        return WK_ZERO;
    t = table[slot_of(w)];
    if (t == w)
        return WK_EXACT;
    if ((t & HIGH32) == (w & HIGH32))
        return WK_PARTIAL;
    return WK_MISS;
}

static inline unsigned int tag_at(const u8* tags, unsigned int i) {
    return (tags[i >> 2] >> ((i & 3) * 2)) & 3;
}

unsigned int wk64_compress(const void* src, void* dst, unsigned int dst_cap) {
    const u8* s = src;
    u8* d = dst;
    u64 table[16] = {0};
    u8 tags[WK64_TAG_BYTES] = {0};
    unsigned int n_idx = 0, n_partial = 0, n_miss = 0, total, i;
    u8 *idx, *low, *full;

    /* pass 1: tags and section sizes */
    for (i = 0; i < WK64_WORDS; i++) {
        u64 w = load64(s + 8 * i);
        unsigned int tag = tag_of(w, table);

        tags[i >> 2] |= (u8)(tag << ((i & 3) * 2));
        n_idx += tag == WK_EXACT || tag == WK_PARTIAL;
        n_partial += tag == WK_PARTIAL;
        n_miss += tag == WK_MISS;
        table[slot_of(w)] = w;
    }
    total = WK64_TAG_BYTES + (n_idx + 1) / 2 + 4 * n_partial + 8 * n_miss;
    if (total > dst_cap)
        return 0;

    /* pass 2: the same walk again, now writing the sections */
    __builtin_memcpy(d, tags, WK64_TAG_BYTES);
    idx = d + WK64_TAG_BYTES;
    low = idx + (n_idx + 1) / 2;
    full = low + 4 * n_partial;
    __builtin_memset(idx, 0, (n_idx + 1) / 2);
    __builtin_memset(table, 0, sizeof(table));
    n_idx = 0;
    for (i = 0; i < WK64_WORDS; i++) {
        u64 w = load64(s + 8 * i);
        unsigned int tag = tag_at(tags, i);

        if (tag == WK_EXACT || tag == WK_PARTIAL) {
            idx[n_idx >> 1] |= (u8)(slot_of(w) << ((n_idx & 1) * 4));
            n_idx++;
        }
        if (tag == WK_PARTIAL) {
            store32(low, (u32)w);
            low += 4;
        } else if (tag == WK_MISS) {
            store64(full, w);
            full += 8;
        }
        table[slot_of(w)] = w;
    }
    return total;
}

/* number of set bits in x, where only the even bits can be set */
static inline u64 count_even_bits(u64 x) {
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (x * 0x0101010101010101ULL) >> 56;
}

struct sections {
    const u8* idx;
    const u8* low;
    const u8* full;
    unsigned int n_idx;
    unsigned int n_partial;
    unsigned int n_miss;
};

/* Counts the tags and checks that src_len is exactly what they need. */
static int parse(const u8* s, unsigned int src_len, struct sections* sec) {
    u64 n_exact = 0, n_partial = 0, n_miss = 0, total;
    unsigned int k;

    if (src_len < WK64_TAG_BYTES)
        return -1;
    for (k = 0; k < WK64_TAG_BYTES / 8; k++) {
        u64 x = load64(s + 8 * k);
        u64 lo = x & 0x5555555555555555ULL;
        u64 hi = (x >> 1) & 0x5555555555555555ULL;

        n_exact += count_even_bits(lo & ~hi);
        n_partial += count_even_bits(hi & ~lo);
        n_miss += count_even_bits(lo & hi);
    }
    total = WK64_TAG_BYTES + (n_exact + n_partial + 1) / 2 + 4 * n_partial + 8 * n_miss;
    if (total != src_len)
        return -1;
    sec->n_idx = (unsigned int)(n_exact + n_partial);
    sec->n_partial = (unsigned int)n_partial;
    sec->n_miss = (unsigned int)n_miss;
    sec->idx = s + WK64_TAG_BYTES;
    sec->low = sec->idx + (sec->n_idx + 1) / 2;
    sec->full = sec->low + 4 * sec->n_partial;
    return 0;
}

int wk64_decompress_switch(const void* src, unsigned int src_len, void* dst) {
    const u8* s = src;
    u8* d = dst;
    u64 table[16] = {0};
    struct sections sec;
    const u8 *low, *full;
    unsigned int i, ii = 0;

    if (parse(s, src_len, &sec))
        return -1;
    low = sec.low;
    full = sec.full;
    for (i = 0; i < WK64_WORDS; i++) {
        u64 w;

        switch (tag_at(s, i)) {
        case WK_ZERO:
            w = 0;
            break;
        case WK_EXACT:
            w = table[(sec.idx[ii >> 1] >> ((ii & 1) * 4)) & 15];
            ii++;
            break;
        case WK_PARTIAL:
            w = (table[(sec.idx[ii >> 1] >> ((ii & 1) * 4)) & 15] & HIGH32) | load32(low);
            ii++;
            low += 4;
            break;
        default:
            w = load64(full);
            full += 8;
            break;
        }
        table[slot_of(w)] = w;
        store64(d + 8 * i, w);
    }
    return 0;
}

int wk64_decompress_zeroskip(const void* src, unsigned int src_len, void* dst) {
    const u8* s = src;
    u8* d = dst;
    u64 table[16] = {0};
    struct sections sec;
    const u8 *low, *full;
    unsigned int i, ii = 0;

    if (parse(s, src_len, &sec))
        return -1;
    low = sec.low;
    full = sec.full;
    for (i = 0; i < WK64_WORDS; i++) {
        u64 w;

        /* a zero tag byte is 4 zero words; they all go to slot_of(0), which is 0 */
        if ((i & 3) == 0 && s[i >> 2] == 0) {
            __builtin_memset(d + 8 * i, 0, 32);
            table[0] = 0;
            i += 3;
            continue;
        }
        switch (tag_at(s, i)) {
        case WK_ZERO:
            w = 0;
            break;
        case WK_EXACT:
            w = table[(sec.idx[ii >> 1] >> ((ii & 1) * 4)) & 15];
            ii++;
            break;
        case WK_PARTIAL:
            w = (table[(sec.idx[ii >> 1] >> ((ii & 1) * 4)) & 15] & HIGH32) | load32(low);
            ii++;
            low += 4;
            break;
        default:
            w = load64(full);
            full += 8;
            break;
        }
        table[slot_of(w)] = w;
        store64(d + 8 * i, w);
    }
    return 0;
}

int wk64_decompress_slots(const void* src, unsigned int src_len, void* dst) {
    const u8* s = src;
    u8* d = dst;
    u64 table[16] = {0};
    struct sections sec;
    const u8 *low, *full;
    unsigned int i, ii = 0;

    if (parse(s, src_len, &sec))
        return -1;
    low = sec.low;
    full = sec.full;
    for (i = 0; i < WK64_WORDS; i++) {
        unsigned int slot;
        u64 w;

        if ((i & 3) == 0 && s[i >> 2] == 0) {
            __builtin_memset(d + 8 * i, 0, 32);
            table[0] = 0;
            i += 3;
            continue;
        }
        /* The slot to update comes from the stream, not from the decoded word, so its address does
         * not wait for the table load. For EXACT the entry is already there. */
        switch (tag_at(s, i)) {
        case WK_ZERO:
            w = 0;
            table[0] = 0;
            break;
        case WK_EXACT:
            w = table[(sec.idx[ii >> 1] >> ((ii & 1) * 4)) & 15];
            ii++;
            break;
        case WK_PARTIAL:
            slot = (sec.idx[ii >> 1] >> ((ii & 1) * 4)) & 15;
            w = (table[slot] & HIGH32) | load32(low);
            table[slot] = w;
            ii++;
            low += 4;
            break;
        default:
            w = load64(full);
            table[slot_of(w)] = w;
            full += 8;
            break;
        }
        store64(d + 8 * i, w);
    }
    return 0;
}

int wk64_decompress_branchless(const void* src, unsigned int src_len, void* dst) {
    /* stands in for an empty section, so that the clamped reads below always have a target */
    static const u8 zeros[8];
    const u8* s = src;
    u8* d = dst;
    u64 table[16] = {0};
    struct sections sec;
    const u8 *ib, *lb, *fb;
    unsigned int i, ilast, llast, flast, ii = 0, li = 0, fi = 0;

    if (parse(s, src_len, &sec))
        return -1;
    ib = sec.n_idx ? sec.idx : zeros;
    lb = sec.n_partial ? sec.low : zeros;
    fb = sec.n_miss ? sec.full : zeros;
    ilast = sec.n_idx ? sec.n_idx - 1 : 0;
    llast = sec.n_partial ? sec.n_partial - 1 : 0;
    flast = sec.n_miss ? sec.n_miss - 1 : 0;

    /* Every word reads all three sections at the current position, clamped to the last entry, and the
     * tag selects with masks what it uses and which positions advance. */
    for (i = 0; i < WK64_WORDS; i++) {
        unsigned int tag = tag_at(s, i);
        unsigned int ic = ii < ilast ? ii : ilast;
        unsigned int lc = li < llast ? li : llast;
        unsigned int fc = fi < flast ? fi : flast;
        u64 t = table[(ib[ic >> 1] >> ((ic & 1) * 4)) & 15];
        u64 is_exact = tag == WK_EXACT;
        u64 is_partial = tag == WK_PARTIAL;
        u64 is_miss = tag == WK_MISS;
        u64 w = (t & (-is_exact | (-is_partial & HIGH32))) | ((u64)load32(lb + 4 * lc) & -is_partial) |
                (load64(fb + 8 * fc) & -is_miss);

        ii += (unsigned int)(is_exact | is_partial);
        li += (unsigned int)is_partial;
        fi += (unsigned int)is_miss;
        table[slot_of(w)] = w;
        store64(d + 8 * i, w);
    }
    return 0;
}
