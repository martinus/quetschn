// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz (explore/seqlz.h) as a zram backend would call it. "seqlz-fast" is seqlz's own compressor.
 * "seqlz" and "seqlz-hc" take the matches from the kernel's lz4 or lz4hc at level 3 instead: compress
 * with it into a per-CPU buffer, split that into sequences, code them with seqlz; they are the
 * reference for what a better matcher is worth. zram's dictionary parameter, if it is exactly a struct
 * seqlz_lengths (2099 bytes), carries other code lengths instead of the ones compiled in.
 */
#include <linux/lz4.h>

#include "seqlz.h"
#include "zram_codec.h"

struct seqlz_ctx {
    void* lz4_mem;
    unsigned char* lz4_out;
    struct seqlz_sequence* seq;
    unsigned char* literals;
    unsigned char* scratch; /* for the literals of seqlz-hc-lit */
    int coded;
};

static int setup(struct quetschn_params* p, const struct seqlz_lengths* built_in) {
    const struct seqlz_lengths* lengths = built_in;
    struct seqlz_tables* t;

    if (p->page_size != SEQLZ_PAGE)
        return -1;
    /* a dictionary of another size is not for seqlz; ignored like lzo ignores dictionaries */
    if (p->dict_size == sizeof(struct seqlz_lengths))
        lengths = p->dict;
    t = quetschn_zalloc(seqlz_tables_size(), &p->allocated);
    if (!t)
        return -1;
    /* the encoder needs a code for every symbol: tables without one fail here, not at every page */
    if (seqlz_tables_init(t, lengths) || !seqlz_all_symbols(t)) {
        quetschn_free(t, &p->allocated);
        return -1;
    }
    p->drv_data = t;
    return 0;
}

static int setup_lz4(struct quetschn_params* p) {
    if (p->level == QUETSCHN_LEVEL_DEFAULT)
        p->level = 1;
    return setup(p, &seqlz_default_lz4);
}

static int setup_lz4hc(struct quetschn_params* p) {
    if (p->level == QUETSCHN_LEVEL_DEFAULT)
        p->level = 3;
    return setup(p, &seqlz_default_lz4hc);
}

static void release(struct quetschn_params* p) {
    quetschn_free(p->drv_data, &p->allocated);
    p->drv_data = NULL;
}

static void destroy(struct quetschn_stream* s) {
    struct seqlz_ctx* ctx = s->context;

    if (!ctx)
        return;
    quetschn_free(ctx->lz4_mem, &s->allocated);
    quetschn_free(ctx->lz4_out, &s->allocated);
    quetschn_free(ctx->seq, &s->allocated);
    quetschn_free(ctx->literals, &s->allocated);
    quetschn_free(ctx->scratch, &s->allocated);
    quetschn_free(ctx, &s->allocated);
    s->context = NULL;
}

static int create(struct quetschn_stream* s, unsigned int workspace) {
    struct seqlz_ctx* ctx = quetschn_zalloc(sizeof(*ctx), &s->allocated);

    if (!ctx)
        return -1;
    s->context = ctx;
    ctx->lz4_mem = quetschn_zalloc(workspace, &s->allocated);
    ctx->lz4_out = quetschn_zalloc(2 * SEQLZ_PAGE, &s->allocated);
    ctx->seq = quetschn_zalloc(SEQLZ_MAX_SEQUENCES * sizeof(*ctx->seq), &s->allocated);
    ctx->literals = quetschn_zalloc(SEQLZ_PAGE, &s->allocated);
    ctx->scratch = quetschn_zalloc(SEQLZ_SCRATCH, &s->allocated);
    if (!ctx->lz4_mem || !ctx->lz4_out || !ctx->seq || !ctx->literals || !ctx->scratch) {
        destroy(s);
        return -1;
    }
    return 0;
}

static int create_lz4(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    return create(s, LZ4_MEM_COMPRESS);
}

static int create_lz4hc(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    return create(s, LZ4HC_MEM_COMPRESS);
}

