// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * drivers/block/zram/backend_lzo.c and backend_lzorle.c. Both ignore the level and dictionaries, and
 * both decompress with lzo1x_decompress_safe(), which understands the lzo-rle stream too.
 */

/* lzo.h expects its includer to have included this, like the kernel sources do */
#include <linux/types.h>

#include <linux/lzo.h>

#include "zram_codec.h"

static int lzo_setup_params(struct quetschn_params* p) {
    (void)p;
    return 0;
}

static void lzo_release_params(struct quetschn_params* p) {
    (void)p;
}

static int lzo_create(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    s->context = quetschn_zalloc(LZO1X_MEM_COMPRESS, &s->allocated);
    return s->context ? 0 : -1;
}

static void lzo_destroy(struct quetschn_stream* s) {
    quetschn_free(s->context, &s->allocated);
    s->context = NULL;
}

static int lzo_decompress(struct quetschn_params* p,
                          struct quetschn_stream* s,
                          const void* src,
                          unsigned int src_len,
                          void* dst,
                          unsigned int* dst_len) {
    size_t len = *dst_len;
    int ret = lzo1x_decompress_safe(src, src_len, dst, &len);

    (void)p;
    (void)s;
    if (ret != LZO_E_OK) {
        return -1;
    }
    *dst_len = (unsigned int)len;
    return 0;
}

static int lzo_compress(struct quetschn_params* p,
                        struct quetschn_stream* s,
                        const void* src,
                        unsigned int src_len,
                        void* dst,
                        unsigned int* dst_len) {
    size_t len = *dst_len;
    int ret = lzo1x_1_compress(src, src_len, dst, &len, s->context);

    (void)p;
    if (ret != LZO_E_OK) {
        return -1;
    }
    *dst_len = (unsigned int)len;
    return 0;
}

static int lzorle_compress(struct quetschn_params* p,
                           struct quetschn_stream* s,
                           const void* src,
                           unsigned int src_len,
                           void* dst,
                           unsigned int* dst_len) {
    size_t len = *dst_len;
    int ret = lzorle1x_1_compress(src, src_len, dst, &len, s->context);

    (void)p;
    if (ret != LZO_E_OK) {
        return -1;
    }
    *dst_len = (unsigned int)len;
    return 0;
}

const struct quetschn_codec quetschn_codec_lzo = {
    .name = "lzo",
    .setup_params = lzo_setup_params,
    .release_params = lzo_release_params,
    .create = lzo_create,
    .destroy = lzo_destroy,
    .compress = lzo_compress,
    .decompress = lzo_decompress,
};

const struct quetschn_codec quetschn_codec_lzo_rle = {
    .name = "lzo-rle",
    .setup_params = lzo_setup_params,
    .release_params = lzo_release_params,
    .create = lzo_create,
    .destroy = lzo_destroy,
    .compress = lzorle_compress,
    .decompress = lzo_decompress,
};
