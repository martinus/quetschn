/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_TABLES_H
#define SEQLZ_TABLES_H

/*
 * What the compressor, the decompressor and the code that builds their tables
 * share: the tables' layout and the token table's entries.
 */

#include "seqlz_format.h"

/*
 * A decode table is indexed by the next bits of the bitstream, as many as the
 * longest code has. A code of l bits fills every entry whose index starts with
 * its bits, so one lookup finds the code, whatever bits come after it.
 *
 * An entry of a table of length values has the code's length in bits 0..3, the
 * number of extra bits in bits 4..7 and the base value in bits 8..23. The value
 * is the base plus the extra bits. The token table's entries are described at
 * token_entry().
 *
 * The encoder's shift counts are masked, e.g. with & 63U, and so is its index
 * into the length values' enc[], which has a power of 2 entries for that.
 * CONFIG_UBSAN_BOUNDS and CONFIG_UBSAN_SHIFT, which distributions enable, add
 * a compare and a branch to every index and shift that the compiler cannot
 * prove in range; with the mask it can.
 */
#define ENC_LEN_SYMBOLS 32U
static_assert(SEQLZ_LEN_SYMBOLS <= ENC_LEN_SYMBOLS,
	      "the length values' enc[] is too small");
/* the arrays indexed by a code's length, 0 to 15 */
#define CODE_LENGTHS 16U
static_assert(SEQLZ_TOKEN_BITS < CODE_LENGTHS, "a token's code length");
static_assert(SEQLZ_MAX_BITS < CODE_LENGTHS, "a length value's code length");
static_assert(SEQLZ_LIT_BITS < CODE_LENGTHS, "a literal's code length");

struct value_table {
	u32 decode[1U << SEQLZ_MAX_BITS];
	u32 enc[ENC_LEN_SYMBOLS]; /* the encoder's code | length << 16 */
};

struct token_table {
	u32 decode[1U << SEQLZ_TOKEN_BITS]; /* token_entry() */
	/*
	 * The encoder's code | length << TOK_ENC_LEN_AT, 2 bytes per token:
	 * 6 KiB, which stay in the L1 cache next to the matcher's hash table.
	 */
	u16 enc[SEQLZ_TOKEN_SYMBOLS + 1];
};

/* a token's code is below 1 << SEQLZ_TOKEN_BITS, its length above, 4 bits */
#define TOK_ENC_LEN_AT 12U
#define TOK_ENC_CODE_MASK ((1U << TOK_ENC_LEN_AT) - 1U)
static_assert(SEQLZ_TOKEN_BITS <= TOK_ENC_LEN_AT,
	      "a token's code fits below its length");
static_assert(SEQLZ_TOKEN_BITS < 16U && TOK_ENC_LEN_AT + 4U <= 16U,
	      "a token's length fits enc[]'s u16 in 4 bits");

struct lit_table {
	u16 decode[1U << SEQLZ_LIT_BITS]; /* code length | byte << 8 */
	u32 enc[256]; /* code length | code << 8, see lit_put() */
};

/*
 * To pick the literal table that codes a block's literals in the fewest bits,
 * the encoder sums their code lengths in all 8 tables. lit_cost has, for each
 * byte value, its code length in all 8 tables in one u64, 8 bits each, so one
 * add per literal sums all tables at once. A lane of 8 bits overflows after
 * LIT_COST_RUN literals of 10 bits; then the sums move to wider lanes.
 */
#define LIT_COST_RUN (255U / SEQLZ_LIT_BITS) /* 25 */
/* byte 2 of a block with coded literals, see SEQLZ_LIT_HEADER */
static_assert((SEQLZ_LIT_SETS == 8U && SEQLZ_LIT_OWN == 8U &&
	       SEQLZ_LIT_CODE_MASK == 15U && SEQLZ_LIT_WIDTH_AT == 4),
	      "byte 2: the fixed tables, then the block's own, in 4 bits");
static_assert((SEQLZ_SIZE_BITS_MAX - SEQLZ_SIZE_BITS_MIN == 7U &&
	       SEQLZ_LIT_DELTA_AT - SEQLZ_LIT_WIDTH_AT == 3),
	      "byte 2 has 3 bits for the width");
/*
 * How far before a literal the byte is that SEQLZ_DELTA_LAST takes its delta
 * to, for the offset last of the sequence before: the smallest multiple of last
 * that is 4 or more, see SEQLZ_LIT_DELTA_AT
 */
static inline unsigned int seqlz_delta_last(unsigned int last)
{
	return last < 4U ? (last == 3U ? 6U : 4U) : last;
}

static_assert(SEQLZ_LIT_STREAMS == 8U,
	      "the literal coder is written out for 8 streams");

/*
 * The code of a block's own literal table, see SEQLZ_LIT_OWN. quad[] decodes
 * the lengths whose codes the next 8 bits hold whole, at most two, each with
 * the run before it where that fits too, with one lookup:
 *   bits  0..3:  the first length
 *   bits  4..7:  the second, or OWN_QUAD_NONE
 *   bits  8..11: the bits up to the end of the first, run and length
 *   bits 12..15: the same up to the end of the second
 *   bits 16..18: the run before the first, at most 7
 *   bits 19..21: the run before the second
 *   bit  31:     the 8 bits start with a run that does not fit like that;
 *                decode[] has its symbol
 */
