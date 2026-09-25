/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_LZ4PAGE_H
#define QUETSCHN_EXPLORE_LZ4PAGE_H

/*
 * EXPERIMENT: lz4's block format, written by a compressor for pages, read by the kernel's
 * LZ4_decompress_safe() as it is. The lzo-rle route: no new decoder, a compressor that knows it has 4
 * KiB and nothing else. Its matcher is seqlz_find's, the last offset and a hash of 5 bytes, with lz4's
 * rules for the end of a block: the last 5 bytes are literals, the last match starts at least 12
 * bytes before the end.
 */

/* 12 for 4 KiB pages, 14 for 16 KiB, set for the whole build (CMake's QUETSCHN_PAGE_BITS) */
#ifndef QUETSCHN_PAGE_BITS
#    define QUETSCHN_PAGE_BITS 12
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LZ4PAGE_TWO_WAY 1U /* the 2 newest positions per hash, the longer match wins */
#define LZ4PAGE_LAZY 2U    /* a match found, the one at the next position if that is longer */

/* The table: 1 << LZ4PAGE_HASH_BITS unsigned shorts. */
#define LZ4PAGE_HASH_BITS (QUETSCHN_PAGE_BITS == 12 ? 12U : 13U) /* page_lz.h's */

/* Compresses one page of 1 << QUETSCHN_PAGE_BITS bytes into lz4's block format. dst_cap at least
 * two pages. Returns the length. */
unsigned int lz4page_compress(unsigned short* table, const void* src, void* dst, unsigned int dst_cap, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif
