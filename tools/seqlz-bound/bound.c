// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * bound <pages file>: per symbol kind of seqlz-fast-lit, the bits its codes take on these pages, and
 * the counts of the symbols, as JSON for bound.py, which compares them with the entropy and with
 * Huffman codes fitted to the same pages. Checks per page that the counted bits give the compressed
 * size. Same-filled pages are left out, as zram does; pages zram stores raw, of 3625 bytes and more,
 * are counted apart. Build from the repository's root:
 *   cc -O2 -Iexplore -o bound tools/seqlz-bound/bound.c explore/seqlz.c explore/seqlz_default_tables.c \
 *      explore/seqlz_lit_sets.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"

static double tok_hist[SEQLZ_TOKEN_SYMBOLS], ll_hist[SEQLZ_LEN_SYMBOLS], ml_hist[SEQLZ_LEN_SYMBOLS];
static double lit_hist[SEQLZ_LIT_SETS][256], raw_hist[256], off_hist[6][4096];
static double b_tok, b_esc, b_off, b_llc, b_llx, b_mlc, b_mlx, b_lit, b_rawlit, b_hdr, b_pad;
static double pages, huge, coded_pages, n_esc, bytes_all;

static void arr(const char* name, const double* a, unsigned int n) {
    unsigned int i;

    printf("\"%s\":[", name);
    for (i = 0; i < n; i++)
        printf("%s%.0f", i ? "," : "", a[i]);
    printf("],");
}

int main(int argc, char** argv) {
    static unsigned char page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    const struct seqlz_lengths* L = &seqlz_default_own;
    FILE* f = fopen(argv[1], "rb");
    unsigned int i;

    if (!t || !f || seqlz_tables_init(t, L))
        return 1;
    while (fread(page, 1, SEQLZ_PAGE, f) == SEQLZ_PAGE) {
        unsigned int len = seqlz_compress(t, &st, page, dst, sizeof dst, 1), n = seqlz_find(&st, page, seq);
        unsigned int coded = dst[1] >> 7, set = dst[2] & 7U, last = 1, extra, k, in = 0, nlit = 0;
        double bits = 0, sbits[8] = {0};

        /* zram's same-filled pages never reach the codec */
        for (k = 1; k < SEQLZ_PAGE && page[k] == page[0]; k++)
            ;
        if (k == SEQLZ_PAGE)
            continue;
        if (len >= 3625) {
            huge++;
            continue;
        }
        pages++;
        bytes_all += len;
        coded_pages += coded;
        for (i = 0; i < n; i++) {
            unsigned int ll = seq[i].literals, ml = i + 1 == n ? 0 : seq[i].match, cls = 0, tok;

            if (ml)
                cls = seqlz_off_class(seq[i].offset, last, &extra);
            tok = seqlz_token(ll, ml, cls);
            tok_hist[tok]++;
            if (L->token[tok]) {
                b_tok += L->token[tok];
                bits += L->token[tok];
            } else {
                b_esc += L->token[SEQLZ_ESCAPE] + 12;
                bits += L->token[SEQLZ_ESCAPE] + 12;
                n_esc++;
            }
            if (ml) {
                off_hist[cls][(seq[i].offset >> SEQLZ_OFF_SHIFT(cls)) & 4095U] += cls != 0;
                b_off += SEQLZ_RAW_BITS(cls);
                bits += SEQLZ_RAW_BITS(cls);
            }
            if (ll >= SEQLZ_LL_CAP) {
                unsigned int s = seqlz_len_symbol(ll - SEQLZ_LL_CAP, &extra);

                ll_hist[s]++;
                b_llc += L->ll[s];
                b_llx += extra;
                bits += L->ll[s] + extra;
            }
            if (ml && ml - 4 >= SEQLZ_ML_CAP) {
                unsigned int s = seqlz_len_symbol(ml - 4 - SEQLZ_ML_CAP, &extra);

                ml_hist[s]++;
                b_mlc += L->ml[s];
                b_mlx += extra;
                bits += L->ml[s] + extra;
            }
            for (k = 0; k < ll; k++, nlit++) {
                unsigned int b = page[in + k];

                if (coded) {
                    lit_hist[set][b]++;
                    b_lit += seqlz_lit_sets[set][b];
                    sbits[nlit % 8] += seqlz_lit_sets[set][b];
                } else {
                    raw_hist[b]++;
                    b_rawlit += 8;
                }
            }
            in += ll + ml;
            if (ml)
                last = seq[i].offset;
        }
        {
            /* the header, and the bits that fill up the last byte of each stream and the bitstream */
            double expect;
            unsigned int width = 5 + ((dst[2] >> 3) & 7U), hdr = coded ? 3 + width : 2;

            b_hdr += 8.0 * hdr;
            expect = hdr + (bits + 7) / 8;
            if (coded) {
                double s = 0;

                for (k = 0; k < 8; k++)
                    s += (unsigned int)((sbits[k] + 7) / 8);
                expect = hdr + s + (unsigned int)((bits + 7) / 8);
            } else
                expect = hdr + nlit + (unsigned int)((bits + 7) / 8);
            if ((unsigned int)expect != len) {
                fprintf(stderr, "page %.0f: %u bytes, the counts say %.0f\n", pages, len, expect);
                return 1;
            }
            b_pad += 8.0 * len - 8.0 * hdr - bits - (coded ? 0 : 8.0 * nlit);
            for (k = 0; coded && k < 8; k++)
                b_pad -= sbits[k];
        }
    }
    printf("{\"pages\":%.0f,\"huge\":%.0f,\"coded_pages\":%.0f,\"bytes\":%.0f,\"esc\":%.0f,",
           pages,
           huge,
           coded_pages,
           bytes_all,
           n_esc);
    printf(
        "\"bits\":{\"tok\":%.0f,\"esc\":%.0f,\"off\":%.0f,\"llc\":%.0f,\"llx\":%.0f,\"mlc\":%.0f,\"mlx\":%.0f,\"lit\":%.0f,\"rawlit\":%.0f,\"hdr\":%.0f,\"pad\":%.0f},",
        b_tok,
        b_esc,
        b_off,
        b_llc,
        b_llx,
        b_mlc,
        b_mlx,
        b_lit,
        b_rawlit,
        b_hdr,
        b_pad);
    arr("tok", tok_hist, SEQLZ_TOKEN_SYMBOLS);
    arr("ll", ll_hist, SEQLZ_LEN_SYMBOLS);
    arr("ml", ml_hist, SEQLZ_LEN_SYMBOLS);
    arr("raw", raw_hist, 256);
    for (i = 1; i < 6; i++) {
        char name[8];

        snprintf(name, sizeof name, "off%u", i);
        arr(name, off_hist[i], 1U << SEQLZ_RAW_BITS(i));
    }
    printf("\"lit\":[");
    for (i = 0; i < SEQLZ_LIT_SETS; i++) {
        unsigned int k;

        printf("%s[", i ? "," : "");
        for (k = 0; k < 256; k++)
            printf("%s%.0f", k ? "," : "", lit_hist[i][k]);
        printf("]");
    }
    printf("]}\n");
    return 0;
}
