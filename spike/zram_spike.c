// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The spike as a zram backend would call it: no level, no dictionary, no per-CPU memory. Two codecs
 * that differ only in the decoder, see wk64.h.
 */
#include "wk64.h"
#include "zram_codec.h"

static int spike_setup_params(struct quetschn_params* p) {
    /* 4 KiB pages only; zram would refuse the algorithm on other page sizes */
    return p->page_size == WK64_PAGE_SIZE ? 0 : -1;
}

static void spike_release_params(struct quetschn_params* p) {
    (void)p;
}

static int spike_create(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    (void)s;
    return 0;
}

static void spike_destroy(struct quetschn_stream* s) {
    (void)s;
}

static int spike_compress(struct quetschn_params* p,
                          struct quetschn_stream* s,
                          const void* src,
                          unsigned int src_len,
                          void* dst,
                          unsigned int* dst_len) {
    unsigned int len;

    (void)p;
    (void)s;
    if (src_len != WK64_PAGE_SIZE)
        return -1;
    len = wk64_compress(src, dst, *dst_len);
    if (len == 0)
        return -1;
    *dst_len = len;
    return 0;
}

static int spike_decompress_switch(struct quetschn_params* p,
                                   struct quetschn_stream* s,
                                   const void* src,
                                   unsigned int src_len,
                                   void* dst,
                                   unsigned int* dst_len) {
    (void)p;
    (void)s;
    if (*dst_len < WK64_PAGE_SIZE || wk64_decompress_switch(src, src_len, dst))
        return -1;
    *dst_len = WK64_PAGE_SIZE;
    return 0;
}

static int spike_decompress_branchless(struct quetschn_params* p,
                                       struct quetschn_stream* s,
                                       const void* src,
                                       unsigned int src_len,
                                       void* dst,
                                       unsigned int* dst_len) {
    (void)p;
    (void)s;
    if (*dst_len < WK64_PAGE_SIZE || wk64_decompress_branchless(src, src_len, dst))
        return -1;
    *dst_len = WK64_PAGE_SIZE;
    return 0;
}

static int spike_decompress_zeroskip(struct quetschn_params* p,
                                     struct quetschn_stream* s,
                                     const void* src,
                                     unsigned int src_len,
                                     void* dst,
                                     unsigned int* dst_len) {
    (void)p;
    (void)s;
    if (*dst_len < WK64_PAGE_SIZE || wk64_decompress_zeroskip(src, src_len, dst))
        return -1;
    *dst_len = WK64_PAGE_SIZE;
    return 0;
}

const struct quetschn_codec quetschn_codec_spike_switch = {
    "spike-switch",
    spike_setup_params,
    spike_release_params,
    spike_create,
    spike_destroy,
    spike_compress,
    spike_decompress_switch,
};

const struct quetschn_codec quetschn_codec_spike_branchless = {
    "spike-branchless",
    spike_setup_params,
    spike_release_params,
    spike_create,
    spike_destroy,
    spike_compress,
    spike_decompress_branchless,
};

const struct quetschn_codec quetschn_codec_spike_zeroskip = {
    "spike-zeroskip",
    spike_setup_params,
    spike_release_params,
    spike_create,
    spike_destroy,
    spike_compress,
    spike_decompress_zeroskip,
};
