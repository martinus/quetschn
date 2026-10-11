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

static void le(u8* p, unsigned long long v, unsigned int n) {
    while (n--) {
        *p++ = (u8)v;
        v >>= 8;
    }
}

/* chapter 16's page: a slab of 48 task records, a text after them, zeros up to the end */
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

static void ab_page(u8* p) {
    unsigned int i;

    for (i = 0; i < SEQLZ_PAGE; i++)
        p[i] = i & 1 ? 'b' : 'a';
}

static void array(const char* name, const u8* p, unsigned int n) {
    unsigned int i;

    printf("\"%s\":[", name);
    for (i = 0; i < n; i++)
        printf("%s%u", i ? "," : "", p[i]);
    printf("]");
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