#define OWN_QUAD_NONE 11U
#define OWN_QUAD_END1_AT 8
#define OWN_QUAD_END2_AT 12
#define OWN_QUAD_RUN1_AT 16
#define OWN_QUAD_RUN2_AT 19
#define OWN_QUAD_RUN_MAX 7U
#define OWN_QUAD_ALONE_AT 31
struct own_table {
	u16 decode[1U << SEQLZ_OWN_CODE_BITS]; /* symbol | code length << 8 */
	u16 enc[SEQLZ_OWN_SYMBOLS]; /* code | code length << 8 */
	u32 quad[1U << SEQLZ_OWN_CODE_BITS];
};

struct seqlz_tables {
	struct token_table token;
	struct lit_table lit[SEQLZ_LIT_SETS];
	u64 lit_cost[256];
	struct own_table own;
	struct value_table ll, ml;
	bool all_symbols; /* see seqlz_all_symbols() */
};

/* the tables of seqlz_tables_init(), read only after that, by all CPUs */
extern struct seqlz_tables seqlz_tables;

static_assert((SEQLZ_TOKEN_BITS + SEQLZ_RAW_BITS(3) <= 31U &&
	       SEQLZ_TOKEN_BITS + SEQLZ_RAW_BITS(5) <= 31U &&
	       SEQLZ_LL_CAP <= 15U && SEQLZ_ML_CAP + SEQLZ_MIN_MATCH <= 63U &&
	       SEQLZ_OFF_SHIFT(1) <= 3U && SEQLZ_OFF_SHIFT(4) <= 3U &&
	       SEQLZ_OFF_SHIFT(5) <= 3U),
	      "token_entry's fields are too narrow");
static_assert(SEQLZ_TOKEN_SYMBOLS <= 1U << SEQLZ_ESCAPE_BITS,
	      "an escaped token has SEQLZ_ESCAPE_BITS bits");
/*
 * The token table's entry for token s with a code of n bits. It holds what the
 * decoder needs, ready to use, so that it never takes the token apart:
 *   bits  0..5:  n. The bitstream shifted left by n has the offset's bits on
 *                top.
 *   bits  6..11: 64 - the offset's bits, mod 64. The bitstream shifted right
 *                by this then is the offset. 0 for class 0, which has none.
 *   bits 12..13: the class's SEQLZ_OFF_SHIFT(), 1 for class 1 and 3 for
 *                classes 4 and 5, whose offset is sent divided by 2 and 8
 *   bits 14..18: n + the offset's bits, how far the bitstream moves on
 *   bits 19..22: ll, at most 15
 *   bits 23..28: ml, at most 35
 *   bit  30:     a length value follows, for ll or ml
 *   bit  31:     the escape; then only bits 14..18 are set, to n
 * Taking the token apart in the decoder cost many instructions per sequence,
 * which in-order cores run at most two per cycle.
 */
#define TOK_BACK_AT 6
#define TOK_SHIFT_AT 12
#define TOK_DROP_AT 14
#define TOK_LL_AT 19
#define TOK_ML_AT 23
#define TOK_VALUE_AT 30
#define TOK_ESCAPE_AT 31
/* the fields of an entry e, as listed above */
#define TOK_N(e) ((e) & 63U)
#define TOK_BACK(e) (((e) >> TOK_BACK_AT) & 63U)
#define TOK_SHIFT(e) (((e) >> TOK_SHIFT_AT) & 3U)
#define TOK_DROP(e) (((e) >> TOK_DROP_AT) & 31U)
#define TOK_LL(e) (((e) >> TOK_LL_AT) & 15U)
#define TOK_ML(e) (((e) >> TOK_ML_AT) & 63U)

static inline u32 token_entry(unsigned int s, unsigned int n)
{
	unsigned int cls = s >> (SEQLZ_LL_BITS + SEQLZ_ML_BITS),
		     raw_bits = SEQLZ_RAW_BITS(cls);
	unsigned int drop = n + raw_bits, ll = s & SEQLZ_LL_CAP,
		     ml = (s >> SEQLZ_LL_BITS) & SEQLZ_ML_CAP;

	if (s == SEQLZ_ESCAPE)
		return 1U << TOK_ESCAPE_AT | n << TOK_DROP_AT;
	return n | ((64U - raw_bits) & 63U) << TOK_BACK_AT |
	       SEQLZ_OFF_SHIFT(cls) << TOK_SHIFT_AT | drop << TOK_DROP_AT |
	       ll << TOK_LL_AT | (ml + SEQLZ_MIN_MATCH) << TOK_ML_AT |
	       (u32)(ll == SEQLZ_LL_CAP || ml == SEQLZ_ML_CAP) << TOK_VALUE_AT;
}

#endif
