// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * pricing.c: the data of the animation "pricing" in chapter 11. Compresses chapter 16's page with raw
 * literals, prices its literals with the encoder's own lit_cost(), and prints as JSON: the literals,
 * the code lengths in all 8 tables of each byte value they use (from seqlz_tables.lit_cost), each stream's bits
 * in each table (lit_bits()), and the table seqlz_compress() picked for the page.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pages.h"
#include "seqlz_compress.c"

int main(void) {
    static u8 page[SEQLZ_PAGE], raw[2 * SEQLZ_PAGE], comp[2 * SEQLZ_PAGE];
    static struct seqlz_state st;
    u64 even[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = {0};
    u64 odd[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = {0};
    unsigned int n_lit, j, k, used[256] = {0}, first = 1;
    const u8* lit;

    if (seqlz_tables_init(&seqlz_default_lengths))
        return 1;
    slab_page(page);
    if (!seqlz_compress(&st, page, raw, sizeof(raw), 1) || !seqlz_compress(&st, page, comp, sizeof(comp), 2))
        return 1;
    n_lit = get_unaligned_le16(raw);
    lit = raw + SEQLZ_HEADER;
    lit_cost(lit, n_lit, even, odd, NULL);
    /* each table's lane must be the sum of the code lengths of the stream's literals */
    for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
        for (k = 0; k < SEQLZ_LIT_SETS; k++) {
            unsigned int i, b = 0;

            for (i = j; i < n_lit; i += SEQLZ_LIT_STREAMS)
                b += seqlz_tables.lit[k].enc[lit[i]] & 63U;
            if (b != lit_bits(even, odd, j, k)) {
                fprintf(stderr, "pricing.c: lit_cost() disagrees with the tables\n");
                return 1;
            }
        }
    /* byte k of lit_cost[b] is b's code length in table k */
    for (j = 0; j < 256; j++)
        for (k = 0; k < SEQLZ_LIT_SETS; k++)
            if ((seqlz_tables.lit_cost[j][0] >> (8 * k) & 255U) != (seqlz_tables.lit[k].enc[j] & 63U))
                return 1;
    printf("{\"set\":%u,\"run\":%u,\"lit\":[", comp[2] & (SEQLZ_LIT_SETS - 1U), LIT_COST_RUN);
    for (k = 0; k < n_lit; k++) {
        printf("%s%u", k ? "," : "", lit[k]);
        used[lit[k]] = 1;
    }
    printf("],\"cost\":{");
    for (k = 0; k < 256; k++) {
        if (!used[k])
            continue;
        printf("%s\"%u\":[", first ? "" : ",", k);
        first = 0;
        for (j = 0; j < SEQLZ_LIT_SETS; j++)
            printf("%s%u", j ? "," : "", (unsigned int)(seqlz_tables.lit_cost[k][0] >> (8 * j) & 255U));
        printf("]");
    }
    printf("},\"bits\":[");
    for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
        printf("%s[", j ? "," : "");
        for (k = 0; k < SEQLZ_LIT_SETS; k++)
            printf("%s%u", k ? "," : "", lit_bits(even, odd, j, k));
        printf("]");
    }
    printf("]}\n");
    return 0;
}
