// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "bdelta.h"

typedef unsigned long long u64;
typedef long long s64;
typedef unsigned int u32;
typedef int s32;
typedef unsigned short u16;
typedef unsigned char u8;

enum { M_ZERO, M_REP, M_B8D1, M_B8D2, M_B8D4, M_B4D1, M_B4D2, M_RAW };

static const u8 payload_size[16] = {0, 8, 17, 25, 41, 22, 38, 64};

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

static inline u16 load16(const u8* p) {
    u16 v;
    __builtin_memcpy(&v, p, 2);
    return v;
}

static inline void store64(u8* p, u64 v) {
    __builtin_memcpy(p, &v, 8);
}

static inline void store32(u8* p, u32 v) {
    __builtin_memcpy(p, &v, 4);
}

static inline void store16(u8* p, u16 v) {
    __builtin_memcpy(p, &v, 2);
}

/* does x fit into a signed integer of the given number of bytes */
static inline int fits(s64 x, unsigned int bytes) {
    s64 lim = (s64)1 << (8 * bytes - 1);
    return x >= -lim && x < lim;
}

/* Tries to encode n values of the given width (8 or 4 bytes) with deltas of dbytes. Returns the
 * payload size, or 0 if some value fits neither as immediate nor relative to the base. The base is the
 * first value that does not fit as immediate. */
static unsigned int try_base_delta(const u8* block, unsigned int width, unsigned int dbytes, u8* out) {
    unsigned int n = 64 / width, i, have_base = 0;
    u64 base = 0;
    u32 mask = 0;
    u8* deltas = out + width + n / 8;

    for (i = 0; i < n; i++) {
        s64 v = width == 8 ? (s64)load64(block + 8 * i) : (s64)(s32)load32(block + 4 * i);
        s64 d;

        if (fits(v, dbytes)) {
            d = v;
        } else {
            if (!have_base) {
                base = (u64)v;
                have_base = 1;
            }
            d = width == 8 ? (s64)((u64)v - base) : (s64)(s32)((u32)v - (u32)base);
            if (!fits(d, dbytes))
                return 0;
            mask |= 1U << i;
        }
        __builtin_memcpy(deltas + dbytes * i, &d, dbytes);
    }
    if (width == 8) {
        store64(out, base);
        out[8] = (u8)mask;
    } else {
        store32(out, (u32)base);
        store16(out + 4, (u16)mask);
    }
    return width + n / 8 + n * dbytes;
}

unsigned int bdelta_compress(const void* src, void* dst, unsigned int dst_cap) {
    static const u8 order[5] = {M_B8D1, M_B4D1, M_B8D2, M_B4D2, M_B8D4};
    static const u8 width[8] = {0, 0, 8, 8, 8, 4, 4, 0};
    static const u8 dbytes[8] = {0, 0, 1, 2, 4, 1, 2, 0};
    const u8* s = src;
    u8* d = dst;
    u8 modes[BDELTA_HEADER] = {0};
    u8 payload[64];
    unsigned int pos = BDELTA_HEADER, b, i, k;

    for (b = 0; b < BDELTA_BLOCKS; b++) {
        const u8* block = s + 64 * b;
        u64 first = load64(block);
        unsigned int mode = M_RAW, all_same = 1;

        for (i = 1; i < 8; i++)
            all_same &= load64(block + 8 * i) == first;
        if (all_same) {
            mode = first == 0 ? M_ZERO : M_REP;
            store64(payload, first);
        } else {
            for (k = 0; k < 5; k++) {
                if (try_base_delta(block, width[order[k]], dbytes[order[k]], payload)) {
                    mode = order[k];
                    break;
                }
            }
            if (mode == M_RAW)
                __builtin_memcpy(payload, block, 64);
        }
        if (pos + payload_size[mode] > dst_cap)
            return 0;
        __builtin_memcpy(d + pos, payload, payload_size[mode]);
        pos += payload_size[mode];
        modes[b / 2] |= (u8)(mode << ((b & 1) * 4));
    }
    __builtin_memcpy(d, modes, BDELTA_HEADER);
    return pos;
}

int bdelta_decompress(const void* src, unsigned int src_len, void* dst) {
    const u8* s = src;
    u8* d = dst;
    const u8* p;
    unsigned int total = BDELTA_HEADER, b, i;

    if (src_len < BDELTA_HEADER)
        return -1;
    for (b = 0; b < BDELTA_BLOCKS; b++)
        total += payload_size[(s[b / 2] >> ((b & 1) * 4)) & 15];
    /* modes 8 to 15 have size 0 in the table; reject them explicitly */
    for (b = 0; b < BDELTA_HEADER; b++)
        if ((s[b] & 0x88) != 0)
            return -1;
    if (total != src_len)
        return -1;

    p = s + BDELTA_HEADER;
    for (b = 0; b < BDELTA_BLOCKS; b++, d += 64) {
        unsigned int mode = (s[b / 2] >> ((b & 1) * 4)) & 15;
        u64 base, mask;

        switch (mode) {
        case M_ZERO:
            __builtin_memset(d, 0, 64);
            break;
        case M_REP:
            base = load64(p);
            for (i = 0; i < 8; i++)
                store64(d + 8 * i, base);
            break;
        case M_B8D1:
            base = load64(p);
            mask = p[8];
            for (i = 0; i < 8; i++)
                store64(d + 8 * i, (u64)(s64)(signed char)p[9 + i] + (base & -((mask >> i) & 1)));
            break;
        case M_B8D2:
            base = load64(p);
            mask = p[8];
            for (i = 0; i < 8; i++)
                store64(d + 8 * i, (u64)(s64)(short)load16(p + 9 + 2 * i) + (base & -((mask >> i) & 1)));
            break;
        case M_B8D4:
            base = load64(p);
            mask = p[8];
            for (i = 0; i < 8; i++)
                store64(d + 8 * i, (u64)(s64)(s32)load32(p + 9 + 4 * i) + (base & -((mask >> i) & 1)));
            break;
        case M_B4D1:
            base = load32(p);
            mask = load16(p + 4);
            for (i = 0; i < 16; i++)
                store32(d + 4 * i, (u32)(s32)(signed char)p[6 + i] + (u32)(base & -((mask >> i) & 1)));
            break;
        case M_B4D2:
            base = load32(p);
            mask = load16(p + 4);
            for (i = 0; i < 16; i++)
                store32(d + 4 * i, (u32)(s32)(short)load16(p + 6 + 2 * i) + (u32)(base & -((mask >> i) & 1)));
            break;
        default:
            __builtin_memcpy(d, p, 64);
            break;
        }
        p += payload_size[mode];
    }
    return 0;
}
