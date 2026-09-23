/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_ZRAM_CODEC_H
#define QUETSCHN_ZRAM_CODEC_H

/*
 * One compressor as zram uses it. Each implementation makes exactly the calls of the matching
 * drivers/block/zram/backend_*.c, without the zram plumbing around them.
 *
 * Like zram's zcomp, a codec works on a stream: memory allocated once per CPU, set up once, then used for
 * every page. The level is zram's algorithm_params level: acceleration for lz4, ignored by lzo, the
 * compression level for zstd.
 *
 * Plain C, and no libc types, because the implementations are compiled like kernel code (-nostdinc).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* zram's ZCOMP_PARAM_NOT_SET: use the codec's default level */
#define QUETSCHN_LEVEL_DEFAULT (-(1 << 30))

struct quetschn_stream {
    int level;
    void* workspace; /* workspace_size() bytes, cache line aligned, zeroed like vzalloc */
    __SIZE_TYPE__ workspace_size;
    unsigned long long state[16]; /* the codec's own pointers and parameters; C and C++ agree on its layout */
};

struct quetschn_codec {
    const char* name;

    /* Bytes zram allocates per CPU for one stream at this level, 0 if zram rejects the level. The
     * resolved level (default applied) is written back to *level. */
    __SIZE_TYPE__ (*workspace_size)(int* level, unsigned int page_size);

    /* Sets up a stream whose level and workspace are filled in. 0 on success. */
    int (*init)(struct quetschn_stream* s, unsigned int page_size);

    /* *dst_len is the capacity on input and the compressed length on output. 0 on success. */
    int (*compress)(struct quetschn_stream* s, const void* src, unsigned int src_len, void* dst, unsigned int* dst_len);

    /* *dst_len is the capacity on input and the decompressed length on output. 0 on success. */
    int (*decompress)(struct quetschn_stream* s, const void* src, unsigned int src_len, void* dst, unsigned int* dst_len);
};

extern const struct quetschn_codec quetschn_codec_lz4;
extern const struct quetschn_codec quetschn_codec_lzo;
extern const struct quetschn_codec quetschn_codec_lzo_rle;
extern const struct quetschn_codec quetschn_codec_zstd;

#ifdef __cplusplus
}
#endif

#endif
