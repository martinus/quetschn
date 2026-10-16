// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * owndump <in.pages> <out.bin>: compresses every page of a corpus at level 3 and writes the compressed
 * pages that have a literal table of their own (coded literals, byte 2's code 8) to out.bin, each as
 * a 2-byte little endian length and its bytes; for own_context.py. Build:
 * gcc -O2 -Isrc -o owndump tools/seqlz-bound/owndump.c src/seqlz_{codes,compress,decompress,default_tables,lit_sets}.c
 */
#include <stdio.h>
#include <stdlib.h>

#include "seqlz.h"

int main(int argc, char** argv) {
    static unsigned char page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE];
    static struct seqlz_state st;
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    unsigned long pages = 0, own = 0;
    FILE *in, *out;

    if (argc != 3 || !t || seqlz_tables_init(t, &seqlz_default_own)) {
        fprintf(stderr, "usage: owndump <in.pages> <out.bin>\n");
        return 1;
    }
    in = fopen(argv[1], "rb");
    out = fopen(argv[2], "wb");
    if (!in || !out) {
        perror("owndump");
        return 1;
    }
    while (fread(page, 1, SEQLZ_PAGE, in) == SEQLZ_PAGE) {
        unsigned int n = seqlz_compress(t, &st, page, dst, sizeof dst, 3);

        pages++;
        if (n > 3 && n < SEQLZ_PAGE && (dst[1] & 0x80) && (dst[2] & 15) == 8) {
            unsigned char len[2] = {n & 255, n >> 8};

            if (fwrite(len, 1, 2, out) != 2 || fwrite(dst, 1, n, out) != n) {
                perror(argv[2]);
                return 1;
            }
            own++;
        }
    }
    printf("%s: %lu pages, %lu with a table of their own\n", argv[1], pages, own);
    return fclose(out) != 0;
}
