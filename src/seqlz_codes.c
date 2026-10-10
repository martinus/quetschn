// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The codes and decode tables, built from the code lengths:
 * seqlz_tables_init().
 */
#include "seqlz_internal.h"

size_t seqlz_tables_size(void)
{
	return sizeof(struct seqlz_tables);
}

/*
 * The decode entry of length value symbol s: its base value and how many extra
 * bits follow its code. build_values() adds the code's length.
 */
static u32 length_entry(unsigned int s)
{
	if (s < SEQLZ_LEN_DIRECT)
		return s << 8;
	return ((1U << (s - SEQLZ_LEN_LOG_BASE)) << 8) |
	       ((s - SEQLZ_LEN_LOG_BASE) << 4);
}

/*
 * Canonical Huffman codes follow from the code lengths alone: the codes of one
 * length are consecutive numbers, in the order of the symbols, and each length
 * goes on where the one before ended, one bit longer. This puts the first code
 * of each length into next[], for n symbols whose codes are no longer than
 * bits; a length of 0 is a symbol without a code.
 *
 * The codes must fill the decode table exactly. Then every bit pattern starts a
 * code, and the decoder never has to check for one that does not. Huffman codes
 * always do. -EINVAL if a code is too long, or the codes do not fill the table
 * exactly: too many of them, gaps, or none at all.
 */
static int first_codes(const u8 *len, unsigned int n, unsigned int bits,
		       unsigned int next[16])
{
	unsigned int of_len[16] = { 0 }, s, l, k = 0, c = 0;

	for (s = 0; s < n; s++) {
		if (len[s] > bits)
			return -EINVAL;
		of_len[len[s]]++;
	}
	for (l = 1; l <= bits; l++)
		k += of_len[l] << (bits - l);
	if (k != (1U << bits))
		return -EINVAL;
	of_len[0] = 0;
	for (l = 1; l <= bits; l++) {
		c = (c + of_len[l - 1]) << 1;
		next[l] = c;
	}
	return 0;
}

/*
 * The token's codes for the encoder, and its decode table. A code of l bits is
 * the top l bits of the index, so its entries are one range of indices, all
 * with the same token_entry().
 */
static int build_token(const u8 *len, struct token_table *t)
{
	unsigned int next[16], s, k;

	if (first_codes(len, SEQLZ_TOKEN_SYMBOLS + 1, SEQLZ_TOKEN_BITS, next))
		return -EINVAL;
	for (s = 0; s <= SEQLZ_TOKEN_SYMBOLS; s++) {
		unsigned int l = len[s], c;

		if (l == 0)
			continue;
		c = next[l]++;
		t->enc[s] = (u16)(c | l << TOK_ENC_LEN_AT);
		for (k = c << (SEQLZ_TOKEN_BITS - l);
		     k < (c + 1U) << (SEQLZ_TOKEN_BITS - l); k++)
			t->decode[k] = token_entry(s, l);
	}
	return 0;
}

/* the same for the length values, with length_entry() */
static int build_values(const u8 *len, struct value_table *t)
{
	unsigned int next[16], s, k;

	if (first_codes(len, SEQLZ_LEN_SYMBOLS, SEQLZ_MAX_BITS, next))
		return -EINVAL;
	for (s = 0; s < SEQLZ_LEN_SYMBOLS; s++) {
		unsigned int l = len[s], c;

		if (l == 0)
			continue;
		c = next[l]++;
		t->enc[s] = c | l << 16;
		for (k = c << (SEQLZ_MAX_BITS - l);
		     k < (c + 1U) << (SEQLZ_MAX_BITS - l); k++)
			t->decode[k] = length_entry(s) | l;
	}
	return 0;
}

/*
 * The same for a literal table. The literals' streams are read most significant
 * bit first, as the sequences' bitstream, so the codes are not bit-reversed.
 */
static int build_lit(const u8 *len, struct lit_table *t)
{
	unsigned int next[16], s, k;

	if (first_codes(len, 256, SEQLZ_LIT_BITS, next))
		return -EINVAL;
	for (s = 0; s < 256; s++) {
		unsigned int l = len[s], c;

		if (l == 0)
			continue;
		c = next[l]++;
		t->enc[s] = l | c << 8;
		for (k = c << (SEQLZ_LIT_BITS - l);
		     k < (c + 1U) << (SEQLZ_LIT_BITS - l); k++)
			t->decode[k] = (u16)(l | s << 8);
	}
	return 0;
}

bool seqlz_all_symbols(const struct seqlz_tables *t)
{
	return t->all_symbols;
}

/*
 * The literal tables, which are not in struct seqlz_lengths, they are fixed:
 * seqlz_lit_sets. And each byte's code length in all of them, see lit_cost.
 */
static int build_lit_sets(struct seqlz_tables *t)
{
	unsigned int k;

	for (k = 0; k < SEQLZ_LIT_SETS; k++)
		if (build_lit(seqlz_lit_sets[k], &t->lit[k]))
			return -EINVAL;
	for (k = 0; k < 256U * SEQLZ_LIT_SETS; k++)
		t->lit_cost[k % 256][k / 256 / 8] |=
			(u64)seqlz_lit_sets[k / 256][k % 256]
			<< (8U * (k / 256 % 8));
	return 0;
}

/*
 * Whether the encoder can write every page: every token has a code or the
 * escape has one, every length value and every literal has one.
 */
static bool has_all_symbols(const struct seqlz_lengths *lengths)
{
	unsigned int k, all = 1;

	for (k = 0; k < SEQLZ_TOKEN_SYMBOLS; k++)
		all &= lengths->token[k] != 0 ||
		       lengths->token[SEQLZ_ESCAPE] != 0;
	/* and the escape code fits, see SEQLZ_MAX_ESCAPE_LEN */
	all &= lengths->token[SEQLZ_ESCAPE] <= SEQLZ_MAX_ESCAPE_LEN;
	for (k = 0; k < SEQLZ_LEN_SYMBOLS; k++)
		all &= lengths->ll[k] != 0 && lengths->ml[k] != 0;
	for (k = 0; k < 256 * SEQLZ_LIT_SETS; k++)
		all &= seqlz_lit_sets[k / 256][k % 256] != 0;
	return all;
}

int seqlz_tables_init(struct seqlz_tables *t,
		      const struct seqlz_lengths *lengths)
{
	/*
	 * zero, so that symbols without a code have code 0, and so have the
	 * entries of the encoder's tables behind the last symbol
	 */
	memset(t, 0, sizeof(*t));
	if (build_token(lengths->token, &t->token) ||
	    build_values(lengths->ll, &t->ll) ||
	    build_values(lengths->ml, &t->ml) || build_lit_sets(t))
		return -EINVAL;
	t->all_symbols = has_all_symbols(lengths);
	return 0;
}
