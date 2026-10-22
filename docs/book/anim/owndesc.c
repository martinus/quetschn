// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * owndesc.c: chapter 4's text page at level 3, compressed by the real code, and the description of its
 * own literal table read symbol by symbol with the code OWN, as the format says: each symbol's bits,
 * what it means, the byte values it covers and the code space after it; then each byte value's length
 * and its canonical code. Fails if the page has no table of its own, if a symbol is not a code of OWN,
 * if the lengths do not fill the code space, or if the page does not decode.
 */
#include <stdio.h>
#include <stdlib.h>

#include "pages.h"

static unsigned int bit_at(const u8* p, unsigned int i) {
    return p[i / 8] >> (7 - i % 8) & 1U;
}

int main(void) {
    static u8 page[SEQLZ_BLOCK_SIZE], dst[2 * SEQLZ_BLOCK_SIZE], out[SEQLZ_BLOCK_SIZE + 16], scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    unsigned int first[17] = {0}, count[17] = {0}, sym_of[17][SEQLZ_OWN_SYMBOLS], len[256] = {0};
    unsigned int n, w, at = 0, b = 0, space = 0, s, l, k, rows = 0, d, next[17] = {0}, code[256] = {0};
    const u8* p;

    if (seqlz_tables_init(&seqlz_default_lengths))
        return 1;
    words_page(page);
    n = seqlz_compress(&st, page, dst, sizeof dst, SEQLZ_LEVEL_OWN);
    if (!n || !(get_unaligned_le16(dst) & SEQLZ_LIT_CODED) || (dst[2] & SEQLZ_LIT_CODE_MASK) != SEQLZ_LIT_OWN ||
        seqlz_decode(dst, n, out, scratch) || memcmp(out, page, SEQLZ_BLOCK_SIZE)) {
        fprintf(stderr, "owndesc.c: the text page has no table of its own at level 3\n");
        return 1;
    }
    w = SEQLZ_SIZE_BITS_MIN + (dst[2] >> SEQLZ_LIT_WIDTH_AT & 7U);
    p = dst + SEQLZ_LIT_HEADER(w);
    /* OWN's canonical code: the symbols of each length in order, and the first code of each length */
    for (s = 0; s < SEQLZ_OWN_SYMBOLS; s++)
        sym_of[seqlz_own_lengths[s]][count[seqlz_own_lengths[s]]++] = s;
    for (l = 1; l <= 16; l++)
        first[l] = (first[l - 1] + count[l - 1]) << 1;
    printf("{\"own\":[");
    for (s = 0; s < SEQLZ_OWN_SYMBOLS; s++)
        printf("%s%u", s ? "," : "", seqlz_own_lengths[s]);
    printf("],\"syms\":[");
    /* the symbols, until the lengths fill the 1024 units of a 10-bit code space */
    while (space < 1024U) {
        unsigned int c = 0, from = at, x = 0, kk = 0;

        for (l = 1;; l++) {
            c = c << 1 | bit_at(p, at++);
            if (l > SEQLZ_OWN_CODE_BITS) {
                fprintf(stderr, "owndesc.c: not a code of OWN\n");
                return 1;
            }
            if (c - first[l] < count[l])
                break;
        }
        s = sym_of[l][c - first[l]];
        if (s <= SEQLZ_OWN_RUN) {
            len[b] = s + 1U;
            space += 1024U >> (s + 1U);
        } else {
            kk = s - SEQLZ_OWN_RUN;
            for (k = 0; k + 1 < kk; k++)
                x = x << 1 | bit_at(p, at++);
            x += 1U << (kk - 1U);
        }
        if (b + x > 256) {
            fprintf(stderr, "owndesc.c: a byte value above 255\n");
            return 1;
        }
        /* bits: the symbol's code, then the run's bits below its top bit, if any */
        printf("%s{\"s\":%u,\"code\":\"", rows ? "," : "", s);
        for (k = from; k < from + l; k++)
            putchar('0' + bit_at(p, k));
        printf("\",\"extra\":\"");
        for (; k < at; k++)
            putchar('0' + bit_at(p, k));
        printf(
            "\",\"at\":%u,\"b\":%u,\"len\":%u,\"run\":%u,\"space\":%u}", from, b, s <= SEQLZ_OWN_RUN ? s + 1U : 0, x, space);
        b += s <= SEQLZ_OWN_RUN ? 1U : x;
        rows++;
    }
    d = (at + 7) / 8;
    printf("],\"used\":%u,\"desc\":[", at);
    for (k = 0; k < d; k++)
        printf("%s%u", k ? "," : "", p[k]);
    /* the canonical codes of the page's own table: by length, then by byte value */
    memset(count, 0, sizeof count);
    for (k = 0; k < 256; k++)
        count[len[k]]++;
    count[0] = 0;
    for (l = 1; l <= 16; l++)
        next[l] = (next[l - 1] + count[l - 1]) << 1;
    printf("],\"codes\":[");
    for (l = 1, rows = 0; l <= 10; l++)
        for (k = 0; k < 256; k++)
            if (len[k] == l) {
                code[k] = next[l]++;
                printf("%s[%u,%u,\"", rows++ ? "," : "", k, l);
                for (s = l; s-- > 0;)
                    putchar('0' + (code[k] >> s & 1U));
                printf("\"]");
            }
    printf("],\"at\":%u,\"len\":%u}\n", SEQLZ_LIT_HEADER(w), n);
    return 0;
}
