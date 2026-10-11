// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * ends.c: chapter 16's page compressed as compress_page() does, raw literals, into a dst of two
 * pages, into the smallest dst it fits, and into one byte less. A callback around encode_emit()
 * records after each sequence the two ends (lit and p), the accumulator's count, the bits the
 * sequence added and what they were, and which bytes the sequence wrote outside the page's bytes:
 * the literal copies past lit and the flush's store below p, found by running the sequence on two
 * copies of dst whose free room holds 0x00 and 0xff. Then what encoder_finish() does. Fails if
 * the page is not what seqlz_compress() writes, or if the bits do not add up.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pages.h"
#include "seqlz_compress.c"

#define CAP_MAX (2 * SEQLZ_PAGE)

/* what one sequence did; positions as offsets in dst */
struct rec {
    unsigned int ll, ml, off, cls, tok, tlen, esc, obits, llbits, mlbits, flushes;
    long lit, p, cnt, room, wr_hi, wr_lo, bits;
};

struct run {
    struct seqlz_encoder e;
    u8* d;
    unsigned int cap, n, fail; /* fail: 1 + the sequence whose room check failed, or 0 */
    struct rec r[SEQLZ_MAX_SEQUENCES];
};

static u8 probe_a[CAP_MAX], probe_b[CAP_MAX];
/* the page as seqlz_compress() writes it: its bitstream, byte 0 first */
static const u8* ref_bs;
static unsigned int ref_bs_len;

static long bits_out(const struct run* R) {
    return (long)(R->d + R->cap - R->e.p) * 8 + R->e.cnt;
}

/* the bits of one put of a length value, as put_len_value() puts them */
static unsigned int len_bits(const struct value_table* vt, unsigned int v) {
    unsigned int extra, s = seqlz_len_symbol(v, &extra) & (ENC_LEN_SYMBOLS - 1U);

    return (vt->enc[s] >> 16 & 15U) + extra;
}

/* runs encode_emit() on a copy of the encoder and of dst whose free room holds fill */
static void probe(const struct run* R, u8* buf, u8 fill, const u8* in, unsigned int ll, unsigned int ml, unsigned int off) {
    struct seqlz_encoder c = R->e;

    memcpy(buf, R->d, R->cap);
    c.p = buf + (R->e.p - R->d);
    c.lit = buf + (R->e.lit - R->d);
    if (c.p > c.lit)
        memset(c.lit, fill, (size_t)(c.p - c.lit));
    encode_emit(&c, in, ll, ml, off);
}

static void rec_emit(void* ctx, const u8* in, unsigned int ll, unsigned int ml, unsigned int off) {
    struct run* R = ctx;
    struct rec* r = &R->r[R->n];
    const struct seqlz_tables* t = R->e.t;
    long before = bits_out(R), lo = R->e.lit - R->d, hi = R->e.p - R->d, i;
    unsigned int raw = 0;

    if (R->fail) {
        encode_emit(&R->e, in, ll, ml, off);
        return;
    }
    memset(r, 0, sizeof(*r));
    r->ll = ll;
    r->ml = ml;
    r->off = ml ? off : 0;
    r->room = R->e.p - R->e.lit;
    r->cls = ml ? seqlz_off_class(off, R->e.last, &raw) : 0;
    r->tok = seqlz_token(ll, ml, r->cls);
    r->esc = t->token.enc[r->tok] == 0;
    token_code(t, r->tok, &r->tlen);
    r->obits = raw;
    r->llbits = ll >= SEQLZ_LL_CAP ? len_bits(&t->ll, ll - SEQLZ_LL_CAP) : 0;
    r->mlbits = ml && ml - 4 >= SEQLZ_ML_CAP ? len_bits(&t->ml, ml - 4 - SEQLZ_ML_CAP) : 0;
    r->flushes = ml && ll >= SEQLZ_LL_CAP ? 2 : 1;
    /* which bytes of the free room the sequence writes: a byte that differs from one fill */
    probe(R, probe_a, 0x00, in, ll, ml, off);
    probe(R, probe_b, 0xff, in, ll, ml, off);
    /* the literals' side writes below the middle of the room after them, the bits' side above */
    r->wr_hi = lo;
    r->wr_lo = hi;
    for (i = lo; i < hi; i++)
        if (probe_a[i] != 0x00 || probe_b[i] != 0xff) {
            if (i < (lo + (long)ll + hi) / 2)
                r->wr_hi = i + 1;
            else if (i < r->wr_lo)
                r->wr_lo = i;
        }
    encode_emit(&R->e, in, ll, ml, off);
    if (R->e.p == R->e.lit) {
        R->fail = R->n + 1;
        r->lit = R->e.lit - R->d;
        r->p = R->e.p - R->d;
        R->n++;
        return;
    }
    r->lit = R->e.lit - R->d;
    r->p = R->e.p - R->d;
    r->cnt = R->e.cnt;
    r->bits = bits_out(R) - before;
    /*
     * From p to the end of dst are the bitstream's final bytes, byte 0 last; the byte below p
     * holds the top cnt bits of the next one and zeros
     */
    for (i = 0; i < (long)R->cap - r->p; i++)
        if (i >= (long)ref_bs_len || R->d[R->cap - 1 - i] != ref_bs[i]) {
            fprintf(stderr, "ends.c: sequence %u: byte %ld of the bitstream\n", R->n, i);
            exit(1);
        }
    if (r->cnt && R->d[r->p - 1] != (ref_bs[i] & (0xff00U >> r->cnt & 0xffU))) {
        fprintf(stderr, "ends.c: sequence %u: the byte below p\n", R->n);
        exit(1);
    }
    if (r->bits != (long)(r->tlen + r->obits + r->llbits + r->mlbits) || (r->esc && r->tlen <= SEQLZ_ESCAPE_BITS)) {
        fprintf(stderr,
                "ends.c: sequence %u: %ld bits, the parts say %u\n",
                R->n,
                r->bits,
                r->tlen + r->obits + r->llbits + r->mlbits);
        exit(1);
    }
    R->n++;
}

