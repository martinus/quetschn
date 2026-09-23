// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The calls of drivers/block/zram/backend_lz4.c without a dictionary: LZ4_compress_fast() with the
 * stream's acceleration, LZ4_decompress_safe().
 */
#include <linux/lz4.h>

#include "zram_codec.h"

static __SIZE_TYPE__ lz4_workspace_size(int* level, unsigned int page_size) {
    (void)page_size;
    if (*level == QUETSCHN_LEVEL_DEFAULT) {
        *level = LZ4_ACCELERATION_DEFAULT;
    } else if (*level < LZ4_ACCELERATION_DEFAULT) {
        return 0; /* lz4_setup_params() rejects it */
    }
    return LZ4_MEM_COMPRESS;
}

static int lz4_init(struct quetschn_stream* s, unsigned int page_size) {
    (void)s;
    (void)page_size;
    return 0;
}

static int lz4_compress(struct quetschn_stream* s, const void* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    int ret = LZ4_compress_fast(src, dst, (int)src_len, (int)*dst_len, s->level, s->workspace);

    if (!ret) {
        return -1;
    }
    *dst_len = (unsigned int)ret;
    return 0;
}

static int lz4_decompress(struct quetschn_stream* s, const void* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    int ret = LZ4_decompress_safe(src, dst, (int)src_len, (int)*dst_len);

    (void)s;
    if (ret < 0) {
        return -1;
    }
    *dst_len = (unsigned int)ret;
    return 0;
}

const struct quetschn_codec quetschn_codec_lz4 = {
    .name = "lz4",
    .workspace_size = lz4_workspace_size,
    .init = lz4_init,
    .compress = lz4_compress,
    .decompress = lz4_decompress,
};
