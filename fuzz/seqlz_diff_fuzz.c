// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz_decode() against tools/seqlz-ref/seqlz_ref.c, the decoder written from docs/format.md alone, on any input: both
 * must call the same inputs valid, and decode a valid one to the same page. A difference is a bug in one
 * of them or a place where docs/format.md is not clear. Each buffer gets an allocation of exactly its size,
 * as in seqlz_decode_fuzz.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"
#include "seqlz_ref.h"
#include "unwritten.h"

static int tables_built;
static struct seqlz_ref ref;
static unsigned char *fast, *slow, *scratch;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned char* src;
    int r_fast, r_ref;

    /* zram never stores more than a page; twice that covers lengths the decoders have to reject */
    if (size > 2U * SEQLZ_BLOCK_SIZE)
        return 0;
    if (!tables_built) {
        const struct seqlz_ref_tables t = {seqlz_default_lengths.token,
                                           seqlz_default_lengths.ll,
                                           seqlz_default_lengths.ml,
                                           seqlz_lit_sets,
                                           seqlz_own_lengths};

        tables_built = 1;
        fast = malloc(SEQLZ_BLOCK_SIZE);
        slow = malloc(SEQLZ_BLOCK_SIZE);
        scratch = malloc(SEQLZ_SCRATCH);
        if (!fast || !slow || !scratch || seqlz_tables_init(&seqlz_default_lengths) ||
            seqlz_ref_init(&ref, &t, QUETSCHN_PAGE_BITS))
            abort();
    }
    src = malloc(size ? size : 1);
    if (!src)
        abort();
    memcpy(src, data, size);
    memset(fast, 0xa5, SEQLZ_BLOCK_SIZE);
    memset(slow, 0x5a, SEQLZ_BLOCK_SIZE);
    unwritten(fast, SEQLZ_BLOCK_SIZE);
    unwritten(slow, SEQLZ_BLOCK_SIZE);
    unwritten(scratch, SEQLZ_SCRATCH);

    r_fast = seqlz_decode(src, (unsigned int)size, fast, scratch);
    r_ref = seqlz_ref_decode(&ref, src, size, slow);
    if ((r_fast == 0) != (r_ref == 0) || (r_fast == 0 && memcmp(fast, slow, SEQLZ_BLOCK_SIZE) != 0))
        abort();
    free(src);
    return 0;
}
