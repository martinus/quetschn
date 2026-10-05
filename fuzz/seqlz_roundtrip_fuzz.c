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

#define DST_CAP (2U * SEQLZ_PAGE)

static struct seqlz_tables* tables;
static struct seqlz_state state;
static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
static unsigned char literals[SEQLZ_PAGE];
/* each an allocation of its own exact size, for ASan */
static unsigned char *dst, *again, *out, *scratch;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/* decodes into out, which holds other bytes before, so that a byte the decoder skips shows */
static int decodes_to(const unsigned char* page, unsigned int len, void* scr) {
    memset(out, 0xa5, SEQLZ_PAGE);
    return seqlz_decode(tables, dst, len, out, scr) == 0 && memcmp(out, page, SEQLZ_PAGE) == 0;
}

static void roundtrip(const unsigned char* page, unsigned int n, unsigned int n_lit, int coded) {
    unsigned int len = seqlz_compress(tables, &state, page, dst, DST_CAP, coded);

    if (len == 0 || len > DST_CAP || !decodes_to(page, len, scratch))
        abort();
    /* raw literals need no scratch */
    if (!coded && !decodes_to(page, len, 0))
        abort();
    if (seqlz_encode(tables, seq, n, literals, n_lit, again, DST_CAP, coded) != len || memcmp(again, dst, len) != 0)
        abort();
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned char* page;
    unsigned int n, k, pos = 0, n_lit = 0;

    if (size < 1 || size > SEQLZ_PAGE + 1U)
        return 0;
    if (!tables) {
        tables = malloc(seqlz_tables_size());
        dst = malloc(DST_CAP);
        again = malloc(DST_CAP);
        out = malloc(SEQLZ_PAGE);
        scratch = malloc(SEQLZ_SCRATCH);
        if (!tables || !dst || !again || !out || !scratch || seqlz_tables_init(tables, &seqlz_default_own) ||
            !seqlz_all_symbols(tables))
            abort();
    }
    page = calloc(1, SEQLZ_PAGE);
    if (!page)
        abort();
    memcpy(page, data + 1, size - 1);
    if (size > 1 && (data[0] & 1))
        for (k = (unsigned int)size - 1U; k < SEQLZ_PAGE; k++)
            page[k] = page[k - (size - 1)];

    /* the sequences and literals of the page, the same for raw and coded literals */
    n = seqlz_find(&state, page, seq);
    for (k = 0; k < n; k++) {
        memcpy(literals + n_lit, page + pos, seq[k].literals);
        n_lit += seq[k].literals;
        pos += seq[k].literals + seq[k].match;
    }
    if (pos != SEQLZ_PAGE)
        abort();
    roundtrip(page, n, n_lit, 0);
    roundtrip(page, n, n_lit, 1);
    free(page);
    return 0;
}
