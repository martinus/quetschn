// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * streams.c: the data of the animation "streams" in chapter 13. Compresses chapter 16's page with
 * coded literals and prints, as JSON, its literals in page order with the code length each has in the
 * page's literal table, the stream sizes of the header, and what seqlz_decode_literals() writes after the
 * literals in its last step. Fails if anything disagrees with seqlz_decode().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pages.h"
#include "seqlz_internal.h"

static int fail(const char* what) {
    fprintf(stderr, "streams.c: %s\n", what);
    return 1;
}

int main(void) {
    static u8 page[SEQLZ_BLOCK_SIZE], raw[2 * SEQLZ_BLOCK_SIZE], comp[2 * SEQLZ_BLOCK_SIZE], out[SEQLZ_BLOCK_SIZE];
    static u8 scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    unsigned int n, n_raw, n_lit, set, width, i, k, bits[SEQLZ_LIT_STREAMS] = {0};
    unsigned int sz[SEQLZ_LIT_STREAMS];
    unsigned long long h = 0;

    slab_page(page);
    n_raw = seqlz_compress(&st, page, raw, sizeof(raw), 1);
    n = seqlz_compress(&st, page, comp, sizeof(comp), 2);
    if (!n || !n_raw)
        return fail("compress");
    n_lit = get_unaligned_le16(comp);
    if (!(n_lit & SEQLZ_LIT_CODED))
        return fail("the page's literals are not coded");
    n_lit &= SEQLZ_LIT_CODED - 1U;
    if (n_lit != get_unaligned_le16(raw))
        return fail("raw and coded pages differ in literals");
    /* byte 2: the table in bits 0 to 2, width - 5 in bits 3 to 5 */
    set = comp[2] & (SEQLZ_LIT_SETS - 1U);
    width = SEQLZ_SIZE_BITS_MIN + ((comp[2] >> SEQLZ_LIT_WIDTH_AT) & 7U);
    for (i = 0; i < width; i++)
        h |= (unsigned long long)comp[SEQLZ_LIT_HEADER(0) + i] << (8 * i);
    for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
        sz[i] = (unsigned int)(h >> (i * width)) & ((1U << width) - 1U);
    memset(scratch, 0xaa, sizeof(scratch));
    if (seqlz_decode(comp, n, out, scratch) || memcmp(out, page, SEQLZ_BLOCK_SIZE))
        return fail("the page does not decode to itself");
    /* the decoder's literals are the encoder's raw literals */
    if (memcmp(scratch, raw + SEQLZ_HEADER, n_lit))
        return fail("decoded literals differ from the raw ones");
    /* literal k in stream k % 8: each stream's code lengths fill exactly its size */
    for (k = 0; k < n_lit; k++)
        bits[k % SEQLZ_LIT_STREAMS] += seqlz_tables.lit[set].enc[scratch[k]] & 63U;
    for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
        if ((bits[i] + 7U) / 8U != sz[i])
            return fail("a stream's codes do not fill its size");
    /* the decode table gives the same length as the encoder's entry */
    for (k = 0; k < 256; k++) {
        u32 e = seqlz_tables.lit[set].enc[k];
        u32 code = e >> 8, len = e & 63U;
        u16 d = seqlz_tables.lit[set].decode[code << (SEQLZ_LIT_BITS - len)];

        if ((d & 63U) != len || d >> 8 != k)
            return fail("decode and enc disagree");
    }
    printf("{\"set\":%u,\"rounds\":%u,\"lit\":[", set, SEQLZ_LIT_ROUNDS);
    for (k = 0; k < n_lit; k++)
        printf("%s%u", k ? "," : "", scratch[k]);
    printf("],\"len\":[");
    for (k = 0; k < n_lit; k++)
        printf("%s%u", k ? "," : "", seqlz_tables.lit[set].enc[scratch[k]] & 63U);
    printf("],\"sz\":[");
    for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
        printf("%s%u", i ? "," : "", sz[i]);
    /* the last step decodes into out[k + i] for all 8 streams, k = n_lit - r */
    printf("],\"spare\":[");
    for (k = n_lit; k < n_lit - n_lit % 40U % 8U + 8U; k++)
        printf("%s%u", k > n_lit ? "," : "", scratch[k]);
    printf("]}\n");
    return 0;
}
