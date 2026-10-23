/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_INTERNAL_H
#define SEQLZ_INTERNAL_H

/*
 * What the files of the compressor and of the decompressor share, besides the
 * tables of seqlz_tables.h.
 */

#include "seqlz_tables.h"

/*
 * The bit operations and the short copies are the compiler's builtins, as in
 * lib/lz4 and lib/zstd. __builtin_ctzll() and __builtin_clz() instead of
 * __ffs64(), __fls() and fls(), which are inline assembly on x86-64 that the
 * compiler cannot look into.
 * __builtin_memcpy() for copies of 8 and 16 bytes, as LZ4_memcpy() and
 * ZSTD_memcpy(): it is always inlined, and memcpy() is not with every
 * CONFIG_FORTIFY_SOURCE, which makes each copy a call. __builtin_bswap64()
 * turns a literal stream's bits around once, in a register, see lit_flush().
 */

/*
 * How many literals the decoder takes from each stream after loading 8 bytes
 * of it: at least 56 bits are then left, enough for 5 codes of 10 bits.
 */
#define SEQLZ_LIT_ROUNDS (56U / SEQLZ_LIT_BITS)
/*
 * The encoder codes a block's literals only where that saves more than 1/16 of
 * them plus this many bytes. Decoding coded literals has a cost that does not
 * depend on how many there are: the literal table has to be read from memory,
 * and the literals decoded into a buffer first. A smaller minimum made blocks a
 * few bytes smaller and writes slower, most on in-order cores.
 */
#define SEQLZ_LIT_CODED_MIN 51U
/*
 * The buffer the decoder decodes coded literals into, one per CPU. 16 bytes
 * more than a block: the decoder copies literals 16 bytes at a time and may
 * read past the last one, and the last step of seqlz_decode_literals() writes
 * up to 8 decoded bytes past it.
 */
#define SEQLZ_SCRATCH (SEQLZ_BLOCK_SIZE + 16U)
/*
 * the largest stream: every 8th literal of a block, all with the longest code
 */
#define LIT_STREAM_MAX \
	((SEQLZ_BLOCK_SIZE / SEQLZ_LIT_STREAMS * SEQLZ_LIT_BITS + 7U) / 8U)
static_assert(LIT_STREAM_MAX < 1U << SEQLZ_SIZE_BITS_MAX,
	      "the widest size holds the largest stream");
static_assert((8U << SEQLZ_CLASS5_BITS) <= SEQLZ_BLOCK_SIZE,
	      "class 5's offsets are inside the block");

/*
 * The compressor finds the matches with its own matcher and writes each
 * sequence as soon as it is found, in one pass over the block.
 */
/*
 * The matcher's hash table has 1 << SEQLZ_HASH_BITS entries of 2 bytes, 8 KiB
 * for blocks of 4 KiB. With 2048 entries blocks were larger and the compressor
 * no faster; with 8192 in-order cores wrote blocks slower. For blocks of 16 KiB
 * it is 16 KiB, as lz4's table.
 */
#define SEQLZ_HASH_BITS (SEQLZ_BLOCK_BITS == 12 ? 12U : 13U)
/*
 * The levels of seqlz_compress(), 1 to SEQLZ_LEVELS, from the fastest to the
 * one with the smallest blocks. Level 1 keeps the literals raw. From
 * SEQLZ_LEVEL_CODED on they are Huffman coded where that saves enough, with
 * one of the fixed tables. SEQLZ_LEVEL_OWN looks one position ahead before it
 * takes a short match, extends matches backwards, gives a block a literal
 * table of its own where that saves enough, and codes the literals as deltas
 * where that looks like it saves bits, see SEQLZ_LIT_DELTA_AT.
 */
#define SEQLZ_LEVELS 3U
#define SEQLZ_LEVEL_CODED 2U
#define SEQLZ_LEVEL_OWN 3U

