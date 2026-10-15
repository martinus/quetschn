// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * accum.c: the encoder's accumulator for the first sequences of chapter 16's page. The page is
 * compressed as compress_page() does; for the first SEQS sequences a copy of the encoder does the
 * puts and flushes of encode_emit() one at a time with the helpers of seqlz_compress.c, and prints
 * each: the value put and its parts, acc and cnt before and after, the word a flush stores and
 * where. Fails if the copy does not end where encode_emit() ends: the same acc, cnt, p and bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pages.h"
#include "seqlz_compress.c"

#define SEQS 4
#define CAP (2 * SEQLZ_PAGE)

struct drv {
    struct seqlz_encoder e; /* the copy, writing into shadow */
    unsigned int n, ops;
};

static u8 shadow[CAP], real_dst[CAP];
static struct seqlz_encoder real;

static void op_head(struct drv* D, const char* kind) {
    printf("%s{\"seq\":%u,\"op\":\"%s\",\"cnt0\":%u,\"p0\":%ld",
           D->ops++ ? "," : "",
           D->n,
           kind,
           D->e.cnt,
           (long)(shadow + CAP - D->e.p));
}

/* after the op: acc, cnt, p counted from the end of dst, and the last 24 bytes of dst */
static void op_tail(struct drv* D) {
    unsigned int i;

    printf(",\"acc1\":\"%016llx\",\"cnt1\":%u,\"p1\":%ld,\"tail\":[",
           (unsigned long long)D->e.acc,
           D->e.cnt,
           (long)(shadow + CAP - D->e.p));
    for (i = CAP - 24; i < CAP; i++)
        printf("%s%u", i > CAP - 24 ? "," : "", shadow[i]);
    printf("]}");
}

/* the parts of a put, from its top bits down */
struct part {
    const char* name;
    unsigned int bits, v;
};

static void put(struct drv* D, const char* what, u64 v, unsigned int n, const struct part* pt, unsigned int np) {
    unsigned int i, sum = 0;
    u64 check = 0;

    for (i = 0; i < np; i++) {
        sum += pt[i].bits;
        check = check << pt[i].bits | pt[i].v;
    }
    if (sum != n || check != v) {
        fprintf(stderr, "accum.c: sequence %u: the parts of %s are not the value put\n", D->n, what);
        exit(1);
    }
    op_head(D, "put");
    printf(",\"what\":\"%s\",\"v\":\"%llx\",\"n\":%u,\"parts\":[", what, (unsigned long long)v, n);
    for (i = 0; i < np; i++)
        printf("%s[\"%s\",%u,%u]", i ? "," : "", pt[i].name, pt[i].bits, pt[i].v);
    printf("]");
    enc_put(&D->e, v, n);
    op_tail(D);
}

static void flush(struct drv* D) {
    struct seqlz_encoder* e = &D->e;

    op_head(D, "flush");
    printf(",\"word\":\"%016llx\"", (unsigned long long)(e->acc << ((64U - e->cnt) & 63U)));
    enc_flush(e);
    op_tail(D);
}

/* a length value as put_len_value() puts it: the symbol's code, then its extra bits */
static void len_value(struct drv* D, const struct value_table* t, const char* what, unsigned int v) {
    unsigned int extra, s = seqlz_len_symbol(v, &extra) & (ENC_LEN_SYMBOLS - 1U);
    u32 entry = t->enc[s];
    struct part pt[2] = {{"code", entry >> 16 & 15U, entry & 0xffffU}, {"extra", extra, v & ((1U << extra) - 1U)}};
    u64 val = (u64)(entry & 0xffffU) << extra | (v & ((1U << extra) - 1U));

    printf("%s{\"seq\":%u,\"op\":\"value\",\"what\":\"%s\",\"value\":%u,\"sym\":%u}", D->ops++ ? "," : "", D->n, what, v, s);
    put(D, what, val, pt[0].bits + extra, pt, extra ? 2 : 1);
}

