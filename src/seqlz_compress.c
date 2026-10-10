// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The compressor: the matcher of page_lz.h, which finds a page's sequences,
 * and the encoder, which writes them as it finds them.
 */
#include "seqlz_internal.h"

#include "page_lz.h"

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

	seqlz_match_page(st->table, src, find_emit, &f);
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

struct seqlz_encoder {
	const struct seqlz_tables *t;
	u64 acc; /* the bits not yet written, the newest in the low bits */
	unsigned int cnt; /* how many bits of acc count */
	u8 *p; /* the next byte of the bitstream goes right below p */
	u8 *lit; /* where the next literal goes */
	const u8 *src_end; /* copies of 16 literals may read up to here */
	unsigned int last; /* the offset of the sequence before, class 0 */
};

/* appends the n bits of v; n < 64 - cnt */
static __always_inline void enc_put(struct seqlz_encoder *e, u64 v,
				    unsigned int n)
{
	e->acc = e->acc << (n & 63U) | v;
	e->cnt += n;
}

/*
 * appends a code from an enc[] entry, code | length << 16, and after it the
 * n_extra low bits of extra
 */
static __always_inline void enc_put_code(struct seqlz_encoder *e, u32 entry,
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
static __always_inline void enc_flush(struct seqlz_encoder *e)
{
	put_unaligned_le64(e->acc << ((64U - e->cnt) & 63U), e->p - 8);
	e->p -= e->cnt >> 3;
	e->cnt &= 7U;
}

/* a length value: its symbol's code and its extra bits */
static __always_inline void put_len_value(struct seqlz_encoder *e,
					  const struct value_table *t,
					  unsigned int v)
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
		*len = te >> TOK_ENC_LEN_AT;
		return te & TOK_ENC_CODE_MASK;
	}
	ee = t->token.enc[SEQLZ_ESCAPE];
	*len = (ee >> TOK_ENC_LEN_AT) + SEQLZ_ESCAPE_BITS;
	return (ee & TOK_ENC_CODE_MASK) << SEQLZ_ESCAPE_BITS | tok;
}

/*
 * Writes one sequence: its literals from in, then its token, offset and length
 * values. ml is 0 for the last sequence.
 */
