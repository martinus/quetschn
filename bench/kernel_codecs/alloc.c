// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "zram_codec.h"

#include <stdlib.h>
#include <string.h>

#ifndef SEQLZ_PAGE_BITS
#    define SEQLZ_PAGE_BITS 12
#endif
#define KERNEL_PAGE ((size_t)1 << SEQLZ_PAGE_BITS)

/*
 * Blocks of a page or more are page aligned, like vmalloc and kmalloc of a page in the kernel; smaller
 * ones are 64 byte aligned. The alignment decides which loads and stores alias in the low 12 address
 * bits, which changes timings, so it has to be the same in every run. The size and what was counted
 * for it are kept in the header in front of the block, and the header is as large as the alignment.
 */
static size_t alignment(size_t size) {
    return size >= 4096 ? 4096 : 64;
}

/*
 * What kmalloc() hands out for size bytes: SLUB's caches up to KMALLOC_MAX_CACHE_SIZE, 2 pages, which
 * are the powers of 2 from 8 and the two of 96 and 192 bytes, and above it pages from the page
 * allocator, a power of 2 of them.
 */
size_t bench_kmalloc_size(size_t size) {
    size_t a = size <= 2 * KERNEL_PAGE ? 8 : KERNEL_PAGE;

    while (a < size) {
        if (a == 64 && size <= 96) {
            return 96;
        }
        if (a == 128 && size <= 192) {
            return 192;
        }
        a *= 2;
    }
    return a;
}

/* what vmalloc() hands out for size bytes: whole pages */
size_t bench_vmalloc_size(size_t size) {
    return (size + KERNEL_PAGE - 1) / KERNEL_PAGE * KERNEL_PAGE;
}

static void* zalloc(size_t size, size_t counted, size_t* counter) {
    size_t const a = alignment(size);
    unsigned char* p = aligned_alloc(a, a + ((size + a - 1) & ~(a - 1)));
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, a + size);
    memcpy(p + a - 64, &size, sizeof(size));
    memcpy(p + a - 64 + sizeof(size), &counted, sizeof(counted));
    *counter += counted;
    return p + a;
}

void* bench_zalloc(size_t size, size_t* counter) {
    return zalloc(size, bench_kmalloc_size(size), counter);
}

void* bench_vzalloc(size_t size, size_t* counter) {
    return zalloc(size, bench_vmalloc_size(size), counter);
}

void bench_free(void* p, size_t* counter) {
    if (p == NULL) {
        return;
    }
    /* the size, and so the alignment, and what was counted are in the last 64 bytes in front of it */
    size_t size = 0, counted = 0;
    memcpy(&size, (unsigned char*)p - 64, sizeof(size));
    memcpy(&counted, (unsigned char*)p - 64 + sizeof(size), sizeof(counted));
    unsigned char* block = (unsigned char*)p - alignment(size);
    *counter -= counted;
    free(block);
}
