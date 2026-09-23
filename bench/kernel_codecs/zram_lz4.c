// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * drivers/block/zram/backend_lz4.c, function by function. Without a dictionary: LZ4_compress_fast()
 * with the level as acceleration, LZ4_decompress_safe(). With a dictionary: LZ4_loadDict() once into a
 * template stream, and per page a copy of the template (f0f6f7871430), LZ4_compress_fast_continue(),
 * LZ4_setStreamDecode() and LZ4_decompress_safe_continue().
 */
#include <linux/lz4.h>
#include <linux/string.h>

#include "zram_codec.h"

struct lz4_ctx {
    void* mem;
    LZ4_streamDecode_t* dstrm;
    LZ4_stream_t* cstrm;
};

static void lz4_release_params(struct quetschn_params* p) {
    quetschn_free(p->drv_data, &p->allocated);
    p->drv_data = NULL;
}

static int lz4_setup_params(struct quetschn_params* p) {
    LZ4_stream_t* dict_stream;
    int ret;

    if (p->level == QUETSCHN_LEVEL_DEFAULT) {
        p->level = LZ4_ACCELERATION_DEFAULT;
    } else if (p->level < LZ4_ACCELERATION_DEFAULT) {
        return -1;
    }
    if (!p->dict || !p->dict_size) {
        return 0;
    }
    dict_stream = quetschn_zalloc(sizeof(*dict_stream), &p->allocated);
    if (!dict_stream) {
        return -1;
    }
    ret = LZ4_loadDict(dict_stream, p->dict, (int)p->dict_size);
    if (ret != (int)p->dict_size) {
        quetschn_free(dict_stream, &p->allocated);
        return -1;
    }
    p->drv_data = dict_stream;
    return 0;
}

static void lz4_destroy(struct quetschn_stream* s) {
    struct lz4_ctx* zctx = s->context;

    if (!zctx) {
        return;
    }
    quetschn_free(zctx->mem, &s->allocated);
    quetschn_free(zctx->dstrm, &s->allocated);
    quetschn_free(zctx->cstrm, &s->allocated);
    quetschn_free(zctx, &s->allocated);
    s->context = NULL;
}

static int lz4_create(struct quetschn_params* p, struct quetschn_stream* s) {
    struct lz4_ctx* zctx = quetschn_zalloc(sizeof(*zctx), &s->allocated);

    if (!zctx) {
        return -1;
    }
    s->context = zctx;
    if (p->dict_size == 0) {
        zctx->mem = quetschn_zalloc(LZ4_MEM_COMPRESS, &s->allocated);
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
    lz4_destroy(s);
    return -1;
}

static int lz4_compress(struct quetschn_params* p,
                        struct quetschn_stream* s,
                        const void* src,
                        unsigned int src_len,
                        void* dst,
                        unsigned int* dst_len) {
    struct lz4_ctx* zctx = s->context;
    int ret;

    if (!zctx->cstrm) {
        ret = LZ4_compress_fast(src, dst, (int)src_len, (int)*dst_len, p->level, zctx->mem);
    } else {
        /* Cstrm needs to be reset */
        memcpy(zctx->cstrm, p->drv_data, sizeof(*zctx->cstrm));
        ret = LZ4_compress_fast_continue(zctx->cstrm, src, dst, (int)src_len, (int)*dst_len, p->level);
    }
    if (!ret) {
        return -1;
    }
    *dst_len = (unsigned int)ret;
    return 0;
}

static int lz4_decompress(struct quetschn_params* p,
                          struct quetschn_stream* s,
                          const void* src,
                          unsigned int src_len,
                          void* dst,
                          unsigned int* dst_len) {
    struct lz4_ctx* zctx = s->context;
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

const struct quetschn_codec quetschn_codec_lz4 = {
    .name = "lz4",
    .setup_params = lz4_setup_params,
    .release_params = lz4_release_params,
    .create = lz4_create,
    .destroy = lz4_destroy,
    .compress = lz4_compress,
    .decompress = lz4_decompress,
};
