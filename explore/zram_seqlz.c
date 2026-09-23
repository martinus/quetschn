// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz (explore/seqlz.h) as a zram backend would call it. The matches come from the kernel's lz4
 * ("seqlz") or lz4hc at level 3 ("seqlz-hc"): compress with it into a per-CPU buffer, split that into
 * sequences, code them with seqlz. zram's dictionary parameter, if it is exactly a struct
 * seqlz_lengths (577 bytes), carries other code lengths instead of the ones compiled in.
 */
#include <linux/lz4.h>

#include "seqlz.h"
#include "zram_codec.h"

#define MAX_SEQUENCES (SEQLZ_PAGE / 4U + 2U)

struct seqlz_ctx {
    void* lz4_mem;
    unsigned char* lz4_out;
    struct seqlz_sequence* seq;
    unsigned char* literals;
};

static int setup(struct quetschn_params* p, const struct seqlz_lengths* built_in) {
    const struct seqlz_lengths* lengths = built_in;
    struct seqlz_tables* t;

    if (p->page_size != SEQLZ_PAGE)
        return -1;
    /* a dictionary of another size is not for seqlz; ignored like lzo ignores dictionaries */
    if (p->dict_size == sizeof(struct seqlz_lengths))
        lengths = p->dict;
    t = quetschn_zalloc(seqlz_tables_size(), &p->allocated);
    if (!t)
        return -1;
    if (seqlz_tables_init(t, lengths)) {
        quetschn_free(t, &p->allocated);
        return -1;
    }
    p->drv_data = t;
    return 0;
}

static int setup_lz4(struct quetschn_params* p) {
    if (p->level == QUETSCHN_LEVEL_DEFAULT)
        p->level = 1;
    return setup(p, &seqlz_default_lz4);
}

static int setup_lz4hc(struct quetschn_params* p) {
    if (p->level == QUETSCHN_LEVEL_DEFAULT)
        p->level = 3;
    return setup(p, &seqlz_default_lz4hc);
}

static void release(struct quetschn_params* p) {
    quetschn_free(p->drv_data, &p->allocated);
    p->drv_data = NULL;
}

static void destroy(struct quetschn_stream* s) {
    struct seqlz_ctx* ctx = s->context;

    if (!ctx)
        return;
    quetschn_free(ctx->lz4_mem, &s->allocated);
    quetschn_free(ctx->lz4_out, &s->allocated);
    quetschn_free(ctx->seq, &s->allocated);
    quetschn_free(ctx->literals, &s->allocated);
    quetschn_free(ctx, &s->allocated);
    s->context = NULL;
}

static int create(struct quetschn_stream* s, unsigned int workspace) {
    struct seqlz_ctx* ctx = quetschn_zalloc(sizeof(*ctx), &s->allocated);

    if (!ctx)
        return -1;
    s->context = ctx;
    ctx->lz4_mem = quetschn_zalloc(workspace, &s->allocated);
    ctx->lz4_out = quetschn_zalloc(2 * SEQLZ_PAGE, &s->allocated);
    ctx->seq = quetschn_zalloc(MAX_SEQUENCES * sizeof(*ctx->seq), &s->allocated);
    ctx->literals = quetschn_zalloc(SEQLZ_PAGE, &s->allocated);
    if (!ctx->lz4_mem || !ctx->lz4_out || !ctx->seq || !ctx->literals) {
        destroy(s);
        return -1;
    }
    return 0;
}

static int create_lz4(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    return create(s, LZ4_MEM_COMPRESS);
}

static int create_lz4hc(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    return create(s, LZ4HC_MEM_COMPRESS);
}

/* Splits an lz4 block into sequences and literals; bench/lz_analysis.cpp does the same in C++. */
static int split(const unsigned char* p, unsigned int n, struct seqlz_ctx* ctx, unsigned int* n_seq, unsigned int* n_lit) {
    unsigned int i = 0, k = 0, lits = 0;

    while (i < n) {
        unsigned int token = p[i++], ll = token >> 4, ml, b;

        if (ll == 15) {
            do {
                if (i >= n)
                    return -1;
                b = p[i++];
                ll += b;
            } while (b == 255);
        }
        if (ll > n - i || ll > SEQLZ_PAGE - lits || k == MAX_SEQUENCES)
            return -1;
        __builtin_memcpy(ctx->literals + lits, p + i, ll);
        lits += ll;
        i += ll;
        ctx->seq[k].literals = (unsigned short)ll;
        ctx->seq[k].match = 0;
        ctx->seq[k].offset = 0;
        if (i == n) {
            k++;
            break;
        }
        if (n - i < 2)
            return -1;
        ctx->seq[k].offset = (unsigned short)(p[i] | (p[i + 1] << 8));
        i += 2;
        ml = (token & 15U) + 4U;
        if ((token & 15U) == 15) {
            do {
                if (i >= n)
                    return -1;
                b = p[i++];
                ml += b;
            } while (b == 255);
        }
        ctx->seq[k].match = (unsigned short)ml;
        k++;
    }
    *n_seq = k;
    *n_lit = lits;
    return 0;
}

static int compress(struct quetschn_params* p,
                    struct quetschn_stream* s,
                    const void* src,
                    unsigned int src_len,
                    void* dst,
                    unsigned int* dst_len,
                    int hc) {
    struct seqlz_ctx* ctx = s->context;
    unsigned int n_seq, n_lit, len;
    int ret;

    if (src_len != SEQLZ_PAGE)
        return -1;
    if (hc)
        ret = LZ4_compress_HC(src, (char*)ctx->lz4_out, (int)src_len, 2 * SEQLZ_PAGE, p->level, ctx->lz4_mem);
    else
        ret = LZ4_compress_fast(src, (char*)ctx->lz4_out, (int)src_len, 2 * SEQLZ_PAGE, p->level, ctx->lz4_mem);
    if (ret <= 0 || split(ctx->lz4_out, (unsigned int)ret, ctx, &n_seq, &n_lit))
        return -1;
    len = seqlz_encode(p->drv_data, ctx->seq, n_seq, ctx->literals, n_lit, dst, *dst_len);
    if (!len)
        return -1;
    *dst_len = len;
    return 0;
}

static int compress_lz4(struct quetschn_params* p,
                        struct quetschn_stream* s,
                        const void* src,
                        unsigned int src_len,
                        void* dst,
                        unsigned int* dst_len) {
    return compress(p, s, src, src_len, dst, dst_len, 0);
}

static int compress_lz4hc(struct quetschn_params* p,
                          struct quetschn_stream* s,
                          const void* src,
                          unsigned int src_len,
                          void* dst,
                          unsigned int* dst_len) {
    return compress(p, s, src, src_len, dst, dst_len, 1);
}

static int decompress(struct quetschn_params* p,
                      struct quetschn_stream* s,
                      const void* src,
                      unsigned int src_len,
                      void* dst,
                      unsigned int* dst_len) {
    (void)s;
    if (*dst_len < SEQLZ_PAGE || seqlz_decode(p->drv_data, src, src_len, dst))
        return -1;
    *dst_len = SEQLZ_PAGE;
    return 0;
}

const struct quetschn_codec quetschn_codec_seqlz = {
    "seqlz",
    setup_lz4,
    release,
    create_lz4,
    destroy,
    compress_lz4,
    decompress,
};

const struct quetschn_codec quetschn_codec_seqlz_hc = {
    "seqlz-hc",
    setup_lz4hc,
    release,
    create_lz4hc,
    destroy,
    compress_lz4hc,
    decompress,
};
