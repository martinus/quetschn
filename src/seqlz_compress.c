// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The compressor: the matcher, which finds a page's sequences, and the
 * encoder, which writes them as it finds them.
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
			struct seqlz_sequence *seq, unsigned int level)
{
	struct find_ctx f = { .seq = seq };

	if (level < 1 || level > SEQLZ_LEVELS)
		return 0;
	if (level >= SEQLZ_LEVEL_OWN)
		seqlz_match_page(st->table, src, true, find_emit, &f);
	else
		seqlz_match_page(st->table, src, false, find_emit, &f);
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
 * copies write up to 16 bytes past its literals, and its flushes write at most
 * 15 bytes below where the bitstream was before it. A sequence has at most two
 * flushes: the first moves the bitstream down by at most 7 bytes, as it has at
 * most 60 bits (see below), and the second writes 8 bytes below that. 16 + 15
 * bytes fit into ENC_ROOM. A page that does not fit stops the writes there, and
 * seqlz_compress() returns 0. It can happen up to ENC_ROOM bytes before dst is
 * full. With the two pages zram gives, it never happens: a sequence with a
 * match covers at least 4 bytes of the page, and its token and offset take at
 * most 31 bits, also with an escaped token, see SEQLZ_MAX_ESCAPE_LEN; the last
 * sequence has no bits. So the bitstream has at most SEQLZ_PAGE / 4 * 31 bits,
 * 3968 bytes for 4 KiB pages, and with a page of literals that leaves more than
 * ENC_ROOM bytes free; for 16 KiB pages 15 872 bytes.
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
	/* at SEQLZ_LEVEL_OWN: write a record before the literals, see LIT_REC */
	bool rec;
	unsigned int pos; /* with rec: where the next sequence starts */
};

/*
 * At SEQLZ_LEVEL_OWN the encoder writes LIT_REC bytes before each sequence's
 * literals: where they start in the page, how many there are and the offset of
 * the sequence before, a u16 each, little endian. Once the page is matched,
 * delta_literals() picks the literals' delta from them, see SEQLZ_LIT_DELTA_AT,
 * and moves the literals down over them. They take room in dst: a page that
 * does not fit with them is matched again without them, without deltas.
 */
#define LIT_REC 6U

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
 * values. ml is 0 for the last sequence, which has only its literals. With rec,
 * a constant, and e->rec, the record before the literals, see LIT_REC: only
 * the matcher of SEQLZ_LEVEL_OWN has the code for it.
 */
