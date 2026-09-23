// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * memlz (https://github.com/rrrlasse/memlz, MIT) as a zram backend would call it, for
 * docs/explored-designs.md. Built only with QUETSCHN_MEMLZ_DIR, from a checkout; not copied here.
 *
 * memlz's state is two hash tables, 768 KiB, and the decoder rebuilds them from the data like the
 * encoder, so both sides reset them for every page: the streaming API with memlz_reset() before each
 * page. This is plain userspace C with libc and, on x86-64, SSE 4.2, like memlz itself; the kernel
 * could not use it like that.
 */
#define MEMLZ_IMPLEMENTATION
#include "memlz.h"

#include "zram_codec.h"

struct memlz_ctx {
    memlz_state* compress;
    memlz_state* decompress;
};

static int memlz_setup_params(struct quetschn_params* p) {
    (void)p;
    return 0;
}

static void memlz_release_params(struct quetschn_params* p) {
    (void)p;
}

static void memlz_destroy(struct quetschn_stream* s) {
    struct memlz_ctx* ctx = s->context;

    if (!ctx)
        return;
    quetschn_free(ctx->compress, &s->allocated);
    quetschn_free(ctx->decompress, &s->allocated);
    quetschn_free(ctx, &s->allocated);
    s->context = NULL;
}

static int memlz_create(struct quetschn_params* p, struct quetschn_stream* s) {
    struct memlz_ctx* ctx = quetschn_zalloc(sizeof(*ctx), &s->allocated);

    (void)p;
    if (!ctx)
        return -1;
    s->context = ctx;
    ctx->compress = quetschn_zalloc(sizeof(memlz_state), &s->allocated);
    ctx->decompress = quetschn_zalloc(sizeof(memlz_state), &s->allocated);
    if (!ctx->compress || !ctx->decompress) {
        memlz_destroy(s);
        return -1;
    }
    return 0;
}

static int memlz_zcompress(struct quetschn_params* p,
                           struct quetschn_stream* s,
                           const void* src,
                           unsigned int src_len,
                           void* dst,
                           unsigned int* dst_len) {
    struct memlz_ctx* ctx = s->context;
    size_t len;

    (void)p;
    if (*dst_len < memlz_max_compressed_len(src_len))
        return -1;
    memlz_reset(ctx->compress);
    len = memlz_stream_compress(dst, src, src_len, ctx->compress);
    if (len == 0 || len > *dst_len)
        return -1;
    *dst_len = (unsigned int)len;
    return 0;
}

static int memlz_zdecompress(struct quetschn_params* p,
                             struct quetschn_stream* s,
                             const void* src,
                             unsigned int src_len,
                             void* dst,
                             unsigned int* dst_len) {
    struct memlz_ctx* ctx = s->context;

    (void)p;
    /* memlz writes as many bytes as its header says, so the header must say what zram expects */
    if (src_len < memlz_header_len() || memlz_compressed_len(src) != src_len || memlz_decompressed_len(src) > *dst_len)
        return -1;
    memlz_reset(ctx->decompress);
    if (memlz_stream_decompress(dst, src, ctx->decompress) != memlz_decompressed_len(src))
        return -1;
    *dst_len = (unsigned int)memlz_decompressed_len(src);
    return 0;
}

const struct quetschn_codec quetschn_codec_memlz = {
    "memlz",
    memlz_setup_params,
    memlz_release_params,
    memlz_create,
    memlz_destroy,
    memlz_zcompress,
    memlz_zdecompress,
};
