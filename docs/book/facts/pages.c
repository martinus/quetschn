// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * pages.c: the facts of the book's made-up pages (pages.h), compressed by the real code. For each
 * page P, raw literals and coded (P.coded.*, when coding pays):
 *   P.len          bytes of the compressed page        P.lits     literals
 *   P.hex          the compressed page, if it has at most 64 bytes
 *   P.seqs         sequences, the last one included    P.tokens   sequences with a token
 *   P.escapes      escaped tokens                      P.values   length values
 *   P.b.bytes      the bitstream's bytes               P.b.hex    them, if at most 64
 *   P.b.bits       bits the sequences take             P.b.pad    bits of padding in the last byte
 *   P.b.bin        those bits as 0 and 1, if at most 256
 *   P.coded.len    with coded literals, the compressed page's bytes (= P.len if coding does not pay)
 *   P.l3.len       the compressed page's bytes at level 3, SEQLZ_LEVEL_OWN
 *   P.l3.own       1 if it has a literal table of its own there
 *   P.l3.delta     1 if its literals are deltas there
 */
#include "facts.h"

static unsigned int value_len(const struct value_table* vt, unsigned int v) {
    unsigned int extra, s = seqlz_len_symbol(v, &extra);

    return (vt->enc[s] >> 16) + extra;
}

static void page_facts(const char* name, void (*make)(u8*)) {
    static u8 page[SEQLZ_BLOCK_SIZE], dst[2 * SEQLZ_BLOCK_SIZE];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    const struct seqlz_tables* t = fact_tables();
    unsigned int len, n, i, last = 1, bits = 0, escapes = 0, values = 0, n_lit, b_bytes, coded;
    char key[64];

    make(page);
    len = fact_compress(page, dst, 0);
    n_lit = get_unaligned_le16(dst);
    b_bytes = len - SEQLZ_HEADER - n_lit;
    n = seqlz_find(&st, page, seq, 2);
    for (i = 0; i + 1 < n; i++) {
        unsigned int raw, cls = seqlz_off_class(seq[i].offset, last, &raw);
        unsigned int tok = seqlz_token(seq[i].literals, seq[i].match, cls), te = t->token.enc[tok];

        if (te)
            bits += te >> TOK_ENC_LEN_AT;
        else {
            bits += (t->token.enc[SEQLZ_ESCAPE] >> TOK_ENC_LEN_AT) + SEQLZ_ESCAPE_BITS;
            escapes++;
        }
        bits += raw;
        if (seq[i].literals >= SEQLZ_LL_CAP) {
            bits += value_len(&t->ll, seq[i].literals - SEQLZ_LL_CAP);
            values++;
        }
        if (seq[i].match - 4U >= SEQLZ_ML_CAP) {
            bits += value_len(&t->ml, seq[i].match - 4U - SEQLZ_ML_CAP);
            values++;
        }
        last = seq[i].offset;
    }
    if ((bits + 7) / 8 != b_bytes) {
        fprintf(stderr, "facts/pages.c: %s: %u bits counted, the bitstream has %u bytes\n", name, bits, b_bytes);
        exit(1);
    }
#define K(suffix) (snprintf(key, sizeof key, "%s.%s", name, suffix), key)
    fact_num(K("len"), len);
    fact_num(K("lits"), n_lit);
    if (len <= 64)
        fact_hex(K("hex"), dst, len);
    fact_num(K("seqs"), n);
    fact_num(K("tokens"), n - 1);
    fact_num(K("escapes"), escapes);
    fact_num(K("values"), values);
    fact_num(K("b.bytes"), b_bytes);
    if (b_bytes <= 64)
        fact_hex(K("b.hex"), dst + SEQLZ_HEADER + n_lit, b_bytes);
    fact_num(K("b.bits"), bits);
    fact_num(K("b.pad"), 8 * b_bytes - bits);
    if (bits <= 256)
        fact_bits(K("b.bin"), dst + SEQLZ_HEADER + n_lit, 0, bits);
    coded = fact_compress(page, dst, 1);
    fact_num(K("coded.len"), coded);
    if (coded <= 256)
        fact_hex(K("coded.hex"), dst, coded);
    fact_num(K("l3.len"), fact_compress_level(page, dst, SEQLZ_LEVEL_OWN));
    fact_num(K("l3.own"), (dst[1] & 0x80U) && (dst[2] & SEQLZ_LIT_CODE_MASK) == SEQLZ_LIT_OWN);
    fact_num(K("l3.delta"), (dst[1] & 0x80U) && (dst[2] >> SEQLZ_LIT_DELTA_AT));
#undef K
}

int main(void) {
    page_facts("ab", ab_page);
    page_facts("cat", cat_page);
    page_facts("second", second_page);
    page_facts("letters", letters_page);
    page_facts("slab", slab_page);
    page_facts("pointers", pointer_page);
    page_facts("words", words_page);
    facts_end();
    return 0;
}
