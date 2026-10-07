// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "seqlz.h"

#include "page_lz.h"

static_assert(PAGE_LZ_PAGE == SEQLZ_PAGE &&
		      PAGE_LZ_HASH_BITS == SEQLZ_HASH_BITS,
	      "page_lz.h and seqlz.h disagree");

/*
 * Decode table entries, 0 for bits that start no code.
 *
 * Length tables: bits 0-3 the code length, bits 4-7 the number of extra bits,
 * bits 8-23 the base value. A value is base + extra bits.
 *
 * Token table: see token_entry().
 */
/*
 * The encoder's arrays have a power of 2 entries and its indices and shifts are
 * masked: the kernel builds with -fsanitize=bounds-strict and -fsanitize=shift,
 * and a check the compiler cannot prove away is a compare and a branch.
 */
#define ENC_LEN_SYMBOLS 32U
static_assert(SEQLZ_LEN_SYMBOLS <= ENC_LEN_SYMBOLS,
	      "the length values' enc[] is too small");
static_assert(SEQLZ_TOKEN_BITS < 16U && SEQLZ_MAX_BITS < 16U &&
		      SEQLZ_LIT_BITS < 16U,
	      "first_codes() has arrays of 16 code lengths");

struct value_table {
	u32 decode[1U << SEQLZ_MAX_BITS];
	u32 enc[ENC_LEN_SYMBOLS]; /* code | length << 16 */
};

struct token_table {
	u32 decode[1U << SEQLZ_TOKEN_BITS]; /* token_entry() */
	/* code | length << 12: 6146 bytes in L1 next to the hash table */
	u16 enc[SEQLZ_TOKEN_SYMBOLS + 1];
};

struct lit_table {
	u16 decode[1U << SEQLZ_LIT_BITS]; /* code length | symbol << 8 */
	u32 enc[256]; /* code length | code << 8 */
};

/*
 * per byte its code lengths in all literal tables, 8 bits each: the bits of the
 * literals in all tables at once are one add per literal, at most LIT_COST_RUN
 * literals before a lane could overflow
 */
#define LIT_COST_WORDS ((SEQLZ_LIT_SETS + 7U) / 8U)
#define LIT_COST_RUN (255U / SEQLZ_LIT_BITS) /* 25 */
static_assert(
	SEQLZ_LIT_SETS == 8U &&
		SEQLZ_SIZE_BITS_MAX - SEQLZ_SIZE_BITS_MIN == 7U &&
		SEQLZ_LIT_ZERO_AT - SEQLZ_LIT_WIDTH_AT == 3 &&
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
	struct value_table ll, ml;
	/*
	 * every symbol has a code, which the encoder needs; the decoder
	 * does not
	 */
	bool all_symbols;
};

size_t seqlz_tables_size(void)
{
	return sizeof(struct seqlz_tables);
}

/* base and extra bits of a length value symbol, see seqlz.h */
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
 * The decoder's entry for token symbol s with a code of n bits: what it needs,
 * ready to use.
 *   bits  0..5:  n: the stream shifted left by it has the raw bits of the
 *                offset on top
 *   bits  6..11: 64 - raw bits, mod 64: then shifted right by it they are
 *                the number; 0 for the last offset
 *   bits 12..13: the shift of the offset, 3 for classes 4 and 5
 *   bits 14..18: n + raw bits, the bits of the sequence
 *   bits 19..22: ll, bits 23..28: ml
 *   bit  30:     a length value follows, for ll or ml
 *   bit  31:     the escape, with n in bits 14..18
 * Unpacking the class at run time took 18 instructions per sequence, which a
 * Cortex-A55 issues at most two per cycle.
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
 * The first canonical code of each length, from n code lengths, none longer
 * than bits; 0 is no code. -EINVAL if a length is longer, or the codes are not
 * a complete prefix code: over-subscribed, with gaps, or none at all. Complete
 * codes fill the decode table exactly, so every bit pattern starts a code and
 * the decoder does not check for one that does not. Huffman codes are complete.
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
 * The token's codes and its decode table: the bitstream is read most
 * significant bit first, so the entries of a code are one range, token_entry()
 * at every index whose top bits are the code.
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
 * A literal table's codes, not reversed: the literals' streams are read most
 * significant bit first, so the decode table is indexed by the next
 * SEQLZ_LIT_BITS bits from the top.
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
	 * zero: the codes of symbols without one, and the encoder's entries
	 * behind the last symbol
	 */
	memset(t, 0, sizeof(*t));
	if (build_token(lengths->token, &t->token) ||
	    build_values(lengths->ll, &t->ll) ||
	    build_values(lengths->ml, &t->ml))
		return -EINVAL;
	{
		/* the literal tables are compiled in, seqlz_lit_sets */
		unsigned int k;

		for (k = 0; k < SEQLZ_LIT_SETS; k++)
			if (build_lit(seqlz_lit_sets[k], &t->lit[k]))
				return -EINVAL;
		for (k = 0; k < 256U * SEQLZ_LIT_SETS; k++)
			t->lit_cost[k % 256][k / 256 / 8] |=
				(u64)seqlz_lit_sets[k / 256][k % 256]
				<< (8U * (k / 256 % 8));
	}
	{
		/*
		 * every token has a code or the escape has one, every length
		 * value has one
		 */
		unsigned int k, all = 1;

		for (k = 0; k < SEQLZ_TOKEN_SYMBOLS; k++)
			all &= lengths->token[k] != 0 ||
			       lengths->token[SEQLZ_ESCAPE] != 0;
		/*
		 * an escaped token and an offset of QUETSCHN_PAGE_BITS bits in
		 * at most 31 bits, the encoder's bound
		 */
		all &= lengths->token[SEQLZ_ESCAPE] <= SEQLZ_MAX_ESCAPE_LEN;
		for (k = 0; k < SEQLZ_LEN_SYMBOLS; k++)
			all &= lengths->ll[k] != 0 && lengths->ml[k] != 0;
		for (k = 0; k < 256 * SEQLZ_LIT_SETS; k++)
			all &= seqlz_lit_sets[k / 256][k % 256] != 0;
		t->all_symbols = all;
	}
	return 0;
}

