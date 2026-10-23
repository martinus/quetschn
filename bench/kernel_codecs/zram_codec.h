/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_ZRAM_CODEC_H
#define SEQLZ_ZRAM_CODEC_H

/*
 * One compressor as zram uses it. Each implementation makes exactly the calls of the matching
 * drivers/block/zram/backend_*.c, without the zram plumbing around them, and the same split as zram's
 * zcomp: per-device params (level, dictionary, what the codec prepares from them), and per-CPU contexts
 * that are created once and then used for every page. The contexts are split as in Sergey
 * Senozhatsky's series "zram: split zcomp into separate R/W streams" (October 2026): one for
 * compression, create_cctx, and an optional one for decompression, create_dctx.
 *
 * Plain C, and no libc types, because the implementations are compiled like kernel code (-nostdinc).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* zram's ZCOMP_PARAM_NOT_SET: use the codec's default level */
#define BENCH_LEVEL_DEFAULT (-(1 << 30))

/* struct zcomp_params: one per zram device and algorithm */
struct bench_params {
    const void* dict;
    __SIZE_TYPE__ dict_size;
    int level;          /* zram's algorithm_params level; setup_params() replaces BENCH_LEVEL_DEFAULT */
    unsigned page_size; /* PAGE_SIZE */
    void* drv_data;
    __SIZE_TYPE__ allocated; /* bytes the codec allocated for these params, e.g. a prepared dictionary */
};

/* struct zcomp_ctx: one per CPU and direction */
struct bench_stream {
    void* context;
    __SIZE_TYPE__ allocated; /* bytes the codec allocated for this stream */
};

struct bench_codec {
    const char* name;

    /* 0 on success, non-zero where zram's setup_params fails, e.g. for a level it rejects */
    int (*setup_params)(struct bench_params* p);
    void (*release_params)(struct bench_params* p);

    /* the context compress() gets, one per CPU. 0 on success */
    int (*create_cctx)(struct bench_params* p, struct bench_stream* s);
    void (*destroy_cctx)(struct bench_stream* s);

    /*
     * The context decompress() gets, one per CPU, or NULL. A codec without it gets the compression
     * context in decompress(), as every zram backend did before the split: the glue of lz4, lzo and
     * zstd copies a kernel from before it, with one context for both directions.
     */
    int (*create_dctx)(struct bench_params* p, struct bench_stream* s);
    void (*destroy_dctx)(struct bench_stream* s);

    /* s is the compression context. *dst_len is the capacity on input and the compressed length on
     * output. 0 on success. */
    int (*compress)(struct bench_params* p,
                    struct bench_stream* s,
                    const void* src,
                    unsigned int src_len,
                    void* dst,
                    unsigned int* dst_len);

    /* s is the decompression context, see create_dctx. *dst_len is the capacity on input and the
     * decompressed length on output. 0 on success. */
    int (*decompress)(struct bench_params* p,
                      struct bench_stream* s,
                      const void* src,
                      unsigned int src_len,
                      void* dst,
                      unsigned int* dst_len);
};

extern const struct bench_codec bench_codec_lz4;
extern const struct bench_codec bench_codec_lz4hc;
extern const struct bench_codec bench_codec_lzo;
extern const struct bench_codec bench_codec_lzo_rle;
extern const struct bench_codec bench_codec_zstd;
extern const struct bench_codec bench_codec_seqlz_1;
extern const struct bench_codec bench_codec_seqlz_2;
extern const struct bench_codec bench_codec_seqlz_3;
extern const struct bench_codec bench_codec_lz4_prefetch;

/*
 * What zram could do before any decompression: ask for the cache lines of the compressed page and of
 * the destination page, for writing, at once, so that their misses overlap. With the page and the
 * output flushed, as the harness's cold decode does, writing into cold lines was the main wait of the
 * decoders (docs/explored-designs.md, seqlz's third decoder round).
 */
static inline void bench_prefetch_page(const void* src, unsigned int src_len, void* dst, unsigned int dst_len) {
    const char* q;
    char* o;

    for (q = (const char*)src + 64; q < (const char*)src + src_len; q += 64)
        __builtin_prefetch(q);
    for (o = (char*)dst; o < (char*)dst + dst_len; o += 64)
        __builtin_prefetch(o, 1);
}

/*
 * bench_zalloc() stands in for kzalloc/kvzalloc, bench_vzalloc() for vzalloc: zeroed, 64 byte
 * aligned, NULL on failure. What the kernel's allocator hands out for the size, not the size, is added
 * to *counter, and subtracted again by bench_free(), so the harness can report how much memory a
 * codec holds per device and per CPU. kvzalloc() is counted as kmalloc(), which it tries first; it
 * takes vmalloc() only when that fails. Implemented in alloc.c with libc.
 */
void* bench_zalloc(__SIZE_TYPE__ size, __SIZE_TYPE__* counter);
void* bench_vzalloc(__SIZE_TYPE__ size, __SIZE_TYPE__* counter);
void bench_free(void* p, __SIZE_TYPE__* counter);
/* what kmalloc() and vmalloc() hand out for size bytes, the page size of SEQLZ_PAGE_BITS */
__SIZE_TYPE__ bench_kmalloc_size(__SIZE_TYPE__ size);
__SIZE_TYPE__ bench_vmalloc_size(__SIZE_TYPE__ size);

#ifdef __cplusplus
}
#endif

#endif