static __always_inline void encode_seq(struct seqlz_encoder *e, const u8 *in,
				       unsigned int ll, unsigned int ml,
				       unsigned int off, bool rec)
{
	const struct seqlz_tables *t = e->t;
	unsigned int k = 0, tlen, raw_bits, cls;
	unsigned int n_rec = rec && e->rec && ll ? LIT_REC : 0;
	u32 code;

	/*
	 * The page does not fit: no more writes. With p at lit, every later
	 * sequence ends here too, and encoder_finish() sees it.
	 */
	if ((size_t)(e->p - e->lit) < ll + n_rec + ENC_ROOM) {
		e->p = e->lit;
		return;
	}
	if (n_rec) {
		put_unaligned_le16((u16)e->pos, e->lit);
		put_unaligned_le16((u16)ll, e->lit + 2);
		put_unaligned_le16((u16)e->last, e->lit + 4);
		e->lit += LIT_REC;
	}
	if (rec)
		e->pos += ll + ml;
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

	/*
	 * The last sequence: no token, the decoder knows it from the literals
	 * left, which fill the rest of the page.
	 */
	if (ml == 0)
		return;
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

/* encode_seq() as the matcher calls it, without and with records */
static __always_inline void encode_emit(void *ctx, const u8 *in,
					unsigned int ll, unsigned int ml,
					unsigned int off)
{
	encode_seq(ctx, in, ll, ml, off, false);
}

static __always_inline void encode_emit_rec(void *ctx, const u8 *in,
					    unsigned int ll, unsigned int ml,
					    unsigned int off)
{
	encode_seq(ctx, in, ll, ml, off, true);
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
 * Writes the u16, and copies the bitstream from the back of dst to right after
 * the literals, the right way round. Returns the length of the page, or 0 if
 * it did not fit into the dst_cap bytes at d.
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
	/*
	 * Every sequence with a match ends with enc_flush(), which already
	 * wrote the bits left over on top of the byte below p, zeros below
	 * them: that byte is the bitstream's last.
	 */
	if (e->cnt > 0)
		e->p--;
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

/*
 * delta_literals() prices the deltas with every DELTA_STEP-th literal. Pricing
 * every literal made pages almost no smaller and the encoder slower. Not every
 * 4th: in fields of 4 or 8 bytes that is the same byte of each field, and
 * pages were larger.
 */
#define DELTA_STEP 5U
static_assert(LIT_COST_WORDS == 1U, "delta_literals() sums one lit_cost word");

/*
 * The bits of the literals in fixed table k, from even and odd lanes of 16
 * bits as lit_cost() sums them: even has tables 0, 2, 4 and 6
 */
static __always_inline unsigned int delta_bits(u64 even, u64 odd,
					       unsigned int k)
{
	return (unsigned int)(((k & 1U ? odd : even) >> (16U * (k / 2U))) &
			      0xffffU);
}

/*
 * Adds the 8-bit lanes of the sums s to the 16-bit lanes of even and odd, as
 * lit_cost() does, and clears s
 */
static __always_inline void delta_spill(u64 *s, u64 *even, u64 *odd)
{
	/* every second byte: splits 8 lanes of 8 bits into 2 * 4 of 16 bits */
	const u64 lanes = 0x00ff00ff00ff00ffULL;
	unsigned int x;

	for (x = 0; x < 4; x++) {
		even[x] += s[x] & lanes;
		odd[x] += s[x] >> 8 & lanes;
		s[x] = 0;
	}
}

/*
 * Each byte of a minus the byte of b at the same place, modulo 256, with no
 * borrow from one byte to the next: the top bit of each byte computed apart
 */
static __always_inline u64 sub_bytes(u64 a, u64 b)
{
	const u64 high = 0x8080808080808080ULL; /* bit 7 of each byte */

	return ((a | high) - (b & ~high)) ^ ((a ^ ~b) & high);
}

/*
 * Picks the delta of the literals from the records before them, see LIT_REC
 * and SEQLZ_LIT_DELTA_AT, and moves the literals down over the records, to
 * right after the u16; e->lit is then their end. With a delta, their deltas
 * are also written to deltas. Returns the delta, 0 for none.
 *
 * Each delta is priced as code_literals() prices the fixed tables, with sums
 * of lit_cost: the delta whose best table takes the fewest bits, 8 bits more
 * for the byte that says which delta. The bits of the fixed tables also tell
 * which delta suits a table of the page's own.
 */
static unsigned int delta_literals(const struct seqlz_tables *t,
				   struct seqlz_encoder *e, u8 *d,
				   const u8 *page, u8 *deltas)
{
	u64 s[4] = { 0 }, even[4] = { 0 }, odd[4] = { 0 };
	unsigned int k, x, j = 0, n = 0, pos, ll, last, best = ~0U, pick = 0;
	u8 *p, *out = d + SEQLZ_HEADER, *to = deltas;

	/*
	 * The sums: each literal's length in all fixed tables, as it is and
	 * with each delta, in the 8-bit lanes of s, LIT_COST_RUN at a time, then
	 * in the 16-bit lanes
	 */
	for (p = d + SEQLZ_HEADER; p < e->lit; p += LIT_REC + ll) {
		pos = get_unaligned_le16(p);
		ll = get_unaligned_le16(p + 2);
		last = seqlz_delta_last(get_unaligned_le16(p + 4));
		for (k = j; k < ll; k += DELTA_STEP) {
			const unsigned int at = pos + k, b = page[at],
					   b4 = k >= 4U ? page[at - 4] : 0U,
					   b8 = k >= 8U ? page[at - 8] : 0U,
					   bl = at >= last ? page[at - last] :
							     0U;

			if (n == LIT_COST_RUN) {
				delta_spill(s, even, odd);
				n = 0;
			}
			s[0] += t->lit_cost[b][0];
			s[SEQLZ_DELTA_4] += t->lit_cost[(u8)(b - b4)][0];
			s[SEQLZ_DELTA_8] += t->lit_cost[(u8)(b - b8)][0];
			s[SEQLZ_DELTA_LAST] += t->lit_cost[(u8)(b - bl)][0];
			n++;
		}
		/* the next sequence's literals go on with the step */
		j = k - ll;
	}
	delta_spill(s, even, odd);
	for (x = 0; x < 4; x++)
		for (k = 0; k < SEQLZ_LIT_SETS; k++) {
			unsigned int b = delta_bits(even[x], odd[x], k) +
					 (x ? 8U / DELTA_STEP : 0U);

			if (b < best) {
				best = b;
				pick = x;
			}
		}
	/*
	 * The literals down over the records, and their deltas into deltas: the
	 * first of a sequence or of the page as they are. 8 bytes at a time,
	 * also past the sequence's last literal: what that writes, the next
	 * sequence's literals overwrite, of the next record it reaches at most
	 * the first 2 bytes, read before, and after the last literals dst has
	 * ENC_ROOM bytes more, see encode_seq(). memcpy() was a call for every
	 * sequence, and a loop over the last bytes mispredicted.
	 */
	p = d + SEQLZ_HEADER;
	if (p < e->lit) {
		pos = get_unaligned_le16(p);
		ll = get_unaligned_le16(p + 2);
		last = seqlz_delta_last(get_unaligned_le16(p + 4));
	}
	while (p < e->lit) {
		const u8 *in = p + LIT_REC, *pin = page + pos;
		const unsigned int n_lit = ll, at = pos, dist = last;

		p += LIT_REC + n_lit;
		if (p < e->lit) {
			pos = get_unaligned_le16(p);
			ll = get_unaligned_le16(p + 2);
			last = seqlz_delta_last(get_unaligned_le16(p + 4));
		}
		/* a load, then a store: the copy overlaps its source */
		for (k = 0; k < n_lit; k += 8)
			put_unaligned_le64(get_unaligned_le64(in + k), out + k);
		out += n_lit;
		if (!pick)
			continue;
		if (pick == SEQLZ_DELTA_LAST) {
			x = dist;
			j = x > at ? x - at : 0;
		} else {
			x = pick == SEQLZ_DELTA_4 ? 4U : 8U;
			j = x;
		}
		for (k = 0; k < j && k < n_lit; k++)
			to[k] = pin[k];
		/* past the last literal only inside the page and deltas */
		if (at + n_lit + 8 <= SEQLZ_PAGE &&
		    (unsigned long)(to - deltas) + n_lit + 8 <= SEQLZ_PAGE) {
			for (; k < n_lit; k += 8) {
				u64 a = get_unaligned_le64(pin + k),
				    r = get_unaligned_le64(pin + k - x);

				put_unaligned_le64(sub_bytes(a, r), to + k);
			}
		} else {
			for (; k < n_lit; k++)
				to[k] = (u8)(pin[k] - *(pin + k - x));
		}
		to += n_lit;
	}
	e->lit = out;
	return pick;
}

/* seqlz_encode() up to the coded literals, with the compressor's encoder */
static unsigned int encode_raw(const struct seqlz_tables *t,
			       const struct seqlz_sequence *seq, unsigned int n,
			       const u8 *literals, unsigned int n_literals,
			       const u8 *page, u8 *deltas, unsigned int *delta,
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
	/* with a page, the records for the deltas, as compress_own() */
	e.rec = page != NULL;
	for (;;) {
		for (i = 0; i < n; i++) {
			unsigned int ll = seq[i].literals;

			encode_seq(&e, in, ll, i + 1 == n ? 0U : seq[i].match,
				   seq[i].offset, true);
			in += ll;
		}
		if (e.p != e.lit || !e.rec)
			break;
		encoder_init(&e, t, dst, dst_cap, literals + n_literals);
		in = literals;
	}
	*delta = e.rec ? delta_literals(t, &e, dst, page, deltas) : 0;
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
 * into the 16-bit lanes, before an 8-bit lane overflows. Unless hist is NULL,
 * it counts the bytes too, for a table of the page's own: the even literals in
 * hist[0], the odd ones in hist[1], so that a run of one byte does not wait for
 * its own count.
 */
static __always_inline void
lit_cost(const struct seqlz_tables *t, const u8 *literals,
	 unsigned int n_literals, u64 even[][LIT_COST_WORDS],
	 u64 odd[][LIT_COST_WORDS], u16 (*hist)[256])
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
			for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
				for (w = 0; w < LIT_COST_WORDS; w++)
					x[j][w] +=
						t->lit_cost[literals[k + j]][w];
				if (hist)
					hist[j & 1U][literals[k + j]]++;
			}
		for (; k < end; k++) {
			for (w = 0; w < LIT_COST_WORDS; w++)
				x[k % SEQLZ_LIT_STREAMS][w] +=
					t->lit_cost[literals[k]][w];
			if (hist)
				hist[k & 1U][literals[k]]++;
		}
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
 * A literal table of the page's own, see SEQLZ_LIT_OWN: what the encoder needs
 * for it, in the matcher's state behind the copy of the literals. So a page
 * gets one only if its literals leave OWN_WORK bytes of the state free, which
 * all pages of 4 KiB do.
 */
#define OWN_WORK 4096U
#define OWN_LITERALS_MAX (sizeof_field(struct seqlz_state, work) - OWN_WORK)
/*
 * The bytes of the description: at most 256 symbols of up to 8 bits, the
 * runs between the bytes with a code take fewer bits than such a byte; a
 * power of 2, own_put() masks its index with it.
 */
#define OWN_DESC_ROOM 512U
/* a page's own table only for pages with this many literals or more */
#define OWN_LITERALS_MIN 512U
/* and only where it saves this many bytes against the best fixed table */
#define OWN_SAVES_MIN 16U

struct own_work {
	/* how often each byte is a literal, see lit_cost(); then the sum */
	u16 freq[2][256];
	u8 bytes[256]; /* the bytes that are literals, by value */
	u8 by_gain[256]; /* the same, by how much a shorter code saves */
	u8 gain[256]; /* that, for each byte in bytes[], from 0 to 15 */
	u8 len[256]; /* the code lengths */
	u32 enc[256]; /* code length | code << 8, as in struct lit_table */
	/* the streams' bytes with the code, see own_sizes() */
	unsigned int sizes[SEQLZ_LIT_STREAMS];
	/* the table as the page sends it, and 8 bytes of room for own_put() */
	u8 desc[OWN_DESC_ROOM + 8];
};

static_assert((sizeof(struct own_work) <= OWN_WORK &&
	       sizeof_field(struct seqlz_state, work) >= OWN_WORK),
	      "the matcher's state holds the work of a page's own table");

/*
 * The code lengths of the n >= 2 bytes in w->bytes, of n_literals literals,
 * into w->len: almost those of a Huffman code, without sorting the bytes by
 * frequency. Each byte gets the shortest length l with freq * 2^l >=
 * n_literals, at most SEQLZ_LIT_BITS, which leaves the code incomplete or, with
 * lengths cut to SEQLZ_LIT_BITS, overfull. freq * 2^l / n_literals, from 1 to
 * 2, says how much a byte would save with a code one bit shorter for the room
 * that takes: the bytes go into 16 buckets by it, the gain. In the order of the
 * gain the code is then made complete: an overfull one lengthens the bytes of
 * the smallest gain, then those of the largest get shorter while there is room.
 */
static void own_lengths(struct own_work *w, unsigned int n,
			unsigned int n_literals)
{
	/* 2^40 / n_literals: freq * 2^l * inv >> 36 is 16 times freq * 2^l / n */
	const u64 inv = div_u64(1ULL << 40, n_literals);
	const unsigned int full = 1U << SEQLZ_LIT_BITS;
	unsigned int k, space = 0, at[17] = { 0 };

	for (k = 0; k < n; k++) {
		unsigned int b = w->bytes[k & 255U], f = w->freq[0][b],
			     l = (unsigned int)(__builtin_clz(f) -
						__builtin_clz(n_literals));
		u64 q;

		l += (f << l) < n_literals;
		l += !l;
		q = ((u64)f << l) * inv >> 36;
		q = l > SEQLZ_LIT_BITS || q < 16U ? 16U : q > 31U ? 31U : q;
		l = min(l, SEQLZ_LIT_BITS);
		w->len[b] = (u8)l;
		w->gain[k & 255U] = (u8)(q - 16U);
		at[q - 15U]++;
		space += full >> l;
	}
	for (k = 1; k < 17; k++)
		at[k] += at[k - 1];
	for (k = 0; k < n; k++)
		w->by_gain[at[w->gain[k & 255U]]++ & 255U] = w->bytes[k & 255U];
	while (space > full)
		for (k = 0; space > full && k < n; k++) {
			unsigned int b = w->by_gain[k & 255U], l = w->len[b];

			if (l < SEQLZ_LIT_BITS) {
				space -= full >> (l + 1U);
				w->len[b] = (u8)(l + 1U);
			}
		}
	/*
	 * One round may leave room: a byte that just got shorter may now be the
	 * longest. The room is a multiple of the longest code's, so another
	 * round fills it.
	 */
	while (space < full)
		for (k = n; k-- > 0;) {
			unsigned int b = w->by_gain[k & 255U], l = w->len[b];

			if (l > 1U && space + (full >> l) <= full) {
				space += full >> l;
				w->len[b] = (u8)(l - 1U);
			}
		}
}

/* log2(1 + (i + 0.5) / 32) in 1/256 bits */
static const u8 log2_frac[32] = { 6,   17,  28,	 38,  49,  59,	68,  78,
				  87,  96,  105, 113, 122, 130, 138, 146,
				  154, 161, 169, 176, 183, 190, 197, 203,
				  210, 216, 223, 229, 235, 241, 247, 253 };

/* log2(f) in 1/256 bits, to about 1/50 bit, f > 0 */
static __always_inline unsigned int log2_256(unsigned int f)
{
	unsigned int e = 31U - (unsigned int)__builtin_clz(f);

	return e << 8 | log2_frac[(f << (31U - e)) >> 26 & 31U];
}

/* 4 counts as the 16-bit lanes of a u64, f[0] in the lowest */
static __always_inline u64 own_lanes(const u16 *f)
{
	return f[0] | (u64)f[1] << 16 | (u64)f[2] << 32 | (u64)f[3] << 48;
}

/* appends the n bits of v to the description in w->desc */
static __always_inline void own_put(struct own_work *w, u64 *acc,
				    unsigned int *cnt, unsigned int *out,
				    unsigned int v, unsigned int n)
{
	*acc = *acc << n | v;
	*cnt += n;
	if (*cnt >= 32U) {
		/* 8 bytes, the 4 after the first are written again later */
		put_unaligned_be64(*acc << ((64U - *cnt) & 63U),
				   w->desc + (*out & (OWN_DESC_ROOM - 1U)));
		*cnt -= 32U;
		*out += 4;
	}
}

/*
 * The page's own code for its literals, from the counts in w->freq: lengths,
 * the encoder's codes, the description in w->desc. Returns the bytes of the
 * description, or 0 if it does not pay: fewer than 2 different bytes, or the
 * literals' entropy, or their bits with the code and the description with the
 * smallest header, say it cannot save OWN_SAVES_MIN bytes against limit, the
 * bytes of the best fixed table with its header.
 */
static unsigned int own_code(const struct seqlz_tables *t, struct own_work *w,
			     unsigned int n_literals, unsigned int limit)
{
	/* the low 15 bits of each lane */
	const u64 low = 0x7fff7fff7fff7fffULL;
	unsigned int k, j, n = 0, count[16] = { 0 }, next[16], l, cnt = 0, prev,
			   out = 0, h = 0, est, bits = 0;
	u64 acc = 0, present[4] = { 0 };

	/*
	 * The two counts added, 4 bytes at a time in 16-bit lanes, which the
	 * sums fit. Each sum that is not 0 sets a bit in present, bit k for
	 * byte k: the top bit of a lane of c | (c & 0x7fff) + 0x7fff is set
	 * where the lane is not 0, and the multiply moves the 4 top bits, at 0,
	 * 16, 32 and 48 after the shift by 15, to bits 48 to 51 without carries.
	 */
	for (k = 0; k < 256; k += 4) {
		u64 c = own_lanes(&w->freq[0][k]) + own_lanes(&w->freq[1][k]);

		for (j = 0; j < 4; j++)
			w->freq[0][k + j] = (u16)(c >> 16 * j);
		c = ((c | ((c & low) + low)) & ~low) >> 15;
		present[k / 64] |= (c * 0x0001000200040008ULL >> 48 & 15U)
				   << (k % 64);
	}
	/* the bytes that are literals, in order, and the literals' entropy */
	for (j = 0; j < 4; j++) {
		u64 m = present[j];

		while (m) {
			unsigned int b = 64U * j +
					 (unsigned int)__builtin_ctzll(m),
				     f = w->freq[0][b];

			w->bytes[n & 255U] = (u8)b;
			n++;
			h += f * log2_256(f);
			m &= m - 1;
		}
	}
	if (n < 2)
		return 0;
	/*
	 * Only where the entropy and a description of about 0.45 bytes per byte
	 * with a code leave the code at least OWN_SAVES_MIN bytes smaller than
	 * limit, and small enough not to stay raw, see code_literals(), is the
	 * code built. On many pages it is not.
	 */
	h = n_literals * log2_256(n_literals) - h;
	est = h / 2048U + n * 115U / 256U;
	if (est + SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN) + OWN_SAVES_MIN >
		    limit ||
	    est + SEQLZ_LIT_CODED_MIN >= n_literals - n_literals / 16U)
		return 0;
	own_lengths(w, n, n_literals);

	/* canonical codes, by length and then by byte, as build_lit() */
	for (k = 0; k < n; k++)
		count[w->len[w->bytes[k & 255U]] & 15U]++;
	for (l = 1, next[0] = 0; l < 16; l++)
		next[l] = (next[l - 1] + count[l - 1]) << 1;
	for (k = 0; k < n; k++) {
		unsigned int b = w->bytes[k & 255U];

		l = w->len[b] & 15U;
		bits += w->freq[0][b] * l;
		w->enc[b] = l | next[l]++ << 8;
	}

	/* the description: each byte's length, a run of those without before */
	for (k = 0, prev = ~0U; k < n; k++) {
		unsigned int b = w->bytes[k & 255U], run = b - prev - 1U, e;

		if (run) {
			unsigned int nb =
				32U - (unsigned int)__builtin_clz(run);

			e = t->own.enc[SEQLZ_OWN_RUN + nb];
			own_put(w, &acc, &cnt, &out,
				(e & 0xffU) << (nb - 1U) |
					(run & ((1U << (nb - 1U)) - 1U)),
				(e >> 8) + nb - 1U);
		}
		e = t->own.enc[(w->len[b] - 1U) & 15U];
		own_put(w, &acc, &cnt, &out, e & 0xffU, e >> 8);
		prev = b;
	}
	for (; cnt >= 8; cnt -= 8)
		w->desc[out++ & (OWN_DESC_ROOM - 1U)] = (u8)(acc >> (cnt - 8));
	if (cnt)
		w->desc[out++ & (OWN_DESC_ROOM - 1U)] = (u8)(acc << (8 - cnt));
	/* the streams' sizes are counted only where this leaves room */
	if (SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN) + out + bits / 8U +
		    OWN_SAVES_MIN >
	    limit)
		return 0;
	return out;
}