/* ---- matcher, see page_lz.h ---- */

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
 * One encoder for both entry points. Literals go to the front of dst, right
 * after the header; the bitstream goes behind the room of a page of literals
 * and is moved in behind the literals at the end. dst has two pages, and that
 * is always enough: a sequence takes at most 31 bits for its token and offset,
 * an escaped token of SEQLZ_MAX_ESCAPE_LEN + 12 bits and an offset of
 * QUETSCHN_PAGE_BITS, and covers at least 4 bytes of the page, so at most
 * SEQLZ_PAGE / 4 * 31 + 31 bits: 3972 bytes for 4 KiB pages, where behind 2 +
 * 4096 + 16 bytes of header, literals and room for their 16-byte copies there
 * are 4078; 15 876 against 16 366 for 16 KiB pages. The token codes are capped
 * at 11 bits and the escape at SEQLZ_MAX_ESCAPE_LEN, so this holds for any
 * tables the encoder takes.
 *
 * The bit writer is a 64-bit accumulator, stored 8 bytes at a time and advanced
 * by the whole bytes. It holds at most 7 bits after a flush, so one flush per
 * sequence is enough: 7 bits, 31 of token and offset and 20 of a match length
 * value (SEQLZ_MAX_BITS + the page's bits) are 58, 22 and 60 for 16 KiB pages.
 * Only a literal length value, another 20 or 22, needs its own flush.
 */
/*
 * where the encoder writes the bitstream in dst d: behind the header, a page of
 * literals and the 16 bytes their copies may write behind them
 */
#define ENC_BITS(d) ((d) + SEQLZ_HEADER + SEQLZ_PAGE + 16U)

struct encoder {
	const struct seqlz_tables *t;
	u64 acc;
	unsigned int cnt;
	u8 *p; /* bitstream */
	u8 *lit; /* literals */
	const u8 *src_end; /* the 16-byte literal copies may read up to here */
	unsigned int last; /* the last offset */
};

/*
 * v has n bits, n < 64 - cnt; most significant bit first, the bits in acc are
 * its low cnt bits
 */
static __always_inline void enc_put(struct encoder *e, u64 v, unsigned int n)
{
	e->acc = e->acc << (n & 63U) | v;
	e->cnt += n;
}

/* a code from an enc[] entry, followed by the n_extra low bits of extra */
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
 * the whole bytes out, big endian; the bits of acc above cnt are old ones,
 * shifted out here
 */
static __always_inline void enc_flush(struct encoder *e)
{
	put_unaligned_be64(e->acc << ((64U - e->cnt) & 63U), e->p);
	e->p += e->cnt >> 3;
	e->cnt &= 7U;
}

static __always_inline void
put_len_value(struct encoder *e, const struct value_table *t, unsigned int v)
{
	unsigned int extra,
		s = seqlz_len_symbol(v, &extra) & (ENC_LEN_SYMBOLS - 1U);

	enc_put_code(e, t->enc[s], v, extra);
}

/*
 * the code of a token and its length; the escape and the token for a token
 * without a code
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

/* one sequence: its literals from in, ml 0 for the last one */
static __always_inline void encode_emit(void *ctx, const u8 *in,
					unsigned int ll, unsigned int ml,
					unsigned int off)
{
	struct encoder *e = ctx;
	const struct seqlz_tables *t = e->t;
	unsigned int k = 0;

	/*
	 * the literals, the first 16 bytes without a loop to mispredict; dst
	 * has room behind them
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
		/* the last sequence */
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
		 * An offset of class 1 to 5 has no more bits than the class
		 * sends, so only class 0, the last offset, needs a mask: it
		 * sends none.
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
					 const u8 *src_end)
{
	*e = (struct encoder){ .t = t,
			       .p = ENC_BITS(d),
			       .lit = d + SEQLZ_HEADER,
			       .src_end = src_end,
			       .last = 1 };
}

/* the last bits, the header, and the bitstream moved in behind the literals */
static unsigned int encoder_finish(struct encoder *e, u8 *d)
{
	u8 *bits = ENC_BITS(d);
	unsigned int n_lit = (unsigned int)(e->lit - (d + SEQLZ_HEADER)), bytes;

	if (e->cnt > 0) {
		put_unaligned_be64(e->acc << ((64U - e->cnt) & 63U), e->p);
		e->p++;
	}
	bytes = (unsigned int)(e->p - bits);
	store16(d, n_lit);
	memmove(e->lit, bits, bytes);
	return SEQLZ_HEADER + n_lit + bytes;
}

static unsigned int encode_raw(const struct seqlz_tables *t,
			       const struct seqlz_sequence *seq, unsigned int n,
			       const u8 *literals, unsigned int n_literals,
			       void *dst, unsigned int dst_cap)
{
	const u8 *in = literals;
	struct encoder e;
	unsigned int i;

	if (dst_cap < 2U * SEQLZ_PAGE || !t->all_symbols || n == 0 ||
	    n > SEQLZ_MAX_SEQUENCES || n_literals > SEQLZ_PAGE)
		return 0;
	encoder_init(&e, t, dst, literals + n_literals);
	for (i = 0; i < n; i++) {
		unsigned int ll = seq[i].literals;

		if (ll > (unsigned int)(literals + n_literals - in))
			return 0;
		encode_emit(&e, in, ll, i + 1 == n ? 0U : seq[i].match,
			    seq[i].offset);
		in += ll;
	}
	return encoder_finish(&e, dst);
}

/*
 * One literal into a stream: the code shifted in from the right, the shift
 * takes the code length from the low 6 bits of the entry. The entries of a
 * stream's literals are summed: the low byte of the sum is their bits, at most
 * 4 * 10 between two flushes.
 */
#define ENC_LIT(acc, sum, b)                           \
	do {                                           \
		u32 e_ = enc[b];                       \
		(acc) = (acc) << (e_ & 63U) | e_ >> 8; \
		(sum) += e_;                           \
	} while (0)

/*
 * The whole bytes of a stream's accumulator out, and the byte of the bits left
 * over, which the next flush writes again. cnt is the number of bits in acc;
 * the bits above it are old ones, shifted out here. 8 bytes at once while the
 * stream has room for them, it ends where the next begins. Swapped once, before
 * the branch: put_unaligned_be64() in both gave code_literals() other code on
 * arm64.
 */
#define ENC_FLUSH(p, end, acc, cnt, sum)                                 \
	do {                                                             \
		unsigned int n_ = (cnt) + ((sum) & 255U);                \
		u64 w_ = __builtin_bswap64((acc) << ((64U - n_) & 63U)); \
		if ((end) - (p) >= 8)                                    \
			put_unaligned_le64(w_, (p));                     \
		else                                                     \
			store_tail((p), w_, (n_ + 7U) >> 3);             \
		(p) += n_ >> 3;                                          \
		(cnt) = n_ & 7U;                                         \
		(sum) = 0;                                               \
	} while (0)

/* the first n bytes of w as put_unaligned_le64() would write them, n < 8 */
static void store_tail(u8 *p, u64 w, unsigned int n)
{
	u8 b[8];
	unsigned int k;

	put_unaligned_le64(w, b);
	for (k = 0; k < n; k++)
		p[k] = b[k];
}

/*
 * the bits of stream j in literal table k, a 16-bit lane of even or odd, see
 * code_literals()
 */
static __always_inline unsigned int lit_bits(u64 even[][LIT_COST_WORDS],
					     u64 odd[][LIT_COST_WORDS],
					     unsigned int j, unsigned int k)
{
	return (unsigned int)(((k & 1U ? odd : even)[j][k / 8] >>
			       (16U * (k % 8 / 2))) &
			      0xffffU);
}

/*
 * The raw page of len bytes in d, as the encoder writes it, turned into one
 * with coded literals where that saves at least 1/16 of them: decoding coded
 * literals costs time per byte, and coding them where they save anything saved
 * less than 0.1 points more. Returns the new length, len if the literals stay
 * raw. d has two pages: the literals and the bitstream are moved to its end,
 * the coded literals are written from the front, and the bitstream is moved in
 * behind them.
 */
static unsigned int code_literals(const struct seqlz_tables *t, u8 *d,
				  unsigned int len)
{
	/* every second byte: 8-bit lanes widened to 16 */
	const u64 lanes = 0x00ff00ff00ff00ffULL;
	const unsigned int n_literals = load16(d), body = len - SEQLZ_HEADER;
	const u8 *literals = d + SEQLZ_HEADER;
	unsigned int bits = ~0U, k, j, coded, set = 0, sizes[SEQLZ_LIT_STREAMS],
		     all, width, header;
	/*
	 * per stream the bits in all tables, 16-bit lanes: tables 0, 2, 4, 6
	 * and 1, 3, 5, 7 of each word
	 */
	u64 even[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { { 0 } },
	    odd[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { { 0 } };
	unsigned int w;
	/* where each stream starts, and the end of the last */
	u8 *q[SEQLZ_LIT_STREAMS + 1];
	const struct lit_table *lt;

	/*
	 * the bits of each stream in each table, LIT_COST_RUN literals of a
	 * stream per sum of 8-bit lanes
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
	lt = &t->lit[set];
	for (j = 0, coded = 0, all = 0; j < SEQLZ_LIT_STREAMS; j++) {
		sizes[j] = (lit_bits(even, odd, j, set) + 7U) / 8U;
		coded += sizes[j];
		all |= sizes[j];
	}
	/*
	 * the sizes in as many bits as the largest needs, at least
	 * SEQLZ_SIZE_BITS_MIN
	 */
	width = 32U - (unsigned int)__builtin_clz(
			      all | 1U << (SEQLZ_SIZE_BITS_MIN - 1U));
	header = SEQLZ_LIT_HEADER(width);
	if (coded + SEQLZ_LIT_CODED_MIN >= n_literals - n_literals / 16U)
		return len;
	/*
	 * The coded literals, and the stores up to 16 bytes behind them, must
	 * stay in front of the moved literals they come from; saving 1/16 makes
	 * sure of it for a page, this is for the reader.
	 */
	if (header + coded + 16U > 2U * SEQLZ_PAGE - body)
		return len;
	literals = d + 2U * SEQLZ_PAGE - body;
	memmove(d + 2U * SEQLZ_PAGE - body, d + SEQLZ_HEADER, body);
	q[0] = d + header;
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		q[j + 1] = q[j] + sizes[j];
	/*
	 * Eight streams, literal k in stream k % 8, so that the decoder has
	 * eight chains side by side; most significant bit first, see
	 * decode_literals(). Four at a time, each straight to its place: one
	 * stream after the other waited for its accumulator, 3.5 cycles per
	 * literal.
	 */
	{
		const u32 *const enc = lt->enc;
		unsigned int half;

		for (half = 0; half < SEQLZ_LIT_STREAMS; half += 4) {
			u8 *p0 = q[half], *p1 = q[half + 1], *p2 = q[half + 2],
			   *p3 = q[half + 3];
			u8 *e0 = p1, *e1 = p2, *e2 = p3, *e3 = q[half + 4];
			u64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
			unsigned int c0 = 0, c1 = 0, c2 = 0, c3 = 0, s0 = 0,
				     s1 = 0, s2 = 0, s3 = 0;

			/*
			 * blocks of 32 literals, 4 per stream between two
			 * flushes; the loop stops 28 literals before the end,
			 * so the rest below has at most 4 per stream
			 */
			for (k = half; k + 28U < n_literals; k += 32) {
				for (j = 0; j < 32U; j += SEQLZ_LIT_STREAMS) {
					ENC_LIT(a0, s0, literals[k + j]);
					ENC_LIT(a1, s1, literals[k + j + 1]);
					ENC_LIT(a2, s2, literals[k + j + 2]);
					ENC_LIT(a3, s3, literals[k + j + 3]);
				}
				ENC_FLUSH(p0, e0, a0, c0, s0);
				ENC_FLUSH(p1, e1, a1, c1, s1);
				ENC_FLUSH(p2, e2, a2, c2, s2);
				ENC_FLUSH(p3, e3, a3, c3, s3);
			}
			/* the rest, at most 4 literals per stream */
			for (; k < n_literals; k++) {
				switch (k % SEQLZ_LIT_STREAMS) {
				case 0:
				case 4:
					ENC_LIT(a0, s0, literals[k]);
					break;
				case 1:
				case 5:
					ENC_LIT(a1, s1, literals[k]);
					break;
				case 2:
				case 6:
					ENC_LIT(a2, s2, literals[k]);
					break;
				default:
					ENC_LIT(a3, s3, literals[k]);
				}
				if ((k & 3U) == 3U)
					k += 4;
			}
			ENC_FLUSH(p0, e0, a0, c0, s0);
			ENC_FLUSH(p1, e1, a1, c1, s1);
			ENC_FLUSH(p2, e2, a2, c2, s2);
			ENC_FLUSH(p3, e3, a3, c3, s3);
		}
	}
	store16(d, SEQLZ_LIT_CODED | n_literals);
	/*
	 * byte 2: the table in bits 0 to 2, width - SEQLZ_SIZE_BITS_MIN in bits
	 * 3 to 5
	 */
	d[2] = (u8)(set | (width - SEQLZ_SIZE_BITS_MIN) << SEQLZ_LIT_WIDTH_AT);
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
	coded += header;
	memmove(d + coded, literals + n_literals, body - n_literals);
	return coded + body - n_literals;
}

unsigned int seqlz_encode(const struct seqlz_tables *t,
			  const struct seqlz_sequence *seq, unsigned int n,
			  const u8 *literals, unsigned int n_literals,
			  void *dst, unsigned int dst_cap, bool coded)
{
	unsigned int len =
		encode_raw(t, seq, n, literals, n_literals, dst, dst_cap);

	return len == 0 || !coded ? len : code_literals(t, dst, len);
}

static unsigned int compress_page(const struct seqlz_tables *t,
				  struct seqlz_state *st, const u8 *src,
				  void *dst, unsigned int dst_cap)
{
	struct encoder e;

	if (dst_cap < 2U * SEQLZ_PAGE || !t->all_symbols)
		return 0;
	encoder_init(&e, t, dst, src + SEQLZ_PAGE);
	match_page(st->table, src, encode_emit, &e);
	return encoder_finish(&e, dst);
}

unsigned int seqlz_compress(const struct seqlz_tables *t,
			    struct seqlz_state *st, const void *src, void *dst,
			    unsigned int dst_cap, bool coded)
{
	unsigned int len = compress_page(t, st, src, dst, dst_cap);

	return len == 0 || !coded ? len : code_literals(t, dst, len);
}

/* ---- decoder ---- */

/*
 * Two choices that an in-order core makes the other way, by the core's id on
 * arm64 kernels; every other build counts as out of order, x86-64 too.
 * - The token table, 2048 entries of 4 bytes, prefetched before a page. An
 *   in-order core waits for each miss it did not see coming: a Cortex-A55 read
 *   cold 14.5 us slower without the prefetches. An out-of-order core fetches
 *   the 43 or so lines a page needs as it needs them, and the 128 prefetches
 *   are in the way: a Cortex-A76 read cold 1.3 to 2.6 us faster without this
 *   one. Every build but an arm64 kernel on an out-of-order core prefetches.
 * - The refill on the fast path, see seqlz_decode(): every second sequence on
 *   an out-of-order core, only when the bits run short on an in-order one,
 *   which cannot hide the refill's load.
 * The numbers are in docs/explored-designs.md.
 */
#if defined(__KERNEL__) && defined(__aarch64__)
#include <asm/cputype.h>
/*
 * Cortex-A53, A55, A510, A520, and Qualcomm's Kryo silver cores. MIDR: the
 * implementer in bits 24 to 31, the part number in bits 4 to 15. ARM (0x41):
 * Cortex-A53 0xd03, A55 0xd05, A510 0xd46, A520 0xd80. Qualcomm (0x51): Kryo
 * 2xx silver 0x801, a Cortex-A53, Kryo 3xx and 4xx silver 0x803 and 0x805. As
 * numbers: the phone's 4.14 tree lacks some of the kernel's macros for them and
 * names others differently.
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
/* for tests of the in-order path: -DSEQLZ_IN_ORDER=1 */
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
 * Most significant bit first, the next bits on top of bits. Refill loads 8
 * bytes at once while at least 8 are left in the stream, and byte by byte at
 * the end, so it never reads past the stream. Past the end it shifts in zeros
 * and count goes negative; memory safety does not depend on the bits, every
 * length and offset is checked where it is used.
 */
struct bit_reader {
	const u8 *p;
	const u8 *end;
	u64 bits;
	/*
	 * negative once more bits were used than the stream has: the page is
	 * not valid
	 */
	int count;
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
		 * the whole bytes that fit beside the count bits; count becomes
		 * 56 + count % 8
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

static inline void drop(struct bit_reader *r, unsigned int n)
{
	r->bits <<= n;
	r->count -= (int)n;
}

/*
 * One value: the table entry of the next code, then base + extra bits, in one
 * step. Needs SEQLZ_MAX_BITS + QUETSCHN_PAGE_BITS bits, 20 or 22, refilled
 * before. The entry is 0 for bits that start no code.
 */
static inline unsigned int value(struct bit_reader *r,
				 const struct value_table *t, u32 *entry)
{
	u32 e = t->decode[r->bits >> (64U - SEQLZ_MAX_BITS)];
	unsigned int n = e & 15U, x = (e >> 4) & 15U;
	/*
	 * the x bits behind the code, in two shifts: one of 64 - x is undefined
	 * for x = 0
	 */
	unsigned int v = ((e >> 8) & 0xffffU) +
			 (unsigned int)(((r->bits << n) >> 1) >> (63U - x));

	*entry = e;
	drop(r, n + x);
	return v;
}

/*
 * The literals' streams, as zstd reads its Huffman coded literals: most
 * significant bit first, 64 bits from ip with the lowest one replaced by a 1,
 * shifted left by the bits of ip's byte already used. Each code shifts the
 * container further left, so the marker's position is the number of bits used
 * since ip: a refill needs no counter, and a stream is two registers, ip and
 * the container. Streams with a pointer, a container and a counter each did not
 * fit into the registers of x86-64. A refill leaves at least 56 bits, the
 * marker is never reached.
 */
/* the 8 bytes at ip, zeros behind end */
static noinline __cold u64 lit_load_tail(const u8 *ip, const u8 *end)
{
	u8 b[8] = { 0 };
	unsigned int k;

	for (k = 0; k < 8U && ip + k < end; k++)
		b[k] = ip[k];
	return get_unaligned_be64(b);
}

/*
 * 8 bytes at ip; past the stream's end they are the next stream's or the
 * sequences', and a stream that uses them fails the check of its size
 */
#define LIT_REFILL(ip, bits)                                              \
	do {                                                              \
		unsigned int used_ = (unsigned int)__builtin_ctzll(bits); \
		(ip) += used_ >> 3;                                       \
		(bits) = ((end - (ip) >= 8 ? get_unaligned_be64(ip) :     \
					     lit_load_tail((ip), end)) |  \
			  1U)                                             \
			 << (used_ & 7U);                                 \
	} while (0)

/* the next literal; the entry: code length in bits 0 to 5, symbol above */
#define LIT_DECODE(out, bits)                                           \
	do {                                                            \
		unsigned int e_ = lt[(bits) >> (64U - SEQLZ_LIT_BITS)]; \
		(out) = (u8)(e_ >> 8);                                  \
		(bits) <<= e_ & 63U;                                    \
	} while (0)

/*
 * the same, but the stream only moves on where mask is 63; where it is 0 the
 * symbol is decoded and not taken
 */
#define LIT_DECODE_MASKED(out, bits, mask)                              \
	do {                                                            \
		unsigned int e_ = lt[(bits) >> (64U - SEQLZ_LIT_BITS)]; \
		(out) = (u8)(e_ >> 8);                                  \
		(bits) <<= e_ & (mask);                                 \
	} while (0)

/*
 * The coded literals of a page into out, see seqlz_encode(). Returns where the
 * sequences' bitstream starts, NULL if the page is not valid. Not inlined: in
 * the loop over the sequences its registers made seqlz-fast's pages 12% slower.
 */
static noinline __aligned(64) const u8 *decode_literals(
	const struct seqlz_tables *t, const u8 *s, unsigned int src_len,
	unsigned int n_lit, u8 *out)
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
	lt = t->lit[s[2] & (SEQLZ_LIT_SETS - 1U)].decode;
	prefetch_lines(lt, sizeof(t->lit[0].decode));
	{
		/*
		 * the sizes copied out first: a 4-byte load at the last one's
		 * byte reads past the header
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
	if (SEQLZ_LIT_HEADER(width) + total > src_len)
		return NULL;
	/*
	 * full rounds only: the last, partial one below decodes no literal that
	 * does not exist
	 */
	for (k = 0; k + SEQLZ_LIT_STREAMS * SEQLZ_LIT_ROUNDS <= n_lit;
	     k += SEQLZ_LIT_STREAMS * SEQLZ_LIT_ROUNDS) {
		unsigned int j;
		const u8 *i0 = ip[0], *i1 = ip[1], *i2 = ip[2], *i3 = ip[3],
			 *i4 = ip[4], *i5 = ip[5], *i6 = ip[6], *i7 = ip[7];

		LIT_REFILL(i0, b0);
		LIT_REFILL(i1, b1);
		LIT_REFILL(i2, b2);
		LIT_REFILL(i3, b3);
		LIT_REFILL(i4, b4);
		LIT_REFILL(i5, b5);
		LIT_REFILL(i6, b6);
		LIT_REFILL(i7, b7);
		ip[0] = i0;
		ip[1] = i1;
		ip[2] = i2;
		ip[3] = i3;
		ip[4] = i4;
		ip[5] = i5;
		ip[6] = i6;
		ip[7] = i7;
		for (j = 0; j < SEQLZ_LIT_ROUNDS; j++) {
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j], b0);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 1], b1);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 2], b2);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 3], b3);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 4], b4);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 5], b5);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 6], b6);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 7], b7);
		}
	}
	{
		/*
		 * The rest, fewer than 40 literals: steps of all 8 streams,
		 * then one more in which the streams at r and above, which have
		 * no literal left, decode one and do not move on, so that each
		 * stream ends right behind its own codes. What they decode
		 * lands behind the literals, in the 16 bytes the scratch has
		 * there. The same steps for every stream; a loop per stream
		 * ends after a different number of literals on every page.
		 */
		const unsigned int rest = n_lit - k,
				   steps = rest / SEQLZ_LIT_STREAMS,
				   r = rest % SEQLZ_LIT_STREAMS;
		const u8 *i0 = ip[0], *i1 = ip[1], *i2 = ip[2], *i3 = ip[3],
			 *i4 = ip[4], *i5 = ip[5], *i6 = ip[6], *i7 = ip[7];
		unsigned int j;

		LIT_REFILL(i0, b0);
		LIT_REFILL(i1, b1);
		LIT_REFILL(i2, b2);
		LIT_REFILL(i3, b3);
		LIT_REFILL(i4, b4);
		LIT_REFILL(i5, b5);
		LIT_REFILL(i6, b6);
		LIT_REFILL(i7, b7);
		for (j = 0; j < steps; j++) {
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j], b0);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 1], b1);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 2], b2);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 3], b3);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 4], b4);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 5], b5);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 6], b6);
			LIT_DECODE(out[k + SEQLZ_LIT_STREAMS * j + 7], b7);
		}
		k += SEQLZ_LIT_STREAMS * steps;
		LIT_DECODE_MASKED(out[k], b0, (0U - (r > 0U)) & 63U);
		LIT_DECODE_MASKED(out[k + 1], b1, (0U - (r > 1U)) & 63U);
		LIT_DECODE_MASKED(out[k + 2], b2, (0U - (r > 2U)) & 63U);
		LIT_DECODE_MASKED(out[k + 3], b3, (0U - (r > 3U)) & 63U);
		LIT_DECODE_MASKED(out[k + 4], b4, (0U - (r > 4U)) & 63U);
		LIT_DECODE_MASKED(out[k + 5], b5, (0U - (r > 5U)) & 63U);
		LIT_DECODE_MASKED(out[k + 6], b6, (0U - (r > 6U)) & 63U);
		LIT_DECODE_MASKED(out[k + 7], b7, (0U - (r > 7U)) & 63U);
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

		/* the codes of each stream's literals fit into its size */
		for (k = 0; k < SEQLZ_LIT_STREAMS; k++)
			if (8L * (ip[k] - start[k]) + __builtin_ctzll(bb[k]) >
			    8L * sz[k])
				return NULL;
	}
	return q + total;
}

