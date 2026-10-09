// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * Any list of sequences through seqlz_encode() or seqlz_encode_own(), as they come, also ones that are
 * not a page: matches shorter than 4, offset 0 or past the position, lengths that do not add up. They
 * must not write past the two pages of dst, and a page they accept must decode to the page its
 * sequences make. The other targets only reach them with the matcher's sequences, which are always
 * valid.
 *
 * Byte 0: bit 0 lets the last sequence's literals fill the page, bit 1 codes the literals, bit 2 codes
 * them as levels 3 and 4 do, with a table of the page's own where that pays. Then 4
 * bytes per sequence: the literals, the match, and two bytes for the offset, each taken as it is.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

#define DST_CAP (2U * SEQLZ_PAGE)

static struct seqlz_tables* tables;
static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
static struct seqlz_hc_state own_state;
static unsigned char literals[SEQLZ_PAGE], page[SEQLZ_PAGE];
/* each an allocation of its own exact size, for ASan */
static unsigned char *dst, *out, *scratch;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned int n = 0, k, i, pos = 0, n_lit = 0, sum = 0, len;

    if (size < 5)
        return 0;
    if (!tables) {
        tables = malloc(seqlz_tables_size());
        dst = malloc(DST_CAP);
        out = malloc(SEQLZ_PAGE);
        scratch = malloc(SEQLZ_SCRATCH);
        if (!tables || !dst || !out || !scratch || seqlz_tables_init(tables, &seqlz_default_own) || !seqlz_all_symbols(tables))
            abort();
    }
    for (k = 1; k + 4 <= size && n < SEQLZ_MAX_SEQUENCES; k += 4, n++) {
        seq[n].literals = data[k];
        seq[n].match = data[k + 1];
        seq[n].offset = (unsigned short)(((unsigned int)data[k + 2] | (unsigned int)data[k + 3] << 8) % (SEQLZ_PAGE + 4U));
        sum += seq[n].literals + seq[n].match;
    }
    sum -= seq[n - 1].match;
    if ((data[0] & 1) && sum - seq[n - 1].literals <= SEQLZ_PAGE) {
        sum -= seq[n - 1].literals;
        seq[n - 1].literals = (unsigned short)(SEQLZ_PAGE - sum);
        sum = SEQLZ_PAGE;
    }
    for (k = 0; k < n; k++)
        n_lit += seq[k].literals;
    if (n_lit > SEQLZ_PAGE)
        n_lit = SEQLZ_PAGE;
    for (k = 0; k < n_lit; k++)
        literals[k] = (unsigned char)(k * 0x9e3779b1U >> 24);

    /* raw literals, a fixed table, or also one of the page's own */
    len = data[0] & 4 ? seqlz_encode_own(tables, seq, n, literals, n_lit, dst, DST_CAP, &own_state)
                      : seqlz_encode(tables, seq, n, literals, n_lit, dst, DST_CAP, data[0] & 2);
    if (len == 0)
        return 0;
    if (len > DST_CAP || sum != SEQLZ_PAGE)
        abort();
    /* accepted, so the sequences make a page: build it and compare */
    n_lit = 0;
    for (k = 0; k < n; k++) {
        for (i = 0; i < seq[k].literals; i++)
            page[pos++] = literals[n_lit++];
        if (k + 1 == n)
            break;
        if (seq[k].match < 4 || seq[k].offset == 0 || seq[k].offset > pos)
            abort();
        for (i = 0; i < seq[k].match; i++, pos++)
            page[pos] = page[pos - seq[k].offset];
    }
    memset(out, 0xa5, SEQLZ_PAGE);
    if (pos != SEQLZ_PAGE || seqlz_decode(tables, dst, len, out, scratch) != 0 || memcmp(out, page, SEQLZ_PAGE) != 0)
        abort();
    return 0;
}
