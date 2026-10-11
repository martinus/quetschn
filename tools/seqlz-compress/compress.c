// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * compress <page>...: each 4 KiB page as seqlz-fast writes it to <page>.fast and as seqlz-fast-lit
 * writes it to <page>.lit, both decoded again and compared before they are written.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

int main(int argc, char** argv) {
    static unsigned char page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    int i, coded;

    if (!t || seqlz_tables_init(t, &seqlz_default_own))
        return 1;
    for (i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");

        if (!f || fread(page, 1, SEQLZ_PAGE, f) != SEQLZ_PAGE) {
            fprintf(stderr, "%s: not a page\n", argv[i]);
            return 1;
        }
        fclose(f);
        for (coded = 0; coded < 2; coded++) {
            unsigned int n = seqlz_compress(t, &st, page, dst, sizeof dst, coded);
            char name[4096];

            if (!n || seqlz_decode(t, dst, n, out, scratch) || memcmp(out, page, SEQLZ_PAGE)) {
                fprintf(stderr, "%s: the roundtrip failed\n", argv[i]);
                return 1;
            }
            snprintf(name, sizeof name, "%s.%s", argv[i], coded ? "lit" : "fast");
            f = fopen(name, "wb");
            if (!f || fwrite(dst, 1, n, f) != n || fclose(f)) {
                perror(name);
                return 1;
            }
            printf("%s: %u bytes\n", name, n);
        }
    }
    free(t);
    return 0;
}