/* encode_emit()'s puts and flushes for a sequence with a match, one at a time, on the copy */
static void traced(struct drv* D, unsigned int ll, unsigned int ml, unsigned int off) {
    struct seqlz_encoder* e = &D->e;
    const struct seqlz_tables* t = e->t;
    unsigned int tlen, raw_bits, cls, tok, esc, offv, np = 0;
    struct part pt[3];
    u32 code;

    cls = seqlz_off_class(off, e->last, &raw_bits);
    tok = seqlz_token(ll, ml, cls);
    esc = t->token.enc[tok] == 0;
    code = token_code(t, tok, &tlen);
    offv = (off >> SEQLZ_OFF_SHIFT(cls)) & (0U - (cls != 0));
    printf("%s{\"seq\":%u,\"op\":\"seq\",\"ll\":%u,\"ml\":%u,\"off\":%u,\"cls\":%u,\"tok\":%u,"
           "\"esc\":%u,\"tlen\":%u,\"raw\":%u,\"shift\":%u}",
           D->ops++ ? "," : "",
           D->n,
           ll,
           ml,
           off,
           cls,
           tok,
           esc,
           tlen,
           raw_bits,
           SEQLZ_OFF_SHIFT(cls));
    if (esc) {
        pt[np++] = (struct part){"escape", tlen - SEQLZ_ESCAPE_BITS, code >> SEQLZ_ESCAPE_BITS};
        pt[np++] = (struct part){"token", SEQLZ_ESCAPE_BITS, tok};
    } else {
        pt[np++] = (struct part){"token", tlen, code};
    }
    if (raw_bits)
        pt[np++] = (struct part){"offset", raw_bits, offv};
    put(D, "token", (u64)code << raw_bits | offv, tlen + raw_bits, pt, np);
    if (ll >= SEQLZ_LL_CAP) {
        len_value(D, &t->ll, "ll", ll - SEQLZ_LL_CAP);
        flush(D);
    }
    if (ml - 4 >= SEQLZ_ML_CAP)
        len_value(D, &t->ml, "ml", ml - 4 - SEQLZ_ML_CAP);
    flush(D);
    e->last = off;
}

static void drv_emit(void* ctx, const u8* in, unsigned int ll, unsigned int ml, unsigned int off) {
    struct drv* D = ctx;
    long k;

    encode_emit(&real, in, ll, ml, off);
    if (D->n < SEQS) {
        if (!ml)
            exit(1);
        traced(D, ll, ml, off);
        k = real_dst + CAP - real.p;
        if (D->e.acc != real.acc || D->e.cnt != real.cnt || D->e.last != real.last || shadow + CAP - D->e.p != k ||
            memcmp(real.p, D->e.p, (size_t)k)) {
            fprintf(stderr, "accum.c: sequence %u: the copy is not where encode_emit() is\n", D->n);
            exit(1);
        }
    }
    D->n++;
}

int main(void) {
    static u8 page[SEQLZ_PAGE], ref[CAP];
    static struct seqlz_state st;
    static struct drv D;
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    unsigned int len, ref_len;

    if (!t || seqlz_tables_init(t, &seqlz_default_own))
        return 1;
    slab_page(page);
    encoder_init(&real, t, real_dst, CAP, page + SEQLZ_PAGE);
    encoder_init(&D.e, t, shadow, CAP, page + SEQLZ_PAGE);
    printf("{\"ops\":[");
    seqlz_match_page(st.table, page, false, drv_emit, &D);
    len = encoder_finish(&real, real_dst, CAP);
    ref_len = seqlz_compress(t, &st, page, ref, CAP, 1);
    if (!len || len != ref_len || memcmp(real_dst, ref, len)) {
        fprintf(stderr, "accum.c: the page is not what seqlz_compress() writes\n");
        return 1;
    }
    printf("]}\n");
    return 0;
}
