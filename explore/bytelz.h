/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_EXPLORE_BYTELZ_H
#define QUETSCHN_EXPLORE_BYTELZ_H

/*
 * bytelz: seqlz-fast's matcher with a byte oriented format instead of Huffman codes, to find out
 * whether it decodes at lz4's speed. `quetschn-lz-analysis --codec seqlz` costed this layout at 29.4%
 * Σ zsmalloc cost on the matches of seqlz-fast, lz4 34.5%, seqlz 26.4% (docs/explored-designs.md).
 *
 * A page is a series of sequences, each:
 *   token               bits 0-2: literal length ll, 7 means 7 + an extension
 *                       bits 3-5: match length - 4, 7 means 7 + an extension
 *                       bits 6-7: the offset: 0 the last offset, 1 the one before, 2 one byte follows
 *                       (offset - 1), 3 two bytes follow (offset, little endian)
 *   extension of ll     if ll is 7: 7 bits per byte, least significant first, the top bit set while
 *                       another byte follows, at most 3 bytes
 *   literals            ll bytes
 *   offset              0, 1 or 2 bytes, see the token
 *   extension of ml     as the one of ll
 * The last offset and the one before start as 1 and 4. The one before sends the last offset back to
 * second place, a new one does the same; the last offset changes nothing.
 * The last sequence ends where its literals reach the end of the page; its token has 0 in bits 3-7 and
 * nothing follows its literals. No header: the decoder knows the page size and the input length.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define BYTELZ_PAGE 4096U

struct bytelz_state {
    unsigned short table[1U << 12]; /* the matcher's hash table, cleared for each page */
};

/* Compresses one page. dst_cap must be at least two pages, which is always enough. Returns the
 * length, or 0 if dst_cap is smaller. */
unsigned int bytelz_compress(struct bytelz_state* st, const void* src, void* dst, unsigned int dst_cap);

/* 0 on success, -1 if src is not a valid page. Never reads outside [src, src + src_len) and never
 * writes outside [dst, dst + BYTELZ_PAGE). */
int bytelz_decode(const void* src, unsigned int src_len, void* dst);

#ifdef __cplusplus
}
#endif

#endif