/* the token of a sequence with a match, see the top of seqlz_format.h */
static inline unsigned int seqlz_token(unsigned int ll, unsigned int ml,
				       unsigned int cls)
{
	unsigned int a = min(ll, SEQLZ_LL_CAP);
	unsigned int b = min(ml - SEQLZ_MIN_MATCH, SEQLZ_ML_CAP);

	return a + (b << SEQLZ_LL_BITS) +
	       (cls << (SEQLZ_LL_BITS + SEQLZ_ML_BITS));
}

/*
 * The matcher's hash table, one per CPU, cleared for each block. Once the
 * matcher is done it holds the raw literals or their deltas while they are
 * coded, and at SEQLZ_LEVEL_OWN, at its end, what the encoder needs for a
 * table of the block's own, see seqlz_code_literals().
 */
struct seqlz_state {
	union {
		u16 table[1U << SEQLZ_HASH_BITS];
		u8 literals[SEQLZ_BLOCK_SIZE];
		u64 work[(2U << SEQLZ_HASH_BITS) / 8U];
	};
};

/*
 * At SEQLZ_LEVEL_OWN the encoder writes LIT_REC bytes before each sequence's
 * literals: where they start in the block, how many there are and the offset of
 * the sequence before, a u16 each, little endian. Once the block is matched,
 * seqlz_delta_literals() picks the literals' delta from them, see
 * SEQLZ_LIT_DELTA_AT, and moves the literals down over them. They take room in
 * dst: a block that does not fit with them is matched again without them,
 * without deltas.
 */
#define LIT_REC 6U

/*
 * Prefetches every cache line of [p, p + size); size is a multiple of
 * PREFETCH_STEP. In a swap-in the decoder's tables are often no longer in
 * the cache: asking for all of their lines at once lets the misses overlap,
 * instead of one after the other as the decoder runs into them. 512 bytes per
 * iteration, 8 lines where they have 64 bytes: with one line per iteration,
 * the loop took more instructions than the prefetches.
 *
 * A table need not start a cache line: struct seqlz_tables is aligned to 8
 * bytes, and the literal and length tables are not at a multiple of 64 in it.
 * Then the loop misses the line of the table's last bytes, and the prefetch of
 * end - 1 asks for it. Where p starts a line, the loop asked for it already.
 */
#define PREFETCH_STEP 512U

static inline void prefetch_lines(const void *p, unsigned long size)
{
	const u8 *q = (const u8 *)p; /* a cast for the C++ of the harness */
	const u8 *const end = q + size;
	unsigned int k;

	for (; q < end; q += PREFETCH_STEP)
		for (k = 0; k < PREFETCH_STEP; k += L1_CACHE_BYTES)
			prefetch(q + k);
	prefetch(end - 1);
}

/* prefetch_lines() takes multiples of PREFETCH_STEP */
static_assert(sizeof_field(struct token_table, decode) % PREFETCH_STEP == 0,
	      "the token table");
static_assert(sizeof_field(struct value_table, decode) % PREFETCH_STEP == 0,
	      "the length values' tables");
static_assert(sizeof_field(struct lit_table, decode) % PREFETCH_STEP == 0,
	      "the literal tables");

/* the coded literals, see seqlz_compress_literals.c */
unsigned int seqlz_delta_literals(u8 *d, u8 **lit_end, const u8 *block,
				  u8 *deltas);
unsigned int seqlz_code_literals(u8 *d, unsigned int len, u8 *spare);
unsigned int seqlz_code_raw_literals(u8 *d, unsigned int len, const u8 *raw);
unsigned int seqlz_code_own(u8 *d, unsigned int len, struct seqlz_state *st,
			    unsigned int delta);

/* the coded literals, see seqlz_decompress_literals.c */
const u8 *seqlz_decode_literals(const u8 *s, unsigned int src_len,
				unsigned int n_lit, u8 *out, u8 *block,
				unsigned int *delta);

#endif
