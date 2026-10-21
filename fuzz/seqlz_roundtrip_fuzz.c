// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * Any page through seqlz_compress() and seqlz_decode(), at every level: the page must come back, the
 * compressed page must fit the two pages zram gives the compressor, and it must be the same bytes as
 * seqlz_encode() writes for seqlz_find()'s sequences at that level, as seqlz.h says. Into a dst of one
 * page, and of a size the input picks, the page must come back too, or not fit.
 *
 * The first input byte picks how the rest becomes a page: repeated until the page is full, which gives
 * the matcher something to find, or followed by zeros.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"
#include "unwritten.h"

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
    unwritten(out, SEQLZ_PAGE);
    unwritten(scratch, SEQLZ_SCRATCH);
    return seqlz_decode(tables, dst, len, out, scr) == 0 && memcmp(out, page, SEQLZ_PAGE) == 0;
}

/* the page at level, against seqlz_encode() for the sequences of seqlz_find() at that level */
static void roundtrip(const unsigned char* page, unsigned int level) {
    unsigned int len, n, k, pos = 0, n_lit = 0;

    unwritten(&state, sizeof(state));
    n = seqlz_find(&state, page, seq, level);
    for (k = 0; k < n; k++) {
        memcpy(literals + n_lit, page + pos, seq[k].literals);
        n_lit += seq[k].literals;
        pos += seq[k].literals + seq[k].match;
    }
    if (pos != SEQLZ_PAGE)
        abort();
    unwritten(&state, sizeof(state));
    unwritten(dst, DST_CAP);
    len = seqlz_compress(tables, &state, page, dst, DST_CAP, level);
    if (len == 0 || len > DST_CAP || !decodes_to(page, len, scratch))
        abort();
    /* level 1's raw literals need no scratch */
    if (level == 1 && !decodes_to(page, len, 0))
        abort();
    unwritten(again, DST_CAP);
    if (seqlz_encode(tables, &state, seq, n, literals, n_lit, page, again, DST_CAP, level) != len ||
        memcmp(again, dst, len) != 0)
        abort();
}

/* into an allocation of exactly cap bytes, for ASan: the page comes back, or it did not fit */
static void small_dst(const unsigned char* page, unsigned int cap, unsigned int level) {
    unsigned char* d = malloc(cap ? cap : 1);
    unsigned int len;

    if (!d)
        abort();
    unwritten(&state, sizeof(state));
    len = seqlz_compress(tables, &state, page, d, cap, level);
    if (len > cap || (len && (memcpy(dst, d, len), !decodes_to(page, len, scratch))))
        abort();
    free(d);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned char* page;
    unsigned int k, level;

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

    for (level = 1; level <= SEQLZ_LEVELS; level++)
        roundtrip(page, level);
    small_dst(page, SEQLZ_PAGE, 2 + (data[0] >> 2 & 1));
    small_dst(page, (unsigned int)(size * 2654435761U % (SEQLZ_PAGE + 64U)), 1 + (data[0] >> 1 & 3) % 3);
    free(page);
    return 0;
}