/*
 * The bytes of each stream with the page's own code, in w->sizes, and their
 * sum: 8 literals a step, the sum of each stream in a register of its own.
 */
static unsigned int own_sizes(struct own_work *w, const u8 *literals,
			      unsigned int n_literals)
{
	unsigned int *const sizes = w->sizes;
	const u8 *const len = w->len;
	unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0,
		     b7 = 0, k, sum = 0;

	for (k = 0; k + SEQLZ_LIT_STREAMS <= n_literals;
	     k += SEQLZ_LIT_STREAMS) {
		b0 += len[literals[k]];
		b1 += len[literals[k + 1]];
		b2 += len[literals[k + 2]];
		b3 += len[literals[k + 3]];
		b4 += len[literals[k + 4]];
		b5 += len[literals[k + 5]];
		b6 += len[literals[k + 6]];
		b7 += len[literals[k + 7]];
	}
	sizes[0] = b0;
	sizes[1] = b1;
	sizes[2] = b2;
	sizes[3] = b3;
	sizes[4] = b4;
	sizes[5] = b5;
	sizes[6] = b6;
	sizes[7] = b7;
	for (; k < n_literals; k++)
		sizes[k % SEQLZ_LIT_STREAMS] += len[literals[k]];
	for (k = 0; k < SEQLZ_LIT_STREAMS; k++) {
		sizes[k] = (sizes[k] + 7U) / 8U;
		sum += sizes[k];
	}
	return sum;
}

