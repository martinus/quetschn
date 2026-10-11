// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * trace_match.c: the pages of chapter 9's step-through, found by a copy of seqlz_match_page() that
 * records every step (instrument.py match). Prints JSON: per page its bytes and the steps. Fails if
 * the sequences the matcher emits do not make the page again.
 *
 * cc -O1 -I<dir of the instrumented page_lz.h> -I<repo>/src trace_match.c
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
                 u32 cand_bytes,
                 u32 cur,
                 int rep_hit,
                 int cand_hit,
                 unsigned int m,
                 unsigned int len);
#define STEP_A() step(__LINE__, pos, anchor, last, h, cand, cand_bytes, (u32)v, -1, -1, 0, 0)
#define STEP_B() step(__LINE__, pos, anchor, last, h, cand, cand_bytes, cur, (int)rep_hit, (int)cand_hit, m, len)
#include "page_lz.h"

/*
 * One step: the line and the variables. The 4 bytes at pos and at cand, and the slot of pos - 2 that
 * a hit inserts, follow from the page, so the page's step-through computes them.
 */
static void step(int line,
                 unsigned int pos,
                 unsigned int anchor,
                 unsigned int last,
                 unsigned int h,
                 unsigned int cand,
                 u32 cand_bytes,
                 u32 cur,
                 int rep_hit,
                 int cand_hit,
                 unsigned int m,
                 unsigned int len) {
    (void)cand_bytes;
    (void)cur;
    printf("%s[%d,%u,%u,%u,%u,%u,%d,%d,%u,%u]",
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
           len);
}

/* the page again, from the sequences: the check that they are right */
struct rebuild {
    u8 out[SEQLZ_PAGE];
    unsigned int n;
};

static void emit(void* ctx, const u8* literals, unsigned int ll, unsigned int ml, unsigned int off) {
    struct rebuild* r = ctx;
    unsigned int k;

    for (k = 0; k < ll && r->n < SEQLZ_PAGE; k++)
        r->out[r->n++] = literals[k];
    for (k = 0; k < ml && r->n < SEQLZ_PAGE; k++, r->n++)
        r->out[r->n] = r->n >= off ? r->out[r->n - off] : 0;
}

static void le(u8* p, unsigned long long v, unsigned int n) {
    while (n--) {
        *p++ = (u8)v;
        v >>= 8;
    }
}

/* chapter 9's page: 512 pointers of 8 bytes */
static void pointer_page(u8* p) {
    unsigned int i;

    for (i = 0; i < 512; i++)
        le(p + 8 * i, 0x7f1234560010ULL + 16ULL * i, 8);
}

/* chapter 16's page, as in trace.c */
static void slab_page(u8* p) {
    static const char* const names[] = {"kworker/0:1", "bash", "systemd", "sshd", "kworker/1:2", "cron"};
    static const char* const text = "zram stores each page in memory, compressed on its own. A page\n"
                                    "that comes back is decompressed while the program waits for it,\n"
                                    "so reading must be quick. Writing happens when memory runs low.\n"
                                    "Most pages hold pointers, small numbers and short strings, with\n"
                                    "many zero bytes between them. The compressor finds the bytes\n"
                                    "that repeat and writes a copy instead; the other bytes stay as\n"
                                    "literals. Text like this has few repeats but uses few letters,\n"
                                    "and a Huffman code gives the frequent ones short codes: e, t,\n"
                                    "a, o and the space take four or five bits, rare ones up to ten.\n"
                                    "Every table is fixed, so a page carries none of its own.\n";
    unsigned int i;

    memset(p, 0, SEQLZ_PAGE);
    memcpy(p, "TASKSLAB", 8);
    le(p + 8, 48, 4);
    le(p + 12, 32, 4);
    for (i = 0; i < 48; i++) {
        u8* r = p + 64 + 32 * i;

        le(r, 0xffff888104a3c040ULL + 32 * ((i * 5 + 1) % 48), 8);
        le(r + 8, 1000 + 3 * i, 4);
        le(r + 12, i % 3, 2);
        le(r + 14, 120, 2);
        strcpy((char*)r + 16, names[i % 6]);
    }
    memcpy(p + 64 + 32 * 48, text, strlen(text));
}

int main(void) {
    static u8 page[SEQLZ_PAGE];
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

    printf(
        "{\"consts\":{\"PAGE\":%u,\"HASH_BITS\":%u,\"LIMIT\":%u},\"pages\":{", SEQLZ_PAGE, SEQLZ_HASH_BITS, SEQLZ_PAGE - 8U);
    for (k = 0; k < sizeof(pages) / sizeof(pages[0]); k++) {
        pages[k].make(page);
        g_src = page;
        g_steps = 0;
        memset(&r, 0, sizeof(r));
        printf("%s\"%s\":{\"steps\":[", k ? "," : "", pages[k].id);
        seqlz_match_page(table, page, emit, &r);
        if (r.n != SEQLZ_PAGE || memcmp(r.out, page, SEQLZ_PAGE)) {
            fprintf(stderr, "trace_match.c: the sequences of page %s do not make it again\n", pages[k].id);
            return 1;
        }
        printf("],\"page\":[");
        for (i = 0; i < SEQLZ_PAGE; i++)
            printf("%s%u", i ? "," : "", page[i]);
        printf("]}");
    }
    printf("}}\n");
    return 0;
}