/*
 * seqlz_decode() for one kind of core, in_order a constant: on an in-order core
 * the loop is the one without the refill every second fast sequence, not the
 * same loop with a branch more
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
	/*
	 * the decoder's tables, so that their misses overlap when they are cold
	 */
	if (prefetch_tokens(in_order))
		prefetch_lines(t->token.decode, sizeof(t->token.decode));
	prefetch_lines(t->ll.decode, sizeof(t->ll.decode));
	prefetch_lines(t->ml.decode, sizeof(t->ml.decode));
	n_lit = load16(s);
	if (n_lit & SEQLZ_LIT_CODED) {
		const u8 *q;

		n_lit &= SEQLZ_LIT_CODED - 1U;
		if (!scratch)
			return -EINVAL;
		q = decode_literals(t, s, src_len, n_lit, scratch);
		if (!q)
			return -EINVAL;
		br = (struct bit_reader){ .p = q, .end = s_end };
		lit = scratch;
		lit_end = lit + n_lit;
		lit_bound = lit + SEQLZ_SCRATCH;
	} else {
		if ((u64)SEQLZ_HEADER + n_lit > src_len)
			return -EINVAL;
		lit = s + SEQLZ_HEADER;
		lit_end = lit + n_lit;
		lit_bound = s_end;
		br = (struct bit_reader){ .p = lit_end, .end = s_end };
	}

	/*
	 * the fast path below needs 64 bytes of room in the page, it stores at
	 * most 54: up to 14 literals and 40 bytes of match; and 16 bytes of
	 * literals or input, a compare each; lit_fast as a number, because the
	 * input can be shorter than 16 bytes
	 */
	d_fast = d_end - 64;
	lit_fast = (unsigned long)lit_bound - 16U;
	/*
	 * and 8 bytes of input for the refill without a check; 0 when there are
	 * fewer at all
	 */
	in_fast = src_len >= 8U ? (unsigned long)s_end - 8U : 0;

	/*
	 * A refill only when token and offset might not fit, 11 + 12 bits: the
	 * next token's lookup then does not wait for the refill's load, and a
	 * refill leaves 56 bits for two or three sequences. The escape and the
	 * length values refill before they read. The token's entry has the
	 * class of the offset, so its raw bits are known without a second
	 * lookup.
	 */
