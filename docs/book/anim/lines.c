// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * lines.c: the token lookups of chapter 16's page. Compresses the page, walks the sequences'
 * bitstream as seqlz_decode() reads it, without copying bytes, and prints the 11-bit index of every
 * token lookup and the code length of the entry it found. Fails if the walk does not end exactly at
 * the end of the bitstream, after as many tokens as the real decode reads: one for each sequence but
 * the last.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pages.h"
#include "seqlz_decompress.c"

/* the bitstream from q to end, read most significant bit first, zeros after its end */
static const u8* g_q;
static unsigned long g_nbits;

static unsigned int peek(unsigned long pos, unsigned int n) {
    unsigned int v = 0, i;

    for (i = 0; i < n; i++, pos++) {
        unsigned int bit = 0;

        if (pos < g_nbits)
            bit = (g_q[pos >> 3] >> (7 - (pos & 7))) & 1U;
        v = v << 1 | bit;
    }
    return v;
}

/* a length value, as seqlz_br_value() reads it */
static unsigned int value(unsigned long* pos, const struct value_table* vt) {
    u32 e = vt->decode[peek(*pos, SEQLZ_MAX_BITS)];
    unsigned int n = e & 15U, x = (e >> 4) & 15U;
    unsigned int v = ((e >> 8) & 0xffffU) + peek(*pos + n, x);

    *pos += n + x;
    return v;
}

int main(void) {
    static u8 page[SEQLZ_PAGE], comp[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    static unsigned int idx[SEQLZ_PAGE], len[SEQLZ_PAGE];
    static struct seqlz_state st;
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    unsigned int n, n_lit, d = 0, used = 0, seqs = 0, i;
    unsigned long pos = 0;
    const u8* q;

    if (!t || seqlz_tables_init(t, &seqlz_default_own))
        return 1;
    slab_page(page);
    n = seqlz_compress(t, &st, page, comp, sizeof(comp), 1);
    if (!n || seqlz_decode(t, comp, n, out, scratch) || memcmp(out, page, SEQLZ_PAGE)) {
        fprintf(stderr, "lines.c: the page does not decode to itself\n");
        return 1;
    }
    n_lit = get_unaligned_le16(comp);
    if (!(n_lit & SEQLZ_LIT_CODED)) {
        fprintf(stderr, "lines.c: the page has raw literals\n");
        return 1;
    }
    /* where the bitstream starts, as seqlz_decode() gets it */
    q = decode_literals(t, comp, n, n_lit & (SEQLZ_LIT_CODED - 1U), scratch);
    if (!q)
        return 1;
    g_q = q;
    g_nbits = 8UL * (unsigned long)(comp + n - q);

    /*
     * as in seqlz_decode(): when the literals left fill the rest of the page, they are the last
     * sequence, which has no token
     */
    n_lit &= SEQLZ_LIT_CODED - 1U;
    while (n_lit - used != SEQLZ_PAGE - d && seqs < SEQLZ_PAGE) {
        unsigned int ix = peek(pos, SEQLZ_TOKEN_BITS);
        u32 tok = t->token.decode[ix];
        unsigned int nl, ml;

        idx[seqs] = ix;
        /* the code's length; an escape's entry has it only in TOK_DROP */
        len[seqs] = tok >> TOK_ESCAPE_AT ? TOK_DROP(tok) : TOK_N(tok);
        if (tok >> TOK_ESCAPE_AT) {
            unsigned int s;

            pos += TOK_DROP(tok);
            s = peek(pos, SEQLZ_ESCAPE_BITS);
            pos += SEQLZ_ESCAPE_BITS;
            if (s >= SEQLZ_TOKEN_SYMBOLS)
                return 1;
            tok = token_entry(s, 0);
        }
        seqs++;
        pos += TOK_DROP(tok);
        nl = TOK_LL(tok);
        ml = TOK_ML(tok);
        if (nl == SEQLZ_LL_CAP)
            nl += value(&pos, &t->ll);
        if (ml == SEQLZ_ML_CAP + 4U)
            ml += value(&pos, &t->ml);
        used += nl;
        d += nl + ml;
    }
    /* the walk must end where the bits end: fewer than 8 bits left, in the last byte */
    if (n_lit - used != SEQLZ_PAGE - d || pos > g_nbits || g_nbits - pos >= 8) {
        fprintf(stderr, "lines.c: walk ends at bit %lu of %lu, byte %u\n", pos, g_nbits, d);
        return 1;
    }
    if (seqs != 179) {
        fprintf(stderr, "lines.c: %u tokens, chapter 16 has 180 sequences, the last without one\n", seqs);
        return 1;
    }
    printf("{\"idx\":[");
    for (i = 0; i < seqs; i++)
        printf("%s%u", i ? "," : "", idx[i]);
    printf("],\"len\":[");
    for (i = 0; i < seqs; i++)
        printf("%s%u", i ? "," : "", len[i]);
    printf("]}\n");
    return 0;
}
