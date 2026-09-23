/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_SPIKE_WK64_H
#define QUETSCHN_SPIKE_WK64_H

/*
 * Phase 2b decoder latency spike (PLAN.md): a WKdm-style format with 64-bit words, and nothing else.
 * Throwaway code, only to measure whether a word-model decoder can beat lz4's cold p99. 4 KiB pages only.
 *
 * Each of the 512 words of a page gets a 2-bit tag:
 *   ZERO     the word is 0
 *   EXACT    the word is in the 16-entry table of recent words, at the slot given by a 4-bit index
 *   PARTIAL  the high 32 bits match that table entry, the low 32 bits are stored
 *   MISS     the whole word is stored
 * The table slot of a word is a hash of its high 32 bits. After each word, encoder and decoder both
 * store the word into its slot, so the table holds the most recent word for each high-bits hash.
 *
 * Layout, all little endian:
 *   128 bytes    tags, 4 per byte, word 0 in the lowest 2 bits of byte 0
 *   ceil(n/2)    table indices of the EXACT and PARTIAL words, 2 per byte, first in the low nibble
 *   4 * partial  low 32 bits of the PARTIAL words
 *   8 * miss     the MISS words
 * The tags alone determine the length of every section, so the decoder checks the total length once and
 * needs no bounds checks in the loop.
 *
 * Freestanding C, no libc, so it compiles with the kernel's flags like the codecs it is compared with.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define WK64_PAGE_SIZE 4096U
#define WK64_WORDS (WK64_PAGE_SIZE / 8U)
#define WK64_TAG_BYTES (WK64_WORDS / 4U)
/* all MISS: tags, no indices, 512 full words */
#define WK64_MAX_COMPRESSED (WK64_TAG_BYTES + 8U * WK64_WORDS)

/* Compresses one page. Returns the compressed length, or 0 if dst_cap is too small. */
unsigned int wk64_compress(const void* src, void* dst, unsigned int dst_cap);

/* Both return 0 on success and -1 if src is not a valid compressed page. They never read outside
 * [src, src + src_len) and never write outside [dst, dst + WK64_PAGE_SIZE). */
int wk64_decompress_switch(const void* src, unsigned int src_len, void* dst);
int wk64_decompress_branchless(const void* src, unsigned int src_len, void* dst);
/* like switch, and a zero tag byte writes its 4 zero words at once */
int wk64_decompress_zeroskip(const void* src, unsigned int src_len, void* dst);

#ifdef __cplusplus
}
#endif

#endif
