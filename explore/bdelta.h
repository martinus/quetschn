/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_BDELTA_H
#define QUETSCHN_EXPLORE_BDELTA_H

/*
 * Base + delta per 64-byte block, after "Base-Delta-Immediate Compression" (Pekhimenko et al., PACT
 * 2012), a design for compressing CPU cache lines where decompression must take a few cycles. A 4 KiB
 * page is 64 blocks of 8 words. Each block gets a 4-bit mode:
 *
 *   mode  name   payload                                      bytes
 *   0     zero   nothing                                      0
 *   1     rep    one 64-bit word, repeated 8 times            8
 *   2     b8d1   64-bit base, 8-bit mask, 8 x 1-byte delta    17
 *   3     b8d2   64-bit base, 8-bit mask, 8 x 2-byte delta    25
 *   4     b8d4   64-bit base, 8-bit mask, 8 x 4-byte delta    41
 *   5     b4d1   32-bit base, 16-bit mask, 16 x 1-byte delta  22
 *   6     b4d2   32-bit base, 16-bit mask, 16 x 2-byte delta  38
 *   7     raw    the 64 bytes                                 64
 *
 * Deltas are signed. A value is base + delta where its mask bit is set, and delta alone (relative to
 * zero, BDI's "immediate") where it is clear, so small integers and pointers mix in one block.
 *
 * Layout: 32 bytes of modes, block 0 in the low nibble of byte 0, then the payloads in block order.
 * The modes determine the total length, the decoder checks it once. Little endian hosts only.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define BDELTA_PAGE_SIZE 4096U
#define BDELTA_BLOCKS (BDELTA_PAGE_SIZE / 64U)
#define BDELTA_HEADER (BDELTA_BLOCKS / 2U)
#define BDELTA_MAX_COMPRESSED (BDELTA_HEADER + BDELTA_PAGE_SIZE)

/* Returns the compressed length, or 0 if dst_cap is too small. */
unsigned int bdelta_compress(const void* src, void* dst, unsigned int dst_cap);

/* 0 on success, -1 if src is not a valid compressed page. Never reads outside [src, src + src_len)
 * and never writes outside [dst, dst + BDELTA_PAGE_SIZE). */
int bdelta_decompress(const void* src, unsigned int src_len, void* dst);

#ifdef __cplusplus
}
#endif

#endif
