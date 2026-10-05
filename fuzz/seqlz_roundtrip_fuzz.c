// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * Any page through seqlz_compress() and seqlz_decode(), with raw and with coded literals: the page must
 * come back, the compressed page must fit the two pages zram gives the compressor, and it must be the
 * same bytes as seqlz_encode() writes for seqlz_find()'s sequences, as seqlz.h says.
 *
 * The first input byte picks how the rest becomes a page: repeated until the page is full, which gives
 * the matcher something to find, or followed by zeros.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

static struct seqlz_tables* tables;
static struct seqlz_state* state;
static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
static unsigned char literals[SEQLZ_PAGE];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

static void roundtrip(const unsigned char* page, int coded) {
    unsigned char *dst = malloc(2U * SEQLZ_PAGE), *again = malloc(2U * SEQLZ_PAGE);
    unsigned char *out = malloc(SEQLZ_PAGE), *scratch = malloc(SEQLZ_SCRATCH);
    unsigned int len, n, k, pos = 0, n_lit = 0;

    if (!dst || !again || !out || !scratch)
        abort();
    len = seqlz_compress(tables, state, page, dst, 2U * SEQLZ_PAGE, coded);
    if (len == 0 || len > 2U * SEQLZ_PAGE)
        abort();
    if (seqlz_decode(tables, dst, len, out, scratch) != 0 || memcmp(out, page, SEQLZ_PAGE) != 0)
        abort();
    /* raw literals need no scratch */
    if (!coded && (seqlz_decode(tables, dst, len, out, 0) != 0 || memcmp(out, page, SEQLZ_PAGE) != 0))
        abort();

    n = seqlz_find(state, page, seq);
    for (k = 0; k < n; k++) {
        memcpy(literals + n_lit, page + pos, seq[k].literals);
        n_lit += seq[k].literals;
        pos += seq[k].literals + seq[k].match;
    }
    if (pos != SEQLZ_PAGE)
        abort();
    if (seqlz_encode(tables, seq, n, literals, n_lit, again, 2U * SEQLZ_PAGE, coded) != len || memcmp(again, dst, len) != 0)
        abort();

    free(scratch);
    free(out);
    free(again);
    free(dst);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned char* page;
    size_t k;

    if (size < 1 || size > SEQLZ_PAGE + 1U)
        return 0;
    if (!tables) {
        tables = malloc(seqlz_tables_size());
        state = malloc(sizeof(*state));
        if (!tables || !state || seqlz_tables_init(tables, &seqlz_default_own) || !seqlz_all_symbols(tables))
            abort();
    }
    page = calloc(1, SEQLZ_PAGE);
    if (!page)
        abort();
    if (size > 1) {
        memcpy(page, data + 1, size - 1);
        if (data[0] & 1)
            for (k = size - 1; k < SEQLZ_PAGE; k++)
                page[k] = page[k - (size - 1)];
    }
    roundtrip(page, 0);
    roundtrip(page, 1);
    free(page);
    return 0;
}
