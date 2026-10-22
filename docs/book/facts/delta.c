// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * delta.c: the facts of level 3's literals as deltas, for chapters 4, 6, 7, 10, 11, 13 and 14. It includes
 * seqlz_compress.c and seqlz_decompress.c to reach the encoder's records, seqlz_delta_literals(),
 * seqlz_code_literals() and undelta_fast():
 *   delta.sub.*, delta.add.*               sub_bytes() and add8() on one example, step by step
 *   delta.step, delta.penalty, delta.rec   DELTA_STEP, the 8 bits of the delta's byte in sums of
 *                                          every DELTA_STEP-th literal, LIT_REC
 *   delta.last.N                           seqlz_delta_last(N)
 *   delta.ptr.*                            chapter 9's page of pointers at level 3: its records, the
 *                                          number after the first with the offset 8 before, the
 *                                          prices of the four choices, the literals plain and as
 *                                          deltas, the pages of levels 2 and 3 and of level 3 without
 *                                          deltas, and undelta_fast() with the picked delta on its
 *                                          second sequence;
 *                                          delta.ptr.xor.* the same literals XORed instead
 */
#include <stdarg.h>

#include "facts.h"
#include "seqlz_compress.c"
#include "seqlz_compress_literals.c"
#include "seqlz_decompress.c"
#include "seqlz_decompress_literals.c"

static char key_buf[128];

static const char* key(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(key_buf, sizeof key_buf, fmt, ap);
    va_end(ap);
    return key_buf;
}

static void fail(const char* what) {
    fprintf(stderr, "facts/delta.c: %s\n", what);
    exit(1);
}

static void fact_x64(const char* name, u64 v) {
    char buf[24];

    snprintf(buf, sizeof buf, "0x%016llx", (unsigned long long)v);
    fact_str(name, buf);
}

static unsigned int lit_len(const struct seqlz_tables* t, unsigned int k, unsigned int b) {
    return t->lit[k].enc[b] & 0xffU;
}

