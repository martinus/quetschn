// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "seqlz.h"

#include "page_lz.h"

static_assert(PAGE_LZ_PAGE == SEQLZ_PAGE &&
		      PAGE_LZ_HASH_BITS == SEQLZ_HASH_BITS,
	      "page_lz.h and seqlz.h disagree");

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
 * The encoder's tables have a power of 2 entries, and its indices and shift
 * counts are masked, e.g. with & 63U. Fedora builds its kernel with
 * -fsanitize=bounds-strict and -fsanitize=shift, which add a compare and a
 * branch to every index and shift that the compiler cannot prove in range;
 * with the mask it can.
 */
#define ENC_LEN_SYMBOLS 32U
static_assert(SEQLZ_LEN_SYMBOLS <= ENC_LEN_SYMBOLS,
	      "the length values' enc[] is too small");
static_assert(SEQLZ_TOKEN_BITS < 16U && SEQLZ_MAX_BITS < 16U &&
		      SEQLZ_LIT_BITS < 16U,
	      "first_codes() has arrays of 16 code lengths");

struct value_table {
	u32 decode[1U << SEQLZ_MAX_BITS];
	u32 enc[ENC_LEN_SYMBOLS]; /* the encoder's code | length << 16 */
};

struct token_table {
	u32 decode[1U << SEQLZ_TOKEN_BITS]; /* token_entry() */
	/*
	 * The encoder's code | length << 12, 2 bytes per token: 6 KiB, which
	 * stay in the L1 cache next to the matcher's hash table.
	 */
	u16 enc[SEQLZ_TOKEN_SYMBOLS + 1];
};

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
static_assert(
	SEQLZ_LIT_SETS == 8U &&
		SEQLZ_SIZE_BITS_MAX - SEQLZ_SIZE_BITS_MIN == 7U &&
		SEQLZ_LIT_OWN_AT - SEQLZ_LIT_WIDTH_AT == 3 &&
		SEQLZ_LIT_ZERO_AT == SEQLZ_LIT_OWN_AT + 1 &&
		(SEQLZ_PAGE / SEQLZ_LIT_STREAMS * SEQLZ_LIT_BITS + 7U) / 8U <
			1U << SEQLZ_SIZE_BITS_MAX,
	"byte 2's 3 bits of table and 3 of width hold the largest stream");
static_assert(
	SEQLZ_LIT_STREAMS == 8U,
	"code_literals() and decode_literals() are written out for 8 streams");

struct seqlz_tables {
	struct token_table token;
	struct lit_table lit[SEQLZ_LIT_SETS];
	u64 lit_cost[256][LIT_COST_WORDS];
	/*
	 * the codes of a page's own literal table's lengths: the decode table,
	 * the symbol in bits 0 to 3 and the code's length from bit 4 on, and the
	 * encoder's code | length << 8
	 */
	u8 lit_own_decode[1U << SEQLZ_LIT_OWN_BITS];
	u16 lit_own_enc[SEQLZ_LIT_OWN_SYMBOLS];
	struct value_table ll, ml;
	/*
	 * the bits of each token, the escape's 12 bits after its code included,
	 * the price of a token for levels 3 and 4
	 */
	u8 token_bits[SEQLZ_TOKEN_SYMBOLS];
	bool all_symbols; /* see seqlz_all_symbols() */
};

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
	return ((1U << (s - 12U)) << 8) | ((s - 12U) << 4);
}

static_assert(SEQLZ_TOKEN_BITS + SEQLZ_RAW_BITS(3) <= 31U &&
		      SEQLZ_TOKEN_BITS + SEQLZ_RAW_BITS(5) <= 31U &&
		      SEQLZ_LL_CAP <= 15U && SEQLZ_ML_CAP + 4U <= 63U &&
		      SEQLZ_OFF_SHIFT(5) <= 3U,
	      "token_entry's fields are too narrow");
static_assert(SEQLZ_TOKEN_SYMBOLS <= 1U << SEQLZ_ESCAPE_BITS,
	      "an escaped token has SEQLZ_ESCAPE_BITS bits");
static_assert(sizeof(((struct token_table *)0)->decode) % 512U == 0 &&
		      sizeof(((struct value_table *)0)->decode) % 512U == 0 &&
		      sizeof(((struct lit_table *)0)->decode) % 512U == 0,
	      "prefetch_lines() takes multiples of 512 bytes");

/*
 * The token table's entry for token s with a code of n bits. It holds what the
 * decoder needs, ready to use, so that it never takes the token apart:
 *   bits  0..5:  n. The bitstream shifted left by n has the offset's bits on
 *                top.
 *   bits  6..11: 64 - the offset's bits, mod 64. The bitstream shifted right
 *                by this then is the offset. 0 for class 0, which has none.
 *   bits 12..13: 3 for classes 4 and 5, whose offset is sent divided by 8
 *   bits 14..18: n + the offset's bits, how far the bitstream moves on
 *   bits 19..22: ll, at most 15
 *   bits 23..28: ml, at most 35
 *   bit  30:     a length value follows, for ll or ml
 *   bit  31:     the escape; then only bits 14..18 are set, to n
 * Taking the token apart in the decoder took 18 instructions per sequence, and
 * the Cortex-A55 runs at most two per cycle.
 */
#define TOK_BACK_AT 6
#define TOK_SHIFT_AT 12
#define TOK_DROP_AT 14
#define TOK_LL_AT 19
#define TOK_ML_AT 23
#define TOK_VALUE_AT 30
#define TOK_ESCAPE_AT 31

static u32 token_entry(unsigned int s, unsigned int n)
{
	unsigned int cls = s >> (SEQLZ_LL_BITS + SEQLZ_ML_BITS),
		     raw_bits = SEQLZ_RAW_BITS(cls);
	unsigned int drop = n + raw_bits, ll = s & SEQLZ_LL_CAP,
		     ml = (s >> SEQLZ_LL_BITS) & SEQLZ_ML_CAP;

	if (s == SEQLZ_ESCAPE)
		return 1U << TOK_ESCAPE_AT | n << TOK_DROP_AT;
	return n | ((64U - raw_bits) & 63U) << TOK_BACK_AT |
	       SEQLZ_OFF_SHIFT(cls) << TOK_SHIFT_AT | drop << TOK_DROP_AT |
	       ll << TOK_LL_AT | (ml + 4U) << TOK_ML_AT |
	       (u32)(ll == SEQLZ_LL_CAP || ml == SEQLZ_ML_CAP) << TOK_VALUE_AT;
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
	unsigned int count[16] = { 0 }, s, l, k = 0, c = 0;

	for (s = 0; s < n; s++) {
		if (len[s] > bits)
			return -EINVAL;
		count[len[s]]++;
	}
	for (l = 1; l <= bits; l++)
		k += count[l] << (bits - l);
	if (k != (1U << bits))
		return -EINVAL;
	count[0] = 0;
	for (l = 1; l <= bits; l++) {
		c = (c + count[l - 1]) << 1;
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
		t->enc[s] = (u16)(c | l << 12);
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
	    build_values(lengths->ml, &t->ml))
		return -EINVAL;
	{
		/*
		 * the literal tables are not in struct seqlz_lengths, they are
		 * fixed: seqlz_lit_sets
		 */
		unsigned int k;

		for (k = 0; k < SEQLZ_LIT_SETS; k++)
			if (build_lit(seqlz_lit_sets[k], &t->lit[k]))
				return -EINVAL;
		/* the code of a page's own table, see SEQLZ_LIT_OWN_NEXT */
		{
			unsigned int next[16], sym, c;

			if (first_codes(seqlz_lit_own_lengths,
					SEQLZ_LIT_OWN_SYMBOLS,
					SEQLZ_LIT_OWN_BITS, next))
				return -EINVAL;
			for (sym = 0; sym < SEQLZ_LIT_OWN_SYMBOLS; sym++) {
				unsigned int l = seqlz_lit_own_lengths[sym];

				c = next[l]++;
				t->lit_own_enc[sym] = (u16)(c | l << 8);
				for (k = c << (SEQLZ_LIT_OWN_BITS - l);
				     k < (c + 1U) << (SEQLZ_LIT_OWN_BITS - l);
				     k++)
					t->lit_own_decode[k] =
						(u8)(sym | l << 4);
			}
		}
		for (k = 0; k < 256U * SEQLZ_LIT_SETS; k++)
			t->lit_cost[k % 256][k / 256 / 8] |=
				(u64)seqlz_lit_sets[k / 256][k % 256]
				<< (8U * (k / 256 % 8));
	}
	{
		/*
		 * Whether the encoder can write every page: every token has a
		 * code or the escape has one, every length value and every
		 * literal has one.
		 */
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
		t->all_symbols = all;
		for (k = 0; k < SEQLZ_TOKEN_SYMBOLS; k++)
			t->token_bits[k] =
				lengths->token[k] ?
					lengths->token[k] :
					(u8)(lengths->token[SEQLZ_ESCAPE] +
					     SEQLZ_ESCAPE_BITS);
	}
	return 0;
}

/* ---- the matcher of page_lz.h, for seqlz_find() ---- */

struct find_ctx {
	struct seqlz_sequence *seq;
	unsigned int n;
};

static __always_inline void find_emit(void *ctx, const u8 *literals,
				      unsigned int ll, unsigned int ml,
				      unsigned int off)
{
	struct find_ctx *f = ctx;

	(void)literals;
	f->seq[f->n].literals = (u16)ll;
	f->seq[f->n].match = (u16)ml;
	f->seq[f->n].offset = (u16)off;
	f->n++;
}

unsigned int seqlz_find(struct seqlz_state *st, const void *src,
			struct seqlz_sequence *seq)
{
	struct find_ctx f = { .seq = seq };

	match_page(st->table, src, find_emit, &f);
	return f.n;
}

/* ---- encoder ---- */

