// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* lz4page (explore/lz4page.h) as a zram backend would call it: a hash table per CPU, and the kernel's
 * LZ4_decompress_safe() to read, as backend_lz4.c without a dictionary does. */
#include <linux/lz4.h>

#include "lz4page.h"
#include "zram_codec.h"

#define PAGE (1U << QUETSCHN_PAGE_BITS)

static int setup(struct quetschn_params* p) {
    return p->page_size == PAGE ? 0 : -1;
}

static void release(struct quetschn_params* p) {
    (void)p;
}

static int create(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    s->context = quetschn_zalloc(sizeof(unsigned short) << LZ4PAGE_HASH_BITS, &s->allocated);
    return s->context ? 0 : -1;
}

static void destroy(struct quetschn_stream* s) {
    quetschn_free(s->context, &s->allocated);
    s->context = 0;
}

static int compress_with(
    struct quetschn_stream* s, const void* src, unsigned int src_len, void* dst, unsigned int* dst_len, unsigned int flags) {
    unsigned int len;

    if (src_len != PAGE)
        return -1;
    len = lz4page_compress(s->context, src, dst, *dst_len, flags);
    if (!len)
        return -1;
    *dst_len = len;
    return 0;
}

static int decompress(struct quetschn_params* p,
                      struct quetschn_stream* s,
                      const void* src,
                      unsigned int src_len,
                      void* dst,
                      unsigned int* dst_len) {
    int ret;

    (void)p;
    (void)s;
    ret = LZ4_decompress_safe(src, dst, (int)src_len, (int)*dst_len);
    if (ret != (int)PAGE)
        return -1;
    *dst_len = PAGE;
    return 0;
}

static int compress_lz4page(struct quetschn_params* p,
                            struct quetschn_stream* s,
                            const void* src,
                            unsigned int src_len,
                            void* dst,
                            unsigned int* dst_len) {
    (void)p;
    return compress_with(s, src, src_len, dst, dst_len, 0U);
}

const struct quetschn_codec quetschn_codec_lz4page = {
    "lz4page",
    setup,
    release,
    create,
    destroy,
    compress_lz4page,
    decompress,
};

static int compress_lz4page_2way(struct quetschn_params* p,
                                 struct quetschn_stream* s,
                                 const void* src,
                                 unsigned int src_len,
                                 void* dst,
                                 unsigned int* dst_len) {
    (void)p;
    return compress_with(s, src, src_len, dst, dst_len, LZ4PAGE_TWO_WAY);
}

const struct quetschn_codec quetschn_codec_lz4page_2way = {
    "lz4page-2way",
    setup,
    release,
    create,
    destroy,
    compress_lz4page_2way,
    decompress,
};

static int compress_lz4page_lazy(struct quetschn_params* p,
                                 struct quetschn_stream* s,
                                 const void* src,
                                 unsigned int src_len,
                                 void* dst,
                                 unsigned int* dst_len) {
    (void)p;
    return compress_with(s, src, src_len, dst, dst_len, LZ4PAGE_LAZY);
}

const struct quetschn_codec quetschn_codec_lz4page_lazy = {
    "lz4page-lazy",
    setup,
    release,
    create,
    destroy,
    compress_lz4page_lazy,
    decompress,
};

static int compress_lz4page_both(struct quetschn_params* p,
                                 struct quetschn_stream* s,
                                 const void* src,
                                 unsigned int src_len,
                                 void* dst,
                                 unsigned int* dst_len) {
    (void)p;
    return compress_with(s, src, src_len, dst, dst_len, LZ4PAGE_TWO_WAY | LZ4PAGE_LAZY);
}

const struct quetschn_codec quetschn_codec_lz4page_both = {
    "lz4page-both",
    setup,
    release,
    create,
    destroy,
    compress_lz4page_both,
    decompress,
};
