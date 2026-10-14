/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_INTERNAL_H
#define SEQLZ_INTERNAL_H

/*
 * What the compressor, the decompressor and the code that builds their tables
 * share: the tables' layout and the token table's entries.
 */

#include "seqlz.h"

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
static_assert((SEQLZ_TOKEN_BITS < 16U && SEQLZ_MAX_BITS < 16U &&
	       SEQLZ_LIT_BITS < 16U),
	      "first_codes() has arrays of 16 code lengths");

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
 * To pick the literal table that codes a page's literals in the fewest bits,
 * the encoder sums their code lengths in all 8 tables. lit_cost has, for each
 * byte value, its code length in all 8 tables in one u64, 8 bits each, so one
 * add per literal sums all tables at once. A lane of 8 bits overflows after
 * LIT_COST_RUN literals of 10 bits; then the sums move to wider lanes.
 */
#define LIT_COST_WORDS ((SEQLZ_LIT_SETS + 7U) / 8U)
#define LIT_COST_RUN (255U / SEQLZ_LIT_BITS) /* 25 */
/* byte 2 of a page with coded literals, see SEQLZ_LIT_HEADER */
static_assert(SEQLZ_LIT_SETS == 8U, "byte 2 has 3 bits for the table");
static_assert(SEQLZ_SIZE_BITS_MAX - SEQLZ_SIZE_BITS_MIN == 7U,
	      "byte 2 has 3 bits for the width");
static_assert(SEQLZ_LIT_ZERO_AT - SEQLZ_LIT_WIDTH_AT == 3,
	      "the width is 3 bits");
/* the largest stream: every 8th literal of a page, all with the longest code */
#define LIT_STREAM_MAX \
	((SEQLZ_PAGE / SEQLZ_LIT_STREAMS * SEQLZ_LIT_BITS + 7U) / 8U)
static_assert(LIT_STREAM_MAX < 1U << SEQLZ_SIZE_BITS_MAX,
	      "the widest size holds the largest stream");
static_assert(SEQLZ_LIT_STREAMS == 8U,
	      "the literal coder is written out for 8 streams");

/* offsets from here on are all class 7 */
#define SEQLZ_OFF_TABLE SEQLZ_PAGE

struct seqlz_tables {
	struct token_table token;
	struct lit_table lit[SEQLZ_LIT_SETS];
	u64 lit_cost[256][LIT_COST_WORDS];
	struct value_table ll, ml;
	bool all_symbols; /* see seqlz_all_symbols() */
	/*
	 * The class of an offset below SEQLZ_OFF_TABLE that is neither of the
	 * repeat offsets, for the encoder: one load instead of the arithmetic
	 * of seqlz_off_class() on every match.
	 */
	u8 off_class[SEQLZ_OFF_TABLE];
};

static_assert((SEQLZ_TOKEN_BITS + SEQLZ_RAW_BITS(3) <= 31U &&
	       SEQLZ_TOKEN_BITS + SEQLZ_RAW_BITS(5) <= 31U &&
	       SEQLZ_LL_CAP <= 15U && SEQLZ_ML_CAP + 4U <= 63U &&
	       SEQLZ_OFF_SHIFT(5) <= 3U && SEQLZ_TOKEN_BITS <= 15U &&
	       SEQLZ_OFF_CLASSES <= 8U),
	      "token_entry's fields are too narrow");
static_assert(SEQLZ_TOKEN_SYMBOLS <= 1U << SEQLZ_ESCAPE_BITS,
	      "an escaped token has SEQLZ_ESCAPE_BITS bits");
/*
 * The token table's entry for token s with a code of n bits. It holds what the
 * decoder needs, ready to use, so that it never takes the token apart:
 *   bits  0..5:  n. The bitstream shifted left by n has the offset's bits on
 *                top. 6 bits, so that the entry itself is the shift count.
 *   bits  6..11: 64 - the offset's bits, mod 64. The bitstream shifted right
 *                by this is the offset's bits; 0 for a class without.
 *   bits 12..13: f, the offset is (its bits | z) << f: 3 for classes 4 and 5,
 *                whose offset is sent divided by 8, 1 to 3 for classes 1 to 3,
 *                the offsets 2, 4 and 8
 *   bit  14:     z, 1 for classes 1 to 3. Class 0 has offset 0 so: the
 *                offset before.
 *   bits 15..19: n + the offset's bits, how far the bitstream moves on
 *   bits 20..23: ll, at most 15
 *   bits 24..29: ml, at most 35
 *   bit  30:     a length value follows, for ll or ml
 *   bit  31:     the escape; then only bits 15..19 are set, to n
 * Taking the token apart in the decoder cost many instructions per sequence,
 * which in-order cores run at most two per cycle.
 */
#define TOK_RIGHT_AT 6
#define TOK_F_AT 12
#define TOK_Z_AT 14
#define TOK_DROP_AT 15
#define TOK_LL_AT 20
#define TOK_ML_AT 24
#define TOK_VALUE_AT 30
#define TOK_ESCAPE_AT 31
/* the fields of an entry e, as listed above */
#define TOK_N(e) ((e) & 63U)
#define TOK_RIGHT(e) (((e) >> TOK_RIGHT_AT) & 63U)
#define TOK_F(e) (((e) >> TOK_F_AT) & 3U)
#define TOK_Z(e) (((e) >> TOK_Z_AT) & 1U)
#define TOK_DROP(e) (((e) >> TOK_DROP_AT) & 31U)
#define TOK_LL(e) (((e) >> TOK_LL_AT) & 15U)
#define TOK_ML(e) (((e) >> TOK_ML_AT) & 63U)

static inline u32 token_entry(unsigned int s, unsigned int n)
{
	unsigned int cls = s >> (SEQLZ_LL_BITS + SEQLZ_ML_BITS),
		     raw_bits = SEQLZ_RAW_BITS(cls);
	unsigned int drop = n + raw_bits, ll = s & SEQLZ_LL_CAP,
		     ml = (s >> SEQLZ_LL_BITS) & SEQLZ_ML_CAP;
	/* the offsets 2, 4 and 8 of classes 1 to 3 are 1 << cls */
	unsigned int z = SEQLZ_OFF_FIXED(cls), f = SEQLZ_OFF_SHIFT(cls);

	if (s == SEQLZ_ESCAPE)
		return 1U << TOK_ESCAPE_AT | n << TOK_DROP_AT;
	return n | ((64U - raw_bits) & 63U) << TOK_RIGHT_AT | f << TOK_F_AT |
	       z << TOK_Z_AT | drop << TOK_DROP_AT | ll << TOK_LL_AT |
	       (ml + 4U) << TOK_ML_AT |
	       (u32)(ll == SEQLZ_LL_CAP || ml == SEQLZ_ML_CAP) << TOK_VALUE_AT;
}

#endif
