// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * abbits.c: chapter 4's page of ab, compressed by the real code, and the fields of its bitstream as
 * the decoder reads them: the token's code, the offset's bits, the match length value's code and its
 * extra bits, with what each means; the escape and the token's 12 bits when the token has no code. Fails
 * if the page is not what abbits.js tells: 2 raw literals, one sequence with a new offset and a match
 * length value, and the last sequence without bits.
 */
#include <stdio.h>
#include <stdlib.h>

#include "pages.h"

static char code_buf[32];

/* the code of symbol s in a table of n code lengths, canonical, as "0101" */
static const char* code_of(const u8* len, unsigned int n, unsigned int s) {
    unsigned int count[17] = {0}, next[17] = {0}, c = 0, k, l;

    for (k = 0; k < n; k++)
        count[len[k]]++;
    count[0] = 0;
    for (l = 1; l <= 16; l++) {
        c = (c + count[l - 1]) << 1;
        next[l] = c;
    }
    for (k = 0; k < s; k++)
        if (len[k] == len[s])
            next[len[s]]++;
    for (k = 0; k < len[s]; k++)
        code_buf[k] = (char)('0' + ((next[len[s]] >> (len[s] - 1 - k)) & 1));
    code_buf[len[s]] = 0;
    return code_buf;
}

int main(void) {
    static u8 page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE], out[SEQLZ_PAGE];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    unsigned int len, n, raw, cls, tok, extra, sym, v, k, n_lit;

    if (!t || seqlz_tables_init(t, &seqlz_default_own))
        return 1;
    ab_page(page);
    len = seqlz_compress(t, &st, page, dst, sizeof dst, 0);
    if (!len || seqlz_decode(t, dst, len, out, NULL) || memcmp(out, page, SEQLZ_PAGE))
        return 1;
    n = seqlz_find(&st, page, seq);
    n_lit = get_unaligned_le16(dst);
    cls = seqlz_off_class(seq[0].offset, 1, &raw);
    tok = seqlz_token(seq[0].literals, seq[0].match, cls);
    v = seq[0].match - 4 - SEQLZ_ML_CAP;
    sym = seqlz_len_symbol(v, &extra);
    if (n != 2 || seq[1].literals != 0 || n_lit != 2 || cls == 0 || seq[0].literals >= SEQLZ_LL_CAP ||
        seq[0].match - 4U < SEQLZ_ML_CAP || extra == 0) {
        fprintf(stderr, "abbits.c: the page of ab is not what abbits.js tells\n");
        return 1;
    }
    printf("{\"len\":%u,\"bytes\":[", len);
    for (k = 0; k < len; k++)
        printf("%s%u", k ? "," : "", dst[k]);
    printf("],\"tok\":%u,", tok);
    if (seqlz_default_own.token[tok]) {
        printf("\"escCode\":\"\",\"tokCode\":\"%s\",", code_of(seqlz_default_own.token, SEQLZ_TOKEN_SYMBOLS + 1, tok));
    } else {
        /* the escape's code, then the token's number in SEQLZ_ESCAPE_BITS plain bits */
        printf("\"escCode\":\"%s\",\"tokCode\":\"", code_of(seqlz_default_own.token, SEQLZ_TOKEN_SYMBOLS + 1, SEQLZ_ESCAPE));
        for (k = SEQLZ_ESCAPE_BITS; k-- > 0;)
            putchar('0' + ((tok >> k) & 1));
        printf("\",");
    }
    /* sent: the offset bits as a number; the class multiplies it by 2 or 8 again, or not */
    printf("\"ll\":%u,\"mlf\":%u,\"cls\":%u,\"rawBits\":%u,\"sent\":%u,\"shift\":%u,\"off\":%u,",
           seq[0].literals,
           SEQLZ_ML_CAP,
           cls,
           raw,
           seq[0].offset >> SEQLZ_OFF_SHIFT(cls),
           SEQLZ_OFF_SHIFT(cls),
           seq[0].offset);
    printf("\"mlSym\":%u,\"mlCode\":\"%s\",\"extra\":%u,\"extraVal\":%u,\"value\":%u,\"ml\":%u}\n",
           sym,
           code_of(seqlz_default_own.ml, SEQLZ_LEN_SYMBOLS, sym),
           extra,
           v - (1U << extra),
           v,
           seq[0].match);
    return 0;
}
