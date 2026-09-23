// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "zram_codec.h"

#include <stdlib.h>
#include <string.h>

/*
 * Blocks of a page or more are page aligned, like vmalloc and kmalloc of a page in the kernel; smaller
 * ones are 64 byte aligned. The alignment decides which loads and stores alias in the low 12 address
 * bits, which changes timings, so it has to be the same in every run. The size is kept in the header
 * in front of the block, and the header is as large as the alignment.
 */
static size_t alignment(size_t size) {
    return size >= 4096 ? 4096 : 64;
}

void* quetschn_zalloc(size_t size, size_t* counter) {
    size_t const a = alignment(size);
    unsigned char* p = aligned_alloc(a, a + ((size + a - 1) & ~(a - 1)));
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, a + size);
    memcpy(p + a - 64, &size, sizeof(size));
    *counter += size;
    return p + a;
}

void quetschn_free(void* p, size_t* counter) {
    if (p == NULL) {
        return;
    }
    /* the size, and so the alignment, is in the last 64 bytes in front of the block */
    size_t size = 0;
    memcpy(&size, (unsigned char*)p - 64, sizeof(size));
    unsigned char* block = (unsigned char*)p - alignment(size);
    *counter -= size;
    free(block);
}
