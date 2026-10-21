// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * xor.c: the facts of level 3's XORed literals, for chapters 4, 6, 7, 10, 11, 13 and 14. It includes
 * seqlz_compress.c and seqlz_decompress.c to reach the encoder's records, xor_literals(),
 * code_literals() and unxor_fast():
 *   xor.step, xor.penalty, xor.rec     XOR_STEP, the 8 bits of the XOR's byte in sums of every
 *                                      XOR_STEP-th literal, LIT_REC
 *   xor.last.N                         seqlz_xor_last(N)
 *   xor.ptr.*                          chapter 9's page of pointers at level 3: its records, the
 *                                      prices of the four choices, the literals plain and XORed,
 *                                      the pages of levels 2 and 3 and of level 3 without the XOR,
 *                                      and unxor_fast() on its second sequence
 */
#include <stdarg.h>

#include "facts.h"
#include "seqlz_compress.c"
#include "seqlz_decompress.c"

static char key_buf[128];

static const char* key(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(key_buf, sizeof key_buf, fmt, ap);
    va_end(ap);
    return key_buf;
}

static void fail(const char* what) {
    fprintf(stderr, "facts/xor.c: %s\n", what);
    exit(1);
}

static unsigned int lit_len(const struct seqlz_tables* t, unsigned int k, unsigned int b) {
    return t->lit[k].enc[b] & 0xffU;
}