/*
 * The encoder writes a page in one pass, as the matcher finds the sequences.
 * The page starts with the literals and the bitstream follows, but how many
 * literals there are is known only at the end. So the literals go to the front
 * of dst, after the u16, and the bitstream goes to the back of dst, backwards:
 * its first byte is the last byte of dst, and it grows down toward the
 * literals. At the end it is copied, the right way round, to right after the
 * literals.
 *
 * So dst can have any size, and a page that does not fit is an error, as for
 * the other compressors. Before each sequence the encoder checks that its
 * literals and ENC_ROOM bytes more are free between the two: a sequence's
 * copies write up to 15 bytes past its literals, and its at most two flushes
 * move the bitstream down by at most 16 bytes, writing 8 bytes below it. A page
 * that does not fit stops the writes there, and seqlz_compress() returns 0. It
 * can happen up to ENC_ROOM bytes before dst is full. With the two pages zram
 * gives, it never happens: a sequence covers at least 4 bytes of the page, and
 * its token and offset take at most 31 bits, also with an escaped token, see
 * SEQLZ_MAX_ESCAPE_LEN. So the bitstream has at most SEQLZ_PAGE / 4 * 31 + 31
 * bits, 3972 bytes for 4 KiB pages, and with a page of literals that leaves
 * more than ENC_ROOM bytes free; for 16 KiB pages 15 876 bytes.
 *
 * Bits are collected in a 64-bit accumulator and written 8 bytes at a time;
 * then the write position moves down by the whole bytes, and the at most 7 bits
 * left over are written again with the next ones. After a write the
 * accumulator holds at most 7 bits, so a sequence's token, offset and match
 * length value fit before the next one: 7 + 31 + 20 = 58 bits, 60 for 16 KiB
 * pages. Only a literal length value, 20 or 22 bits more, needs a write of its
 * own.
 */
#define ENC_ROOM 32U

struct encoder {
	const struct seqlz_tables *t;
	u64 acc; /* the bits not yet written, the newest in the low bits */
	unsigned int cnt; /* how many bits of acc count */
	u8 *p; /* the next byte of the bitstream goes right below p */
	u8 *lit; /* where the next literal goes */
	const u8 *src_end; /* copies of 16 literals may read up to here */
	unsigned int last; /* the offset of the sequence before, class 0 */
};

/* appends the n bits of v; n < 64 - cnt */
static __always_inline void enc_put(struct encoder *e, u64 v, unsigned int n)
{
	e->acc = e->acc << (n & 63U) | v;
	e->cnt += n;
}

/*
 * appends a code from an enc[] entry, code | length << 16, and after it the
 * n_extra low bits of extra
 */
static __always_inline void enc_put_code(struct encoder *e, u32 entry,
					 unsigned int extra,
					 unsigned int n_extra)
{
	unsigned int len = entry >> 16 & 15U;

	n_extra &= 31U;
	enc_put(e,
		(u64)(entry & 0xffffU) << n_extra |
			(extra & ((1U << n_extra) - 1U)),
		len + n_extra);
}

/*
 * Writes the whole bytes of the accumulator below p, the first bit as the top
 * bit of the byte right below p, the next byte below that. The shift puts the
 * cnt bits that count on top, older bits above them fall out, and a little
 * endian store of the word puts its top byte last, at p - 1. The last byte is
 * written even when it is not full, and again by the next write.
 */
static __always_inline void enc_flush(struct encoder *e)
{
	put_unaligned_le64(e->acc << ((64U - e->cnt) & 63U), e->p - 8);
	e->p -= e->cnt >> 3;
	e->cnt &= 7U;
}

/* a length value: its symbol's code and its extra bits */
static __always_inline void
put_len_value(struct encoder *e, const struct value_table *t, unsigned int v)
{
	unsigned int extra,
		s = seqlz_len_symbol(v, &extra) & (ENC_LEN_SYMBOLS - 1U);

	enc_put_code(e, t->enc[s], v, extra);
}

/*
 * The code of a token and its length. A token without a code, whose enc[] entry
 * is 0, is the escape's code followed by the token in SEQLZ_ESCAPE_BITS bits.
 */
static __always_inline u32 token_code(const struct seqlz_tables *t,
				      unsigned int tok, unsigned int *len)
{
	u32 te = t->token.enc[tok], ee;

	if (te != 0) {
		*len = te >> 12;
		return te & 0xfffU;
	}
	ee = t->token.enc[SEQLZ_ESCAPE];
	*len = (ee >> 12) + SEQLZ_ESCAPE_BITS;
	return (ee & 0xfffU) << SEQLZ_ESCAPE_BITS | tok;
}

/*
 * Writes one sequence: its literals from in, then its token, offset and length
 * values. ml is 0 for the last sequence.
 */
static __always_inline void encode_emit(void *ctx, const u8 *in,
					unsigned int ll, unsigned int ml,
					unsigned int off)
{
	struct encoder *e = ctx;
	const struct seqlz_tables *t = e->t;
	unsigned int k = 0;

	/*
	 * The page does not fit: no more writes. With p at lit, every later
	 * sequence ends here too, and encoder_finish() sees it.
	 */
	if ((size_t)(e->p - e->lit) < ll + ENC_ROOM) {
		e->p = e->lit;
		return;
	}
	/*
	 * The literals, 16 bytes at a time as long as the input has 16 bytes
	 * more; dst has room for what goes past ll. The first 16 bytes without
	 * a loop, which could mispredict.
	 */
	if ((unsigned int)(e->src_end - in) >= 16U) {
		__builtin_memcpy(e->lit, in, 16);
		k = 16;
	}
	for (; k < ll && (unsigned int)(e->src_end - in) >= k + 16U; k += 16)
		__builtin_memcpy(e->lit + k, in + k, 16);
	for (; k < ll; k++)
		e->lit[k] = in[k];
	e->lit += ll;

	if (ml == 0) {
		/* the last sequence: no match, so ml - 4 = 0 and class 0 */
		unsigned int tlen;
		u32 code = token_code(t, seqlz_token(ll, 0, 0), &tlen);

		enc_put(e, code, tlen);
		if (ll >= SEQLZ_LL_CAP)
			put_len_value(e, &t->ll, ll - SEQLZ_LL_CAP);
		enc_flush(e);
		return;
	}
	{
		/* token and offset in one put, the length values after them */
		unsigned int raw_bits,
			cls = seqlz_off_class(off, e->last, &raw_bits);
		unsigned int tlen;
		u32 code = token_code(t, seqlz_token(ll, ml, cls), &tlen);

		/*
		 * The offset's bits right after the token's code. Classes 1 to
		 * 5 send all bits off has. Class 0 sends none, so there off is
		 * masked to 0.
		 */
		enc_put(e,
			(u64)code << raw_bits | ((off >> SEQLZ_OFF_SHIFT(cls)) &
						 (0U - (cls != 0))),
			tlen + raw_bits);
		if (ll >= SEQLZ_LL_CAP) {
			put_len_value(e, &t->ll, ll - SEQLZ_LL_CAP);
			enc_flush(e);
		}
		if (ml - 4 >= SEQLZ_ML_CAP)
			put_len_value(e, &t->ml, ml - 4 - SEQLZ_ML_CAP);
		enc_flush(e);
		e->last = off;
	}
}

static __always_inline void encoder_init(struct encoder *e,
					 const struct seqlz_tables *t, u8 *d,
					 unsigned int dst_cap,
					 const u8 *src_end)
{
	*e = (struct encoder){ .t = t,
			       .p = d + dst_cap,
			       .lit = d + SEQLZ_HEADER,
			       .src_end = src_end,
			       .last = 1 };
}

/*
 * Writes the last bits and the u16, and copies the bitstream from the back of
 * dst to right after the literals, the right way round. Returns the length of
 * the page, or 0 if it did not fit into the dst_cap bytes at d.
 */
static unsigned int encoder_finish(struct encoder *e, u8 *d,
				   unsigned int dst_cap)
{
	u8 *end = d + dst_cap, *out = e->lit;
	const u8 *in;
	unsigned int n_lit = (unsigned int)(e->lit - (d + SEQLZ_HEADER)), bytes,
		     k = 0;

	if (e->p == e->lit)
		return 0;
	if (e->cnt > 0) {
		put_unaligned_le64(e->acc << ((64U - e->cnt) & 63U), e->p - 8);
		e->p--;
	}
	in = e->p;
	bytes = (unsigned int)(end - in);
	store16(d, n_lit);
	/*
	 * Byte k of the bitstream is at end - 1 - k. 8 bytes at a time while
	 * the copy does not overlap them, else one at a time, in place: only a
	 * page that nearly fills dst gets there.
	 */
	if ((size_t)(in - out) >= bytes) {
		for (; k + 8U <= bytes; k += 8)
			put_unaligned_be64(get_unaligned_le64(end - 8 - k),
					   out + k);
		for (; k < bytes; k++)
			out[k] = *(end - 1 - k);
	} else {
		u8 *lo = e->p, *hi = end - 1;

		for (; lo < hi; lo++, hi--) {
			u8 b = *lo;

			*lo = *hi;
			*hi = b;
		}
		memmove(out, in, bytes);
	}
	return SEQLZ_HEADER + n_lit + bytes;
}

/* seqlz_encode() up to the coded literals, with the compressor's encoder */
static unsigned int encode_raw(const struct seqlz_tables *t,
			       const struct seqlz_sequence *seq, unsigned int n,
			       const u8 *literals, unsigned int n_literals,
			       void *dst, unsigned int dst_cap)
{
	const u8 *in = literals;
	struct encoder e;
	unsigned int i, pos = 0, lits = 0;

	if (dst_cap < SEQLZ_HEADER + ENC_ROOM || !t->all_symbols || n == 0 ||
	    n > SEQLZ_MAX_SEQUENCES || n_literals > SEQLZ_PAGE)
		return 0;
	/*
	 * Only sequences that make a page, checked as the decoder checks them,
	 * before anything is written. dst has room only because every match
	 * covers at least 4 bytes: a shorter one makes ml - 4 wrap, and its
	 * length value takes more bits than the page could save.
	 */
	for (i = 0; i < n; i++) {
		lits += seq[i].literals;
		pos += seq[i].literals;
		if (pos > SEQLZ_PAGE)
			return 0;
		if (i + 1 == n)
			break;
		if (seq[i].match < 4U || seq[i].offset == 0 ||
		    seq[i].offset > pos)
			return 0;
		pos += seq[i].match;
	}
	if (pos != SEQLZ_PAGE || lits != n_literals)
		return 0;
	encoder_init(&e, t, dst, dst_cap, literals + n_literals);
	for (i = 0; i < n; i++) {
		unsigned int ll = seq[i].literals;

		encode_emit(&e, in, ll, i + 1 == n ? 0U : seq[i].match,
			    seq[i].offset);
		in += ll;
	}
	return encoder_finish(&e, dst, dst_cap);
}

/*
 * Appends one literal's code to a stream's accumulator. The enc[] entry has the
 * code's length in its low bits and the code from bit 8 on, see build_lit(). A
 * stream's entries are also summed: the low byte of the sum is the length of
 * their codes, at most 4 * 10 bits between two flushes, which fits.
 */
static __always_inline u64 lit_put(u64 acc, unsigned int *sum, const u32 *enc,
				   u8 b)
{
	u32 e = enc[b];

	acc = acc << (e & 63U) | e >> 8;
	*sum += e;
	return acc;
}

