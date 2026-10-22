// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * compress <page>...: each 4 KiB page as seqlz_compress() writes it at each level, to <page>.1,
 * <page>.2 and <page>.3, each decoded again and compared before it is written.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

int main(int argc, char** argv) {
    static unsigned char page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    int i;
    unsigned int level;

    if (seqlz_tables_init(&seqlz_default_lengths))
        return 1;
    for (i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");

        if (!f || fread(page, 1, SEQLZ_PAGE, f) != SEQLZ_PAGE) {
            fprintf(stderr, "%s: not a page\n", argv[i]);
            return 1;
        }
        fclose(f);
        for (level = 1; level <= SEQLZ_LEVELS; level++) {
            unsigned int n = seqlz_compress(&st, page, dst, sizeof dst, level);
            char name[4096];

            if (!n || seqlz_decode(dst, n, out, scratch) || memcmp(out, page, SEQLZ_PAGE)) {
                fprintf(stderr, "%s: the roundtrip failed\n", argv[i]);
                return 1;
            }
            snprintf(name, sizeof name, "%s.%u", argv[i], level);
            f = fopen(name, "wb");
            if (!f || fwrite(dst, 1, n, f) != n || fclose(f)) {
                perror(name);
                return 1;
            }
            printf("%s: %u bytes\n", name, n);
        }
    }
    return 0;
}