/*
 * The bits the stream sizes are written in: as many as the largest needs, at
 * least SEQLZ_SIZE_BITS_MIN.
 */
static __always_inline unsigned int size_width(const unsigned int *sizes)
{
	unsigned int all = 1U << (SEQLZ_SIZE_BITS_MIN - 1U), j;

	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		all |= sizes[j];
	return 32U - (unsigned int)__builtin_clz(all);
}

/*
 * Turns the page of len bytes in d, with raw literals as the encoder wrote it,
 * into one with Huffman coded literals, where that saves more than 1/16 of them
 * plus SEQLZ_LIT_CODED_MIN bytes. Decoding coded literals costs time for every
 * byte, and coding them wherever they save anything at all saved almost no
 * more memory. Returns the new length, or len if the literals stay raw.
 *
 * The code is the fixed table that codes them in the fewest bits, or, with own
 * not NULL, a table of the page's own, where that and its description take at
 * least OWN_SAVES_MIN bytes less.
 *
 * The coded literals are written over the raw ones in d, so they are read from
 * a copy: raw if the caller has one, else spare, at least a page, which they
 * are copied into once it is clear they get coded. The bitstream stays behind
 * the raw literals until it moves down behind the coded ones at the end. So d
 * needs no room beyond len, and the page is coded in any dst it fits into raw.
 *
 * With delta not 0, raw has the literals' deltas, see SEQLZ_LIT_DELTA_AT,
 * which are coded instead, and the header says so; if they stay raw, d keeps
 * them as they are.
 */
