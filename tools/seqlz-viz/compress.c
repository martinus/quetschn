// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * compress <page>...: each 4 KiB page as seqlz-fast writes it to <page>.fast, as seqlz-fast-lit
 * writes it to <page>.lit and as level 3 writes it to <page>.hc, each decoded again and compared
 * before it is written. Prints which of them have a literal table of their own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

int main(int argc, char** argv) {
    static unsigned char page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    static struct seqlz_hc_state hc;
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
        for (coded = 0; coded < 3; coded++) {
            static const char* const ext[3] = {"fast", "lit", "hc"};
            unsigned int n = coded < 2 ? seqlz_compress(t, &st, page, dst, sizeof dst, coded)
                                       : seqlz_compress_hc(t, &hc, page, dst, sizeof dst, 0);
            char name[4096];

            if (!n || seqlz_decode(t, dst, n, out, scratch) || memcmp(out, page, SEQLZ_PAGE)) {
                fprintf(stderr, "%s: the roundtrip failed\n", argv[i]);
                return 1;
            }
            snprintf(name, sizeof name, "%s.%s", argv[i], ext[coded]);
            f = fopen(name, "wb");
            if (!f || fwrite(dst, 1, n, f) != n || fclose(f)) {
                perror(name);
                return 1;
            }
            printf("%s: %u bytes%s\n",
                   name,
                   n,
                   n > 2 && (dst[1] & 0x80) && (dst[2] >> SEQLZ_LIT_OWN_AT & 1) ? ", own literal table" : "");
        }
    }
    free(t);
    return 0;
}
