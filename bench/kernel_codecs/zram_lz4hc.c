// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * drivers/block/zram/backend_lz4hc.c, function by function. Without a dictionary: LZ4_compress_HC()
 * with the level, LZ4_decompress_safe(). With a dictionary, per page: LZ4_resetStreamHC(),
 * LZ4_loadDictHC() and LZ4_compress_HC_continue(), LZ4_setStreamDecode() and
 * LZ4_decompress_safe_continue(). Unlike backend_lz4.c there is no template stream: zram loads the
 * dictionary again for every page. The output is the lz4 format, so the decoder is lz4's.
 */
#include <linux/lz4.h>

#include "zram_codec.h"

struct lz4hc_ctx {
    void* mem;
    LZ4_streamDecode_t* dstrm;
    LZ4_streamHC_t* cstrm;
};

static void lz4hc_release_params(struct quetschn_params* p) {
    (void)p;
}

static int lz4hc_setup_params(struct quetschn_params* p) {
    if (p->level == QUETSCHN_LEVEL_DEFAULT) {
        p->level = LZ4HC_DEFAULT_CLEVEL;
    } else if (p->level < 1 || p->level > LZ4HC_MAX_CLEVEL) {
        return -1;
    }
    return 0;
}

static void lz4hc_destroy(struct quetschn_stream* s) {
    struct lz4hc_ctx* zctx = s->context;

    if (!zctx) {
        return;
    }
    quetschn_free(zctx->dstrm, &s->allocated);
    quetschn_free(zctx->cstrm, &s->allocated);
    quetschn_free(zctx->mem, &s->allocated);
    quetschn_free(zctx, &s->allocated);
    s->context = NULL;
}

static int lz4hc_create(struct quetschn_params* p, struct quetschn_stream* s) {
    struct lz4hc_ctx* zctx = quetschn_zalloc(sizeof(*zctx), &s->allocated);

    if (!zctx) {
        return -1;
    }
    s->context = zctx;
    if (p->dict_size == 0) {
        zctx->mem = quetschn_zalloc(LZ4HC_MEM_COMPRESS, &s->allocated);
        if (!zctx->mem) {
            goto error;
        }
    } else {
        zctx->dstrm = quetschn_zalloc(sizeof(*zctx->dstrm), &s->allocated);
        zctx->cstrm = quetschn_zalloc(sizeof(*zctx->cstrm), &s->allocated);
        if (!zctx->dstrm || !zctx->cstrm) {
            goto error;
        }
    }
    return 0;

error:
    lz4hc_destroy(s);
    return -1;
}

static int lz4hc_compress(struct quetschn_params* p,
                          struct quetschn_stream* s,
                          const void* src,
                          unsigned int src_len,
                          void* dst,
                          unsigned int* dst_len) {
    struct lz4hc_ctx* zctx = s->context;
    int ret;

    if (!zctx->cstrm) {
        ret = LZ4_compress_HC(src, dst, (int)src_len, (int)*dst_len, p->level, zctx->mem);
    } else {
        /* Cstrm needs to be reset */
        LZ4_resetStreamHC(zctx->cstrm, p->level);
        ret = LZ4_loadDictHC(zctx->cstrm, p->dict, (int)p->dict_size);
        if (ret != (int)p->dict_size) {
            return -1;
        }
        ret = LZ4_compress_HC_continue(zctx->cstrm, src, dst, (int)src_len, (int)*dst_len);
    }
    if (!ret) {
        return -1;
    }
    *dst_len = (unsigned int)ret;
    return 0;
}

static int lz4hc_decompress(struct quetschn_params* p,
                            struct quetschn_stream* s,
                            const void* src,
                            unsigned int src_len,
                            void* dst,
                            unsigned int* dst_len) {
    struct lz4hc_ctx* zctx = s->context;
    int ret;

    if (!zctx->dstrm) {
        ret = LZ4_decompress_safe(src, dst, (int)src_len, (int)*dst_len);
    } else {
        /* Dstrm needs to be reset */
        ret = LZ4_setStreamDecode(zctx->dstrm, p->dict, (int)p->dict_size);
        if (!ret) {
            return -1;
        }
        ret = LZ4_decompress_safe_continue(zctx->dstrm, src, dst, (int)src_len, (int)*dst_len);
    }
    if (ret < 0) {
        return -1;
    }
    *dst_len = (unsigned int)ret;
    return 0;
}

const struct quetschn_codec quetschn_codec_lz4hc = {
    .name = "lz4hc",
    .setup_params = lz4hc_setup_params,
    .release_params = lz4hc_release_params,
    .create = lz4hc_create,
    .destroy = lz4hc_destroy,
    .compress = lz4hc_compress,
    .decompress = lz4hc_decompress,
};
