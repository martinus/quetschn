// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * drivers/block/zram/backend_zstd.c, function by function.
 *
 * setup_params always creates a cdict and a ddict, also without a dictionary (then empty), because
 * backend_zstd.c does. Without a dictionary a stream is a cctx and a dctx in two vzalloc'd workspaces,
 * used with zstd_compress_cctx() / zstd_decompress_dctx() and the parameters from
 * zstd_get_params(level, PAGE_SIZE). With a dictionary the stream's cctx and dctx come from
 * zstd_create_*_advanced(), which allocate through the custom allocator, also later during compression,
 * and pages go through zstd_compress_using_cdict() / zstd_decompress_using_ddict().
 *
 * The only difference to zram: zram passes one custom allocator with a NULL opaque pointer everywhere.
 * Here the opaque pointer is the params' or the stream's byte counter, so that the harness can say what
 * is allocated per device and what per CPU.
 */
#include <linux/zstd.h>

#include "zram_codec.h"

struct zstd_ctx {
    zstd_cctx* cctx;
    zstd_dctx* dctx;
    void* cctx_mem;
    void* dctx_mem;
};

struct zstd_params {
    zstd_custom_mem custom_mem;
    zstd_cdict* cdict;
    zstd_ddict* ddict;
    zstd_parameters cprm;
};

static void* zstd_custom_alloc(void* opaque, size_t size) {
    return quetschn_zalloc(size, opaque);
}

static void zstd_custom_free(void* opaque, void* address) {
    quetschn_free(address, opaque);
}

static zstd_custom_mem counting_mem(size_t* counter) {
    zstd_custom_mem mem = {zstd_custom_alloc, zstd_custom_free, counter};
    return mem;
}

static void zstd_release_params(struct quetschn_params* p) {
    struct zstd_params* zp = p->drv_data;

    p->drv_data = NULL;
    if (!zp) {
        return;
    }
    zstd_free_cdict(zp->cdict);
    zstd_free_ddict(zp->ddict);
    quetschn_free(zp, &p->allocated);
}

static int zstd_setup_params(struct quetschn_params* p) {
    zstd_compression_parameters prm;
    struct zstd_params* zp = quetschn_zalloc(sizeof(*zp), &p->allocated);

    if (!zp) {
        return -1;
    }
    p->drv_data = zp;
    if (p->level == QUETSCHN_LEVEL_DEFAULT) {
        p->level = zstd_default_clevel();
    } else if (p->level < zstd_min_clevel() || p->level > zstd_max_clevel()) {
        goto error;
    }

    zp->cprm = zstd_get_params(p->level, p->page_size);
    zp->custom_mem = counting_mem(&p->allocated);
    prm = zstd_get_cparams(p->level, p->page_size, p->dict_size);

    zp->cdict = zstd_create_cdict_byreference(p->dict, p->dict_size, prm, zp->custom_mem);
    if (!zp->cdict) {
        goto error;
    }
    zp->ddict = zstd_create_ddict_byreference(p->dict, p->dict_size, zp->custom_mem);
    if (!zp->ddict) {
        goto error;
    }
    return 0;

error:
    zstd_release_params(p);
    return -1;
}

static void zstd_destroy(struct quetschn_stream* s) {
    struct zstd_ctx* zctx = s->context;

    if (!zctx) {
        return;
    }
    /* embedded into cctx_mem / dctx_mem without a dictionary, allocated by zstd with one */
    if (zctx->cctx_mem) {
        quetschn_free(zctx->cctx_mem, &s->allocated);
    } else {
        zstd_free_cctx(zctx->cctx);
    }
    if (zctx->dctx_mem) {
        quetschn_free(zctx->dctx_mem, &s->allocated);
    } else {
        zstd_free_dctx(zctx->dctx);
    }
    quetschn_free(zctx, &s->allocated);
    s->context = NULL;
}

static int zstd_create(struct quetschn_params* p, struct quetschn_stream* s) {
    struct zstd_ctx* zctx = quetschn_zalloc(sizeof(*zctx), &s->allocated);
    zstd_parameters prm;
    size_t sz;

    if (!zctx) {
        return -1;
    }
    s->context = zctx;
    if (p->dict_size == 0) {
        prm = zstd_get_params(p->level, p->page_size);
        sz = zstd_cctx_workspace_bound(&prm.cParams);
        zctx->cctx_mem = quetschn_zalloc(sz, &s->allocated);
        if (!zctx->cctx_mem) {
            goto error;
        }
        zctx->cctx = zstd_init_cctx(zctx->cctx_mem, sz);
        if (!zctx->cctx) {
            goto error;
        }
        sz = zstd_dctx_workspace_bound();
        zctx->dctx_mem = quetschn_zalloc(sz, &s->allocated);
        if (!zctx->dctx_mem) {
            goto error;
        }
        zctx->dctx = zstd_init_dctx(zctx->dctx_mem, sz);
        if (!zctx->dctx) {
            goto error;
        }
    } else {
        zctx->cctx = zstd_create_cctx_advanced(counting_mem(&s->allocated));
        if (!zctx->cctx) {
            goto error;
        }
        zctx->dctx = zstd_create_dctx_advanced(counting_mem(&s->allocated));
        if (!zctx->dctx) {
            goto error;
        }
    }
    return 0;

error:
    zstd_destroy(s);
    return -1;
}

static int zstd_compress(struct quetschn_params* p,
                         struct quetschn_stream* s,
                         const void* src,
                         unsigned int src_len,
                         void* dst,
                         unsigned int* dst_len) {
    struct zstd_params* zp = p->drv_data;
    struct zstd_ctx* zctx = s->context;
    size_t ret;

    if (p->dict_size == 0) {
        ret = zstd_compress_cctx(zctx->cctx, dst, *dst_len, src, src_len, &zp->cprm);
    } else {
        ret = zstd_compress_using_cdict(zctx->cctx, dst, *dst_len, src, src_len, zp->cdict);
    }
    if (zstd_is_error(ret)) {
        return -1;
    }
    *dst_len = (unsigned int)ret;
    return 0;
}

static int zstd_decompress(struct quetschn_params* p,
                           struct quetschn_stream* s,
                           const void* src,
                           unsigned int src_len,
                           void* dst,
                           unsigned int* dst_len) {
    struct zstd_params* zp = p->drv_data;
    struct zstd_ctx* zctx = s->context;
    size_t ret;

    if (p->dict_size == 0) {
        ret = zstd_decompress_dctx(zctx->dctx, dst, *dst_len, src, src_len);
    } else {
        ret = zstd_decompress_using_ddict(zctx->dctx, dst, *dst_len, src, src_len, zp->ddict);
    }
    if (zstd_is_error(ret)) {
        return -1;
    }
    *dst_len = (unsigned int)ret;
    return 0;
}

const struct quetschn_codec quetschn_codec_zstd = {
    .name = "zstd",
    .setup_params = zstd_setup_params,
    .release_params = zstd_release_params,
    .create = zstd_create,
    .destroy = zstd_destroy,
    .compress = zstd_compress,
    .decompress = zstd_decompress,
};