/*
 * the first n bytes of w as put_unaligned_le64() would write them, n < 8: at
 * the end of a stream, where 8 bytes would run into the next one
 */
static void store_tail(u8 *p, u64 w, unsigned int n)
{
	u8 b[8];
	unsigned int k;

	put_unaligned_le64(w, b);
	for (k = 0; k < n; k++)
		p[k] = b[k];
}

/*
 * Writes the whole bytes of a stream's accumulator and returns p moved on by
 * them, as enc_flush() does for the sequences. cnt is the bits left from before
 * the last flush, the low byte of sum the bits added since. 8 bytes at once
 * while the stream has room for them; it ends at end, where the next stream
 * starts. The bytes are swapped once, before the branch: put_unaligned_be64()
 * in both branches gave code_literals() other code on arm64.
 */
static __always_inline u8 *lit_flush(u8 *p, const u8 *end, u64 acc,
				     unsigned int *cnt, unsigned int *sum)
{
	unsigned int n = *cnt + (*sum & 255U);
	u64 w = __builtin_bswap64(acc << ((64U - n) & 63U));

	if (end - p >= 8)
		put_unaligned_le64(w, p);
	else
		store_tail(p, w, (n + 7U) >> 3);
	p += n >> 3;
	*cnt = n & 7U;
	*sum = 0;
	return p;
}

/* the bits of stream j in literal table k, from its lane in even or odd */
static __always_inline unsigned int lit_bits(u64 even[][LIT_COST_WORDS],
					     u64 odd[][LIT_COST_WORDS],
					     unsigned int j, unsigned int k)
{
	return (unsigned int)(((k & 1U ? odd : even)[j][k / 8] >>
			       (16U * (k % 8 / 2))) &
			      0xffffU);
}

/*
 * A page's own literal table, see SEQLZ_LIT_OWN_NEXT: what the encoder needs
 * for it, in the matcher's state, which is free once the sequences are found.
 */
struct own_work {
	u32 freq[256]; /* how often each byte is a literal */
	/* the weights, then the code lengths, see own_huffman(); and a stop */
	u32 w[512];
	u8 sym[256]; /* the bytes with a code, by frequency */
	u8 tmp[256];
	u8 len[256]; /* the code lengths */
	u8 order[256]; /* the bytes by code length, then by value */
	u32 enc[256]; /* code length | code << 8, as in struct lit_table */
	u32 bits; /* the literals' bits with the code */
	/*
	 * the table as the page sends it: at most 256 bytes of up to 14 bits
	 * and 9 steps of 4, 452 bytes, and 8 bytes of room for own_put()
	 */
	u8 hdr[512 + 8];
	u16 freq2[256]; /* a second histogram, see own_code() */
};

static_assert(sizeof(struct own_work) <= sizeof(struct seqlz_state),
	      "the matcher's state holds the work of a page's own table");

/*
 * The Huffman code lengths of the n >= 2 bytes in w->sym, sorted by frequency,
 * into w->w[0..n), longest first, at most SEQLZ_LIT_BITS. Moffat and
 * Katajainen's in-place algorithm, three linear passes over the weights: the
 * inner nodes with their parents, their depths, the leaves' depths. A code that
 * is too long halves the weights and starts over.
 */
static void own_huffman(struct own_work *w, unsigned int n)
{
	u32 *const a = w->w;
	unsigned int k, root, leaf, next, avbl, used, depth, shift = 0;

	for (k = 0; k < n; k++)
		a[k] = w->freq[w->sym[k]];
	for (;;) {
		a[0] += a[1];
		root = 0;
		leaf = 2;
		/*
		 * Without branches: which of the two queues has the lighter
		 * node is no pattern a predictor learns. a[n] is a weight no
		 * node reaches, for the end of the leaves.
		 */
		a[n & 511U] = ~0U;
		for (next = 1; next < n - 1; next++) {
			unsigned int in = a[root & 511U] < a[leaf & 511U],
				     pick = in ? root : leaf;

			a[next & 511U] = a[pick & 511U];
			a[root & 511U] = in ? next : a[root & 511U];
			root += in;
			leaf += !in;
			in = root < next && a[root & 511U] < a[leaf & 511U];
			pick = in ? root : leaf;
			a[next & 511U] += a[pick & 511U];
			a[root & 511U] = in ? next : a[root & 511U];
			root += in;
			leaf += !in;
		}
		a[n - 2] = 0;
		for (next = n - 2; next-- > 0;)
			a[next] = a[a[next] & 511U] + 1U;
		avbl = 1;
		used = 0;
		depth = 0;
		root = n - 2;
		next = n - 1;
		while (avbl > 0) {
			while (root < n && a[root] == depth) {
				used++;
				root--;
			}
			while (avbl > used) {
				a[next--] = depth;
				avbl--;
			}
			avbl = 2 * used;
			depth++;
			used = 0;
		}
		if (a[0] <= SEQLZ_LIT_BITS)
			return;
		/*
		 * Flatter weights, rounded up so that none becomes 0. The
		 * shift only grows, so the loop ends: with all weights 1 the
		 * code is at most 8 bits.
		 */
		shift += a[0] - SEQLZ_LIT_BITS;
		for (k = 0; k < n; k++)
			a[k] = (w->freq[w->sym[k]] + (1U << shift) - 1U) >>
			       shift;
	}
}

/* appends the n bits of v to the description in w->hdr */
static __always_inline void own_put(struct own_work *w, u64 *acc,
				    unsigned int *cnt, unsigned int *out,
				    unsigned int v, unsigned int n)
{
	*acc = *acc << n | v;
	*cnt += n;
	if (*cnt >= 32U) {
		/* 8 bytes, the 4 after the first are written again later */
		put_unaligned_be64(*acc << ((64U - *cnt) & 63U),
				   w->hdr + (*out & 511U));
		*cnt -= 32U;
		*out += 4;
	}
}

/*
 * The page's own code for its literals: lengths, the encoder's codes, the
 * table's description in w->hdr. Returns the bytes of that description, or 0
 * if the literals have fewer than 2 different bytes, where the fixed tables
 * do as well. The bytes that occur are collected once, in w->tmp by value; all
 * later steps go over them only, not over all 256.
 */
static unsigned int own_code(const struct seqlz_tables *t, struct own_work *w,
			     const u8 *literals, unsigned int n_literals)
{
	unsigned int k, n = 0, lo[128] = { 0 }, count[16] = { 0 }, next[16],
			at[16], l, top = 0, cnt = 0, prev, out = 0;
	u64 acc = 0;
	u16 *const h2 = w->freq2;

	/* two histograms, so that a run of one byte does not wait for itself */
	memset(w->freq, 0, sizeof(w->freq));
	memset(h2, 0, sizeof(w->freq2));
	for (k = 0; k + 2 <= n_literals; k += 2) {
		w->freq[literals[k]]++;
		h2[literals[k + 1]]++;
	}
	if (k < n_literals)
		w->freq[literals[k]]++;
	for (k = 0; k < 256; k++) {
		u32 f = w->freq[k] + h2[k];

		w->freq[k] = f;
		w->tmp[n & 255U] = (u8)k;
		n += f != 0;
	}
	if (n < 2)
		return 0;
	/*
	 * By frequency: a counting sort of the frequencies below 127, the
	 * others in the last bucket, few, sorted by insertion.
	 */
	for (k = 0; k < n; k++)
		lo[min(w->freq[w->tmp[k]], 127U)]++;
	for (k = 0, l = 0; k < 128; k++) {
		unsigned int c = lo[k];

		lo[k] = l;
		l += c;
	}
	for (k = 0; k < n; k++)
		w->sym[lo[min(w->freq[w->tmp[k]], 127U)]++ & 255U] = w->tmp[k];
	for (k = lo[126]; k < n; k++) {
		unsigned int b = w->sym[k], f = w->freq[b], i = k;

		for (; i > lo[126] && w->freq[w->sym[i - 1]] > f; i--)
			w->sym[i] = w->sym[i - 1];
		w->sym[i] = (u8)b;
	}
	own_huffman(w, n);
	for (k = 0; k < n; k++)
		w->len[w->sym[k]] = (u8)w->w[k];

	/* canonical codes, by length and then by byte, as build_lit() */
	for (k = 0; k < n; k++)
		count[w->len[w->tmp[k]] & 15U]++;
	for (l = 1, next[0] = 0, at[0] = 0; l < 16; l++) {
		next[l] = (next[l - 1] + count[l - 1]) << 1;
		at[l] = at[l - 1] + count[l - 1];
		top = count[l] ? l : top;
	}
	w->bits = 0;
	for (k = 0; k < n; k++) {
		unsigned int b = w->tmp[k];

		l = w->len[b] & 15U;
		w->bits += w->freq[b] * l;
		w->enc[b] = l | next[l]++ << 8;
		w->order[at[l]++ & 255U] = (u8)b;
	}

	/* the description: the bytes by length, each as its distance */
	for (l = 1, k = 0; l <= top; l++) {
		prev = ~0U;
		for (; k < n && w->len[w->order[k]] == l; k++) {
			unsigned int d = w->order[k] - prev,
				     sym = 31U - (unsigned int)__builtin_clz(d),
				     e = t->lit_own_enc[sym];

			own_put(w, &acc, &cnt, &out,
				(e & 0xffU) << sym | (d - (1U << sym)),
				(e >> 8) + sym);
			prev = w->order[k];
		}
		if (l < top) {
			unsigned int e = t->lit_own_enc[SEQLZ_LIT_OWN_NEXT];

			own_put(w, &acc, &cnt, &out, e & 0xffU, e >> 8);
		}
	}
	for (; cnt >= 8; cnt -= 8)
		w->hdr[out++ & 511U] = (u8)(acc >> (cnt - 8));
	if (cnt)
		w->hdr[out++ & 511U] = (u8)(acc << (8 - cnt));
	return out;
}

/*
 * Turns the page of len bytes in d, with raw literals as the encoder wrote it,
 * into one with Huffman coded literals, where that saves at least 1/16 of them
 * and SEQLZ_LIT_CODED_MIN bytes. Decoding coded literals costs time for every
 * byte, and coding them wherever they save anything at all saved less than
 * 0.1% more memory. Returns the new length, or len if the literals stay raw.
 *
 * It works in the dst_cap bytes of d: the raw literals and the bitstream move
 * to the end, the coded literals are written from the front, and the bitstream
 * moves down behind them at the end. If dst_cap has no room for both, the
 * literals stay raw; in zram's two pages there always is.
 */
