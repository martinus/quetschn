/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * seqlz: compression of single memory pages of 4 KiB, for zram. An LZ format
 * whose sequences and literals are Huffman coded with tables that are part of
 * the format, so a page carries none. Documentation/staging/seqlz.rst specifies
 * the format.
 */
#ifndef _LINUX_SEQLZ_H
#define _LINUX_SEQLZ_H

#include <asm/page.h>

/*
 * the work memory of seqlz_compress(): the matcher's table of positions, which
 * holds the literals and the work of a page's own literal table once the
 * matcher is done
 */
#define SEQLZ_MEM_COMPRESS 8192
/* the work memory of seqlz_decompress(): the decoded literals of a page */
#define SEQLZ_MEM_DECOMPRESS (PAGE_SIZE + 16)

/*
 * The levels of seqlz_compress(), from the fastest to the smallest pages: 1
 * keeps the literals as they are, 2 Huffman codes them where that saves
 * enough, 3 also searches longer for matches and codes the literals with a
 * table of the page's own where that saves more.
 */
#define SEQLZ_LEVEL_MIN 1
#define SEQLZ_LEVEL_DEFAULT 2
#define SEQLZ_LEVEL_MAX 3

/**
 * seqlz_compress() - Compress one page
 * @src: the page, PAGE_SIZE bytes
 * @dst: where the compressed page goes
 * @dst_len: the bytes at @dst; every page fits into 2 * PAGE_SIZE
 * @wrkmem: SEQLZ_MEM_COMPRESS bytes, as kmalloc() aligns them
 * @level: from SEQLZ_LEVEL_MIN to SEQLZ_LEVEL_MAX
 *
 * Context: Any context. It does not sleep and allocates nothing.
 *
 * Return: the length of the compressed page, -E2BIG if it does not fit into
 * @dst_len bytes, or -EINVAL for another @level. The page is written with its
 * literals raw first, also at the levels that code them, and -E2BIG comes as
 * soon as that is within 32 bytes of @dst_len, even where the page with coded
 * literals would be smaller.
 */
int seqlz_compress(const void *src, void *dst, unsigned int dst_len,
		   void *wrkmem, int level);

/**
 * seqlz_decompress() - Decompress one page
 * @src: the compressed page
 * @src_len: its length
 * @dst: PAGE_SIZE bytes for the page
 * @wrkmem: SEQLZ_MEM_DECOMPRESS bytes, or NULL if every page it gets was
 *          compressed at SEQLZ_LEVEL_MIN
 *
 * Safe for any input: it never reads outside @src and @src_len, and never
 * writes outside @dst and @wrkmem, and its time is bounded by the page size.
 *
 * Context: Any context. It does not sleep and allocates nothing.
 *
 * Return: 0, or -EINVAL if @src is not a valid page, or a page with coded
 * literals and @wrkmem is NULL.
 */
int seqlz_decompress(const void *src, unsigned int src_len, void *dst,
		     void *wrkmem);

#endif
