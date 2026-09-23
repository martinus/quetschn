/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_SHUFFLE_H
#define QUETSCHN_EXPLORE_SHUFFLE_H

/*
 * Byte shuffle with a stride of 8, as blosc does for arrays: byte j of every 8-byte word goes to plane
 * j. For a page of n words, dst[j * n + i] = src[8 * i + j]. Pointers into the same region then become
 * long runs of equal bytes in the high planes, which a byte-oriented LZ finds. Both directions work on
 * 8x8-byte blocks, transposed with 64-bit shifts and masks, no SIMD, so they compile with the kernel's
 * flags. size must be a multiple of 64. Little endian hosts only.
 */

#ifdef __cplusplus
extern "C" {
#endif

void shuffle8(const void* src, void* dst, unsigned int size);
void unshuffle8(const void* src, void* dst, unsigned int size);

#ifdef __cplusplus
}
#endif

#endif