static unsigned int code_literals(const struct seqlz_tables *t, u8 *d,
				  unsigned int dst_cap, unsigned int len,
				  struct own_work *own)
{
	/* every second byte: splits 8 lanes of 8 bits into 2 * 4 of 16 bits */
	const u64 lanes = 0x00ff00ff00ff00ffULL;
	const unsigned int n_literals = load16(d), body = len - SEQLZ_HEADER;
	const u8 *literals = d + SEQLZ_HEADER;
	unsigned int bits = ~0U, k, j, coded, set = 0, sizes[SEQLZ_LIT_STREAMS],
		     all, width, header, own_bytes = 0;
	/*
	 * the bits of each stream in each table, in lanes of 16 bits: even has
	 * tables 0, 2, 4 and 6, odd tables 1, 3, 5 and 7
	 */
	u64 even[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { { 0 } },
	    odd[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { { 0 } };
	unsigned int w;
	/* where each stream starts, and the end of the last */
	u8 *q[SEQLZ_LIT_STREAMS + 1];
	const u32 *enc;

	/*
	 * Sums the code lengths of each stream's literals in all tables:
	 * LIT_COST_RUN literals per stream in the 8-bit lanes of x, then into
	 * the 16-bit lanes of even and odd, before an 8-bit lane overflows.
	 */
	for (k = 0; k < n_literals;) {
		u64 x[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { { 0 } };
		unsigned int end =
			n_literals - k < SEQLZ_LIT_STREAMS * LIT_COST_RUN ?
				n_literals :
				k + SEQLZ_LIT_STREAMS * LIT_COST_RUN;

		for (; k + SEQLZ_LIT_STREAMS <= end; k += SEQLZ_LIT_STREAMS)
			for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
				for (w = 0; w < LIT_COST_WORDS; w++)
					x[j][w] +=
						t->lit_cost[literals[k + j]][w];
		for (; k < end; k++)
			for (w = 0; w < LIT_COST_WORDS; w++)
				x[k % SEQLZ_LIT_STREAMS][w] +=
					t->lit_cost[literals[k]][w];
		for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
			for (w = 0; w < LIT_COST_WORDS; w++) {
				even[j][w] += x[j][w] & lanes;
				odd[j][w] += x[j][w] >> 8 & lanes;
			}
	}
	/* the table with the fewest bits */
	for (k = 0; k < SEQLZ_LIT_SETS; k++) {
		unsigned int b = 0;

		for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
			b += lit_bits(even, odd, j, k);
		if (b < bits) {
			bits = b;
			set = k;
		}
	}
	enc = t->lit[set].enc;
	for (j = 0, coded = 0, all = 0; j < SEQLZ_LIT_STREAMS; j++) {
		sizes[j] = (lit_bits(even, odd, j, set) + 7U) / 8U;
		coded += sizes[j];
		all |= sizes[j];
	}
	/*
	 * the sizes are written in as many bits as the largest needs, at least
	 * SEQLZ_SIZE_BITS_MIN
	 */
	width = 32U - (unsigned int)__builtin_clz(
			      all | 1U << (SEQLZ_SIZE_BITS_MIN - 1U));
	header = SEQLZ_LIT_HEADER(width);
	/*
	 * A table of the page's own where it and its description take at least
	 * SEQLZ_LIT_OWN_MIN bytes less than the best fixed table.
	 */
#ifdef SEQLZ_OWN_SWITCH
	{
		extern int seqlz_own_off;
		if (seqlz_own_off)
			own = NULL;
	}
#endif
	if (own && n_literals >= SEQLZ_LIT_OWN_MIN) {
		unsigned int hdr = own_code(t, own, literals, n_literals),
			     ob[SEQLZ_LIT_STREAMS] = { 0 }, osum = 0, oall = 0,
			     owidth;

		/*
		 * The streams' sizes only where the literals' bits alone, with
		 * the smallest header, would leave the fixed table behind:
		 * on about half of the pages it does not.
		 */
		if (hdr && SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN) + hdr +
					   own->bits / 8U + SEQLZ_LIT_OWN_MIN >
				   header + coded)
			hdr = 0;
		for (k = 0; hdr && k < n_literals; k++)
			ob[k % SEQLZ_LIT_STREAMS] += own->len[literals[k]];
		for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
			ob[j] = (ob[j] + 7U) / 8U;
			osum += ob[j];
			oall |= ob[j];
		}
		owidth = 32U - (unsigned int)__builtin_clz(
				       oall | 1U << (SEQLZ_SIZE_BITS_MIN - 1U));
#ifdef OWN_LOG
		{
			extern void own_log(unsigned int, unsigned int,
					    unsigned int, unsigned int);
			own_log(n_literals, header + coded,
				SEQLZ_LIT_HEADER(owidth) + hdr + osum, bits);
		}
#endif
		if (hdr &&
		    SEQLZ_LIT_HEADER(owidth) + hdr + osum + SEQLZ_LIT_OWN_MIN <=
			    header + coded) {
			own_bytes = hdr;
			width = owidth;
			header = SEQLZ_LIT_HEADER(width) + hdr;
			coded = osum;
			for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
				sizes[j] = ob[j];
			enc = own->enc;
		}
	}
	if (header - SEQLZ_LIT_HEADER(width) + coded + SEQLZ_LIT_CODED_MIN >=
	    n_literals - n_literals / 16U)
		return len;
	/*
	 * The coded literals are written from the front of d while the raw ones
	 * are still read from the end, and a write may go 16 bytes too far. The
	 * writes must never reach the literals not read yet. In zram's two
	 * pages, saving 1/16 makes sure of that already; in a smaller dst this
	 * check keeps the literals raw.
	 */
	if (header + coded + 16U > dst_cap - body)
		return len;
	literals = d + dst_cap - body;
	memmove(d + dst_cap - body, d + SEQLZ_HEADER, body);
	q[0] = d + header;
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		q[j + 1] = q[j] + sizes[j];
	/*
	 * Literal k goes into stream k % 8, most significant bit first, see
	 * decode_literals(). Four streams at a time, each with an accumulator
	 * of its own, so that the four chains of shifts run side by side. One
	 * stream after the other made each literal wait for the one before, 3.5
	 * cycles per literal.
	 */
	{
		unsigned int half;

		for (half = 0; half < SEQLZ_LIT_STREAMS; half += 4) {
			u8 *p0 = q[half], *p1 = q[half + 1], *p2 = q[half + 2],
			   *p3 = q[half + 3];
			u8 *e0 = p1, *e1 = p2, *e2 = p3, *e3 = q[half + 4];
			u64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
			unsigned int c0 = 0, c1 = 0, c2 = 0, c3 = 0, s0 = 0,
				     s1 = 0, s2 = 0, s3 = 0;

			/*
			 * Blocks of 32 literals, 4 per stream, then a flush: 4
			 * codes of up to 10 bits fit the accumulator. The loop
			 * stops 28 literals before the end, so the rest below
			 * has at most 4 per stream.
			 */
			for (k = half; k + 28U < n_literals; k += 32) {
				for (j = 0; j < 32U; j += SEQLZ_LIT_STREAMS) {
					a0 = lit_put(a0, &s0, enc,
						     literals[k + j]);
					a1 = lit_put(a1, &s1, enc,
						     literals[k + j + 1]);
					a2 = lit_put(a2, &s2, enc,
						     literals[k + j + 2]);
					a3 = lit_put(a3, &s3, enc,
						     literals[k + j + 3]);
				}
				p0 = lit_flush(p0, e0, a0, &c0, &s0);
				p1 = lit_flush(p1, e1, a1, &c1, &s1);
				p2 = lit_flush(p2, e2, a2, &c2, &s2);
				p3 = lit_flush(p3, e3, a3, &c3, &s3);
			}
			/*
			 * the rest, at most 4 literals per stream; the other
			 * half's 4 of every 8 are skipped
			 */
			for (; k < n_literals; k++) {
				switch (k % SEQLZ_LIT_STREAMS) {
				case 0:
				case 4:
					a0 = lit_put(a0, &s0, enc, literals[k]);
					break;
				case 1:
				case 5:
					a1 = lit_put(a1, &s1, enc, literals[k]);
					break;
				case 2:
				case 6:
					a2 = lit_put(a2, &s2, enc, literals[k]);
					break;
				default:
					a3 = lit_put(a3, &s3, enc, literals[k]);
				}
				if ((k & 3U) == 3U)
					k += 4;
			}
			p0 = lit_flush(p0, e0, a0, &c0, &s0);
			p1 = lit_flush(p1, e1, a1, &c1, &s1);
			p2 = lit_flush(p2, e2, a2, &c2, &s2);
			p3 = lit_flush(p3, e3, a3, &c3, &s3);
		}
	}
	store16(d, SEQLZ_LIT_CODED | n_literals);
	/*
	 * byte 2: the table in bits 0 to 2, width - SEQLZ_SIZE_BITS_MIN in bits
	 * 3 to 5
	 */
	d[2] = (u8)((own_bytes ? 1U << SEQLZ_LIT_OWN_AT : set) |
		    (width - SEQLZ_SIZE_BITS_MIN) << SEQLZ_LIT_WIDTH_AT);
	if (own_bytes)
		memcpy(d + SEQLZ_LIT_HEADER(width), own->hdr, own_bytes);
	/*
	 * the 8 sizes, width bits each, lowest bit first: exactly width bytes,
	 * from byte 3 on
	 */
	{
		u64 acc = 0;
		unsigned int cnt = 0;
		u8 *p = d + SEQLZ_LIT_HEADER(0);

		for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
			acc |= (u64)sizes[j] << cnt;
			for (cnt += width; cnt >= 8U; cnt -= 8U) {
				*p++ = (u8)acc;
				acc >>= 8;
			}
		}
	}
	/* the bitstream, down to right after the coded literals */
	coded += header;
	memmove(d + coded, literals + n_literals, body - n_literals);
	return coded + body - n_literals;
}

unsigned int seqlz_encode(const struct seqlz_tables *t,
			  const struct seqlz_sequence *seq, unsigned int n,
			  const u8 *literals, unsigned int n_literals,
			  void *dst, unsigned int dst_cap, unsigned int coded)
{
	struct own_work own;
	unsigned int len =
		encode_raw(t, seq, n, literals, n_literals, dst, dst_cap);

	return len == 0 || !coded ?
		       len :
		       code_literals(t, dst, dst_cap, len,
				     coded >= SEQLZ_CODED_OWN ? &own : NULL);
}

