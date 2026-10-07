// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqs <pages file> <output>: the sequences seqlz-fast-lit's matcher finds on the pages zram keeps
 * compressed, for offsets.py, which prices layouts of the offset classes on them. Same-filled pages and
 * pages of 3625 bytes and more are left out, as in bound.c. One record of four u16 per sequence:
 * min(ll, 15) + 16 * min(ml - 4, 31) (0 for ml of the last sequence), the offset (0 for the last
 * sequence), the offset before it, and 1 for the first sequence of a page. Build from the repository's
 * root:
 *   cc -O2 -Isrc -o seqs tools/seqlz-bound/seqs.c src/seqlz.c src/seqlz_default_tables.c \
 *      src/seqlz_lit_sets.c
 */
#include <stdio.h>
#include <stdlib.h>

#include "seqlz.h"

int main(int argc, char** argv) {
    static unsigned char page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    FILE *f, *o;
    unsigned int pages = 0, i, k;

    if (argc != 3 || !(f = fopen(argv[1], "rb")) || !(o = fopen(argv[2], "wb")) || !t ||
        seqlz_tables_init(t, &seqlz_default_own)) {
        fprintf(stderr, "usage: seqs <pages file> <output>\n");
        return 1;
    }
    while (fread(page, 1, SEQLZ_PAGE, f) == SEQLZ_PAGE) {
        unsigned int n, last = 1;

        for (k = 1; k < SEQLZ_PAGE && page[k] == page[0]; k++)
            ;
        if (k == SEQLZ_PAGE || seqlz_compress(t, &st, page, dst, sizeof dst, 1) >= 3625)
            continue;
        n = seqlz_find(&st, page, seq);
        for (i = 0; i < n; i++) {
            unsigned int ll = seq[i].literals, ml = i + 1 == n ? 0 : seq[i].match;
            unsigned short r[4];

            r[0] = (unsigned short)((ll < 15 ? ll : 15) + 16 * (ml ? (ml - 4 < 31 ? ml - 4 : 31) : 0));
            r[1] = (unsigned short)(ml ? seq[i].offset : 0);
            r[2] = (unsigned short)last;
            r[3] = i == 0;
            if (fwrite(r, sizeof r[0], 4, o) != 4)
                return 1;
            if (ml)
                last = seq[i].offset;
        }
        pages++;
    }
    fprintf(stderr, "%u pages\n", pages);
    return fclose(o) != 0;
}
