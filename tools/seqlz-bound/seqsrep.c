// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqsrep <pages file> <output> <level>: the sequences of seqs.c, at any level, for reps.py, which prices
 * more repeat offsets on them. One record of six u16 per sequence with a match: the token as in seqs.c,
 * the offset, the last three different offsets before it, the most recent first (the first is 1 at the
 * start of a page, as in the format, the others 0 until there is one), and 1 for the first sequence of
 * a page. A match at one of them moves it to the front, as zstd's repeat offsets. Build from the
 * repository's root:
 *   tools/gen-tables/gen-tables.sh seqlz_tables.c && cc -O2 -Isrc -o seqsrep tools/seqlz-bound/seqsrep.c
 * src/seqlz_{codes,compress,compress_literals,decompress,decompress_literals,default_tables,lit_sets}.c
 * seqlz_tables.c
 */
#include <stdio.h>
#include <stdlib.h>

#include "seqlz.h"

int main(int argc, char** argv) {
    static unsigned char page[SEQLZ_BLOCK_SIZE], dst[2 * SEQLZ_BLOCK_SIZE];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    FILE *f, *o;
    unsigned int pages = 0, i, k, level;

    if (argc != 4 || !(f = fopen(argv[1], "rb")) || !(o = fopen(argv[2], "wb"))) {
        fprintf(stderr, "usage: seqsrep <pages file> <output> <level>\n");
        return 1;
    }
    level = (unsigned int)atoi(argv[3]);
    while (fread(page, 1, SEQLZ_BLOCK_SIZE, f) == SEQLZ_BLOCK_SIZE) {
        unsigned int n, r1 = 1, r2 = 0, r3 = 0;

        for (k = 1; k < SEQLZ_BLOCK_SIZE && page[k] == page[0]; k++)
            ;
        if (k == SEQLZ_BLOCK_SIZE || seqlz_compress(&st, page, dst, sizeof dst, level) >= 3625)
            continue;
        n = seqlz_find(&st, page, seq, level);
        for (i = 0; i + 1 < n; i++) {
            unsigned int ll = seq[i].literals, ml = seq[i].match, off = seq[i].offset;
            unsigned short r[6];

            r[0] = (unsigned short)((ll < 15 ? ll : 15) + 16 * (ml - 4 < 31 ? ml - 4 : 31));
            r[1] = (unsigned short)off;
            r[2] = (unsigned short)r1;
            r[3] = (unsigned short)r2;
            r[4] = (unsigned short)r3;
            r[5] = i == 0;
            if (fwrite(r, sizeof r[0], 6, o) != 6)
                return 1;
            if (off == r2) {
                r2 = r1;
                r1 = off;
            } else if (off != r1) {
                r3 = r2;
                r2 = r1;
                r1 = off;
            }
        }
        pages++;
    }
    fprintf(stderr, "%s level %u: %u pages\n", argv[1], level, pages);
    return fclose(o) != 0;
}
