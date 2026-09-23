// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "zram_codec.h"

#include <stdlib.h>
#include <string.h>

/* The size is kept in front of the block, which keeps the returned pointer 64 byte aligned. */
#define QUETSCHN_ALLOC_HEADER 64

void* quetschn_zalloc(size_t size, size_t* counter) {
    unsigned char* p = aligned_alloc(QUETSCHN_ALLOC_HEADER, QUETSCHN_ALLOC_HEADER + ((size + 63) & ~(size_t)63));
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, QUETSCHN_ALLOC_HEADER + size);
    memcpy(p, &size, sizeof(size));
    *counter += size;
    return p + QUETSCHN_ALLOC_HEADER;
}

void quetschn_free(void* p, size_t* counter) {
    if (p == NULL) {
        return;
    }
    unsigned char* block = (unsigned char*)p - QUETSCHN_ALLOC_HEADER;
    size_t size = 0;
    memcpy(&size, block, sizeof(size));
    *counter -= size;
    free(block);
}
