// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * Any page through seqlz_compress() and seqlz_decode(), with raw and with coded literals: the page must
 * come back, the compressed page must fit the two pages zram gives the compressor, and it must be the
 * same bytes as seqlz_encode() writes for seqlz_find()'s sequences, as seqlz.h says. Into a dst of one
 * page, and of a size the input picks, the page must come back too, or not fit. The same for levels 3
 * and 4, seqlz_compress_hc() and seqlz_find_hc().
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
static struct seqlz_hc_state hc_state;
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

/* the sequences and literals of the page at level 1 or 2 (level 0), or 3 or 4; returns how many */
static unsigned int find(const unsigned char* page, int level, unsigned int* n_lit) {
    unsigned int n = level ? seqlz_find_hc(tables, &hc_state, page, seq, level == 4) : seqlz_find(&state, page, seq), k,
                 pos = 0;

    *n_lit = 0;
    for (k = 0; k < n; k++) {
        memcpy(literals + *n_lit, page + pos, seq[k].literals);
        *n_lit += seq[k].literals;
        pos += seq[k].literals + seq[k].match;
    }
    if (pos != SEQLZ_PAGE)
        abort();
    return n;
}

/* levels 3 and 4: with coded literals, a table of the page's own included, the same bytes as seqlz_encode()
 * for seqlz_find_hc()'s sequences */
static void roundtrip_hc(const unsigned char* page, unsigned int cap, int deep) {
    unsigned int n_lit, n = find(page, deep ? 4 : 3, &n_lit),
                        len = seqlz_compress_hc(tables, &hc_state, page, dst, DST_CAP, deep);
    unsigned char* d;

    if (len == 0 || len > DST_CAP || !decodes_to(page, len, scratch))
        abort();
    if (seqlz_encode(tables, seq, n, literals, n_lit, again, DST_CAP, SEQLZ_CODED_OWN) != len || memcmp(again, dst, len) != 0)
        abort();
    d = malloc(cap ? cap : 1);
    if (!d)
        abort();
    len = seqlz_compress_hc(tables, &hc_state, page, d, cap, deep);
    if (len > cap || (len && (memcpy(dst, d, len), !decodes_to(page, len, scratch))))
        abort();
    free(d);
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

/* into an allocation of exactly cap bytes, for ASan: the page comes back, or it did not fit */
static void small_dst(const unsigned char* page, unsigned int cap, int coded) {
    unsigned char* d = malloc(cap ? cap : 1);
    unsigned int len;

    if (!d)
        abort();
    len = seqlz_compress(tables, &state, page, d, cap, coded);
    if (len > cap || (len && (memcpy(dst, d, len), !decodes_to(page, len, scratch))))
        abort();
    free(d);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned char* page;
    unsigned int n, k, n_lit;

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
    n = find(page, 0, &n_lit);
    roundtrip(page, n, n_lit, 0);
    roundtrip(page, n, n_lit, 1);
    small_dst(page, SEQLZ_PAGE, 1);
    small_dst(page, (unsigned int)(size * 2654435761U % (SEQLZ_PAGE + 64U)), data[0] & 2);
    roundtrip_hc(page, (unsigned int)(size * 2246822519U % (SEQLZ_PAGE + 64U)), data[0] & 4);
    free(page);
    return 0;
}
