// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz_endian: generated pages through seqlz_compress() and seqlz_decode(), raw and coded, and a hash
 * of all the compressed bytes. The format is little endian, so the hash must be the same on every CPU:
 * CI runs this on x86-64 and, under qemu, on big-endian s390x, and compares the two lines. Plain C
 * without the test framework, so that it builds in seconds under qemu.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

static unsigned long long rng_state = 0x9e3779b97f4a7c15ULL;

static unsigned long long next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* page k: random bytes, a short pattern repeated (offsets 1 to 7 take the decoder's pattern copies),
 * 8-byte numbers counting up, few byte values with long runs, words from a small vocabulary (coded
 * literals and matches), or a page that is half zeros */
static void make_page(unsigned char* p, unsigned int k) {
    static const char letters[] = "eeeeettttaaaooiinnsshrdlu       \n";
    unsigned int i, j;

    switch (k % 6) {
    case 0:
        for (i = 0; i < SEQLZ_PAGE; i++)
            p[i] = (unsigned char)next();
        break;
    case 1: {
        unsigned int len = 1 + k / 6 % 7;
        unsigned char pat[7];

        for (j = 0; j < len; j++)
            pat[j] = (unsigned char)next();
        for (i = 0; i < SEQLZ_PAGE; i++)
            p[i] = i % 61 == 0 ? (unsigned char)next() : pat[i % len];
        break;
    }
    case 2: {
        unsigned long long v = 0x00007f1234560000ULL + (next() & 0xfff0U);

        for (i = 0; i < SEQLZ_PAGE; i += 8, v += 16 + (next() & 8U))
            for (j = 0; j < 8; j++)
                p[i + j] = (unsigned char)(v >> (8 * j));
        break;
    }
    case 3:
        for (i = 0; i < SEQLZ_PAGE;) {
            unsigned char b = (unsigned char)(next() % 5);
            unsigned int run = 1 + (unsigned int)(next() % 40);

            for (j = 0; j < run && i < SEQLZ_PAGE; j++)
                p[i++] = b;
        }
        break;
    case 4: {
        char words[32][8];

        for (j = 0; j < 32; j++) {
            unsigned int len = 2 + (unsigned int)(next() % 6), c;

            for (c = 0; c < len; c++)
                words[j][c] = letters[next() % (sizeof(letters) - 1)];
            words[j][len] = 0;
        }
        for (i = 0; i < SEQLZ_PAGE;) {
            const char* w = next() % 3 ? words[next() % 32] : &letters[next() % (sizeof(letters) - 1)];

            for (j = 0; w[j] && i < SEQLZ_PAGE; j++)
                p[i++] = (unsigned char)w[j];
        }
        break;
    }
    default:
        memset(p, 0, SEQLZ_PAGE);
        for (i = 0; i < SEQLZ_PAGE / 2; i++)
            p[(next() % SEQLZ_PAGE)] = (unsigned char)(next() % 7);
    }
}

int main(void) {
    static unsigned char page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    unsigned long long hash = 0xcbf29ce484222325ULL;
    unsigned int k, i, coded_pages = 0, failed = 0;
    int coded;

    if (!t || seqlz_tables_init(t, &seqlz_default_own))
        return 2;
    for (k = 0; k < 600; k++) {
        make_page(page, k);
        for (coded = 0; coded < 2; coded++) {
            unsigned int n = seqlz_compress(t, &st, page, dst, sizeof dst, coded);

            if (!n || seqlz_decode(t, dst, n, out, scratch) || memcmp(out, page, SEQLZ_PAGE))
                failed++;
            coded_pages += n > 1 && (dst[1] & 0x80U);
            for (i = 0; i < n; i++)
                hash = (hash ^ dst[i]) * 0x100000001b3ULL;
        }
    }
    free(t);
    printf("pages 600, coded %u, failed %u, hash %016llx\n", coded_pages, failed, hash);
    return failed != 0 || coded_pages == 0;
}