static unsigned int compress_page(const struct seqlz_tables *t,
				  struct seqlz_state *st, const u8 *src,
				  void *dst, unsigned int dst_cap)
{
	struct encoder e;

	if (dst_cap < SEQLZ_HEADER + ENC_ROOM || !t->all_symbols)
		return 0;
	encoder_init(&e, t, dst, dst_cap, src + SEQLZ_PAGE);
	match_page(st->table, src, encode_emit, &e);
	return encoder_finish(&e, dst, dst_cap);
}

unsigned int seqlz_compress(const struct seqlz_tables *t,
			    struct seqlz_state *st, const void *src, void *dst,
			    unsigned int dst_cap, bool coded)
{
	unsigned int len = compress_page(t, st, src, dst, dst_cap);

	return len == 0 || !coded ? len :
				    code_literals(t, dst, dst_cap, len, NULL);
}

/* ---- levels 3 and 4: a hash chain, the matches priced by the tables ---- */

/*
 * The matcher of levels 3 and 4 looks at more places than match_page() and
 * takes the match that saves the most bits, by the code lengths of the tables.
 * The tables are fixed, so the price of each choice is known before the page
 * is written. Same format, same decoder as levels 1 and 2.
 *
 * Every position of the page goes into a hash chain: head[] has the newest
 * position for each hash of 5 bytes, chain[] for each position the one before
 * it with the same hash. At a position where the repeated offset or the newest
 * position of the chain has the same 4 bytes, the search goes down the chain,
 * 4 steps at level 3 and 16 at level 4. Then the lazy steps, one at level 3
 * and two at level 4: if a match one position on saves more bits, that one is
 * taken instead.
 *
 * A match saves its length in literals, at HC_LIT_PRICE each, and costs its
 * token, the offset's bits and its length value. A price from 6 to 7.5 bits
 * gave pages within 0.1% of each other, 5 bits made them 0.2% to 0.5% larger.
 * A price per page, from what the literal tables cost its bytes, gave pages
 * within 0.1% of this one.
 */
#define HC_LIT_PRICE 52U /* in 1/8 bits, 6.5 bits */

/* the parameters of levels 3 and 4 */
struct hc_level {
	unsigned int depth; /* how many positions of the chain are tried */
	unsigned int lazy; /* how many lazy steps at most */
	/* a match this long ends the search and the lazy steps */
	unsigned int good;
	bool all; /* every position inside a match goes into the chain */
	/* the search also starts where the second position of the chain matches */
	bool second;
};

/* the bits of a length value: its symbol's code and its extra bits */
static __always_inline unsigned int len_value_bits(const struct value_table *t,
						   unsigned int v)
{
	unsigned int extra,
		s = seqlz_len_symbol(v, &extra) & (ENC_LEN_SYMBOLS - 1U);

	return (t->enc[s] >> 16 & 15U) + extra;
}

/* the hash of the 5 bytes at p, as hash5() in page_lz.h */
static __always_inline unsigned int hc_hash(const u8 *p)
{
	return (unsigned int)(((get_unaligned_le64(p) << 24) *
			       889523592379ULL) >>
			      (64U - SEQLZ_HC_HASH_BITS));
}

/* puts pos into the chain, and returns the position the chain had before */
static __always_inline unsigned int hc_insert(struct seqlz_hc_state *st,
					      const u8 *src, unsigned int pos)
{
	unsigned int h = hc_hash(src + pos), before = st->head[h];

	st->chain[pos & (SEQLZ_PAGE - 1U)] = (u16)before;
	st->head[h] = (u16)pos;
	return before;
}

struct hc_match {
	int gain; /* the bits saved against literals, in 1/8 bits */
	unsigned int len, off;
};

/*
 * The match at pos that saves the most bits: the repeated offset, then
 * l->depth positions of the chain from cand on. ll is the literals before pos,
 * last the repeated offset. A match of the chain is counted only if it can be
 * longer than min_len and the longest so far: its first 4 bytes and the 4 up
 * to that length match. A shorter one with a cheaper offset is lost, rarely.
 * m->len is 0 if no match saves bits.
 */
static __always_inline void hc_search(const struct seqlz_tables *t,
				      const struct seqlz_hc_state *st,
				      const u8 *src, const struct hc_level *l,
				      struct hc_match *m, unsigned int pos,
				      unsigned int cand, unsigned int ll,
				      unsigned int last, unsigned int min_len)
{
	const u8 *end = src + SEQLZ_PAGE;
	u32 cur = get_unaligned_le32(src + pos), tail;
	unsigned int d, best, raw, cls, len, bits;
	/* the literal length value is the same for every match at pos */
	int base =
		ll >= SEQLZ_LL_CAP ?
			-(int)(8U * len_value_bits(&t->ll, ll - SEQLZ_LL_CAP)) :
			0;

	m->gain = 0;
	m->len = 0;
	m->off = 0;
	if (get_unaligned_le32(src + pos - last) == cur) {
		len = 4U + count(src + pos + 4, src + pos - last + 4, end);
		bits = t->token_bits[seqlz_token(ll, len, 0) &
				     (SEQLZ_TOKEN_SYMBOLS - 1U)];
		if (len - 4U >= SEQLZ_ML_CAP)
			bits += len_value_bits(&t->ml, len - 4U - SEQLZ_ML_CAP);
		if (base + (int)(len * HC_LIT_PRICE - 8U * bits) > 0) {
			m->gain = base + (int)(len * HC_LIT_PRICE - 8U * bits);
			m->len = len;
			m->off = last;
			if (len >= l->good)
				return;
		}
	}
	best = m->len > min_len ? m->len : min_len;
	if (best < 3)
		best = 3;
	if (pos + best >= SEQLZ_PAGE)
		return;
	tail = get_unaligned_le32(src + pos + best - 3);
	for (d = l->depth; d; d--) {
		/* one branch for all three, which come in no order */
		unsigned int head_hit = get_unaligned_le32(src + cand) == cur,
			     tail_hit = get_unaligned_le32(src + cand + best -
							   3) == tail,
			     new_off = pos - cand != last;

		if (head_hit & tail_hit & new_off) {
			int gain;

			len = 4U + count(src + pos + 4, src + cand + 4, end);
			cls = seqlz_off_class(pos - cand, last, &raw);
			bits = t->token_bits[seqlz_token(ll, len, cls) &
					     (SEQLZ_TOKEN_SYMBOLS - 1U)] +
			       raw;
			if (len - 4U >= SEQLZ_ML_CAP)
				bits += len_value_bits(&t->ml,
						       len - 4U - SEQLZ_ML_CAP);
			gain = base + (int)(len * HC_LIT_PRICE - 8U * bits);
			if (gain > m->gain) {
				m->gain = gain;
				m->len = len;
				m->off = pos - cand;
			}
			if (len >= l->good)
				return;
			if (len > best) {
				best = len;
				if (pos + best >= SEQLZ_PAGE)
					return;
				tail = get_unaligned_le32(src + pos + best - 3);
			}
		}
		if (cand == 0)
			break;
		cand = st->chain[cand & (SEQLZ_PAGE - 1U)];
	}
}

/* finds the sequences of a page, and calls emit() for each, as match_page() */
static __always_inline void
hc_match_page(const struct seqlz_tables *t, struct seqlz_hc_state *st,
	      const u8 *src, const struct hc_level *l, emit_fn emit, void *ctx)
{
	/* every hash reads 8 bytes */
	const unsigned int limit = SEQLZ_PAGE - 8U;
	unsigned int pos = 1, anchor = 0, last = 1, cand, steps, end, p;
	struct hc_match m, n;

	/*
	 * Only head[] is cleared. chain[] is read only at positions that went
	 * into it for this page, and at position 0, the value of an empty head,
	 * where a chain ends.
	 */
	memset(st->head, 0, sizeof(st->head));
	st->chain[0] = 0;
	while (pos < limit) {
		/*
		 * The positions where neither the repeated offset nor the
		 * newest position with the same hash have the same 4 bytes, in
		 * a loop of their own, as in match_page(). One branch for both.
		 */
		for (;;) {
			u32 cur = get_unaligned_le32(src + pos);
			unsigned int rep_hit, cand_hit;

			cand = hc_insert(st, src, pos);
			rep_hit = get_unaligned_le32(src + pos - last) == cur;
			cand_hit = get_unaligned_le32(src + cand) == cur;
			if (rep_hit | cand_hit)
				break;
			/*
			 * At level 4 also the position before it in the
			 * chain: with 2048 heads, the newest position with
			 * the same hash is often one of other bytes.
			 */
			if (l->second) {
				const u8 *second =
					src +
					st->chain[cand & (SEQLZ_PAGE - 1U)];

				if (get_unaligned_le32(second) == cur)
					break;
			}
			if (++pos >= limit)
				goto done;
		}
		hc_search(t, st, src, l, &m, pos, cand, pos - anchor, last, 0);
		if (m.len == 0) {
			pos++;
			continue;
		}
		/*
		 * The lazy steps. A match one position on has to be longer to
		 * win, so it is counted only from there.
		 */
		p = pos + 1;
		for (steps = l->lazy;
		     steps && m.len < l->good && pos + 1 < limit; steps--) {
			unsigned int next = pos + 1;
			u32 cur = get_unaligned_le32(src + next);

			cand = hc_insert(st, src, next);
			p = next + 1;
			(void)cur;
			hc_search(t, st, src, l, &n, next, cand, next - anchor,
				  last, m.len - 1);
			if (n.gain <= m.gain)
				break;
			m = n;
			pos = next;
		}
		/*
		 * The match may have started before pos: extend it backwards
		 * into the literals.
		 */
		{
			unsigned int from = pos - m.off;

			while (pos > anchor && from > 0 &&
			       src[pos - 1] == src[from - 1]) {
				pos--;
				from--;
				m.len++;
			}
		}
		emit(ctx, src + anchor, pos - anchor, m.len, m.off);
		last = m.off;
		end = pos + m.len < limit ? pos + m.len : limit;
		/*
		 * The positions inside the match, from p on, the ones the lazy
		 * steps did not insert: all of them at level 4, the last 2 at
		 * level 3. All of them made pages 0.2% to 0.9% smaller, and the
		 * first 8 took level 3 8% more time.
		 */
		if (!l->all && p + 2 < end)
			p = end - 2;
		for (; p < end; p++)
			hc_insert(st, src, p);
		pos += m.len;
		anchor = pos;
	}
done:
	emit(ctx, src + anchor, SEQLZ_PAGE - anchor, 0, 0);
}

/*
 * Level 3: 4 positions of the chain, one lazy step, the last 2 positions of a
 * match into the chain. Level 4: 16 positions, two lazy steps, every position.
 */
static const struct hc_level hc_level3 = { .depth = 4,
					   .lazy = 1,
					   .good = 64,
					   .all = false,
					   .second = false };
