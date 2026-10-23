/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_FORMAT_H
#define SEQLZ_FORMAT_H

#include "seqlz_compat.h"

/*
 * seqlz compresses one block at a time, a memory page for zram. Like lz4, a
 * block is a list of sequences: some literal bytes, copied as they are, then a
 * match, a copy of bytes that came earlier in the same block. lz4 writes the
 * numbers of a sequence in whole bytes. seqlz writes them with Huffman codes,
 * and the code tables are fixed: trained once on real memory pages and compiled
 * in, so a block carries no table of its own. That makes blocks smaller than
 * lz4's, and decoding the codes costs time.
 *
 * A sequence has three numbers:
 *   ll:     the number of literals before the match
 *   ml:     the length of the match, at least 4 bytes; 0 for the last sequence
 *   offset: how far back the match starts, 1 to the bytes before it
 *
 * Most of them are small, and many offsets are the same as the one before. So
 * ll, ml and a class of the offset share one Huffman symbol, the token, and one
 * table lookup in the decoder gives all three:
 *   token = min(ll, 15) + 16 * min(ml - 4, 31) + 512 * class
 * which are 3072 symbols. A rare token has no code; it is sent as the code of
 * the escape, followed by the token's number in SEQLZ_ESCAPE_BITS bits.
 *
 * The offset classes, and the bits of the offset that follow the token:
 *   0:    the offset of the sequence before, 1 at the start of a block; none
 *   1:    2; the offset divided by 2, 1 bit, which is always 1: 0 is
 *         offset 0, invalid. With the bit the decoder takes every class's
 *         offset with the same shift and mask.
 *   2:    below 256; 8 bits
 *   3:    below the block size; 12 bits for blocks of 4 KiB
 *   4, 5: a multiple of 8, below 256 or below 2048; the offset divided by 8,
 *         5 and 8 bits
 * Memory pages are full of 8-byte aligned data, so many offsets are multiples
 * of 8; 16-bit values make offset 2 the most frequent single offset.
 *
 * An ll of 15 or more is followed by ll - 15 as a length value, an ml - 4 of 31
 * or more by ml - 4 - 31. A length value below 16 is its own symbol. A larger
 * one is sent as the position of its highest bit, symbol 12 + bit_width(v) - 1,
 * followed by the bits below that one as they are. ll and ml have a table each.
 *
 * Everything goes into one bitstream, most significant bit first: per sequence
 * the token, the offset's bits, then the length values. A block does not store
 * how many sequences it has. The last one has no match and no bits: its
 * literals are the ones left, and they end at the end of the block. A decoder
 * knows it is there when the literals left are as many as the bytes the block
 * still needs, as every match has at least 4 bytes.
 *
 * A block starts with a u16, little endian: the number of literals of all
 * sequences. The literals follow, all of them one after the other, then the
 * bitstream. Text and code have many literals, so a block can have its literals
 * Huffman coded too, with one of SEQLZ_LIT_SETS fixed tables or with a table of
 * its own, and as deltas to earlier bytes of the block before that. Then the
 * u16 has SEQLZ_LIT_CODED set and a header follows, see SEQLZ_LIT_HEADER.
 *
 * docs/format.md is the specification.
 */

/*
 * A block is what seqlz compresses at a time, a memory page for zram: 1 <<
 * SEQLZ_BLOCK_BITS bytes, 12 for 4 KiB and 14 for 16 KiB, the page size the
 * build is for.
 */
#ifndef SEQLZ_PAGE_BITS
#define SEQLZ_PAGE_BITS 12
#endif
#define SEQLZ_BLOCK_BITS SEQLZ_PAGE_BITS
#define SEQLZ_BLOCK_SIZE (1U << SEQLZ_BLOCK_BITS)
/*
 * The longest code of a length value: its decode table has 256 entries, 1 KiB.
 * A table twice as large made cold reads slower.
 */
#define SEQLZ_MAX_BITS 8U
/*
 * The longest code of a token: its decode table has 2048 entries, 8 KiB. With
 * 10 bits more tokens take the escape: blocks are larger and decode slower,
 * and only in-order cores read cold blocks faster, with half the table to
 * prefetch.
 */
#define SEQLZ_TOKEN_BITS 11U
/*
 * How many bits of ll and ml - 4 the token holds; larger ones need a length
 * value after the token, which costs a second table lookup. With 5 bits for the
 * match length half as many matches need one as with 4, at the same memory.
 * At most 4 and 5: the decoder's token entry has 4 bits for ll and 6 for ml,
 * see token_entry() in seqlz_tables.h.
 */
#define SEQLZ_LL_BITS 4U
#define SEQLZ_ML_BITS 5U
/*
 * The offset classes at the top. Two more save little, and the ones that would
 * save most, offsets sent without bits, need a slower decoder.
 */
#define SEQLZ_OFF_CLASSES 6U
#define SEQLZ_TOKEN_SYMBOLS \
	(SEQLZ_OFF_CLASSES << (SEQLZ_LL_BITS + SEQLZ_ML_BITS))
