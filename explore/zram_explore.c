// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * Candidate designs from docs/explored-designs.md as zram backends would call them. shuffle-lz4 needs
 * the kernel's lz4 and a per-CPU page buffer for the shuffled page; bdelta needs nothing.
 */
#include <linux/lz4.h>

#include "bdelta.h"
#include "shuffle.h"
#include "zram_codec.h"

#define PAGE 4096U

struct shuffle_ctx {
    void* lz4_mem;
    void* page; /* the shuffled page, compressed or decompressed */
};

static int page_only_setup_params(struct quetschn_params* p) {
    if (p->page_size != PAGE)
        return -1;
    if (p->level == QUETSCHN_LEVEL_DEFAULT)
        p->level = LZ4_ACCELERATION_DEFAULT;
    return p->level < LZ4_ACCELERATION_DEFAULT ? -1 : 0;
}

static void nothing_release_params(struct quetschn_params* p) {
    (void)p;
}

static void shuffle_destroy(struct quetschn_stream* s) {
    struct shuffle_ctx* ctx = s->context;

    if (!ctx)
        return;
    quetschn_free(ctx->lz4_mem, &s->allocated);
    quetschn_free(ctx->page, &s->allocated);
    quetschn_free(ctx, &s->allocated);
    s->context = (void*)0;
}

static int shuffle_create(struct quetschn_params* p, struct quetschn_stream* s) {
    struct shuffle_ctx* ctx = quetschn_zalloc(sizeof(*ctx), &s->allocated);

    (void)p;
    if (!ctx)
        return -1;
    s->context = ctx;
    ctx->lz4_mem = quetschn_zalloc(LZ4_MEM_COMPRESS, &s->allocated);
    ctx->page = quetschn_zalloc(PAGE, &s->allocated);
    if (!ctx->lz4_mem || !ctx->page) {
        shuffle_destroy(s);
        return -1;
    }
    return 0;
}

static int shuffle_compress(struct quetschn_params* p,
                            struct quetschn_stream* s,
                            const void* src,
                            unsigned int src_len,
                            void* dst,
                            unsigned int* dst_len) {
    struct shuffle_ctx* ctx = s->context;
    int ret;

    if (src_len != PAGE)
        return -1;
    shuffle8(src, ctx->page, PAGE);
    ret = LZ4_compress_fast(ctx->page, dst, (int)PAGE, (int)*dst_len, p->level, ctx->lz4_mem);
    if (!ret)
        return -1;
    *dst_len = (unsigned int)ret;
    return 0;
}

static int shuffle_decompress(struct quetschn_params* p,
                              struct quetschn_stream* s,
                              const void* src,
                              unsigned int src_len,
                              void* dst,
                              unsigned int* dst_len) {
    struct shuffle_ctx* ctx = s->context;

    (void)p;
    if (*dst_len < PAGE || LZ4_decompress_safe(src, ctx->page, (int)src_len, (int)PAGE) != (int)PAGE)
        return -1;
    unshuffle8(ctx->page, dst, PAGE);
    *dst_len = PAGE;
    return 0;
}

static int nothing_create(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    (void)s;
    return 0;
}

static void nothing_destroy(struct quetschn_stream* s) {
    (void)s;
}

static int bdelta_zcompress(struct quetschn_params* p,
                            struct quetschn_stream* s,
                            const void* src,
                            unsigned int src_len,
                            void* dst,
                            unsigned int* dst_len) {
    unsigned int len;

    (void)p;
    (void)s;
    if (src_len != PAGE)
        return -1;
    len = bdelta_compress(src, dst, *dst_len);
    if (!len)
        return -1;
    *dst_len = len;
    return 0;
}

static int bdelta_zdecompress(struct quetschn_params* p,
                              struct quetschn_stream* s,
                              const void* src,
                              unsigned int src_len,
                              void* dst,
                              unsigned int* dst_len) {
    (void)p;
    (void)s;
    if (*dst_len < PAGE || bdelta_decompress(src, src_len, dst))
        return -1;
    *dst_len = PAGE;
    return 0;
}

const struct quetschn_codec quetschn_codec_shuffle_lz4 = {
    "shuffle-lz4",
    page_only_setup_params,
    nothing_release_params,
    shuffle_create,
    shuffle_destroy,
    shuffle_compress,
    shuffle_decompress,
};

const struct quetschn_codec quetschn_codec_bdelta = {
    "bdelta",
    page_only_setup_params,
    nothing_release_params,
    nothing_create,
    nothing_destroy,
    bdelta_zcompress,
    bdelta_zdecompress,
};
