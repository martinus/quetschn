// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * trace.c: the pages of chapter 14's step-through, compressed by seqlz_compress() and decoded by a
 * copy of seqlz_decode() that records every step (instrument.py). Prints JSON: per page its bytes,
 * the compressed bytes, the literals the decoder reads, and the steps. Fails if a page does not
 * decode to itself.
 *
 * cc -O1 -I<repo>/src -I<dir of dec_inst.c> trace.c <repo>/src/seqlz_{codes,compress,default_tables,lit_sets}.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz_internal.h"

struct seqlz_bit_reader;
static void stepper_step(int line,
                         const u8* d,
                         const u8* lit,
                         const struct seqlz_bit_reader* br,
                         unsigned int tok,
                         unsigned int nl,
                         unsigned int len,
                         unsigned int off,
                         unsigned int last,
                         unsigned int skip);
#define STEP_OUT() stepper_step(__LINE__, d, lit, &br, tok, 0, 0, 0, last, skip)
#define STEP_IN() stepper_step(__LINE__, d, lit, &br, tok, nl, len, off, last, skip)
#include "dec_inst.c"
#include "pages.h"

static const u8 *g_dst, *g_src, *g_lit;
static unsigned int g_steps;

static void stepper_step(int line,
                         const u8* d,
                         const u8* lit,
                         const struct seqlz_bit_reader* br,
                         unsigned int tok,
                         unsigned int nl,
                         unsigned int len,
                         unsigned int off,
                         unsigned int last,
                         unsigned int skip) {
    printf("%s[%d,%ld,%ld,%ld,%d,%u,%u,%u,%u,%u,%u,%u,%u]",
           g_steps++ ? "," : "",
           line,
           (long)(d - g_dst),
           lit ? (long)(lit - g_lit) : -1L,
           br->p ? (long)(br->p - g_src) : -1L,
           br->count,
           (unsigned int)(br->bits >> 32),
           (unsigned int)br->bits,
           tok,
           nl,
           len,
           off,
           last,
           skip);
}

int main(void) {
    static u8 page[SEQLZ_PAGE], comp[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    static const struct {
        const char* id;
        void (*make)(u8*);
        int coded;
    } pages[] = {
        {"ab", ab_page, 0},
        {"slab", slab_page, 1},
    };
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    unsigned int k;

    if (!t || seqlz_tables_init(t, &seqlz_default_own))
        return 1;
    printf("{\"consts\":{\"PAGE\":%u,\"SCRATCH\":%u,\"FAST_PAGE_ROOM\":%u,\"FAST_LIT_ROOM\":%u,"
           "\"FAST_IN_ROOM\":%u,\"TOKEN_BITS\":%u,\"LL_CAP\":%u,\"ML_CAP\":%u,\"HEADER\":%u},\"pages\":{",
           SEQLZ_PAGE,
           SEQLZ_SCRATCH,
           FAST_PAGE_ROOM,
           FAST_LIT_ROOM,
           FAST_IN_ROOM,
           SEQLZ_TOKEN_BITS,
           SEQLZ_LL_CAP,
           SEQLZ_ML_CAP,
           SEQLZ_HEADER);
    for (k = 0; k < sizeof(pages) / sizeof(pages[0]); k++) {
        unsigned int n, n_lit;
        int coded;

        pages[k].make(page);
        n = seqlz_compress(t, &st, page, comp, sizeof(comp), pages[k].coded);
        if (!n)
            return 1;
        n_lit = get_unaligned_le16(comp);
        coded = (n_lit & SEQLZ_LIT_CODED) != 0;
        n_lit &= SEQLZ_LIT_CODED - 1U;
        g_dst = out;
        g_src = comp;
        g_lit = coded ? scratch : comp + SEQLZ_HEADER;
        g_steps = 0;
        memset(out, 0, sizeof(out));
        printf("%s\"%s\":{\"coded\":%d,", k ? "," : "", pages[k].id, coded);
        printf("\"steps\":[");
        if (seqlz_decode(t, comp, n, out, scratch) || memcmp(out, page, SEQLZ_PAGE)) {
            fprintf(stderr, "trace.c: page %s does not decode to itself\n", pages[k].id);
            return 1;
        }
        printf("],");
        array("page", page, SEQLZ_PAGE);
        printf(",");
        array("comp", comp, n);
        printf(",");
        array("lit", g_lit, n_lit);
        printf("}");
    }
    printf("}}\n");
    return 0;
}
