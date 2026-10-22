/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * seqlz: compression of single blocks of 4 KiB, for zram, where a block is one
 * memory page with 4 KiB pages. An LZ format whose sequences and literals are
 * Huffman coded with tables that are part of the format, so a block carries
 * none. Documentation/staging/seqlz.rst specifies the format, which calls a
 * block a page. The format may change from one kernel to the next, so a
 * compressed block is only for the kernel that wrote it, not for storage.
 */
#ifndef _LINUX_SEQLZ_H
#define _LINUX_SEQLZ_H

/* the bytes of a block, whatever PAGE_SIZE is */
#define SEQLZ_BLOCK_SIZE 4096
/* room at dst with which seqlz_compress() never fails */
#define SEQLZ_COMPRESS_BOUND (2 * SEQLZ_BLOCK_SIZE)

/*
 * the work memory of seqlz_compress(): the matcher's table of positions, which
 * holds the literals and the work of a block's own literal table once the
 * matcher is done
 */
#define SEQLZ_MEM_COMPRESS 8192
/* the work memory of seqlz_decompress(): the decoded literals of a block */
#define SEQLZ_MEM_DECOMPRESS (SEQLZ_BLOCK_SIZE + 16)

/*
 * The levels of seqlz_compress(), from the fastest to the smallest blocks: 1
 * keeps the literals as they are, 2 Huffman codes them where that saves
 * enough, 3 also searches longer for matches, can store the literals as their
 * difference to an earlier byte and codes them with a table of the block's own
 * where that saves more.
 */
#define SEQLZ_LEVEL_MIN 1
#define SEQLZ_LEVEL_DEFAULT 2
#define SEQLZ_LEVEL_MAX 3

/**
 * seqlz_compress() - Compress one block
 * @src: the block, SEQLZ_BLOCK_SIZE bytes
 * @dst: where the compressed block goes
 * @dst_len: the bytes at @dst; SEQLZ_COMPRESS_BOUND are always enough
 * @wrkmem: SEQLZ_MEM_COMPRESS bytes, as kmalloc() aligns them
 * @level: from SEQLZ_LEVEL_MIN to SEQLZ_LEVEL_MAX
 *
 * Context: Any context. It does not sleep and allocates nothing.
 *
 * Return: the length of the compressed block, -E2BIG if it does not fit into
 * @dst_len bytes, which never happens with SEQLZ_COMPRESS_BOUND, or -EINVAL for
 * another @level. With less room, -E2BIG can come for a block whose coded
 * literals would fit: it is written with raw literals first.
 */
int seqlz_compress(const void *src, void *dst, unsigned int dst_len,
		   void *wrkmem, int level);

/**
 * seqlz_decompress() - Decompress one block
 * @src: the compressed block
 * @src_len: its length
 * @dst: SEQLZ_BLOCK_SIZE bytes for the block
 * @wrkmem: SEQLZ_MEM_DECOMPRESS bytes, or NULL if every block it gets was
 *          compressed at SEQLZ_LEVEL_MIN
 *
 * Safe for any input: it never reads outside @src and @src_len, and never
 * writes outside @dst and @wrkmem, and its time is bounded by the block size.
 *
 * Context: Any context. It does not sleep and allocates nothing.
 *
 * Return: 0, or -EINVAL if @src is not a valid block, or a block with coded
 * literals and @wrkmem is NULL.
 */
int seqlz_decompress(const void *src, unsigned int src_len, void *dst,
		     void *wrkmem);

#endif
