// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz_decode() against tools/seqlz_ref.c, the decoder written from FORMAT.md alone, on any input: both
 * must call the same inputs valid, and decode a valid one to the same page. A difference is a bug in one
 * of them or a place where FORMAT.md is not clear. Each buffer gets an allocation of exactly its size,
 * as in seqlz_decode_fuzz.c.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"
#include "seqlz_ref.h"

static struct seqlz_tables* tables;
static struct seqlz_ref ref;
static unsigned char *fast, *slow, *scratch;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned char* src;
    int r_fast, r_ref;

    /* zram never stores more than a page; twice that covers lengths the decoders have to reject */
    if (size > 2U * SEQLZ_PAGE)
        return 0;
    if (!tables) {
        const struct seqlz_ref_tables t = {
            seqlz_default_own.token, seqlz_default_own.ll, seqlz_default_own.ml, seqlz_lit_sets};

        tables = malloc(seqlz_tables_size());
        fast = malloc(SEQLZ_PAGE);
        slow = malloc(SEQLZ_PAGE);
        scratch = malloc(SEQLZ_SCRATCH);
        if (!tables || !fast || !slow || !scratch || seqlz_tables_init(tables, &seqlz_default_own) ||
            seqlz_ref_init(&ref, &t, QUETSCHN_PAGE_BITS))
            abort();
    }
    src = malloc(size ? size : 1);
    if (!src)
        abort();
    memcpy(src, data, size);
    memset(fast, 0xa5, SEQLZ_PAGE);
    memset(slow, 0x5a, SEQLZ_PAGE);

    r_fast = seqlz_decode(tables, src, (unsigned int)size, fast, scratch);
    r_ref = seqlz_ref_decode(&ref, src, size, slow);
    if ((r_fast == 0) != (r_ref == 0) || (r_fast == 0 && memcmp(fast, slow, SEQLZ_PAGE) != 0))
        abort();
    free(src);
    return 0;
}