/* compress_page() up to encoder_finish(), with rec_emit() around encode_emit() */
static void
run(struct run* R, const struct seqlz_tables* t, struct seqlz_state* st, const u8* page, u8* dst, unsigned int cap) {
    memset(R, 0, sizeof(*R));
    R->d = dst;
    R->cap = cap;
    encoder_init(&R->e, t, dst, cap, page + SEQLZ_PAGE);
    seqlz_match_page(st->table, page, rec_emit, R);
}

static void print_recs(const struct run* R) {
    unsigned int i;

    printf("\"seq\":[");
    for (i = 0; i < R->n; i++) {
        const struct rec* r = &R->r[i];

        printf("%s[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%ld,%ld,%ld,%ld,%ld,%ld,%ld]",
               i ? "," : "",
               r->ll,
               r->ml,
               r->off,
               r->cls,
               r->tok,
               r->tlen,
               r->esc,
               r->obits,
               r->llbits,
               r->mlbits,
               r->flushes,
               r->lit,
               r->p,
               r->cnt,
               r->room,
               r->wr_hi,
               r->wr_lo,
               r->bits);
    }
    printf("]");
}

int main(void) {
    static u8 page[SEQLZ_PAGE], ref[CAP_MAX], dst[CAP_MAX];
    static struct seqlz_state st;
    static struct run R, R0;
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    unsigned int ref_len, len, n_lit, bytes, cap, small, i, k;
    long p_seq, cnt_seq;
    unsigned int cs[3];

    if (!t || seqlz_tables_init(t, &seqlz_default_own))
        return 1;
    slab_page(page);
    ref_len = seqlz_compress(t, &st, page, ref, CAP_MAX, 0);
    if (!ref_len)
        return 1;
    ref_bs = ref + SEQLZ_HEADER + get_unaligned_le16(ref);
    ref_bs_len = (unsigned int)(ref + ref_len - ref_bs);
    /* the smallest dst the page fits into */
    for (small = SEQLZ_HEADER + ENC_ROOM; small < CAP_MAX; small++)
        if (seqlz_compress(t, &st, page, dst, small, 0))
            break;
    cs[0] = CAP_MAX;
    cs[1] = small;
    cs[2] = small - 1;

    printf("{\"room\":%u,\"header\":%u,", ENC_ROOM, SEQLZ_HEADER);
    printf("\"runs\":[");
    for (k = 0; k < 3; k++) {
        cap = cs[k];
        memset(dst, 0, sizeof(dst));
        run(&R, t, &st, page, dst, cap);
        /* what encoder_finish() starts from */
        p_seq = R.e.p - dst;
        cnt_seq = R.e.cnt;
        n_lit = (unsigned int)(R.e.lit - (dst + SEQLZ_HEADER));
        len = encoder_finish(&R.e, dst, cap);
        if (k < 2 && (len != ref_len || memcmp(dst, ref, len))) {
            fprintf(stderr, "ends.c: dst of %u: the page is not what seqlz_compress() writes\n", cap);
            return 1;
        }
        if (k == 2 && (len != 0 || !R.fail || seqlz_compress(t, &st, page, probe_a, cap, 0))) {
            fprintf(stderr, "ends.c: dst of %u bytes should fail\n", cap);
            return 1;
        }
        if (k == 0) {
            R0 = R;
        } else {
            /* the same sequences, the same ends counted from lit and from the end of dst */
            for (i = 0; i < R.n - (R.fail != 0); i++) {
                const struct rec *a = &R0.r[i], *b = &R.r[i];

                if (a->lit != b->lit || CAP_MAX - a->p != cap - b->p || a->bits != b->bits ||
                    a->wr_hi - a->lit != b->wr_hi - b->lit || CAP_MAX - a->wr_lo != cap - b->wr_lo) {
                    fprintf(stderr, "ends.c: dst of %u differs at sequence %u\n", cap, i);
                    return 1;
                }
            }
        }
        bytes = len ? len - SEQLZ_HEADER - n_lit : 0;
        printf("%s{\"cap\":%u,\"n\":%u,\"fail\":%u,\"p_seq\":%ld,\"cnt_seq\":%ld,\"n_lit\":%u,"
               "\"bytes\":%u,\"len\":%u,\"in_place\":%d,",
               k ? "," : "",
               cap,
               R.n,
               R.fail,
               p_seq,
               cnt_seq,
               n_lit,
               bytes,
               len,
               len && (unsigned int)(p_seq - (cnt_seq > 0) - (SEQLZ_HEADER + n_lit)) < bytes);
        if (k == 2) {
            const struct rec* r = &R.r[R.fail - 1];

            printf("\"fail_room\":%ld,", r->room);
        }
        if (k == 0)
            print_recs(&R);
        else
            printf("\"seq\":null");
        printf("}");
    }
    printf("],");
    /* the literals and the bitstream, byte 0 first, as the page holds them */
    n_lit = get_unaligned_le16(ref);
    array("lits", ref + SEQLZ_HEADER, n_lit);
    printf(",");
    array("bs", ref + SEQLZ_HEADER + n_lit, ref_len - SEQLZ_HEADER - n_lit);
    printf(",\"len\":%u}\n", ref_len);
    return 0;
}
