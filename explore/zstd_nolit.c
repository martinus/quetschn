// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * zstd as zram uses it without a dictionary (bench/kernel_codecs/zram_zstd.c), with one change: the
 * literals are never Huffman coded. The difference to zram's zstd at the same level is what entropy
 * coding of the literals is worth, see docs/explored-designs.md. zstd_compress_cctx() resets all
 * parameters for every page, so this repeats what it does and sets ZSTD_c_literalCompressionMode on top.
 */
#include <linux/zstd.h>

#include "zram_codec.h"

struct nolit_ctx {
    zstd_cctx* cctx;
    zstd_dctx* dctx;
    void* cctx_mem;
    void* dctx_mem;
};

static int nolit_setup_params(struct quetschn_params* p) {
    if (p->level == QUETSCHN_LEVEL_DEFAULT)
        p->level = zstd_default_clevel();
    return p->level < zstd_min_clevel() || p->level > zstd_max_clevel() ? -1 : 0;
}

static void nolit_release_params(struct quetschn_params* p) {
    (void)p;
}

static void nolit_destroy(struct quetschn_stream* s) {
    struct nolit_ctx* ctx = s->context;

    if (!ctx)
        return;
    quetschn_free(ctx->cctx_mem, &s->allocated);
    quetschn_free(ctx->dctx_mem, &s->allocated);
    quetschn_free(ctx, &s->allocated);
    s->context = NULL;
}

static int nolit_create(struct quetschn_params* p, struct quetschn_stream* s) {
    struct nolit_ctx* ctx = quetschn_zalloc(sizeof(*ctx), &s->allocated);
    zstd_parameters prm;
    size_t sz;

    if (!ctx)
        return -1;
    s->context = ctx;
    prm = zstd_get_params(p->level, p->page_size);
    sz = zstd_cctx_workspace_bound(&prm.cParams);
    ctx->cctx_mem = quetschn_zalloc(sz, &s->allocated);
    if (!ctx->cctx_mem || !(ctx->cctx = zstd_init_cctx(ctx->cctx_mem, sz)))
        goto error;
    sz = zstd_dctx_workspace_bound();
    ctx->dctx_mem = quetschn_zalloc(sz, &s->allocated);
    if (!ctx->dctx_mem || !(ctx->dctx = zstd_init_dctx(ctx->dctx_mem, sz)))
        goto error;
    return 0;

error:
    nolit_destroy(s);
    return -1;
}

#define TRY(call)                \
    do {                         \
        if (zstd_is_error(call)) \
            return -1;           \
    } while (0)

static int nolit_compress(struct quetschn_params* p,
                          struct quetschn_stream* s,
                          const void* src,
                          unsigned int src_len,
                          void* dst,
                          unsigned int* dst_len) {
    struct nolit_ctx* ctx = s->context;
    zstd_parameters prm = zstd_get_params(p->level, p->page_size);
    zstd_cctx* c = ctx->cctx;
    size_t ret;

    /* zstd_cctx_init() in lib/zstd/zstd_compress_module.c */
    TRY(ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters));
    TRY(ZSTD_CCtx_setPledgedSrcSize(c, src_len));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, (int)prm.cParams.windowLog));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_hashLog, (int)prm.cParams.hashLog));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_chainLog, (int)prm.cParams.chainLog));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_searchLog, (int)prm.cParams.searchLog));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_minMatch, (int)prm.cParams.minMatch));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_targetLength, (int)prm.cParams.targetLength));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_strategy, (int)prm.cParams.strategy));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, prm.fParams.contentSizeFlag));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, prm.fParams.checksumFlag));
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_dictIDFlag, !prm.fParams.noDictIDFlag));
    /* the one change */
    TRY(ZSTD_CCtx_setParameter(c, ZSTD_c_literalCompressionMode, ZSTD_ps_disable));
    ret = ZSTD_compress2(c, dst, *dst_len, src, src_len);
    if (zstd_is_error(ret))
        return -1;
    *dst_len = (unsigned int)ret;
    return 0;
}

static int nolit_decompress(struct quetschn_params* p,
                            struct quetschn_stream* s,
                            const void* src,
                            unsigned int src_len,
                            void* dst,
                            unsigned int* dst_len) {
    struct nolit_ctx* ctx = s->context;
    size_t ret;

    (void)p;
    ret = zstd_decompress_dctx(ctx->dctx, dst, *dst_len, src, src_len);
    if (zstd_is_error(ret))
        return -1;
    *dst_len = (unsigned int)ret;
    return 0;
}

const struct quetschn_codec quetschn_codec_zstd_nolit = {
    "zstd-nolit",
    nolit_setup_params,
    nolit_release_params,
    nolit_create,
    nolit_destroy,
    nolit_compress,
    nolit_decompress,
};