/* Splits an lz4 block into sequences and literals; bench/lz_analysis.cpp does the same in C++. */
static int split(const unsigned char* p, unsigned int n, struct seqlz_ctx* ctx, unsigned int* n_seq, unsigned int* n_lit) {
    unsigned int i = 0, k = 0, lits = 0;

    while (i < n) {
        unsigned int token = p[i++], ll = token >> 4, ml, b;

        if (ll == 15) {
            do {
                if (i >= n)
                    return -1;
                b = p[i++];
                ll += b;
            } while (b == 255);
        }
        if (ll > n - i || ll > SEQLZ_PAGE - lits || k == SEQLZ_MAX_SEQUENCES)
            return -1;
        __builtin_memcpy(ctx->literals + lits, p + i, ll);
        lits += ll;
        i += ll;
        ctx->seq[k].literals = (unsigned short)ll;
        ctx->seq[k].match = 0;
        ctx->seq[k].offset = 0;
        if (i == n) {
            k++;
            break;
        }
        if (n - i < 2)
            return -1;
        ctx->seq[k].offset = (unsigned short)(p[i] | (p[i + 1] << 8));
        i += 2;
        ml = (token & 15U) + 4U;
        if ((token & 15U) == 15) {
            do {
                if (i >= n)
                    return -1;
                b = p[i++];
                ml += b;
            } while (b == 255);
        }
        ctx->seq[k].match = (unsigned short)ml;
        k++;
    }
    *n_seq = k;
    *n_lit = lits;
    return 0;
}

static int compress(struct quetschn_params* p,
                    struct quetschn_stream* s,
                    const void* src,
                    unsigned int src_len,
                    void* dst,
                    unsigned int* dst_len,
                    int hc) {
    struct seqlz_ctx* ctx = s->context;
    unsigned int n_seq, n_lit, len;
    int ret;

    if (src_len != SEQLZ_PAGE)
        return -1;
    if (hc)
        ret = LZ4_compress_HC(src, (char*)ctx->lz4_out, (int)src_len, 2 * SEQLZ_PAGE, p->level, ctx->lz4_mem);
    else
        ret = LZ4_compress_fast(src, (char*)ctx->lz4_out, (int)src_len, 2 * SEQLZ_PAGE, p->level, ctx->lz4_mem);
    if (ret <= 0 || split(ctx->lz4_out, (unsigned int)ret, ctx, &n_seq, &n_lit))
        return -1;
    len = ctx->coded ? seqlz_encode_coded(p->drv_data, ctx->seq, n_seq, ctx->literals, n_lit, dst, *dst_len)
                     : seqlz_encode(p->drv_data, ctx->seq, n_seq, ctx->literals, n_lit, dst, *dst_len);
    if (!len)
        return -1;
    *dst_len = len;
    return 0;
}

static int compress_lz4(struct quetschn_params* p,
                        struct quetschn_stream* s,
                        const void* src,
                        unsigned int src_len,
                        void* dst,
                        unsigned int* dst_len) {
    return compress(p, s, src, src_len, dst, dst_len, 0);
}

static int compress_lz4hc(struct quetschn_params* p,
                          struct quetschn_stream* s,
                          const void* src,
                          unsigned int src_len,
                          void* dst,
                          unsigned int* dst_len) {
    return compress(p, s, src, src_len, dst, dst_len, 1);
}

static int decompress(struct quetschn_params* p,
                      struct quetschn_stream* s,
                      const void* src,
                      unsigned int src_len,
                      void* dst,
                      unsigned int* dst_len) {
    struct seqlz_ctx* ctx = s->context;

    quetschn_prefetch_page(src, src_len, dst, SEQLZ_PAGE);
    if (*dst_len < SEQLZ_PAGE || seqlz_decode_scratch(p->drv_data, src, src_len, dst, ctx ? ctx->scratch : 0))
        return -1;
    *dst_len = SEQLZ_PAGE;
    return 0;
}

static void fast_destroy(struct quetschn_stream* s) {
    quetschn_free(s->context, &s->allocated);
    s->context = NULL;
}

static int fast_create(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    s->context = quetschn_zalloc(sizeof(struct seqlz_state), &s->allocated);
    return s->context ? 0 : -1;
}

