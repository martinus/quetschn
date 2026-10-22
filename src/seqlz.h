/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_H
#define SEQLZ_H

/*
 * seqlz for userspace: the harness, the tests and the tools call these. The
 * kernel has its own interface, see tools/kernel-port/seqlz.h.
 */

#include "seqlz_internal.h"

/* a sequence as the matcher finds it, see seqlz_find() */
struct seqlz_sequence {
	u16 literals;
	u16 match; /* 0 for the last sequence */
	u16 offset;
};

/**
 * seqlz_tables_build() - Build the codes and decode tables from code lengths
 * @t: the tables to write
 * @lengths: the code lengths, e.g. seqlz_default_lengths
 *
 * The codec uses seqlz_tables, which a program for the build host writes with
 * this from seqlz_default_lengths, see tools/gen-tables/. The tests build
 * others.
 *
 * Return: 0, or -EINVAL if the lengths are not a complete prefix code: a code
 * longer than its table allows, more codes than fit, gaps, or none at all; or
 * if a symbol the encoder may have to write has no code.
 */
int seqlz_tables_build(struct seqlz_tables *t,
		       const struct seqlz_lengths *lengths);

/*
 * the code lengths of the format, see seqlz_default_tables.c and
 * seqlz_lit_sets.c
 */
extern const struct seqlz_lengths seqlz_default_lengths;
extern const u8 seqlz_lit_sets[SEQLZ_LIT_SETS][256];
extern const u8 seqlz_own_lengths[SEQLZ_OWN_SYMBOLS];

/**
 * seqlz_decode() - Decompress one block
 * @src: the compressed block
 * @src_len: its length
 * @dst: SEQLZ_BLOCK_SIZE bytes for the block
 * @scratch: SEQLZ_SCRATCH bytes for coded literals, or NULL
 *
 * Safe for any input: it never reads outside @src and @src_len, and never
 * writes outside @dst and @scratch. Without @scratch, blocks with coded
 * literals are not valid.
 *
 * Return: 0, or -EINVAL if @src is not a valid block for the tables.
 */
int seqlz_decode(const void *src, unsigned int src_len, void *dst,
		 void *scratch);

/* every sequence but the last covers at least 4 bytes of the block */
#define SEQLZ_MAX_SEQUENCES (SEQLZ_BLOCK_SIZE / 4U + 1U)

/**
 * seqlz_find() - The sequences of a block, as the compressor finds them
 * @st: the matcher's state
 * @src: the block, SEQLZ_BLOCK_SIZE bytes
 * @seq: room for SEQLZ_MAX_SEQUENCES sequences
 * @level: the level of seqlz_compress() whose matcher to use
 *
 * For tests and tools. The last sequence has no match; the literals are the
 * bytes of the block no match covers.
 *
 * Return: the number of sequences, or 0 if @level is not one.
 */
unsigned int seqlz_find(struct seqlz_state *st, const void *src,
			struct seqlz_sequence *seq, unsigned int level);

/**
 * seqlz_encode() - Write a compressed block from given sequences
 * @st: work memory for a block's own literal table at SEQLZ_LEVEL_OWN, or NULL
 *      below that level
 * @seq: the sequences, the last one with match 0
 * @n: how many
 * @literals: the literals of all sequences, one after the other
 * @n_literals: how many
 * @block: the block the sequences describe, for the deltas of SEQLZ_LEVEL_OWN,
 *        or NULL: then the literals are not coded as deltas
 * @dst: where the compressed block goes
 * @dst_cap: the bytes at @dst
 * @level: what to do with the literals, as seqlz_compress() at that level
 *
 * For tests and tools, which make their own sequences; zram uses
 * seqlz_compress().
 *
 * The sequences describe a block if each but the last has a match of at least
 * 4 bytes, with an offset from 1 to the bytes of the block before the match,
 * and the literals and matches add up to the block, with all @n_literals
 * literals used.
 *
 * Return: the length of the compressed block, or 0 if it does not fit into
 * @dst_cap bytes, the sequences do not describe a block, or @level is not one.
 */
unsigned int seqlz_encode(struct seqlz_state *st,
			  const struct seqlz_sequence *seq, unsigned int n,
			  const u8 *literals, unsigned int n_literals,
			  const void *block, void *dst, unsigned int dst_cap,
			  unsigned int level);

/**
 * seqlz_compress() - Compress one block
 * @st: the matcher's state, one per CPU
 * @src: the block, SEQLZ_BLOCK_SIZE bytes
 * @dst: where the compressed block goes
 * @dst_cap: the bytes at @dst
 * @level: from 1 to SEQLZ_LEVELS, see there
 *
 * Two pages, as zram's buffer has, are always enough, even for random data,
 * see the encoder in seqlz_compress.c. A smaller @dst works too: a block that
 * does not fit is an error, which can happen up to 32 bytes before @dst_cap is
 * full.
 * The block is written with raw literals first, so it has to fit that way; the
 * coded literals then go over the raw ones, which @st holds meanwhile. At
 * SEQLZ_LEVEL_OWN the encoder needs 6 bytes more per sequence with literals for
 * their deltas; without that room the block is written without them. The same
 * bytes as seqlz_encode() for the sequences of seqlz_find() and @src, at the
 * same level.
 *
 * Return: the length of the compressed block, or 0 if it does not fit into
 * @dst_cap bytes, or @level is not one.
 */
unsigned int seqlz_compress(struct seqlz_state *st, const void *src, void *dst,
			    unsigned int dst_cap, unsigned int level);

#endif
