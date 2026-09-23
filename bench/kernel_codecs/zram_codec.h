/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_ZRAM_CODEC_H
#define QUETSCHN_ZRAM_CODEC_H

/*
 * One compressor as zram uses it. Each implementation makes exactly the calls of the matching
 * drivers/block/zram/backend_*.c, without the zram plumbing around them.
 *
 * Plain C, and no libc types, because the implementations are compiled like kernel code (-nostdinc).
 */

#ifdef __cplusplus
extern "C" {
#endif

struct quetschn_codec {
    const char* name;

    /* bytes of compression workspace zram allocates per CPU for this codec */
    __SIZE_TYPE__ workspace_size;

    /* *dst_len is the capacity on input and the compressed length on output. 0 on success. */
    int (*compress)(const void* src, unsigned int src_len, void* dst, unsigned int* dst_len, void* workspace);

    /* *dst_len is the capacity on input and the decompressed length on output. 0 on success. */
    int (*decompress)(const void* src, unsigned int src_len, void* dst, unsigned int* dst_len);
};

extern const struct quetschn_codec quetschn_codec_lz4;
extern const struct quetschn_codec quetschn_codec_lzo;
extern const struct quetschn_codec quetschn_codec_lzo_rle;

#ifdef __cplusplus
}
#endif

#endif
