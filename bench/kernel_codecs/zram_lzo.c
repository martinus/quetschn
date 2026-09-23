// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The calls of drivers/block/zram/backend_lzo.c and backend_lzorle.c. Both decompress with
 * lzo1x_decompress_safe(), which understands the lzo-rle stream too.
 */
/* lzo.h expects its includer to have included this, like the kernel sources do */
#include <linux/types.h>

#include <linux/lzo.h>

#include "zram_codec.h"

static int lzo_decompress(const void* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    size_t len = *dst_len;
    int ret = lzo1x_decompress_safe(src, src_len, dst, &len);

    if (ret != LZO_E_OK)
        return -1;
    *dst_len = (unsigned int)len;
    return 0;
}

static int lzo_compress(const void* src, unsigned int src_len, void* dst, unsigned int* dst_len, void* workspace) {
    size_t len = *dst_len;
    int ret = lzo1x_1_compress(src, src_len, dst, &len, workspace);

    if (ret != LZO_E_OK)
        return -1;
    *dst_len = (unsigned int)len;
    return 0;
}

static int lzorle_compress(const void* src, unsigned int src_len, void* dst, unsigned int* dst_len, void* workspace) {
    size_t len = *dst_len;
    int ret = lzorle1x_1_compress(src, src_len, dst, &len, workspace);

    if (ret != LZO_E_OK)
        return -1;
    *dst_len = (unsigned int)len;
    return 0;
}

const struct quetschn_codec quetschn_codec_lzo = {
    .name = "lzo",
    .workspace_size = LZO1X_MEM_COMPRESS,
    .compress = lzo_compress,
    .decompress = lzo_decompress,
};

const struct quetschn_codec quetschn_codec_lzo_rle = {
    .name = "lzo-rle",
    .workspace_size = LZO1X_MEM_COMPRESS,
    .compress = lzorle_compress,
    .decompress = lzo_decompress,
};
