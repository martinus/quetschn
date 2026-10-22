// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * trace_match.c: the pages of chapter 9's step-through, found by a copy of seqlz_match_block() that
 * records every step (instrument.py match). Prints JSON: per page its bytes and the steps. Fails if
 * the sequences the matcher emits do not make the page again.
 *
 * cc -O1 -I<dir of the instrumented seqlz_match.h> -I<repo>/src trace_match.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz_internal.h"

static const u8* g_src;
static unsigned int g_steps;

static void step(int line,
                 unsigned int pos,
                 unsigned int anchor,
                 unsigned int last,
                 unsigned int h,
                 unsigned int cand,
                 int rep_hit,
                 int cand_hit,
                 unsigned int m,
                 unsigned int len);
#define STEP_A() step(__LINE__, pos, anchor, last, h, cand, -1, -1, 0, 0)
#define STEP_B() step(__LINE__, pos, anchor, last, h, cand, (int)rep_hit, (int)cand_hit, m, len)
#include "pages.h"
#include "seqlz_match.h"

/*
 * One step: the line and the variables, and the slot that position pos - 2 hashes to, where a hit
 * inserts it, from the real seqlz_hash5(). The 4 bytes at pos and at cand follow from the page, so
 * the page's step-through takes them from there.
 */
static void step(int line,
                 unsigned int pos,
                 unsigned int anchor,
                 unsigned int last,
                 unsigned int h,
                 unsigned int cand,
                 int rep_hit,
                 int cand_hit,
                 unsigned int m,
                 unsigned int len) {
    unsigned int h2 = pos >= 2 && pos - 2 + 8 <= SEQLZ_BLOCK_SIZE ? seqlz_hash5(get_unaligned_le64(g_src + pos - 2)) : 0;

    printf("%s[%d,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u]",
           g_steps++ ? "," : "",
           line,
           pos,
           anchor,
           last,
           h,
           cand,
           rep_hit,
           cand_hit,
           m,
           len,
           h2);
}

/* the page again, from the sequences: the check that they are right */
struct rebuild {
    u8 out[SEQLZ_BLOCK_SIZE];
    unsigned int n;
};

static void emit(void* ctx, const u8* literals, unsigned int ll, unsigned int ml, unsigned int off) {
    struct rebuild* r = ctx;
    unsigned int k;

    for (k = 0; k < ll && r->n < SEQLZ_BLOCK_SIZE; k++)
        r->out[r->n++] = literals[k];
    for (k = 0; k < ml && r->n < SEQLZ_BLOCK_SIZE; k++, r->n++)
        r->out[r->n] = r->n >= off ? r->out[r->n - off] : 0;
}

int main(void) {
    static u8 page[SEQLZ_BLOCK_SIZE];
    static u16 table[1U << SEQLZ_HASH_BITS];
    static struct rebuild r;
    static const struct {
        const char* id;
        void (*make)(u8*);
    } pages[] = {
        {"pointers", pointer_page},
        {"slab", slab_page},
    };
    unsigned int k, i;

    printf("{\"consts\":{\"PAGE\":%u,\"HASH_BITS\":%u,\"LIMIT\":%u},\"pages\":{",
           SEQLZ_BLOCK_SIZE,
           SEQLZ_HASH_BITS,
           SEQLZ_BLOCK_SIZE - 8U);
    for (k = 0; k < sizeof(pages) / sizeof(pages[0]); k++) {
        pages[k].make(page);
        g_src = page;
        g_steps = 0;
        memset(&r, 0, sizeof(r));
        printf("%s\"%s\":{\"steps\":[", k ? "," : "", pages[k].id);
        seqlz_match_block(table, page, false, emit, &r);
        if (r.n != SEQLZ_BLOCK_SIZE || memcmp(r.out, page, SEQLZ_BLOCK_SIZE)) {
            fprintf(stderr, "trace_match.c: the sequences of page %s do not make it again\n", pages[k].id);
            return 1;
        }
        printf("],\"page\":[");
        for (i = 0; i < SEQLZ_BLOCK_SIZE; i++)
            printf("%s%u", i ? "," : "", page[i]);
        printf("]}");
    }
    printf("}}\n");
    return 0;
}