static const struct hc_level hc_level4 = { .depth = 16,
					   .lazy = 2,
					   .good = 256,
					   .all = true,
					   .second = true };

static __always_inline unsigned int
compress_page_hc(const struct seqlz_tables *t, struct seqlz_hc_state *st,
		 const u8 *src, void *dst, unsigned int dst_cap,
		 const struct hc_level *l)
{
	struct encoder e;
	unsigned int len;

	if (dst_cap < SEQLZ_HEADER + ENC_ROOM || !t->all_symbols)
		return 0;
	encoder_init(&e, t, dst, dst_cap, src + SEQLZ_PAGE);
	hc_match_page(t, st, src, l, encode_emit, &e);
	len = encoder_finish(&e, dst, dst_cap);
	return len == 0 ? 0 :
			  code_literals(t, dst, dst_cap, len,
					(struct own_work *)st);
}

/* one copy of the matcher for each level, with its parameters as constants */
static noinline unsigned int compress_level3(const struct seqlz_tables *t,
					     struct seqlz_hc_state *st,
					     const u8 *src, void *dst,
					     unsigned int dst_cap)
{
	return compress_page_hc(t, st, src, dst, dst_cap, &hc_level3);
}

static noinline unsigned int compress_level4(const struct seqlz_tables *t,
					     struct seqlz_hc_state *st,
					     const u8 *src, void *dst,
					     unsigned int dst_cap)
{
	return compress_page_hc(t, st, src, dst, dst_cap, &hc_level4);
}

unsigned int seqlz_compress_hc(const struct seqlz_tables *t,
			       struct seqlz_hc_state *st, const void *src,
			       void *dst, unsigned int dst_cap, bool deep)
{
	return deep ? compress_level4(t, st, src, dst, dst_cap) :
		      compress_level3(t, st, src, dst, dst_cap);
}

/* ---- the matcher of levels 3 and 4, for seqlz_find_hc() ---- */

unsigned int seqlz_find_hc(const struct seqlz_tables *t,
			   struct seqlz_hc_state *st, const void *src,
			   struct seqlz_sequence *seq, bool deep)
{
	struct find_ctx f = { .seq = seq };

	hc_match_page(t, st, src, deep ? &hc_level4 : &hc_level3, find_emit,
		      &f);
	return f.n;
}

/* ---- decoder ---- */

/*
 * Two choices of the decoder depend on whether the core runs instructions in
 * order, as the small cores of phones do, or out of order. An arm64 kernel asks
 * the core; every other build counts as out of order, x86-64 too.
 * - The token table, 2048 entries of 4 bytes, is prefetched before each page.
 *   An in-order core stops at every cache miss it did not see coming: without
 *   the prefetches a Cortex-A55 read a cold page 14.5 us slower. An
 *   out-of-order core goes on with other work during a miss, and the 128
 *   prefetches are in its way, a page needs about 43 of the lines: without
 *   this prefetch a Cortex-A76 read a cold page 1.3 to 2.6 us faster.
 * - On the fast path, an out-of-order core loads more bits every second
 *   sequence, an in-order core only when they run short, see decode_page().
 */
#if defined(__KERNEL__) && defined(__aarch64__)
#include <asm/cputype.h>
/*
 * The in-order cores: Cortex-A53, A55, A510, A520, and Qualcomm's Kryo silver
 * cores. The core's id, MIDR, has the implementer in bits 24 to 31 and the part
 * number in bits 4 to 15. ARM (0x41): Cortex-A53 0xd03, A55 0xd05, A510 0xd46,
 * A520 0xd80. Qualcomm (0x51): Kryo 2xx silver 0x801, a Cortex-A53, Kryo 3xx
 * and 4xx silver 0x803 and 0x805. The numbers are written out because the
 * phone's 4.14 kernel lacks some of the kernel's macros for them, and names
 * others differently.
 */
static inline int in_order_core(void)
{
	u32 m = read_cpuid_id(), imp = m >> 24, part = (m >> 4) & 0xfffU;

	return (imp == 0x41U && (part == 0xd03U || part == 0xd05U ||
				 part == 0xd46U || part == 0xd80U)) ||
	       (imp == 0x51U &&
		(part == 0x801U || part == 0x803U || part == 0x805U));
}

static inline int prefetch_tokens(int in_order)
{
	return in_order;
}
#else
/* tests take the in-order path on any CPU with -DSEQLZ_IN_ORDER=1 */
#ifndef SEQLZ_IN_ORDER
#define SEQLZ_IN_ORDER 0
#endif
static inline int in_order_core(void)
{
	return SEQLZ_IN_ORDER;
}

static inline int prefetch_tokens(int in_order)
{
	(void)in_order;
	return 1;
}
#endif

/*
 * Reads the sequences' bitstream, most significant bit first: bits has the
 * next bits on top, count says how many of them are real. A refill loads 8
 * bytes at once while 8 are left, and byte by byte at the end, so it never
 * reads past the stream. A page that wants more bits than it has gets zeros,
 * and count goes negative: the page is not valid. That bits are right does not
 * matter for memory safety, every length and offset is checked where it is
 * used.
 */
struct bit_reader {
	const u8 *p;
	const u8 *end;
	u64 bits;
	int count; /* negative once more bits were taken than there are */
};

static inline void refill(struct bit_reader *r)
{
	if (r->end - r->p >= 8) {
		u64 v;

		/*
		 * count is at least 0 here: it only goes negative at the end of
		 * the stream
		 */
		v = get_unaligned_be64(r->p);
		r->bits |= v >> r->count;
		/*
		 * p moves on by the bytes that went in whole, and count is 56
		 * to 63
		 */
		r->p += (63 - r->count) >> 3;
		r->count |= 56;
	} else {
		while (r->count >= 0 && r->count <= 56 && r->p < r->end) {
			r->bits |= (u64)*r->p++ << (56 - r->count);
			r->count += 8;
		}
	}
}

/* takes n bits */
static inline void drop(struct bit_reader *r, unsigned int n)
{
	r->bits <<= n;
	r->count -= (int)n;
}

/*
 * Reads one length value. The entry of its code has the base value and the
 * number of extra bits, which follow the code. Takes up to SEQLZ_MAX_BITS +
 * QUETSCHN_PAGE_BITS bits, 20 or 22; the caller refills before.
 */
static inline unsigned int value(struct bit_reader *r,
				 const struct value_table *t, u32 *entry)
{
	u32 e = t->decode[r->bits >> (64U - SEQLZ_MAX_BITS)];
	unsigned int n = e & 15U, x = (e >> 4) & 15U;
	/*
	 * the x extra bits after the code; two shifts, because a shift by 64 -
	 * x is undefined for x = 0
	 */
	unsigned int v = ((e >> 8) & 0xffffU) +
			 (unsigned int)(((r->bits << n) >> 1) >> (63U - x));

	*entry = e;
	drop(r, n + x);
	return v;
}

/*
 * The literals' streams are read as zstd reads its Huffman coded literals,
 * most significant bit first. A stream is a pointer ip and 64 bits: the 8
 * bytes at ip with the lowest bit set to 1, shifted left by the bits of ip's
 * first byte already used. Each code shifts them further left, so the
 * position of that 1 tells how many bits were used since ip, and a refill
 * needs no counter. With a counter, 8 streams did not fit into the registers
 * of x86-64. A refill leaves at least 56 bits, codes never reach the 1.
 */
/*
 * the 8 bytes at ip, zeros after end; only the last refills of a page need it,
 * so it stays out of the loop
 */
static noinline __cold u64 lit_load_tail(const u8 *ip, const u8 *end)
{
	u8 b[8] = { 0 };
	unsigned int k;

	for (k = 0; k < 8U && ip + k < end; k++)
		b[k] = ip[k];
	return get_unaligned_be64(b);
}

/*
 * Loads the next 8 bytes of a stream, returns its new bits and moves ip on.
 * Past the stream's end the bytes belong to the next stream or the sequences: a
 * stream that used them fails the check of its size at the end of
 * decode_literals().
 */
static __always_inline u64 lit_refill(const u8 **ip, u64 bits, const u8 *end)
{
	unsigned int used = (unsigned int)__builtin_ctzll(bits);
	const u8 *p = *ip + (used >> 3);

	*ip = p;
	return ((end - p >= 8 ? get_unaligned_be64(p) : lit_load_tail(p, end)) |
		1U)
	       << (used & 7U);
}

/*
 * Decodes the next literal of a stream into out and returns the stream's new
 * bits. The table entry has the code's length in bits 0 to 5 and the byte from
 * bit 8 on. The stream moves on by the code's length where mask is 63; where it
 * is 0 the literal is decoded but not taken, see the end of decode_literals().
 */
static __always_inline u64 lit_decode(u8 *out, u64 bits, const u16 *lt,
				      unsigned int mask)
{
	unsigned int e = lt[bits >> (64U - SEQLZ_LIT_BITS)];

	*out = (u8)(e >> 8);
	return bits << (e & mask);
}

/*
 * Reads a page's own literal table from p, see SEQLZ_LIT_OWN_NEXT, and writes
 * its 1 << SEQLZ_LIT_BITS decode entries to lt, in the order of the table:
 * code lengths from short to long, within a length by byte, which is also the
 * order of the bytes in the page. Each byte fills the next 1 << (SEQLZ_LIT_BITS
 * - l) entries, a whole number of 8-byte stores from l = 8 down. Returns the
 * byte after the table, or NULL if it is not valid: a byte above 255 or twice,
 * a length above SEQLZ_LIT_BITS, more codes than fit, bits that are not zero
 * after the last code, or fewer bytes than the table needs. At most 256 bytes
 * and SEQLZ_LIT_BITS lengths, so the loop ends for any input.
 */
