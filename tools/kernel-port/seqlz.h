/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * seqlz: compression of single memory pages of 4 KiB, for zram. An LZ format
 * whose sequences and literals are Huffman coded with tables that are part of
 * the format, so a page carries none. The format is specified in
 * https://github.com/martinus/quetschn/blob/main/docs/format.md.
 */
#ifndef _LINUX_SEQLZ_H
#define _LINUX_SEQLZ_H

#include <asm/page.h>

/* the work memory of seqlz_compress(): the matcher's table of positions */
#define SEQLZ_MEM_COMPRESS 8192
/* the work memory of seqlz_decompress(): the decoded literals of a page */
#define SEQLZ_MEM_DECOMPRESS (PAGE_SIZE + 16)

/* the literals as they are, or Huffman coded where that saves enough */
#define SEQLZ_LEVEL_RAW 1
#define SEQLZ_LEVEL_CODED 2

/**
 * seqlz_compress() - Compress one page
 * @src: the page, PAGE_SIZE bytes
 * @dst: where the compressed page goes
 * @dst_len: the bytes at @dst; every page fits into 2 * PAGE_SIZE
 * @wrkmem: SEQLZ_MEM_COMPRESS bytes, as kmalloc() aligns them
 * @level: SEQLZ_LEVEL_RAW or SEQLZ_LEVEL_CODED
 *
 * Context: Any context. It does not sleep and allocates nothing.
 *
 * Return: the length of the compressed page, -E2BIG if it does not fit into
 * @dst_len bytes, which can happen up to 32 bytes before @dst is full, or
 * -EINVAL for another @level.
 */
int seqlz_compress(const void *src, void *dst, unsigned int dst_len,
		   void *wrkmem, int level);

/**
 * seqlz_decompress() - Decompress one page
 * @src: the compressed page
 * @src_len: its length
 * @dst: PAGE_SIZE bytes for the page
 * @wrkmem: SEQLZ_MEM_DECOMPRESS bytes
 *
 * Safe for any input: it never reads outside @src and @src_len, and never
 * writes outside @dst and @wrkmem, and its time is bounded by the page size.
 *
 * Context: Any context. It does not sleep and allocates nothing.
 *
 * Return: 0, or -EINVAL if @src is not a valid page.
 */
int seqlz_decompress(const void *src, unsigned int src_len, void *dst,
		     void *wrkmem);

#endif
