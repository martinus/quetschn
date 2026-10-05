// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * make_seeds <decode dir> <roundtrip dir>: the fuzzers' first inputs, from made-up pages, never from
 * page dumps: compressed pages with raw and with coded literals for seqlz_decode_fuzz, and the pages
 * themselves for seqlz_roundtrip_fuzz.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

#define N_PAGES 8

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static uint64_t next(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void make_page(int kind, unsigned char* p) {
    static const char* const words[] = {"the ", "page ", "swap ", "memory ", "of ", "zram ", "kernel ", "and "};
    unsigned int k = 0;

    memset(p, 0, SEQLZ_PAGE);
    switch (kind) {
    case 0: /* zeros */
        break;
    case 1: /* text */
        while (k < SEQLZ_PAGE) {
            const char* w = words[next() % 8];
            while (*w && k < SEQLZ_PAGE)
                p[k++] = (unsigned char)*w++;
        }
        break;
    case 2: /* pointers close to each other */
        for (; k + 8 <= SEQLZ_PAGE; k += 8) {
            uint64_t v = 0x00007f3a12340000ULL + (next() % 4096) * 16;
            memcpy(p + k, &v, 8);
        }
        break;
    case 3: /* random, nothing to compress */
        for (; k < SEQLZ_PAGE; k++)
            p[k] = (unsigned char)next();
        break;
    case 4: /* 16 different bytes: no matches, but literals worth coding */
        for (; k < SEQLZ_PAGE; k++)
            p[k] = (unsigned char)(0x40 + next() % 16);
        break;
    case 5: /* half text, half random */
        make_page(1, p);
        for (k = SEQLZ_PAGE / 2; k < SEQLZ_PAGE; k++)
            p[k] = (unsigned char)next();
        break;
    case 6: /* short periods, matches with offsets below 8 */
        for (; k < SEQLZ_PAGE; k++)
            p[k] = (unsigned char)("abcabcxyzxyzxyz"[(k / 64) % 3 * 3 + k % 3] + (k / 512));
        break;
    default: /* 32-bit counters */
        for (; k + 4 <= SEQLZ_PAGE; k += 4) {
            uint32_t v = 1000 + k / 4;
            memcpy(p + k, &v, 4);
        }
        break;
    }
}

static void write_file(const char* dir, const char* name, int i, const void* p, size_t n) {
    char path[4096];
    FILE* f;

    snprintf(path, sizeof(path), "%s/%s%d", dir, name, i);
    f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f)) {
        perror(path);
        exit(1);
    }
}

int main(int argc, char** argv) {
    static unsigned char page[SEQLZ_PAGE], in[SEQLZ_PAGE + 1], dst[2 * SEQLZ_PAGE];
    static struct seqlz_state st;
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    int kind, coded;

    if (argc != 3 || !t || seqlz_tables_init(t, &seqlz_default_own)) {
        fprintf(stderr, "usage: make_seeds <decode dir> <roundtrip dir>\n");
        return 1;
    }
    for (kind = 0; kind < N_PAGES; kind++) {
        make_page(kind, page);
        for (coded = 0; coded < 2; coded++) {
            unsigned int len = seqlz_compress(t, &st, page, dst, sizeof(dst), coded);
            if (!len)
                return 1;
            write_file(argv[1], coded ? "coded-" : "raw-", kind, dst, len);
        }
        in[0] = 0;
        memcpy(in + 1, page, SEQLZ_PAGE);
        write_file(argv[2], "page-", kind, in, sizeof(in));
    }
    /* a short input repeated over the page */
    write_file(argv[2], "repeat-", 0, "\001seqlz", 6);
    free(t);
    return 0;
}