static const u8 *own_table(const struct seqlz_tables *t, const u8 *p,
			   const u8 *end, u16 *lt)
{
	unsigned int fill = 0, len = 1, pos = 0, sym, n;
	int prev = -1;
	u64 seen[4] = { 0 };

	while (fill < 1U << SEQLZ_LIT_BITS) {
		const u8 *at = p + (pos >> 3);
		/* the next bits, zeros past the end */
		u64 w = (end - at >= 8 ? get_unaligned_be64(at) :
					 lit_load_tail(at, end))
			<< (pos & 7U);
		unsigned int
			e = t->lit_own_decode[w >> (64U - SEQLZ_LIT_OWN_BITS)],
			b, run;
		u64 v;
		u16 *o;

		sym = e & 15U;
		n = e >> 4;
		if (sym == SEQLZ_LIT_OWN_NEXT) {
			pos += n;
			prev = -1;
			if (++len > SEQLZ_LIT_BITS)
				return NULL;
			continue;
		}
		/* d = 1 << sym and the sym bits after the code */
		w <<= n;
		b = (unsigned int)(prev + (int)(1U << sym) +
				   (int)(sym ? w >> (64U - sym) : 0U));
		pos += n + sym;
		run = (1U << SEQLZ_LIT_BITS) >> len;
		if (b > 255U || (seen[b >> 6] >> (b & 63U) & 1U) ||
		    fill + run > 1U << SEQLZ_LIT_BITS)
			return NULL;
		seen[b >> 6] |= 1ULL << (b & 63U);
		prev = (int)b;
		v = (u64)(len | b << 8) * 0x0001000100010001ULL;
		o = lt + fill;
		fill += run;
		if (run >= 4U) {
			for (; o < lt + fill; o += 4)
				__builtin_memcpy(o, &v, 8);
		} else {
			*o = (u16)v;
			if (run == 2U)
				o[1] = (u16)v;
		}
	}
	/* the table's bytes are in the page, the bits after it are zero */
	if ((pos + 7U) / 8U > (unsigned long)(end - p) ||
	    ((pos & 7U) && (p[pos >> 3] << (pos & 7U) & 0xffU)))
		return NULL;
	return p + (pos + 7U) / 8U;
}

/*
 * Decodes the coded literals of a page into out, the scratch, see
 * SEQLZ_LIT_HEADER for the layout. Returns where the sequences' bitstream
 * starts, or NULL if the page is not valid. Not inlined: inside the loop over
 * the sequences its registers made pages with raw literals 12% slower too.
 */
static noinline __aligned(64) const u8 *decode_literals(
	const struct seqlz_tables *t, const u8 *s, unsigned int src_len,
	unsigned int n_lit, u8 *out, u8 *page)
{
	const u8 *const end = s + src_len;
	/*
	 * byte 2: the table in bits 0 to 2, width - SEQLZ_SIZE_BITS_MIN in bits
	 * 3 to 5, bits 6 and 7 zero
	 */
	const unsigned int width =
		SEQLZ_SIZE_BITS_MIN +
		(src_len > 2U ?
			 (s[2] >> SEQLZ_LIT_WIDTH_AT) &
				 (SEQLZ_SIZE_BITS_MAX - SEQLZ_SIZE_BITS_MIN) :
			 0U);
	const u8 *q = s + SEQLZ_LIT_HEADER(width);
	const u8 *ip[SEQLZ_LIT_STREAMS];
	const u8 *start[SEQLZ_LIT_STREAMS];
	unsigned int sz[SEQLZ_LIT_STREAMS];
	u64 b0 = 1, b1 = 1, b2 = 1, b3 = 1, b4 = 1, b5 = 1, b6 = 1, b7 = 1;
	unsigned int k;
	u64 total = 0;
	const u16 *lt;

	if (src_len < SEQLZ_LIT_HEADER(width) || s[2] >> SEQLZ_LIT_ZERO_AT ||
	    n_lit > SEQLZ_PAGE)
		return NULL;
	if (s[2] >> SEQLZ_LIT_OWN_AT & 1U) {
		/* the page's own table, built in the page itself */
		if (s[2] & (SEQLZ_LIT_SETS - 1U))
			return NULL;
		q = own_table(t, q, end, (u16 *)page);
		if (!q)
			return NULL;
		lt = (const u16 *)page;
	} else {
		lt = t->lit[s[2] & (SEQLZ_LIT_SETS - 1U)].decode;
		prefetch_lines(lt, sizeof(t->lit[0].decode));
	}
	{
		/*
		 * The sizes are copied out of the page first: a 4-byte load at
		 * the last size's byte would read past the header.
		 */
		u8 h[SEQLZ_SIZE_BITS_MAX + 4U] = { 0 };

		__builtin_memcpy(h, s + SEQLZ_LIT_HEADER(0), width);
		for (k = 0; k < SEQLZ_LIT_STREAMS; k++) {
			sz[k] = (get_unaligned_le32(h + k * width / 8U) >>
				 (k * width % 8U)) &
				((1U << width) - 1U);
			start[k] = q + total;
			ip[k] = start[k];
			total += sz[k];
		}
	}
	if ((u64)(q - s) + total > src_len)
		return NULL;
	/*
	 * Full rounds of SEQLZ_LIT_ROUNDS literals per stream, all 8 streams
	 * side by side. The rest below.
	 */
	for (k = 0; k + SEQLZ_LIT_STREAMS * SEQLZ_LIT_ROUNDS <= n_lit;
	     k += SEQLZ_LIT_STREAMS * SEQLZ_LIT_ROUNDS) {
		unsigned int j;
		const u8 *i0 = ip[0], *i1 = ip[1], *i2 = ip[2], *i3 = ip[3],
			 *i4 = ip[4], *i5 = ip[5], *i6 = ip[6], *i7 = ip[7];

		b0 = lit_refill(&i0, b0, end);
		b1 = lit_refill(&i1, b1, end);
		b2 = lit_refill(&i2, b2, end);
		b3 = lit_refill(&i3, b3, end);
		b4 = lit_refill(&i4, b4, end);
		b5 = lit_refill(&i5, b5, end);
		b6 = lit_refill(&i6, b6, end);
		b7 = lit_refill(&i7, b7, end);
		ip[0] = i0;
		ip[1] = i1;
		ip[2] = i2;
		ip[3] = i3;
		ip[4] = i4;
		ip[5] = i5;
		ip[6] = i6;
		ip[7] = i7;
		for (j = 0; j < SEQLZ_LIT_ROUNDS; j++) {
			b0 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j], b0, lt,
					63U);
			b1 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 1], b1,
					lt, 63U);
			b2 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 2], b2,
					lt, 63U);
			b3 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 3], b3,
					lt, 63U);
			b4 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 4], b4,
					lt, 63U);
			b5 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 5], b5,
					lt, 63U);
			b6 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 6], b6,
					lt, 63U);
			b7 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 7], b7,
					lt, 63U);
		}
	}
	{
		/*
		 * The rest, fewer than 40 literals: rest / 8 steps of all 8
		 * streams, then one more step for the r streams that have a
		 * literal left. The others decode a literal in that step too,
		 * but with a mask of 0 they do not move on, so that each stream
		 * ends right after its own codes; what they decode lands after
		 * the literals, in the 16 bytes the scratch has there. All
		 * streams take the same steps: a loop per stream would end
		 * after a different number of literals on every page, and
		 * mispredict.
		 */
		const unsigned int rest = n_lit - k,
				   steps = rest / SEQLZ_LIT_STREAMS,
				   r = rest % SEQLZ_LIT_STREAMS;
		const u8 *i0 = ip[0], *i1 = ip[1], *i2 = ip[2], *i3 = ip[3],
			 *i4 = ip[4], *i5 = ip[5], *i6 = ip[6], *i7 = ip[7];
		unsigned int j;

		b0 = lit_refill(&i0, b0, end);
		b1 = lit_refill(&i1, b1, end);
		b2 = lit_refill(&i2, b2, end);
		b3 = lit_refill(&i3, b3, end);
		b4 = lit_refill(&i4, b4, end);
		b5 = lit_refill(&i5, b5, end);
		b6 = lit_refill(&i6, b6, end);
		b7 = lit_refill(&i7, b7, end);
		for (j = 0; j < steps; j++) {
			b0 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j], b0, lt,
					63U);
			b1 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 1], b1,
					lt, 63U);
			b2 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 2], b2,
					lt, 63U);
			b3 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 3], b3,
					lt, 63U);
			b4 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 4], b4,
					lt, 63U);
			b5 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 5], b5,
					lt, 63U);
			b6 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 6], b6,
					lt, 63U);
			b7 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 7], b7,
					lt, 63U);
		}
		k += SEQLZ_LIT_STREAMS * steps;
		b0 = lit_decode(&out[k], b0, lt, (0U - (r > 0U)) & 63U);
		b1 = lit_decode(&out[k + 1], b1, lt, (0U - (r > 1U)) & 63U);
		b2 = lit_decode(&out[k + 2], b2, lt, (0U - (r > 2U)) & 63U);
		b3 = lit_decode(&out[k + 3], b3, lt, (0U - (r > 3U)) & 63U);
		b4 = lit_decode(&out[k + 4], b4, lt, (0U - (r > 4U)) & 63U);
		b5 = lit_decode(&out[k + 5], b5, lt, (0U - (r > 5U)) & 63U);
		b6 = lit_decode(&out[k + 6], b6, lt, (0U - (r > 6U)) & 63U);
		b7 = lit_decode(&out[k + 7], b7, lt, (0U - (r > 7U)) & 63U);
		ip[0] = i0;
		ip[1] = i1;
		ip[2] = i2;
		ip[3] = i3;
		ip[4] = i4;
		ip[5] = i5;
		ip[6] = i6;
		ip[7] = i7;
	}
	{
		u64 bb[SEQLZ_LIT_STREAMS] = { b0, b1, b2, b3, b4, b5, b6, b7 };

		/*
		 * Each stream must end in its last byte: the bits used, the
		 * bytes it moved past plus the 1's position, are 0 to 7 fewer
		 * than its size, and the bits after its last code are 0. Those
		 * are read from the page, not from the 64 bits: when the
		 * stream's last 8 bytes are in them, its last bit is where the
		 * 1 is. A stream that read into the next one fails here, and so
		 * does one with bytes after its codes, also one without
		 * literals that has a size.
		 */
		for (k = 0; k < SEQLZ_LIT_STREAMS; k++) {
			long left = 8L * sz[k] - (8L * (ip[k] - start[k]) +
						  __builtin_ctzll(bb[k]));

			if (left < 0 || left > 7 ||
			    (left > 0 &&
			     (start[k][sz[k] - 1U] & ((1U << left) - 1U))))
				return NULL;
		}
	}
	return q + total;
}

/*
 * The next token's entry. A refill only when token and offset might not fit,
 * 11 + 12 bits: then the token's lookup does not wait for the refill's load,
 * and a refill leaves 56 bits for two or three sequences. The escape and the
 * length values refill before they read. The token's entry knows the offset's
 * class, so the offset's bits are known without a second lookup.
 */
static __always_inline u32 next_token(struct bit_reader *br,
				      const struct token_table *t)
{
	if (br->count < (int)(SEQLZ_TOKEN_BITS + QUETSCHN_PAGE_BITS))
		refill(br);
	return t->decode[br->bits >> (64U - SEQLZ_TOKEN_BITS)];
}

/*
 * seqlz_decode() for one kind of core. in_order is a constant, so there are two
 * loops: an in-order core runs one without the refill of every second fast
 * sequence, not the same loop with one more branch.
 */