/*
 * The escape, the symbol after the last token. Codes of at most 11 bits have
 * room for 2048 symbols, and there are 3072 tokens, so the rare ones get no
 * code: they are sent as the escape's code and then the token in
 * SEQLZ_ESCAPE_BITS bits. A code for every token would take code space from
 * the frequent ones.
 */
#define SEQLZ_ESCAPE SEQLZ_TOKEN_SYMBOLS
/* the fewest bits that hold every token, 3072 < 4096 */
#define SEQLZ_ESCAPE_BITS 12U
/*
 * The most bits a token and its offset take, the escape included. The encoder
 * counts on it to know that zram's two pages are always room enough for a
 * compressed block, see ENC_ROOM in seqlz_compress.c. It limits the escape's
 * code: the longest offset follows an escaped token, in SEQLZ_BLOCK_BITS bits.
 */
#define SEQLZ_SEQ_BITS 31U
#define SEQLZ_MAX_ESCAPE_LEN \
	(SEQLZ_SEQ_BITS - SEQLZ_BLOCK_BITS - SEQLZ_ESCAPE_BITS)
/* the shortest match; a token holds ml - SEQLZ_MIN_MATCH */
#define SEQLZ_MIN_MATCH 4U
/* the largest ll and ml - SEQLZ_MIN_MATCH a token holds: 15 and 31 */
#define SEQLZ_LL_CAP ((1U << SEQLZ_LL_BITS) - 1U)
#define SEQLZ_ML_CAP ((1U << SEQLZ_ML_BITS) - 1U)
/* length values below 16 are their own symbol */
#define SEQLZ_LEN_DIRECT 16U
/*
 * A larger one is the symbol SEQLZ_LEN_LOG_BASE plus the position of its
 * highest bit, which is 4 or more, so these symbols follow the direct ones.
 */
#define SEQLZ_LEN_LOG_BASE (SEQLZ_LEN_DIRECT - 4U)
/*
 * The symbols of length values: 16 direct ones, then one per highest bit from 4
 * up to SEQLZ_BLOCK_BITS - 1. The largest length value a block needs is below
 * SEQLZ_BLOCK_SIZE, so no higher bit is needed.
 */
#define SEQLZ_LEN_SYMBOLS (SEQLZ_LEN_LOG_BASE + SEQLZ_BLOCK_BITS)
/* the u16 at the start of every block */
#define SEQLZ_HEADER 2U
/*
 * Set in that u16 when the literals are Huffman coded. A block has at most
 * 16 KiB of literals, so the top bit is free.
 */
#define SEQLZ_LIT_CODED 0x8000U

/*
 * The code lengths of the token table and of the two tables of length values,
 * 0 for a symbol without a code. Canonical Huffman codes follow from the
 * lengths alone, so this is all a table is. The ones compiled in,
 * seqlz_default_lengths, are part of the format.
 */
struct seqlz_lengths {
	u8 token[SEQLZ_TOKEN_SYMBOLS + 1]; /* the last one is the escape */
	u8 ll[SEQLZ_LEN_SYMBOLS];
	u8 ml[SEQLZ_LEN_SYMBOLS];
};

/*
 * The symbol of a length value, and how many of its bits follow the symbol's
 * code as they are, see the top of this file.
 */
static inline unsigned int seqlz_len_symbol(unsigned int v,
					    unsigned int *extra_bits)
{
	unsigned int b;

	if (v < SEQLZ_LEN_DIRECT) {
		*extra_bits = 0;
		return v;
	}
	b = 31U - (unsigned int)__builtin_clz(v);
	*extra_bits = b;
	return SEQLZ_LEN_LOG_BASE + b; /* v >= 16, so b >= 4 */
}

/* the bits of class 5, multiples of 8 below 8 << SEQLZ_CLASS5_BITS */
#define SEQLZ_CLASS5_BITS 8U
/*
 * How many bits of the offset follow the token, per class: 0, 1, 8, the block's
 * bits, 5, SEQLZ_CLASS5_BITS. One nibble per class in one constant, 0x85C810
 * for blocks of 4 KiB, so that it is a shift and a mask without a branch:
 * computed from the block's bits, class 3's made the compiler branch on the
 * class.
 */
#define SEQLZ_RAW_NIBBLES                                             \
	(0U | 1U << 4 | 8U << 8 | SEQLZ_BLOCK_BITS << 12 | 5U << 16 | \
	 SEQLZ_CLASS5_BITS << 20)
#define SEQLZ_RAW_BITS(cls) ((SEQLZ_RAW_NIBBLES >> (4U * (cls))) & 15U)
/*
 * Class 1 sends the offset divided by 2, classes 4 and 5 divided by 8: a shift
 * by 1 and by 3, one nibble per class as above
 */
#define SEQLZ_SHIFT_NIBBLES (0U | 1U << 4 | 3U << 16 | 3U << 20)
#define SEQLZ_OFF_SHIFT(cls) ((SEQLZ_SHIFT_NIBBLES >> (4U * (cls))) & 15U)

/*
 * The class of an offset, and how many of its bits follow the token. last is
 * the offset of the sequence before. Without a branch, because the encoder
 * calls it for every match and the classes come in no order a branch predictor
 * could learn.
 */
