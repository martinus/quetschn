// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The calls of drivers/block/zram/backend_zstd.c without a dictionary: parameters from
 * zstd_get_params(level, PAGE_SIZE), a cctx and a dctx in memory allocated once per stream, then
 * zstd_compress_cctx() and zstd_decompress_dctx() for every page.
 */
#include <linux/zstd.h>

#include "zram_codec.h"

struct zstd_state {
    zstd_cctx* cctx;
    zstd_dctx* dctx;
    zstd_parameters params;
};

_Static_assert(sizeof(struct zstd_state) <= sizeof(((struct quetschn_stream*)0)->state),
               "quetschn_stream.state is too small for zstd");

static int zstd_resolve_level(int* level) {
    if (*level == QUETSCHN_LEVEL_DEFAULT) {
        *level = zstd_default_clevel();
    }
    return *level >= zstd_min_clevel() && *level <= zstd_max_clevel();
}

/*
 * zstd_create() makes two vzalloc allocations, both page aligned. Here they share one workspace, so the
 * dctx starts at the next cache line: from level 19 on the cctx size is not a multiple of 8, and
 * zstd_init_dctx() rejects a misaligned workspace. That adds at most 63 bytes to the reported size.
 */
static size_t cctx_size(int level, unsigned int page_size) {
    zstd_parameters prm = zstd_get_params(level, page_size);

    return (zstd_cctx_workspace_bound(&prm.cParams) + 63) & ~(size_t)63;
}

static __SIZE_TYPE__ zstd_workspace_size(int* level, unsigned int page_size) {
    if (!zstd_resolve_level(level)) {
        return 0; /* zstd_setup_params() rejects it */
    }
    return cctx_size(*level, page_size) + zstd_dctx_workspace_bound();
}

static int zstd_init(struct quetschn_stream* s, unsigned int page_size) {
    struct zstd_state* st = (struct zstd_state*)s->state;
    size_t csize = cctx_size(s->level, page_size);

    st->params = zstd_get_params(s->level, page_size);
    st->cctx = zstd_init_cctx(s->workspace, csize);
    st->dctx = zstd_init_dctx((unsigned char*)s->workspace + csize, zstd_dctx_workspace_bound());
    return st->cctx && st->dctx ? 0 : -1;
}

static int zstd_compress(struct quetschn_stream* s, const void* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    struct zstd_state* st = (struct zstd_state*)s->state;
    size_t ret = zstd_compress_cctx(st->cctx, dst, *dst_len, src, src_len, &st->params);

    if (zstd_is_error(ret)) {
        return -1;
    }
    *dst_len = (unsigned int)ret;
    return 0;
}

static int
zstd_decompress(struct quetschn_stream* s, const void* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    struct zstd_state* st = (struct zstd_state*)s->state;
    size_t ret = zstd_decompress_dctx(st->dctx, dst, *dst_len, src, src_len);

    if (zstd_is_error(ret)) {
        return -1;
    }
    *dst_len = (unsigned int)ret;
    return 0;
}

const struct quetschn_codec quetschn_codec_zstd = {
    .name = "zstd",
    .workspace_size = zstd_workspace_size,
    .init = zstd_init,
    .compress = zstd_compress,
    .decompress = zstd_decompress,
};