static __always_inline int decode_page(const struct seqlz_tables *t,
				       const void *src, unsigned int src_len,
				       void *dst, void *scratch,
				       const int in_order)
{
	const u8 *s = src;
	const u8 *const s_end = s + src_len;
	u8 *d = dst;
	u8 *const d_end = d + SEQLZ_PAGE;
	const u8 *lit, *lit_end, *lit_bound;
	u8 *d_fast;
	unsigned long lit_fast, in_fast;
	struct bit_reader br;
	unsigned int n_lit, last = 1, tok, skip = 0;

	if (src_len < SEQLZ_HEADER)
		return -EINVAL;
	/* the decoder's tables first, so that their cache misses overlap */
	if (prefetch_tokens(in_order))
		prefetch_lines(t->token.decode, sizeof(t->token.decode));
	prefetch_lines(t->ll.decode, sizeof(t->ll.decode));
	prefetch_lines(t->ml.decode, sizeof(t->ml.decode));
	n_lit = load16(s);
	if (n_lit & SEQLZ_LIT_CODED) {
		/* decoded into the scratch first, the bitstream follows them */
		const u8 *q;

		n_lit &= SEQLZ_LIT_CODED - 1U;
		if (!scratch)
			return -EINVAL;
		q = decode_literals(t, s, src_len, n_lit, scratch, dst);
		if (!q)
			return -EINVAL;
		br = (struct bit_reader){ .p = q, .end = s_end };
		lit = scratch;
		lit_end = lit + n_lit;
		lit_bound = lit + SEQLZ_SCRATCH;
	} else {
		/* raw, right after the u16, the bitstream follows them */
		if ((u64)SEQLZ_HEADER + n_lit > src_len)
			return -EINVAL;
		lit = s + SEQLZ_HEADER;
		lit_end = lit + n_lit;
		lit_bound = s_end;
		br = (struct bit_reader){ .p = lit_end, .end = s_end };
	}

	/*
	 * The fast path below copies without checking the room left, so it is
	 * taken only where there is enough, one compare each: 64 bytes in the
	 * page, it writes at most 54, 14 literals and 40 bytes of match; 16
	 * bytes of literals; 8 bytes of input for a refill. lit_fast and
	 * in_fast are numbers, not pointers: with less input, lit_bound - 16
	 * would point before it.
	 */
	d_fast = d_end - 64;
	lit_fast = (unsigned long)lit_bound - 16U;
	/* 0 if the input is shorter than 8 bytes: no refill without a check */
	in_fast = src_len >= 8U ? (unsigned long)s_end - 8U : 0;

	tok = next_token(&br, &t->token);
	for (;;) {
		unsigned int nl, len, off;
		u32 e;

		if (tok >> TOK_ESCAPE_AT) {
			/* escape: SEQLZ_ESCAPE_BITS bits of the token follow */
			unsigned int idx;

			drop(&br, (tok >> TOK_DROP_AT) & 31U);
			refill(&br);
			idx = (unsigned int)(br.bits >>
					     (64U - SEQLZ_ESCAPE_BITS));
			drop(&br, SEQLZ_ESCAPE_BITS);
			if (idx >= SEQLZ_TOKEN_SYMBOLS)
				return -EINVAL;
			tok = token_entry(idx, 0);
		}
		{
			/*
			 * The offset: shift the token's code out on top, then
			 * the offset's bits down. Class 0 has none, back is 0,
			 * and the offset is the one before.
			 */
			unsigned int back = (tok >> TOK_BACK_AT) & 63U;
			unsigned int raw =
				(unsigned int)((br.bits << (tok & 63U)) >> back)
				<< ((tok >> TOK_SHIFT_AT) & 3U);

			drop(&br, (tok >> TOK_DROP_AT) & 31U);
			off = back ? raw : last;
		}
		nl = (tok >> TOK_LL_AT) & 15U;
		len = (tok >> TOK_ML_AT) & 63U;
		/*
		 * The fast path: no length value follows, and there is room,
		 * see d_fast. Then nl is at most 14 and len at most 34, and the
		 * copies are 16 literal bytes and 16 to 40 bytes of match,
		 * without loops. The last sequence never takes it: its literals
		 * end at the end of the page, too close.
		 */
		if (!(tok & (1U << TOK_VALUE_AT)) && d <= d_fast &&
		    (unsigned long)lit <= lit_fast &&
		    (in_order || (unsigned long)br.p <= in_fast)) {
			u64 a, b;

			/*
			 * The next token now, before the copies: its entry
			 * takes a while to load, and on an in-order core such
			 * as the Cortex-A55 the copies fill that time. No
			 * length value follows, so the next token's code comes
			 * right after this offset.
			 *
			 * The refill: on an out-of-order core every second fast
			 * sequence, without checking the bits left. Whether
			 * they run short depends on the codes before, which the
			 * branch predictor learns only for a page it has seen,
			 * and a swap-in decodes a page once. A refill leaves at
			 * least 56 bits, and two fast sequences take at most
			 * 50, 11 for each token and up to 14 for each offset;
			 * after a slow one it always refills. 8 bytes of input
			 * are there, and count is at least 0. An in-order core
			 * refills only when the bits run short: its load would
			 * stall every second sequence.
			 */
			if (in_order) {
				tok = next_token(&br, &t->token);
			} else {
				if (!skip) {
					u64 v = get_unaligned_be64(br.p);

					br.bits |= v >> br.count;
					/* as in refill() */
					br.p += (63 - br.count) >> 3;
					br.count |= 56;
				}
				skip ^= 1U;
				tok = t->token.decode[br.bits >>
						      (64U - SEQLZ_TOKEN_BITS)];
			}
			/*
			 * No check of nl against the literals left, which saved
			 * 3 instructions per sequence. The 16 bytes read are
			 * inside lit_bound, so lit stays inside it. If nl goes
			 * past lit_end, bytes of the bitstream were taken as
			 * literals, and the slow path below, where every page
			 * ends, rejects the page because lit > lit_end.
			 */
			__builtin_memcpy(&a, lit, 8);
			__builtin_memcpy(&b, lit + 8, 8);
			__builtin_memcpy(d, &a, 8);
			__builtin_memcpy(d + 8, &b, 8);
			d += nl;
			lit += nl;
			last = off;
			if (off - 1U >= (unsigned int)(d - (u8 *)dst))
				return -EINVAL;
			if (off >= 8U) {
				/*
				 * With 8 <= off < 16 a load reads what the
				 * store before it wrote, as an overlapping
				 * match needs. From m, not d + 8 - off: clang
				 * computed that sum again for every load.
				 */
				const u8 *m = d - off;

				__builtin_memcpy(&a, m, 8);
				__builtin_memcpy(d, &a, 8);
				__builtin_memcpy(&b, m + 8, 8);
				__builtin_memcpy(d + 8, &b, 8);
				if (len > 16U) {
					__builtin_memcpy(&a, m + 16, 8);
					__builtin_memcpy(d + 16, &a, 8);
					__builtin_memcpy(&b, m + 24, 8);
					__builtin_memcpy(d + 24, &b, 8);
					if (len > 32U) {
						__builtin_memcpy(&a, m + 32, 8);
						__builtin_memcpy(d + 32, &a, 8);
					}
				}
			} else {
				/*
				 * off < 8: the off bytes before d repeated to 8
				 * bytes in one register, as in copy_match(),
				 * here with a multiply. repeat[off] has a 1 in
				 * every byte at a multiple of off, so the
				 * multiply puts a copy of the off bytes there.
				 * Five stores step bytes apart cover at least
				 * 28 bytes, the loop the longer matches.
				 */
				static const u64 repeat[8] = {
					0,
					0x0101010101010101ULL,
					0x0001000100010001ULL,
					0x0001000001000001ULL,
					0x0000000100000001ULL,
					0x0000010000000001ULL,
					0x0001000000000001ULL,
					0x0100000000000001ULL
				};
				unsigned int step = page_lz_step_for[off], k;

				/*
				 * little endian, so that the off bytes before d
				 * are the low ones
				 */
				a = get_unaligned_le64(d - off);
				a = (a & (~0ULL >> (64U - 8U * off))) *
				    repeat[off];
				put_unaligned_le64(a, d);
				put_unaligned_le64(a, d + step);
				put_unaligned_le64(a, d + 2U * step);
				put_unaligned_le64(a, d + 3U * step);
				put_unaligned_le64(a, d + 4U * step);
				for (k = 5U * step; k < len; k += step)
					put_unaligned_le64(a, d + k);
			}
			d += len;
			continue;
		}
		/* the slow path: length values, and every check */
		if (nl == SEQLZ_LL_CAP) {
			refill(&br);
			nl += value(&br, &t->ll, &e);
		}
		if (lit > lit_end || nl > (unsigned int)(lit_end - lit) ||
		    nl > (unsigned int)(d_end - d))
			return -EINVAL;
		copy_literals(d, d_end, lit, lit_bound, nl);
		d += nl;
		lit += nl;
		if (d == d_end) {
			/*
			 * the page is full, so this must be the last sequence:
			 * ml - 4 = 0 and class 0
			 */
			if (len != 4U || ((tok >> TOK_BACK_AT) & 63U) != 0U)
				return -EINVAL;
			break;
		}

		if (len == SEQLZ_ML_CAP + 4U) {
			refill(&br);
			len += value(&br, &t->ml, &e);
		}
		last = off;
		/* off must point into the page; off - 1 wraps for 0 */
		if (off - 1U >= (unsigned int)(d - (u8 *)dst) ||
		    len > (unsigned int)(d_end - d))
			return -EINVAL;
		copy_match(d, d_end, off, len);
		d += len;
		tok = next_token(&br, &t->token);
		skip = 0;
	}
	/*
	 * The page must end where its bits end: every byte read, fewer than 8
	 * bits left in the last one, and those 0, as the compressor writes
	 * them. zram has no checksum, so this is what tells a damaged page.
	 */
	if (d != d_end || lit != lit_end || br.count < 0 || br.p != br.end ||
	    br.count >= 8 ||
	    (br.count > 0 && (br.end[-1] & ((1U << br.count) - 1U))))
		return -EINVAL;
	return 0;
}

int seqlz_decode(const struct seqlz_tables *t, const void *src,
		 unsigned int src_len, void *dst, void *scratch)
{
	/*
	 * two copies of the loop in an arm64 kernel, one per kind of core; one
	 * everywhere else
	 */
	return in_order_core() ? decode_page(t, src, src_len, dst, scratch, 1) :
				 decode_page(t, src, src_len, dst, scratch, 0);
}