static unsigned int code_literals(const struct seqlz_tables *t, u8 *d,
				  unsigned int len, const u8 *raw, u8 *spare,
				  struct own_work *own, unsigned int delta)
{
	const unsigned int n_literals = get_unaligned_le16(d),
			   bitstream = len - SEQLZ_HEADER - n_literals;
	const u8 *literals = raw ? raw : d + SEQLZ_HEADER;
	unsigned int bits = ~0U, k, j, coded, set = 0, sizes[SEQLZ_LIT_STREAMS],
		     width, header, desc = 0;
	const u32 *enc;
	/* the bits of each stream in each table, see lit_cost() */
	u64 even[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { 0 },
	    odd[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { 0 };
	/* where each stream starts, and the end of the last */
	u8 *q[SEQLZ_LIT_STREAMS + 1];

	if (own &&
	    (n_literals < OWN_LITERALS_MIN || n_literals > OWN_LITERALS_MAX))
		own = NULL;
	if (own)
		memset(own->freq, 0, sizeof(own->freq));
	lit_cost(t, literals, n_literals, even, odd, own ? own->freq : NULL);
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
	for (j = 0, coded = 0; j < SEQLZ_LIT_STREAMS; j++) {
		sizes[j] = (lit_bits(even, odd, j, set) + 7U) / 8U;
		coded += sizes[j];
	}
	width = size_width(sizes);
	/* with a delta, its byte before the sizes */
	header = SEQLZ_LIT_HEADER(width) + (unsigned int)(delta != 0);
	enc = t->lit[set].enc;
	if (own) {
		unsigned int n = own_code(t, own, n_literals, header + coded),
			     own_coded, own_width;

		if (n) {
			own_coded = own_sizes(own, literals, n_literals);
			own_width = size_width(own->sizes);
			/* both without the byte of a delta, which both need */
			if (SEQLZ_LIT_HEADER(own_width) + n + own_coded +
				    OWN_SAVES_MIN <=
			    header + coded - (delta ? 1U : 0U)) {
				desc = n;
				width = own_width;
				header = SEQLZ_LIT_HEADER(width) +
					 (unsigned int)(delta != 0) + desc;
				coded = own_coded;
				memcpy(sizes, own->sizes, sizeof(sizes));
				enc = own->enc;
			}
		}
	}
	if (desc + coded + SEQLZ_LIT_CODED_MIN >= n_literals - n_literals / 16U)
		return len;
	/*
	 * The streams end at d + header + coded, which is before the
	 * bitstream at d + SEQLZ_HEADER + n_literals: desc + coded is at least
	 * SEQLZ_LIT_CODED_MIN bytes below n_literals, see the static_assert
	 * below.
	 */
	literals = raw ? raw : memcpy(spare, literals, n_literals);
	q[0] = d + header;
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		q[j + 1] = q[j] + sizes[j];
	lit_streams(enc, literals, n_literals, q);
	put_unaligned_le16((u16)(SEQLZ_LIT_CODED | n_literals), d);
	/*
	 * byte 2: the literals' code in bits 0 to 3, width - SEQLZ_SIZE_BITS_MIN
	 * in bits 4 to 6, in bit 7 whether they are deltas
	 */
	d[2] = (u8)((desc ? SEQLZ_LIT_OWN : set) |
		    (width - SEQLZ_SIZE_BITS_MIN) << SEQLZ_LIT_WIDTH_AT |
		    (unsigned int)(delta != 0) << SEQLZ_LIT_DELTA_AT);
	if (delta)
		d[SEQLZ_LIT_HEADER(0)] = (u8)delta;
	lit_sizes(d + SEQLZ_LIT_HEADER(0) + (unsigned int)(delta != 0), sizes,
		  width);
	if (desc)
		memcpy(d + SEQLZ_LIT_HEADER(width) + (unsigned int)(delta != 0),
		       own->desc, desc);
	/* the bitstream, down to right after the coded literals */
	coded += header;
	memmove(d + coded, d + SEQLZ_HEADER + n_literals, bitstream);
	return coded + bitstream;
}

static_assert((SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MAX) + 1U <=
	       SEQLZ_HEADER + SEQLZ_LIT_CODED_MIN),
	      "code_literals() writes the coded literals before the bitstream");
static_assert(sizeof_field(struct seqlz_state, table) >= SEQLZ_PAGE,
	      "the raw literals take no memory beyond the matcher's table");

/* behind the literals in the matcher's state, see OWN_WORK */
static struct own_work *own_work(struct seqlz_state *st)
{
	return (struct own_work *)(st->work + OWN_LITERALS_MAX / 8U);
}

/*
 * code_literals() at SEQLZ_LEVEL_OWN: the literals' deltas, as delta_literals()
 * left them in the matcher's state, unless that is none or they would stay
 * raw; else as they are.
 */
static unsigned int code_own(const struct seqlz_tables *t, u8 *d,
			     unsigned int len, struct seqlz_state *st,
			     unsigned int delta)
{
	if (delta) {
		unsigned int n = code_literals(t, d, len, st->literals, NULL,
					       own_work(st), delta);

		if (n != len)
			return n;
	}
	return code_literals(t, d, len, NULL, st->literals, own_work(st), 0);
}

unsigned int seqlz_encode(const struct seqlz_tables *t, struct seqlz_state *st,
			  const struct seqlz_sequence *seq, unsigned int n,
			  const u8 *literals, unsigned int n_literals,
			  const void *page, void *dst, unsigned int dst_cap,
			  unsigned int level)
{
	unsigned int len, delta;

	if (level < 1 || level > SEQLZ_LEVELS ||
	    (level >= SEQLZ_LEVEL_OWN && !st))
		return 0;
	len = encode_raw(t, seq, n, literals, n_literals,
			 level >= SEQLZ_LEVEL_OWN ? page : NULL,
			 st ? st->literals : NULL, &delta, dst, dst_cap);
	if (len == 0 || level < SEQLZ_LEVEL_CODED)
		return len;
	if (level < SEQLZ_LEVEL_OWN)
		return code_literals(t, dst, len, literals, NULL, NULL, 0);
	return code_own(t, dst, len, st, delta);
}

static unsigned int compress_page(const struct seqlz_tables *t,
				  struct seqlz_state *st, const u8 *src,
				  void *dst, unsigned int dst_cap)
{
	struct seqlz_encoder e;

	if (dst_cap < SEQLZ_HEADER + ENC_ROOM || !t->all_symbols)
		return 0;
	encoder_init(&e, t, dst, dst_cap, src + SEQLZ_PAGE);
	seqlz_match_page(st->table, src, false, encode_emit, &e);
	return encoder_finish(&e, dst, dst_cap);
}

/*
 * compress_page() for SEQLZ_LEVEL_OWN: the lazy matcher, with the records for
 * the deltas, and without them if the page does not fit with them; *delta
 * gets the delta. A function of its own, so that the other levels' code stays
 * as it is without it: inlined next to it, their pages were slower to write.
 */
static noinline unsigned int compress_own(const struct seqlz_tables *t,
					  struct seqlz_state *st, const u8 *src,
					  void *dst, unsigned int dst_cap,
					  unsigned int *delta)
{
	struct seqlz_encoder e;

	*delta = 0;
	if (dst_cap < SEQLZ_HEADER + ENC_ROOM || !t->all_symbols)
		return 0;
	encoder_init(&e, t, dst, dst_cap, src + SEQLZ_PAGE);
	e.rec = true;
	for (;;) {
		seqlz_match_page(st->table, src, true, encode_emit_rec, &e);
		if (e.p != e.lit || !e.rec)
			break;
		encoder_init(&e, t, dst, dst_cap, src + SEQLZ_PAGE);
	}
	if (e.rec)
		*delta = delta_literals(t, &e, dst, src, st->literals);
	return encoder_finish(&e, dst, dst_cap);
}

unsigned int seqlz_compress(const struct seqlz_tables *t,
			    struct seqlz_state *st, const void *src, void *dst,
			    unsigned int dst_cap, unsigned int level)
{
	unsigned int len, delta;

	if (level < 1 || level > SEQLZ_LEVELS)
		return 0;
	if (level >= SEQLZ_LEVEL_OWN) {
		len = compress_own(t, st, src, dst, dst_cap, &delta);
		return len ? code_own(t, dst, len, st, delta) : 0;
	}
	len = compress_page(t, st, src, dst, dst_cap);
	if (len == 0 || level < SEQLZ_LEVEL_CODED)
		return len;
	return code_literals(t, dst, len, NULL, st->literals, NULL, 0);
}