int main(void) {
    static u8 page[SEQLZ_BLOCK_SIZE], dst[2 * SEQLZ_BLOCK_SIZE], d2[2 * SEQLZ_BLOCK_SIZE], out[SEQLZ_BLOCK_SIZE],
        scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    static unsigned int hist[256], plain_hist[256];
    static u8 deltas[SEQLZ_BLOCK_SIZE];
    const struct seqlz_tables* t = fact_tables();
    struct seqlz_encoder e;
    unsigned int k, x, i, n, pick = 0, best = ~0U, n_lit, recs = 0, last8 = 0, with_rec, distinct = 0, distinct_plain = 0;
    unsigned int price[4], table[4], len, rec_pos[3], rec_ll[3], rec_last[3], rec_dist[3];
    char hex[64];
    u8* p;

    fact_num("delta.step", DELTA_STEP);
    fact_num("delta.penalty", 8U / DELTA_STEP);
    fact_num("delta.rec", LIT_REC);
    {
        /*
         * sub_bytes() and add8() on two words whose low bytes borrow and carry, step by step as the
         * book explains them, and a plain subtraction and addition of the same words
         */
        const u64 a = 0x0100U, b = 0x0010U, high = 0x8080808080808080ULL, low = 0x7f7f7f7f7f7f7f7fULL;
        u8 w[16];
        u64 r = sub_bytes(a, b);

        fact_x64("delta.sub.a", a);
        fact_x64("delta.sub.b", b);
        fact_x64("delta.sub.a_hi", a | high);
        fact_x64("delta.sub.b_lo", b & ~high);
        fact_x64("delta.sub.diff", (a | high) - (b & ~high));
        fact_x64("delta.sub.fix", (a ^ ~b) & high);
        fact_x64("delta.sub.r", r);
        fact_x64("delta.sub.plain", a - b);
        put_unaligned_le64(b, w);
        put_unaligned_le64(r, w + 8);
        add8(w + 8, 8);
        fact_x64("delta.add.a_lo", r & low);
        fact_x64("delta.add.b_lo", b & low);
        fact_x64("delta.add.sum", (r & low) + (b & low));
        fact_x64("delta.add.fix", (r ^ b) & ~low);
        fact_x64("delta.add.r", get_unaligned_le64(w + 8));
        fact_x64("delta.add.plain", r + b);
        if (get_unaligned_le64(w + 8) != a)
            fail("add8() does not undo sub_bytes()");
    }
    for (k = 1; k <= 9; k++)
        fact_num(key("delta.last.%u", k), seqlz_delta_last(k));

    /* the page of pointers through compress_own()'s steps, the records looked at before seqlz_delta_literals() */
    pointer_page(page);
    encoder_init(&e, dst, sizeof(dst), page + SEQLZ_BLOCK_SIZE);
    e.rec = true;
    seqlz_match_block(st.table, page, true, encode_emit_rec, &e);
    if (e.p == e.lit || !e.rec)
        fail("the page of pointers does not fit with its records");
    with_rec = (unsigned int)(e.lit - (dst + SEQLZ_HEADER));
    for (p = dst + SEQLZ_HEADER; p < e.lit; p += LIT_REC + get_unaligned_le16(p + 2)) {
        if (recs < 3) {
            rec_pos[recs] = get_unaligned_le16(p);
            rec_ll[recs] = get_unaligned_le16(p + 2);
            rec_last[recs] = get_unaligned_le16(p + 4);
            rec_dist[recs] = seqlz_delta_last(rec_last[recs]);
            fact_num(key("delta.ptr.rec.%u.pos", recs), get_unaligned_le16(p));
            fact_num(key("delta.ptr.rec.%u.ll", recs), get_unaligned_le16(p + 2));
            fact_num(key("delta.ptr.rec.%u.last", recs), get_unaligned_le16(p + 4));
            fact_num(key("delta.ptr.rec.%u.dist", recs), seqlz_delta_last(get_unaligned_le16(p + 4)));
            fact_hex(key("delta.ptr.rec.%u.hex", recs), p, LIT_REC);
        }
        last8 += recs > 0 && get_unaligned_le16(p + 4) == 8U;
        recs++;
    }
    fact_num("delta.ptr.recs", recs);
    fact_num("delta.ptr.recs_last8", last8);
    fact_num("delta.ptr.with_rec", with_rec);

    /* the prices as seqlz_delta_literals() takes them, here as plain sums of code lengths */
    {
        unsigned int sum[4][SEQLZ_LIT_SETS] = {{0}}, j = 0, sampled = 0, ll, pos, last;

        for (p = dst + SEQLZ_HEADER; p < e.lit; p += LIT_REC + ll) {
            pos = get_unaligned_le16(p);
            ll = get_unaligned_le16(p + 2);
            last = seqlz_delta_last(get_unaligned_le16(p + 4));
            for (i = j; i < ll; i += DELTA_STEP) {
                unsigned int at = pos + i, b = page[at];
                unsigned int ref[4] = {
                    0, at >= 4 ? page[at - 4] : 0U, at >= 8 ? page[at - 8] : 0U, at >= last ? page[at - last] : 0U};

                for (x = 0; x < 4; x++)
                    for (k = 0; k < SEQLZ_LIT_SETS; k++)
                        sum[x][k] += lit_len(t, k, (u8)(b - ref[x]));
                sampled++;
            }
            j = i - ll;
        }
        fact_num("delta.ptr.sampled", sampled);
        for (x = 0; x < 4; x++) {
            price[x] = ~0U;
            for (k = 0; k < SEQLZ_LIT_SETS; k++) {
                unsigned int b = sum[x][k] + (x ? 8U / DELTA_STEP : 0U);

                if (b < price[x]) {
                    price[x] = b;
                    table[x] = k;
                }
            }
            fact_num(key("delta.ptr.price.%u", x), price[x]);
            fact_num(key("delta.ptr.table.%u", x), table[x]);
            if (price[x] < best) {
                best = price[x];
                pick = x;
            }
        }
    }
    x = seqlz_delta_literals(dst, &e.lit, page, st.literals);
    if (x != pick)
        fail("seqlz_delta_literals() picks another delta than the prices");
    fact_num("delta.ptr.pick", pick);
    memcpy(deltas, st.literals, SEQLZ_BLOCK_SIZE);
    n_lit = (unsigned int)(e.lit - (dst + SEQLZ_HEADER));
    fact_num("delta.ptr.n_lit", n_lit);
    if (with_rec != n_lit + LIT_REC * recs)
        fail("the records are not LIT_REC bytes each");
    fact_hex("delta.ptr.plain", dst + SEQLZ_HEADER, 12);
    fact_hex("delta.ptr.deltas", st.literals, 12);
    for (i = 0; i < n_lit; i++) {
        hist[st.literals[i]]++;
        plain_hist[dst[SEQLZ_HEADER + i]]++;
    }
    for (i = 0; i < 256; i++) {
        distinct += hist[i] != 0;
        distinct_plain += plain_hist[i] != 0;
    }
    /*
     * the same literals XORed with the same bytes instead, the format before the deltas: the byte a
     * delta is to is the literal minus its delta, 0 for a literal kept as it is
     */
    {
        unsigned int xor_hist[256] = {0}, xor_distinct = 0;

        for (i = 0; i < n_lit; i++) {
            u8 b = dst[SEQLZ_HEADER + i];

            xor_hist[b ^ (u8)(b - st.literals[i])]++;
        }
        for (i = 0; i < 256; i++)
            xor_distinct += xor_hist[i] != 0;
        fact_num("delta.ptr.xor.distinct", xor_distinct);
        fact_num("delta.ptr.xor.h.10", xor_hist[0x10]);
        fact_num("delta.ptr.xor.h.30", xor_hist[0x30]);
        fact_num("delta.ptr.xor.h.70", xor_hist[0x70]);
        fact_num("delta.ptr.xor.h.f0", xor_hist[0xf0]);
    }
    fact_num("delta.ptr.distinct.plain", distinct_plain);
    fact_num("delta.ptr.distinct.deltas", distinct);
    fact_num("delta.ptr.h.10", hist[0x10]);
    /* the whole page at levels 2 and 3, and level 3 without deltas */
    len = fact_compress_level(page, d2, 2);
    fact_num("delta.ptr.l2.len", len);
    len = fact_compress_level(page, d2, SEQLZ_LEVEL_OWN);
    fact_num("delta.ptr.l3.len", len);
    if (!(d2[1] & 0x80U) || !(d2[2] >> SEQLZ_LIT_DELTA_AT))
        fail("level 3 does not store the page of pointers' literals as deltas");
    snprintf(hex, sizeof hex, "0x%02x", d2[2]);
    fact_str("delta.ptr.l3.t", hex);
    fact_num("delta.ptr.l3.code", d2[2] & SEQLZ_LIT_CODE_MASK);
    fact_num("delta.ptr.l3.w", SEQLZ_SIZE_BITS_MIN + (d2[2] >> SEQLZ_LIT_WIDTH_AT & 7U));
    fact_num("delta.ptr.l3.dt_at", SEQLZ_LIT_HEADER(0));
    fact_num("delta.ptr.l3.dt", d2[SEQLZ_LIT_HEADER(0)]);
    fact_hex("delta.ptr.l3.head", d2, SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN + (d2[2] >> SEQLZ_LIT_WIDTH_AT & 7U)) + 1);
    fact_num("delta.ptr.l3.n", get_unaligned_le16(d2) & (SEQLZ_LIT_CODED - 1U));
    if (seqlz_decode(d2, len, out, scratch) || memcmp(out, page, SEQLZ_BLOCK_SIZE))
        fail("the page of pointers does not decode to itself");
    {
        /* level 3 without deltas: compress_own() with the records off, then seqlz_code_own() with delta 0 */
        unsigned int raw, n0;

        encoder_init(&e, d2, sizeof(d2), page + SEQLZ_BLOCK_SIZE);
        seqlz_match_block(st.table, page, true, encode_emit, &e);
        raw = encoder_finish(&e, d2, sizeof(d2));
        n0 = seqlz_code_own(d2, raw, &st, 0);
        fact_num("delta.ptr.l3plain.len", n0);
    }

    /*
     * undelta_fast() on the second sequence, as the fast path calls it: the page up to the sequence,
     * then 16 bytes copied from the deltas at d
     */
    {
        u8 buf[SEQLZ_BLOCK_SIZE];
        const unsigned int dist = pick == SEQLZ_DELTA_4 ? 4U : pick == SEQLZ_DELTA_8 ? 8U : rec_dist[1];

        n = rec_pos[1];
        memcpy(buf, page, n);
        memcpy(buf + n, deltas + rec_ll[0], 16);
        fact_hex("delta.ptr.fast.before", buf + n, 8);
        fact_hex("delta.ptr.fast.ref", buf + n - dist, 8);
        undelta_fast(pick, buf + n, buf, rec_ll[1], rec_last[1]);
        fact_hex("delta.ptr.fast.after", buf + n, 8);
        if (memcmp(buf + n, page + n, rec_ll[1]))
            fail("undelta_fast() does not give the page's bytes back");
    }
    facts_end();
    return 0;
}
