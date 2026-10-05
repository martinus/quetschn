// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz_decode() on any input, with and without the scratch for coded literals. It must never read
 * outside the input, never write outside the page and the scratch, and return. The input, the page and
 * the scratch each get an allocation of exactly their size, so that ASan sees a byte too far, also when
 * AFL++ hands in a larger buffer.
 *
 * Without a scratch only pages with raw literals are valid, and those decode the same with one: a page
 * that decodes without the scratch and not with it, or to other bytes, is a bug as well.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

static struct seqlz_tables* tables;
static unsigned char *with, *without, *scratch;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned char* src;
    int r_with, r_without;

    /* zram never stores more than a page; twice that covers lengths the decoder has to reject */
    if (size > 2U * SEQLZ_PAGE)
        return 0;
    if (!tables) {
        tables = malloc(seqlz_tables_size());
        with = malloc(SEQLZ_PAGE);
        without = malloc(SEQLZ_PAGE);
        scratch = malloc(SEQLZ_SCRATCH);
        if (!tables || !with || !without || !scratch || seqlz_tables_init(tables, &seqlz_default_own))
            abort();
    }
    src = malloc(size ? size : 1);
    if (!src)
        abort();
    memcpy(src, data, size);
    /* the same bytes before every input, so that a decoder that reads what it did not write behaves
     * the same when the input is run again */
    memset(with, 0xa5, SEQLZ_PAGE);
    memset(without, 0x5a, SEQLZ_PAGE);
    memset(scratch, 0x3c, SEQLZ_SCRATCH);

    r_with = seqlz_decode(tables, src, (unsigned int)size, with, scratch);
    r_without = seqlz_decode(tables, src, (unsigned int)size, without, 0);
    if (r_with != 0 && r_with != -1)
        abort();
    if (r_without == 0 && (r_with != 0 || memcmp(with, without, SEQLZ_PAGE) != 0))
        abort();
    free(src);
    return 0;
}