static inline unsigned int seqlz_off_class(unsigned int off, unsigned int last,
					   unsigned int *raw_bits)
{
	unsigned int is_new = (off == last) - 1U,
		     aligned = ((off & 7U) == 0) &
			       (off < 8U << SEQLZ_CLASS5_BITS);
	/*
	 * 2 for offsets below 256, else 3; offset 2 moves to 1, a multiple of
	 * 8 that class 5 holds to 4 or 5; the same offset as before is class 0
	 */
	unsigned int cls = (3U - (off < 256U) - (off == 2U) + 2U * aligned) &
			   is_new;

	*raw_bits = SEQLZ_RAW_BITS(cls);
	return cls;
}

/*
 * Coded literals: the longest code, and the number of fixed tables a block can
 * choose from. The encoder takes the table that codes the block's literals in
 * the fewest bits. Codes of 9 bits made blocks larger and were no faster. 16
 * tables saved a few bytes per block, and made cold reads slower, for the
 * larger tables.
 */
#define SEQLZ_LIT_BITS 10U
#define SEQLZ_LIT_SETS 8U
/*
 * The coded literals are 8 bitstreams, literal k in stream k % 8. A literal's
 * code length is known only after its table lookup, so one stream is one long
 * chain of lookups; the decoder runs 8 such chains side by side. 4 streams made
 * blocks a little smaller and decoding no faster.
 */
#define SEQLZ_LIT_STREAMS 8U
/*
 * The header of a block with coded literals, 3 + w bytes, one more with deltas:
 * the u16 of every block, then byte 2, with the literals' code in bits 0 to 3,
 * w - SEQLZ_SIZE_BITS_MIN in bits 4 to 6 and in bit 7 whether the literals are
 * deltas, then with deltas their byte, see SEQLZ_LIT_DELTA_AT, then the sizes
 * of the 8 streams in bytes, w bits each, lowest bit first. The code is one of
 * the SEQLZ_LIT_SETS fixed tables, or SEQLZ_LIT_OWN; codes above it are
 * invalid. The decoder needs the sizes to know where each stream starts.
 *
 * w is the fewest bits that hold the largest size, but at least 5. A stream
 * holds every 8th literal of at most 10 bits, at most 640 bytes in a 4 KiB
 * block and 2560 in a 16 KiB block, so w is at most 12. Sizes of 2 bytes each
 * made blocks larger; widths of 3 and 4 bits are left out, few blocks would use
 * them.
 */
#define SEQLZ_LIT_HEADER(w) (3U + (w))
#define SEQLZ_SIZE_BITS_MIN 5U
#define SEQLZ_SIZE_BITS_MAX 12U
/* the fields of byte 2, as above */
#define SEQLZ_LIT_CODE_MASK 15U
#define SEQLZ_LIT_WIDTH_AT 4
#define SEQLZ_LIT_DELTA_AT 7
/*
 * With bit 7 of byte 2 set, each literal was coded as its delta to the byte x
 * before it in the block, the literal minus that byte, modulo 256; literals
 * fewer than x bytes from the block's start are as they are. Byte 3, before the
 * stream sizes, says which x:
 *   SEQLZ_DELTA_4:    4
 *   SEQLZ_DELTA_8:    8
 *   SEQLZ_DELTA_LAST: the offset of the sequence before, 1 for the first, or
 *                     the smallest multiple of it that is 4 or more
 * Other values are invalid. Counters, sizes and addresses in fields of 4 or 8
 * bytes differ little from those of the field before, and a literal after a
 * match little from the byte the match would have copied. With x 4 or more,
 * the decoder can add 4 bytes at a time.
 */
#define SEQLZ_DELTA_4 1U
#define SEQLZ_DELTA_8 2U
#define SEQLZ_DELTA_LAST 3U
/*
 * The literals' code SEQLZ_LIT_OWN: a table of the block's own, which follows
 * the stream sizes, before the streams. It is the code length of every byte
 * that has a code, from 1 to SEQLZ_LIT_BITS, in the order of the bytes, and
 * before such a length the number of bytes without a code since the last one,
 * the run, where that is not 0. Both are sent as symbols of a fixed prefix
 * code, seqlz_own_lengths, of at most SEQLZ_OWN_CODE_BITS bits: symbol l - 1 is
 * the length l, symbol SEQLZ_OWN_RUN + k a run of k bits, followed by the k - 1
 * bits below its top one. The table ends where its codes fill the code space;
 * the bits after it up to the next byte are zero. The codes are canonical, as
 * those of the fixed tables.
 *
 * The decoder builds the table in the block it decodes into: the literals are
 * decoded before the first byte of the block is written, so the table needs no
 * memory of its own.
 */
#define SEQLZ_LIT_OWN 8U
#define SEQLZ_OWN_RUN 9U
/*
 * the 10 lengths, then runs of 1 to 9 bits; a run of 9 bits is more than a
 * table has bytes, and invalid, but makes the code complete
 */
#define SEQLZ_OWN_SYMBOLS (SEQLZ_OWN_RUN + 10U)
#define SEQLZ_OWN_CODE_BITS 8U

#endif