static __always_inline void encode_emit(void *ctx, const u8 *in,
					unsigned int ll, unsigned int ml,
					unsigned int off)
{
	struct seqlz_encoder *e = ctx;
	const struct seqlz_tables *t = e->t;
	unsigned int k = 0, tlen, raw_bits, cls;
	u32 code;

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
		code = token_code(t, seqlz_token(ll, 0, 0), &tlen);
		enc_put(e, code, tlen);
		if (ll >= SEQLZ_LL_CAP)
			put_len_value(e, &t->ll, ll - SEQLZ_LL_CAP);
		enc_flush(e);
		return;
	}
	/* token and offset in one put, the length values after them */
	cls = seqlz_off_class(off, e->last, &raw_bits);
	code = token_code(t, seqlz_token(ll, ml, cls), &tlen);
	/*
	 * The offset's bits right after the token's code. Classes 1 to 5 send
	 * all bits off has. Class 0 sends none, so there off is masked to 0.
	 */
	enc_put(e,
		(u64)code << raw_bits |
			((off >> SEQLZ_OFF_SHIFT(cls)) & (0U - (cls != 0))),
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

static __always_inline void encoder_init(struct seqlz_encoder *e,
					 const struct seqlz_tables *t, u8 *d,
					 unsigned int dst_cap,
					 const u8 *src_end)
{
	*e = (struct seqlz_encoder){ .t = t,
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
static unsigned int encoder_finish(struct seqlz_encoder *e, u8 *d,
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
	put_unaligned_le16((u16)n_lit, d);
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
	struct seqlz_encoder e;
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
 * Sums the code lengths of each stream's literals in all tables, into the
 * 16-bit lanes of even and odd: even has tables 0, 2, 4 and 6, odd tables 1,
 * 3, 5 and 7. LIT_COST_RUN literals per stream in the 8-bit lanes of x, then
 * into the 16-bit lanes, before an 8-bit lane overflows.
 */
static __always_inline void lit_cost(const struct seqlz_tables *t,
				     const u8 *literals,
				     unsigned int n_literals,
				     u64 even[][LIT_COST_WORDS],
				     u64 odd[][LIT_COST_WORDS])
{
	/* every second byte: splits 8 lanes of 8 bits into 2 * 4 of 16 bits */
	const u64 lanes = 0x00ff00ff00ff00ffULL;
	unsigned int k, j, w;

	for (k = 0; k < n_literals;) {
		u64 x[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { 0 };
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
}

/*
 * Writes the n_literals literals into the 8 streams that start at q[0] to q[7],
 * q[8] the end of the last. Literal k goes into stream k % 8, most significant
 * bit first, see decode_literals(). Four streams at a time, each with an
 * accumulator of its own, so that the four chains of shifts run side by side.
 * One stream after the other made each literal wait for the one before.
 */
static __always_inline void lit_streams(const u32 *enc, const u8 *literals,
					unsigned int n_literals, u8 *const *q)
{
	unsigned int half, k, j;

	for (half = 0; half < SEQLZ_LIT_STREAMS; half += 4) {
		u8 *p0 = q[half], *p1 = q[half + 1], *p2 = q[half + 2],
		   *p3 = q[half + 3];
		u8 *e0 = p1, *e1 = p2, *e2 = p3, *e3 = q[half + 4];
		u64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
		unsigned int c0 = 0, c1 = 0, c2 = 0, c3 = 0, s0 = 0, s1 = 0,
			     s2 = 0, s3 = 0;

		/*
		 * Blocks of 32 literals, 4 per stream, then a flush: 4 codes of
		 * up to 10 bits fit the accumulator. The loop stops 28 literals
		 * before the end, so the rest below has at most 4 per stream.
		 */
		for (k = half; k + 28U < n_literals; k += 32) {
			for (j = 0; j < 32U; j += SEQLZ_LIT_STREAMS) {
				a0 = lit_put(a0, &s0, enc, literals[k + j]);
				a1 = lit_put(a1, &s1, enc, literals[k + j + 1]);
				a2 = lit_put(a2, &s2, enc, literals[k + j + 2]);
				a3 = lit_put(a3, &s3, enc, literals[k + j + 3]);
			}
			p0 = lit_flush(p0, e0, a0, &c0, &s0);
			p1 = lit_flush(p1, e1, a1, &c1, &s1);
			p2 = lit_flush(p2, e2, a2, &c2, &s2);
			p3 = lit_flush(p3, e3, a3, &c3, &s3);
		}
		/*
		 * the rest, at most 4 literals per stream; the other half's 4
		 * of every 8 are skipped
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

/*
 * The 8 stream sizes, width bits each, lowest bit first: exactly width bytes
 * at p.
 */
static __always_inline void lit_sizes(u8 *p, const unsigned int *sizes,
				      unsigned int width)
{
	u64 acc = 0;
	unsigned int cnt = 0, j;

	for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
		acc |= (u64)sizes[j] << cnt;
		for (cnt += width; cnt >= 8U; cnt -= 8U) {
			*p++ = (u8)acc;
			acc >>= 8;
		}
	}
}

/*
 * Turns the page of len bytes in d, with raw literals as the encoder wrote it,
 * into one with Huffman coded literals, where that saves more than 1/16 of them
 * plus SEQLZ_LIT_CODED_MIN bytes. Decoding coded literals costs time for every
 * byte, and coding them wherever they save anything at all saved almost no
 * more memory. Returns the new length, or len if the literals stay raw.
 *
 * The coded literals are written over the raw ones in d, so they are read from
 * a copy: raw if the caller has one, else spare, at least a page, which they
 * are copied into once it is clear they get coded. The bitstream stays behind
 * the raw literals until it moves down behind the coded ones at the end. So d
 * needs no room beyond len, and the page is coded in any dst it fits into raw.
 */
static unsigned int code_literals(const struct seqlz_tables *t, u8 *d,
				  unsigned int len, const u8 *raw, u8 *spare)
{
	const unsigned int n_literals = get_unaligned_le16(d),
			   bitstream = len - SEQLZ_HEADER - n_literals;
	const u8 *literals = d + SEQLZ_HEADER;
	unsigned int bits = ~0U, k, j, coded, set = 0, sizes[SEQLZ_LIT_STREAMS],
		     all, width, header;
	/* the bits of each stream in each table, see lit_cost() */
	u64 even[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { 0 },
	    odd[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { 0 };
	/* where each stream starts, and the end of the last */
	u8 *q[SEQLZ_LIT_STREAMS + 1];

	lit_cost(t, literals, n_literals, even, odd);
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
	for (j = 0, coded = 0, all = 0; j < SEQLZ_LIT_STREAMS; j++) {
		sizes[j] = (lit_bits(even, odd, j, set) + 7U) / 8U;
		coded += sizes[j];
		all |= sizes[j];
	}
	/*
	 * the sizes are written in as many bits as the largest needs, at least
	 * SEQLZ_SIZE_BITS_MIN
	 */
	all |= 1U << (SEQLZ_SIZE_BITS_MIN - 1U);
	width = 32U - (unsigned int)__builtin_clz(all);
	header = SEQLZ_LIT_HEADER(width);
	if (coded + SEQLZ_LIT_CODED_MIN >= n_literals - n_literals / 16U)
		return len;
	/*
	 * The streams end at d + header + coded, which is before the
	 * bitstream at d + SEQLZ_HEADER + n_literals: coded is at least
	 * SEQLZ_LIT_CODED_MIN bytes below n_literals, see the static_assert
	 * below.
	 */
	literals = raw ? raw : memcpy(spare, literals, n_literals);
	q[0] = d + header;
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		q[j + 1] = q[j] + sizes[j];
	lit_streams(t->lit[set].enc, literals, n_literals, q);
	put_unaligned_le16((u16)(SEQLZ_LIT_CODED | n_literals), d);
	/*
	 * byte 2: the table in bits 0 to 2, width - SEQLZ_SIZE_BITS_MIN in bits
	 * 3 to 5
	 */
	d[2] = (u8)(set | (width - SEQLZ_SIZE_BITS_MIN) << SEQLZ_LIT_WIDTH_AT);
	lit_sizes(d + SEQLZ_LIT_HEADER(0), sizes, width);
	/* the bitstream, down to right after the coded literals */
	coded += header;
	memmove(d + coded, d + SEQLZ_HEADER + n_literals, bitstream);
	return coded + bitstream;
}

static_assert((SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MAX) <=
	       SEQLZ_HEADER + SEQLZ_LIT_CODED_MIN),
	      "code_literals() writes the coded literals before the bitstream");
static_assert(sizeof_field(struct seqlz_state, table) >= SEQLZ_PAGE,
	      "the raw literals take no memory beyond the matcher's table");

unsigned int seqlz_encode(const struct seqlz_tables *t,
			  const struct seqlz_sequence *seq, unsigned int n,
			  const u8 *literals, unsigned int n_literals,
			  void *dst, unsigned int dst_cap, bool coded)
{
	unsigned int len =
		encode_raw(t, seq, n, literals, n_literals, dst, dst_cap);

	return len == 0 || !coded ? len :
				    code_literals(t, dst, len, literals, NULL);
}

static unsigned int compress_page(const struct seqlz_tables *t,
				  struct seqlz_state *st, const u8 *src,
				  void *dst, unsigned int dst_cap)
{
	struct seqlz_encoder e;

	if (dst_cap < SEQLZ_HEADER + ENC_ROOM || !t->all_symbols)
		return 0;
	encoder_init(&e, t, dst, dst_cap, src + SEQLZ_PAGE);
	seqlz_match_page(st->table, src, encode_emit, &e);
	return encoder_finish(&e, dst, dst_cap);
}

unsigned int seqlz_compress(const struct seqlz_tables *t,
			    struct seqlz_state *st, const void *src, void *dst,
			    unsigned int dst_cap, bool coded)
{
	unsigned int len = compress_page(t, st, src, dst, dst_cap);

	return len == 0 || !coded ?
		       len :
		       code_literals(t, dst, len, NULL, st->literals);
}