int main(void) {
    static u8 page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE], d2[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    static unsigned int hist[256], plain_hist[256];
    static u8 xored[SEQLZ_PAGE];
    const struct seqlz_tables* t = fact_tables();
    struct seqlz_encoder e;
    unsigned int k, x, i, n, pick = 0, best = ~0U, n_lit, recs = 0, with_rec, distinct = 0, distinct_plain = 0;
    unsigned int price[4], table[4], len, rec_pos[3], rec_ll[3], rec_last[3], rec_dist[3];
    char hex[64];
    u8* p;

    fact_num("xor.step", XOR_STEP);
    fact_num("xor.penalty", 8U / XOR_STEP);
    fact_num("xor.rec", LIT_REC);
    for (k = 1; k <= 9; k++)
        fact_num(key("xor.last.%u", k), seqlz_xor_last(k));

    /* the page of pointers through compress_own()'s steps, the records looked at before xor_literals() */
    pointer_page(page);
    encoder_init(&e, t, dst, sizeof(dst), page + SEQLZ_PAGE);
    e.rec = true;
    seqlz_match_page(st.table, page, true, encode_emit_rec, &e);
    if (e.p == e.lit || !e.rec)
        fail("the page of pointers does not fit with its records");
    with_rec = (unsigned int)(e.lit - (dst + SEQLZ_HEADER));
    for (p = dst + SEQLZ_HEADER; p < e.lit; p += LIT_REC + get_unaligned_le16(p + 2)) {
        if (recs < 3) {
            rec_pos[recs] = get_unaligned_le16(p);
            rec_ll[recs] = get_unaligned_le16(p + 2);
            rec_last[recs] = get_unaligned_le16(p + 4);
            rec_dist[recs] = seqlz_xor_last(rec_last[recs]);
            fact_num(key("xor.ptr.rec.%u.pos", recs), get_unaligned_le16(p));
            fact_num(key("xor.ptr.rec.%u.ll", recs), get_unaligned_le16(p + 2));
            fact_num(key("xor.ptr.rec.%u.last", recs), get_unaligned_le16(p + 4));
            fact_num(key("xor.ptr.rec.%u.dist", recs), seqlz_xor_last(get_unaligned_le16(p + 4)));
            fact_hex(key("xor.ptr.rec.%u.hex", recs), p, LIT_REC);
        }
        recs++;
    }
    fact_num("xor.ptr.recs", recs);
    fact_num("xor.ptr.with_rec", with_rec);

    /* the prices as xor_literals() takes them, here as plain sums of code lengths */
    {
        unsigned int sum[4][SEQLZ_LIT_SETS] = {{0}}, j = 0, sampled = 0, ll, pos, last;

        for (p = dst + SEQLZ_HEADER; p < e.lit; p += LIT_REC + ll) {
            pos = get_unaligned_le16(p);
            ll = get_unaligned_le16(p + 2);
            last = seqlz_xor_last(get_unaligned_le16(p + 4));
            for (i = j; i < ll; i += XOR_STEP) {
                unsigned int at = pos + i, b = page[at];
                unsigned int ref[4] = {
                    0, i >= 4 ? page[at - 4] : 0U, i >= 8 ? page[at - 8] : 0U, at >= last ? page[at - last] : 0U};

                for (x = 0; x < 4; x++)
                    for (k = 0; k < SEQLZ_LIT_SETS; k++)
                        sum[x][k] += lit_len(t, k, b ^ ref[x]);
                sampled++;
            }
            j = i - ll;
        }
        fact_num("xor.ptr.sampled", sampled);
        for (x = 0; x < 4; x++) {
            price[x] = ~0U;
            for (k = 0; k < SEQLZ_LIT_SETS; k++) {
                unsigned int b = sum[x][k] + (x ? 8U / XOR_STEP : 0U);

                if (b < price[x]) {
                    price[x] = b;
                    table[x] = k;
                }
            }
            fact_num(key("xor.ptr.price.%u", x), price[x]);
            fact_num(key("xor.ptr.table.%u", x), table[x]);
            if (price[x] < best) {
                best = price[x];
                pick = x;
            }
        }
    }
    x = xor_literals(t, &e, dst, page, st.literals);
    if (x != pick)
        fail("xor_literals() picks another XOR than the prices");
    fact_num("xor.ptr.pick", pick);
    memcpy(xored, st.literals, SEQLZ_PAGE);
    n_lit = (unsigned int)(e.lit - (dst + SEQLZ_HEADER));
    fact_num("xor.ptr.n_lit", n_lit);
    if (with_rec != n_lit + LIT_REC * recs)
        fail("the records are not LIT_REC bytes each");
    fact_hex("xor.ptr.plain", dst + SEQLZ_HEADER, 12);
    fact_hex("xor.ptr.xored", st.literals, 12);
    for (i = 0; i < n_lit; i++) {
        hist[st.literals[i]]++;
        plain_hist[dst[SEQLZ_HEADER + i]]++;
    }
    for (i = 0; i < 256; i++) {
        distinct += hist[i] != 0;
        distinct_plain += plain_hist[i] != 0;
    }
    fact_num("xor.ptr.distinct.plain", distinct_plain);
    fact_num("xor.ptr.distinct.xored", distinct);
    fact_num("xor.ptr.h.10", hist[0x10]);
    fact_num("xor.ptr.h.30", hist[0x30]);
    fact_num("xor.ptr.h.70", hist[0x70]);
    fact_num("xor.ptr.h.f0", hist[0xf0]);
    /* the whole page at levels 2 and 3, and level 3 without the XOR */
    len = fact_compress_level(page, d2, 2);
    fact_num("xor.ptr.l2.len", len);
    len = fact_compress_level(page, d2, SEQLZ_LEVEL_OWN);
    fact_num("xor.ptr.l3.len", len);
    if (!(d2[1] & 0x80U) || !(d2[2] >> SEQLZ_LIT_XOR_AT))
        fail("level 3 does not XOR the page of pointers");
    snprintf(hex, sizeof hex, "0x%02x", d2[2]);
    fact_str("xor.ptr.l3.t", hex);
    fact_num("xor.ptr.l3.code", d2[2] & SEQLZ_LIT_CODE_MASK);
    fact_num("xor.ptr.l3.w", SEQLZ_SIZE_BITS_MIN + (d2[2] >> SEQLZ_LIT_WIDTH_AT & 7U));
    fact_num("xor.ptr.l3.xr_at", SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN + (d2[2] >> SEQLZ_LIT_WIDTH_AT & 7U)));
    fact_num("xor.ptr.l3.xr", d2[SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN + (d2[2] >> SEQLZ_LIT_WIDTH_AT & 7U))]);
    fact_hex("xor.ptr.l3.head", d2, SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN + (d2[2] >> SEQLZ_LIT_WIDTH_AT & 7U)) + 1);
    fact_num("xor.ptr.l3.n", get_unaligned_le16(d2) & (SEQLZ_LIT_CODED - 1U));
    if (seqlz_decode(t, d2, len, out, scratch) || memcmp(out, page, SEQLZ_PAGE))
        fail("the page of pointers does not decode to itself");
    {
        /* level 3 without the XOR: compress_own() with the records off, then code_own() with xor 0 */
        unsigned int raw, n0;

        encoder_init(&e, t, d2, sizeof(d2), page + SEQLZ_PAGE);
        seqlz_match_page(st.table, page, true, encode_emit, &e);
        raw = encoder_finish(&e, d2, sizeof(d2));
        n0 = code_own(t, d2, raw, &st, 0);
        fact_num("xor.ptr.l3plain.len", n0);
    }

    /*
     * unxor_fast() on the second sequence, as the fast path calls it: the page up to the sequence,
     * then 16 bytes copied from the XORed literals at d
     */
    {
        u8 buf[SEQLZ_PAGE];

        n = rec_pos[1];
        memcpy(buf, page, n);
        memcpy(buf + n, xored + rec_ll[0], 16);
        fact_hex("xor.ptr.fast.before", buf + n, 8);
        fact_hex("xor.ptr.fast.ref", buf + n - rec_dist[1], 8);
        unxor_fast(SEQLZ_XOR_LAST, buf + n, buf, rec_ll[1], rec_last[1]);
        fact_hex("xor.ptr.fast.after", buf + n, 8);
        if (memcmp(buf + n, page + n, rec_ll[1]))
            fail("unxor_fast() does not give the page's bytes back");
    }
    facts_end();
    return 0;
}