#define NEXT_TOKEN()                                                         \
	do {                                                                 \
		if (br.count < (int)(SEQLZ_TOKEN_BITS + QUETSCHN_PAGE_BITS)) \
			refill(&br);                                         \
		tok = t->token.decode[br.bits >> (64U - SEQLZ_TOKEN_BITS)];  \
	} while (0)

	NEXT_TOKEN();
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
			 * the raw bits: the token's code shifted out on top,
			 * then down to their place
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
		 * Far from the end of the page and of the literals, and no
		 * length value: 16 literal bytes and 16 or 32 bytes of match
		 * copied without checking the room behind them; nl + len is at
		 * most 14 + 34, and the last sequence always has its literals
		 * up to the end of the page.
		 */
		if (!(tok & (1U << TOK_VALUE_AT)) && d <= d_fast &&
		    (unsigned long)lit <= lit_fast &&
		    (in_order || (unsigned long)br.p <= in_fast)) {
			u64 a, b;

			/*
			 * The next token now, before the copies: no length
			 * value follows this one, and the copies fill the time
			 * the load takes on an in-order core such as the
			 * Cortex-A55. A fast sequence is never the last, that
			 * one ends the page.
			 *
			 * The refill: on an out-of-order core every second fast
			 * sequence, without a check of the bits left. Whether
			 * they run short depends on the codes before, which the
			 * branch predictor learns only for a page it has seen,
			 * and a swap-in decodes a page once. A refill leaves at
			 * least 56 bits, two fast sequences take at most
			 * 2 * (11 + 14), after a slow one it always refills; 8
			 * bytes of input are there, and count is at least 0. On
			 * an in-order core only when they run short: there the
			 * load would wait in every second sequence.
			 */
			if (in_order) {
				NEXT_TOKEN();
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
			 * No check of nl against the literals left: the 16
			 * bytes are inside lit_bound, so lit stays inside too.
			 * Past lit_end it reads the next bytes of the input,
			 * and the page is rejected by the next sequence on the
			 * path below, where every page ends, because lit >
			 * lit_end. 3 instructions less per sequence.
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
				 * for 8 <= off < 16 each load reads what the
				 * stores before it wrote, which is right. From
				 * m, not d + 8 - off: clang computed that sum
				 * anew for every load.
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
				 * The off bytes before d repeated to 8 bytes;
				 * step, the largest multiple of off up to 8,
				 * apart it is the same 8 bytes again, so the
				 * same register is stored at each step, without
				 * loads. Five stores cover at least 28 bytes.
				 * repeat[off] has a 1 in each byte at a
				 * multiple of off, so the multiply puts the off
				 * bytes there.
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
			 * the last sequence: ml - 4 = 0 and class 0, nothing
			 * else is a valid page
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
		/* off - 1 wraps for 0 */
		if (off - 1U >= (unsigned int)(d - (u8 *)dst) ||
		    len > (unsigned int)(d_end - d))
			return -EINVAL;
		copy_match(d, d_end, off, len);
		d += len;
		NEXT_TOKEN();
		skip = 0;
	}
#undef NEXT_TOKEN
	if (d != d_end || lit != lit_end || br.count < 0)
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
