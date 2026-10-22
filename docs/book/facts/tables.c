// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * tables.c: the facts of the tables and constants every chapter may quote. tok.code.N and tok.len.N
 * for each token (code "" and length 0 for one without a code, which is escaped), ll.* and ml.* for
 * the length values, lit.S.code.B for byte B in literal table S; the sizes of the structs; and how
 * many lines src/ has. Built for 16 KiB pages too, as k16.*.
 */
// facts: also 16k
#include "facts.h"

static void table(const char* name, const unsigned char* len, unsigned int n) {
    static char code[SEQLZ_TOKEN_SYMBOLS + 1][17];
    char key[64];
    unsigned int s, counts[17] = {0};

    fact_canonical(len, n, code);
    for (s = 0; s < n; s++) {
        snprintf(key, sizeof key, "%s.code.%u", name, s);
        fact_str(key, code[s]);
        snprintf(key, sizeof key, "%s.len.%u", name, s);
        fact_num(key, len[s]);
        counts[len[s]]++;
    }
    for (s = 1; s <= 16; s++) {
        snprintf(key, sizeof key, "%s.codes_of.%u", name, s);
        fact_num(key, counts[s]);
    }
    snprintf(key, sizeof key, "%s.symbols", name);
    fact_num(key, n);
    snprintf(key, sizeof key, "%s.with_code", name);
    fact_num(key, n - counts[0]);
}

int main(void) {
    char key[32];
    unsigned int k;

    table("tok", seqlz_default_lengths.token, SEQLZ_TOKEN_SYMBOLS + 1);
    table("ll", seqlz_default_lengths.ll, SEQLZ_LEN_SYMBOLS);
    table("ml", seqlz_default_lengths.ml, SEQLZ_LEN_SYMBOLS);
    for (k = 0; k < SEQLZ_LIT_SETS; k++) {
        snprintf(key, sizeof key, "lit.%u", k);
        table(key, seqlz_lit_sets[k], 256);
    }
    fact_num("const.LEN_SYMBOLS", SEQLZ_LEN_SYMBOLS);
    fact_num("const.TOKEN_SYMBOLS", SEQLZ_TOKEN_SYMBOLS);
    fact_num("const.SCRATCH", SEQLZ_SCRATCH);
    fact_num("const.MAX_SEQUENCES", SEQLZ_MAX_SEQUENCES);
    fact_num("size.lengths", sizeof(struct seqlz_lengths));
    fact_num("size.state", sizeof(struct seqlz_state));
    fact_num("size.tables", (long long)sizeof(struct seqlz_tables));
    facts_end();
    return 0;
}
