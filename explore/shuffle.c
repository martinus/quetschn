// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "shuffle.h"

typedef unsigned long long u64;
typedef unsigned char u8;

static inline u64 load64(const u8* p) {
    u64 v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static inline void store64(u8* p, u64 v) {
    __builtin_memcpy(p, &v, 8);
}

/* swaps the bytes selected by mask in b with the bytes shift bits higher in a */
#define SWAP_BLOCKS(a, b, shift, mask)              \
    do {                                            \
        u64 t_ = (((a) >> (shift)) ^ (b)) & (mask); \
        (a) ^= t_ << (shift);                       \
        (b) ^= t_;                                  \
    } while (0)

/* Transposes an 8x8 byte matrix, row j in r[j], column k in byte k of each row. Swap the off-diagonal
 * 4x4 blocks, then the 2x2 blocks inside them, then single bytes. */
static inline void transpose8x8(u64* r) {
    SWAP_BLOCKS(r[0], r[4], 32, 0x00000000ffffffffULL);
    SWAP_BLOCKS(r[1], r[5], 32, 0x00000000ffffffffULL);
    SWAP_BLOCKS(r[2], r[6], 32, 0x00000000ffffffffULL);
    SWAP_BLOCKS(r[3], r[7], 32, 0x00000000ffffffffULL);
    SWAP_BLOCKS(r[0], r[2], 16, 0x0000ffff0000ffffULL);
    SWAP_BLOCKS(r[1], r[3], 16, 0x0000ffff0000ffffULL);
    SWAP_BLOCKS(r[4], r[6], 16, 0x0000ffff0000ffffULL);
    SWAP_BLOCKS(r[5], r[7], 16, 0x0000ffff0000ffffULL);
    SWAP_BLOCKS(r[0], r[1], 8, 0x00ff00ff00ff00ffULL);
    SWAP_BLOCKS(r[2], r[3], 8, 0x00ff00ff00ff00ffULL);
    SWAP_BLOCKS(r[4], r[5], 8, 0x00ff00ff00ff00ffULL);
    SWAP_BLOCKS(r[6], r[7], 8, 0x00ff00ff00ff00ffULL);
}

void shuffle8(const void* src, void* dst, unsigned int size) {
    const u8* s = src;
    u8* d = dst;
    unsigned int n = size / 8, b, j;
    u64 r[8];

    for (b = 0; b < n; b += 8) {
        for (j = 0; j < 8; j++)
            r[j] = load64(s + 8 * (b + j));
        transpose8x8(r);
        for (j = 0; j < 8; j++)
            store64(d + j * n + b, r[j]);
    }
}

void unshuffle8(const void* src, void* dst, unsigned int size) {
    const u8* s = src;
    u8* d = dst;
    unsigned int n = size / 8, b, j;
    u64 r[8];

    for (b = 0; b < n; b += 8) {
        for (j = 0; j < 8; j++)
            r[j] = load64(s + j * n + b);
        transpose8x8(r);
        for (j = 0; j < 8; j++)
            store64(d + 8 * (b + j), r[j]);
    }
}
