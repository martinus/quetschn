// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The calls of drivers/block/zram/backend_lz4.c without a dictionary: LZ4_compress_fast() with the
 * default acceleration, LZ4_decompress_safe().
 */
#include <linux/lz4.h>

#include "zram_codec.h"

static int lz4_compress(const void* src, unsigned int src_len, void* dst, unsigned int* dst_len, void* workspace) {
    int ret = LZ4_compress_fast(src, dst, (int)src_len, (int)*dst_len, LZ4_ACCELERATION_DEFAULT, workspace);

    if (!ret)
        return -1;
    *dst_len = (unsigned int)ret;
    return 0;
}

static int lz4_decompress(const void* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    int ret = LZ4_decompress_safe(src, dst, (int)src_len, (int)*dst_len);

    if (ret < 0)
        return -1;
    *dst_len = (unsigned int)ret;
    return 0;
}

const struct quetschn_codec quetschn_codec_lz4 = {
    .name = "lz4",
    .workspace_size = LZ4_MEM_COMPRESS,
    .compress = lz4_compress,
    .decompress = lz4_decompress,
};
