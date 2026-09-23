/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_ZRAM_CODEC_H
#define QUETSCHN_ZRAM_CODEC_H

/*
 * One compressor as zram uses it. Each implementation makes exactly the calls of the matching
 * drivers/block/zram/backend_*.c, without the zram plumbing around them, and the same split as zram's
 * zcomp: per-device params (level, dictionary, what the codec prepares from them), and a per-CPU stream
 * that is created once and then used for every page.
 *
 * Plain C, and no libc types, because the implementations are compiled like kernel code (-nostdinc).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* zram's ZCOMP_PARAM_NOT_SET: use the codec's default level */
#define QUETSCHN_LEVEL_DEFAULT (-(1 << 30))

/* struct zcomp_params: one per zram device and algorithm */
struct quetschn_params {
    const void* dict;
    __SIZE_TYPE__ dict_size;
    int level;          /* zram's algorithm_params level; setup_params() replaces QUETSCHN_LEVEL_DEFAULT */
    unsigned page_size; /* PAGE_SIZE */
    void* drv_data;
    __SIZE_TYPE__ allocated; /* bytes the codec allocated for these params, e.g. a prepared dictionary */
};

/* struct zcomp_ctx: one per CPU */
struct quetschn_stream {
    void* context;
    __SIZE_TYPE__ allocated; /* bytes the codec allocated for this stream */
};

struct quetschn_codec {
    const char* name;

    /* 0 on success, non-zero where zram's setup_params fails, e.g. for a level it rejects */
    int (*setup_params)(struct quetschn_params* p);
    void (*release_params)(struct quetschn_params* p);

    /* 0 on success */
    int (*create)(struct quetschn_params* p, struct quetschn_stream* s);
    void (*destroy)(struct quetschn_stream* s);

    /* *dst_len is the capacity on input and the compressed length on output. 0 on success. */
    int (*compress)(struct quetschn_params* p,
                    struct quetschn_stream* s,
                    const void* src,
                    unsigned int src_len,
                    void* dst,
                    unsigned int* dst_len);

    /* *dst_len is the capacity on input and the decompressed length on output. 0 on success. */
    int (*decompress)(struct quetschn_params* p,
                      struct quetschn_stream* s,
                      const void* src,
                      unsigned int src_len,
                      void* dst,
                      unsigned int* dst_len);
};

extern const struct quetschn_codec quetschn_codec_lz4;
extern const struct quetschn_codec quetschn_codec_lz4hc;
extern const struct quetschn_codec quetschn_codec_lzo;
extern const struct quetschn_codec quetschn_codec_lzo_rle;
extern const struct quetschn_codec quetschn_codec_zstd;
/* the Phase 2b decoder spike, spike/zram_spike.c */
extern const struct quetschn_codec quetschn_codec_spike_switch;
extern const struct quetschn_codec quetschn_codec_spike_branchless;
extern const struct quetschn_codec quetschn_codec_spike_zeroskip;
extern const struct quetschn_codec quetschn_codec_spike_slots;
/* candidate designs, explore/zram_explore.c */
extern const struct quetschn_codec quetschn_codec_shuffle_lz4;
extern const struct quetschn_codec quetschn_codec_bdelta;
extern const struct quetschn_codec quetschn_codec_zstd_nolit;
extern const struct quetschn_codec quetschn_codec_seqlz;
extern const struct quetschn_codec quetschn_codec_seqlz_hc;
extern const struct quetschn_codec quetschn_codec_seqlz_fast;
extern const struct quetschn_codec quetschn_codec_bytelz;
/* optional, explore/zram_memlz.c with QUETSCHN_MEMLZ_DIR */
extern const struct quetschn_codec quetschn_codec_memlz;

/*
 * Stands in for kzalloc/vzalloc/kvzalloc: zeroed, 64 byte aligned, NULL on failure. The size is added
 * to *counter, and subtracted again by quetschn_free(), so the harness can report how much memory a
 * codec holds per device and per CPU. Implemented in alloc.c with libc.
 */
void* quetschn_zalloc(__SIZE_TYPE__ size, __SIZE_TYPE__* counter);
void quetschn_free(void* p, __SIZE_TYPE__* counter);

#ifdef __cplusplus
}
#endif

#endif
