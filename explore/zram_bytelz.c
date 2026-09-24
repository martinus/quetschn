// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* bytelz (explore/bytelz.h) as a zram backend would call it: no parameters, a hash table per CPU. */
#include "bytelz.h"
#include "zram_codec.h"

static int setup(struct quetschn_params* p) {
    return p->page_size == BYTELZ_PAGE ? 0 : -1;
}

static void release(struct quetschn_params* p) {
    (void)p;
}

static int create(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    s->context = quetschn_zalloc(sizeof(struct bytelz_state), &s->allocated);
    return s->context ? 0 : -1;
}

static void destroy(struct quetschn_stream* s) {
    quetschn_free(s->context, &s->allocated);
    s->context = 0;
}

static int compress(struct quetschn_params* p,
                    struct quetschn_stream* s,
                    const void* src,
                    unsigned int src_len,
                    void* dst,
                    unsigned int* dst_len) {
    unsigned int len;

    (void)p;
    if (src_len != BYTELZ_PAGE)
        return -1;
    len = bytelz_compress(s->context, src, dst, *dst_len);
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
    (void)p;
    (void)s;
    quetschn_prefetch_page(src, src_len, dst, BYTELZ_PAGE);
    if (*dst_len < BYTELZ_PAGE || bytelz_decode(src, src_len, dst))
        return -1;
    *dst_len = BYTELZ_PAGE;
    return 0;
}

const struct quetschn_codec quetschn_codec_bytelz = {
    "bytelz",
    setup,
    release,
    create,
    destroy,
    compress,
    decompress,
};