static int fast_compress(struct quetschn_params* p,
                         struct quetschn_stream* s,
                         const void* src,
                         unsigned int src_len,
                         void* dst,
                         unsigned int* dst_len) {
    unsigned int len;

    if (src_len != SEQLZ_PAGE)
        return -1;
    len = seqlz_compress(p->drv_data, s->context, src, dst, *dst_len);
    if (!len)
        return -1;
    *dst_len = len;
    return 0;
}

static int setup_own(struct quetschn_params* p) {
    return setup(p, &seqlz_default_own);
}

/* seqlz-fast's context is its hash table, it has no scratch: pages with coded literals are invalid */
static int fast_decompress(struct quetschn_params* p,
                           struct quetschn_stream* s,
                           const void* src,
                           unsigned int src_len,
                           void* dst,
                           unsigned int* dst_len) {
    (void)s;
    quetschn_prefetch_page(src, src_len, dst, SEQLZ_PAGE);
    if (*dst_len < SEQLZ_PAGE || seqlz_decode(p->drv_data, src, src_len, dst))
        return -1;
    *dst_len = SEQLZ_PAGE;
    return 0;
}

const struct quetschn_codec quetschn_codec_seqlz_fast = {
    "seqlz-fast",
    setup_own,
    release,
    fast_create,
    fast_destroy,
    fast_compress,
    fast_decompress,
};

/* EXPERIMENT: seqlz-fast with the literals Huffman coded too; the hash table and the scratch */
struct fast_lit_ctx {
    struct seqlz_state st;
    unsigned char scratch[SEQLZ_SCRATCH];
};

static int fast_lit_create(struct quetschn_params* p, struct quetschn_stream* s) {
    (void)p;
    s->context = quetschn_zalloc(sizeof(struct fast_lit_ctx), &s->allocated);
    return s->context ? 0 : -1;
}

static int fast_lit_compress(struct quetschn_params* p,
                             struct quetschn_stream* s,
                             const void* src,
                             unsigned int src_len,
                             void* dst,
                             unsigned int* dst_len) {
    struct fast_lit_ctx* c = s->context;
    unsigned int len;

    if (src_len != SEQLZ_PAGE)
        return -1;
    len = seqlz_compress_coded(p->drv_data, &c->st, src, dst, *dst_len, c->scratch);
    if (!len)
        return -1;
    *dst_len = len;
    return 0;
}

static int fast_lit_decompress(struct quetschn_params* p,
                               struct quetschn_stream* s,
                               const void* src,
                               unsigned int src_len,
                               void* dst,
                               unsigned int* dst_len) {
    struct fast_lit_ctx* c = s->context;

    quetschn_prefetch_page(src, src_len, dst, SEQLZ_PAGE);
    if (*dst_len < SEQLZ_PAGE || seqlz_decode_scratch(p->drv_data, src, src_len, dst, c->scratch))
        return -1;
    *dst_len = SEQLZ_PAGE;
    return 0;
}

const struct quetschn_codec quetschn_codec_seqlz_fast_lit = {
    "seqlz-fast-lit",
    setup_own,
    release,
    fast_lit_create,
    fast_destroy,
    fast_lit_compress,
    fast_lit_decompress,
};

const struct quetschn_codec quetschn_codec_seqlz = {
    "seqlz",
    setup_lz4,
    release,
    create_lz4,
    destroy,
    compress_lz4,
    decompress,
};

static int create_lz4hc_coded(struct quetschn_params* p, struct quetschn_stream* s) {
    int ret = create_lz4hc(p, s);

    if (!ret)
        ((struct seqlz_ctx*)s->context)->coded = 1;
    return ret;
}

/* EXPERIMENT: seqlz-hc with the literals Huffman coded too, for zram's recompression */
const struct quetschn_codec quetschn_codec_seqlz_hc_lit = {
    "seqlz-hc-lit",
    setup_lz4hc,
    release,
    create_lz4hc_coded,
    destroy,
    compress_lz4hc,
    decompress,
};

const struct quetschn_codec quetschn_codec_seqlz_hc = {
    "seqlz-hc",
    setup_lz4hc,
    release,
    create_lz4hc,
    destroy,
    compress_lz4hc,
    decompress,
};
