// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The compressor: the matcher, which finds a block's sequences, and the
 * encoder, which writes them as it finds them.
 */
#include "seqlz.h"
#include "seqlz_match.h"

/* ---- the matcher of seqlz_match.h, for seqlz_find() ---- */

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
		seqlz_match_block(st->table, src, true, find_emit, &f);
	else
		seqlz_match_block(st->table, src, false, find_emit, &f);
	return f.n;
}

/* ---- encoder ---- */

/*
 * The encoder writes a block in one pass, as the matcher finds the sequences.
 * The block starts with the literals and the bitstream follows, but how many
 * literals there are is known only at the end. So the literals go to the front
 * of dst, after the u16, and the bitstream goes to the back of dst, backwards:
 * its first byte is the last byte of dst, and it grows down toward the
 * literals. At the end it is copied, the right way round, to right after the
 * literals.
 *
 * So dst can have any size, and a block that does not fit is an error, as for
 * the other compressors. Before each sequence the encoder checks that its
 * literals and ENC_ROOM bytes more are free between the two: a sequence's
 * copies write up to 16 bytes past its literals, and its flushes write at most
 * 15 bytes below where the bitstream was before it. A sequence has at most two
 * flushes: the first moves the bitstream down by at most 7 bytes, as it has at
 * most 60 bits (see below), and the second writes 8 bytes below that. 16 + 15
 * bytes fit into ENC_ROOM. A block that does not fit stops the writes there,
 * and seqlz_compress() returns 0. It can happen up to ENC_ROOM bytes before dst
 * is full. With the two pages zram gives, it never happens: a sequence with a
 * match covers at least 4 bytes of the block, and its token and offset take at
 * most 31 bits, also with an escaped token, see SEQLZ_MAX_ESCAPE_LEN; the last
 * sequence has no bits. So the bitstream has at most SEQLZ_BLOCK_SIZE / 4 * 31
 * bits, 3968 bytes for blocks of 4 KiB, and with a block of literals that
 * leaves more than ENC_ROOM bytes free; for blocks of 16 KiB 15 872 bytes.
 *
 * Bits are collected in a 64-bit accumulator and written 8 bytes at a time;
 * then the write position moves down by the whole bytes, and the at most 7 bits
 * left over are written again with the next ones. After a write the accumulator
 * holds at most 7 bits, so a sequence's token, offset and match length value
 * fit before the next one: 7 + 31 + 20 = 58 bits, 60 for 16 KiB blocks. Only a
 * literal length value, 20 or 22 bits more, needs a write of its own.
 */
#define ENC_ROOM 32U

struct seqlz_encoder {
	u64 acc; /* the bits not yet written, the newest in the low bits */
	unsigned int cnt; /* how many bits of acc count */
	u8 *p; /* the next byte of the bitstream goes right below p */
	u8 *lit; /* where the next literal goes */
	const u8 *src_end; /* copies of 16 literals may read up to here */
	unsigned int last; /* the offset of the sequence before, class 0 */
	/*
	 * at SEQLZ_LEVEL_OWN: write a record before the literals, see LIT_REC
	 */
	bool rec;
	unsigned int pos; /* with rec: where the next sequence starts */
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
static __always_inline u32 token_code(unsigned int tok, unsigned int *len)
{
	u32 te = seqlz_tables.token.enc[tok], ee;

	if (te != 0) {
		*len = te >> TOK_ENC_LEN_AT;
		return te & TOK_ENC_CODE_MASK;
	}
	ee = seqlz_tables.token.enc[SEQLZ_ESCAPE];
	*len = (ee >> TOK_ENC_LEN_AT) + SEQLZ_ESCAPE_BITS;
	return (ee & TOK_ENC_CODE_MASK) << SEQLZ_ESCAPE_BITS | tok;
}

/*
 * Writes one sequence: its literals from in, then its token, offset and length
 * values. ml is 0 for the last sequence, which has only its literals. With rec,
 * a constant, and e->rec, the record before the literals, see LIT_REC: the
 * callers that write records pass rec true, the matcher of the levels below
 * SEQLZ_LEVEL_OWN false, so its code has none of it.
 */
static __always_inline void encode_seq(struct seqlz_encoder *e, const u8 *in,
				       unsigned int ll, unsigned int ml,
				       unsigned int off, bool rec)
{
	unsigned int k = 0, tlen, raw_bits, cls;
	unsigned int n_rec = rec && e->rec && ll ? LIT_REC : 0;
	u32 code;

	/*
	 * The block does not fit: no more writes. With p at lit, every later
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
	 * left, which fill the rest of the block.
	 */
	if (ml == 0)
		return;
	/* token and offset in one put, the length values after them */
	cls = seqlz_off_class(off, e->last, &raw_bits);
	code = token_code(seqlz_token(ll, ml, cls), &tlen);
	/*
	 * The offset's bits right after the token's code. Classes 1 to 5 send
	 * all bits off has. Class 0 sends none, so there off is masked to 0.
	 */
	enc_put(e,
		(u64)code << raw_bits |
			((off >> SEQLZ_OFF_SHIFT(cls)) & (0U - (cls != 0))),
		tlen + raw_bits);
	if (ll >= SEQLZ_LL_CAP) {
		put_len_value(e, &seqlz_tables.ll, ll - SEQLZ_LL_CAP);
		enc_flush(e);
	}
	if (ml - SEQLZ_MIN_MATCH >= SEQLZ_ML_CAP)
		put_len_value(e, &seqlz_tables.ml,
			      ml - SEQLZ_MIN_MATCH - SEQLZ_ML_CAP);
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

static __always_inline void encoder_init(struct seqlz_encoder *e, u8 *d,
					 unsigned int dst_cap,
					 const u8 *src_end)
{
	*e = (struct seqlz_encoder){ .p = d + dst_cap,
				     .lit = d + SEQLZ_HEADER,
				     .src_end = src_end,
				     .last = 1 };
}

/*
 * Writes the u16, and copies the bitstream from the back of dst to right after
 * the literals, the right way round. Returns the length of the block, or 0 if
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
	 * block that nearly fills dst gets there.
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
static unsigned int encode_raw(const struct seqlz_sequence *seq, unsigned int n,
			       const u8 *literals, unsigned int n_literals,
			       const u8 *block, u8 *deltas, unsigned int *delta,
			       void *dst, unsigned int dst_cap)
{
	const u8 *in = literals;
	struct seqlz_encoder e;
	unsigned int i, pos = 0, lits = 0;

	if (dst_cap < SEQLZ_HEADER + ENC_ROOM || n == 0 ||
	    n > SEQLZ_MAX_SEQUENCES || n_literals > SEQLZ_BLOCK_SIZE)
		return 0;
	/*
	 * Only sequences that make a block, checked as the decoder checks them,
	 * before anything is written. dst has room only because every match
	 * covers at least 4 bytes: a shorter one makes ml - 4 wrap, and its
	 * length value takes more bits than the block could save.
	 */
	for (i = 0; i < n; i++) {
		lits += seq[i].literals;
		pos += seq[i].literals;
		if (pos > SEQLZ_BLOCK_SIZE)
			return 0;
		if (i + 1 == n)
			break;
		if (seq[i].match < SEQLZ_MIN_MATCH || seq[i].offset == 0 ||
		    seq[i].offset > pos)
			return 0;
		pos += seq[i].match;
	}
	if (pos != SEQLZ_BLOCK_SIZE || lits != n_literals)
		return 0;
	encoder_init(&e, dst, dst_cap, literals + n_literals);
	/* with a block, the records for the deltas, as compress_own() */
	e.rec = block != NULL;
	for (;;) {
		for (i = 0; i < n; i++) {
			unsigned int ll = seq[i].literals;

			encode_seq(&e, in, ll, i + 1 == n ? 0U : seq[i].match,
				   seq[i].offset, true);
			in += ll;
		}
		/*
		 * encoder_init() clears rec: the pass without records is the
		 * last
		 */
		if (e.p != e.lit || !e.rec)
			break;
		encoder_init(&e, dst, dst_cap, literals + n_literals);
		in = literals;
	}
	*delta = e.rec ? seqlz_delta_literals(dst, &e.lit, block, deltas) : 0;
	return encoder_finish(&e, dst, dst_cap);
}

unsigned int seqlz_encode(struct seqlz_state *st,
			  const struct seqlz_sequence *seq, unsigned int n,
			  const u8 *literals, unsigned int n_literals,
			  const void *block, void *dst, unsigned int dst_cap,
			  unsigned int level)
{
	unsigned int len, delta;

	if (level < 1 || level > SEQLZ_LEVELS ||
	    (level >= SEQLZ_LEVEL_OWN && !st))
		return 0;
	len = encode_raw(seq, n, literals, n_literals,
			 level >= SEQLZ_LEVEL_OWN ? block : NULL,
			 st ? st->literals : NULL, &delta, dst, dst_cap);
	if (len == 0 || level < SEQLZ_LEVEL_CODED)
		return len;
	if (level < SEQLZ_LEVEL_OWN) {
		/* the copy the coder reads the literals from, st may be NULL */
		u8 spare[SEQLZ_BLOCK_SIZE];

		return seqlz_code_literals(dst, len, spare);
	}
	return seqlz_code_own(dst, len, st, delta);
}

static unsigned int compress_block(struct seqlz_state *st, const u8 *src,
				   void *dst, unsigned int dst_cap)
{
	struct seqlz_encoder e;

	if (dst_cap < SEQLZ_HEADER + ENC_ROOM)
		return 0;
	encoder_init(&e, dst, dst_cap, src + SEQLZ_BLOCK_SIZE);
	seqlz_match_block(st->table, src, false, encode_emit, &e);
	return encoder_finish(&e, dst, dst_cap);
}

/*
 * compress_block() for SEQLZ_LEVEL_OWN: the lazy matcher, with the records for
 * the deltas, and without them if the block does not fit with them; *delta
 * gets the delta. A function of its own, so that the other levels' code stays
 * as it is without it: inlined next to it, their blocks were slower to write.
 */
static noinline unsigned int compress_own(struct seqlz_state *st, const u8 *src,
					  void *dst, unsigned int dst_cap,
					  unsigned int *delta)
{
	struct seqlz_encoder e;

	*delta = 0;
	if (dst_cap < SEQLZ_HEADER + ENC_ROOM)
		return 0;
	encoder_init(&e, dst, dst_cap, src + SEQLZ_BLOCK_SIZE);
	e.rec = true;
	for (;;) {
		seqlz_match_block(st->table, src, true, encode_emit_rec, &e);
		/*
		 * encoder_init() clears rec: the pass without records is the
		 * last
		 */
		if (e.p != e.lit || !e.rec)
			break;
		encoder_init(&e, dst, dst_cap, src + SEQLZ_BLOCK_SIZE);
	}
	if (e.rec) {
		/* not &e.lit: e would leave the function and its registers */
		u8 *lit = e.lit;

		*delta = seqlz_delta_literals(dst, &lit, src, st->literals);
		e.lit = lit;
	}
	return encoder_finish(&e, dst, dst_cap);
}

unsigned int seqlz_compress(struct seqlz_state *st, const void *src, void *dst,
			    unsigned int dst_cap, unsigned int level)
{
	unsigned int len, delta;

	if (level < 1 || level > SEQLZ_LEVELS)
		return 0;
	if (level >= SEQLZ_LEVEL_OWN) {
		len = compress_own(st, src, dst, dst_cap, &delta);
		return len ? seqlz_code_own(dst, len, st, delta) : 0;
	}
	len = compress_block(st, src, dst, dst_cap);
	if (len == 0 || level < SEQLZ_LEVEL_CODED)
		return len;
	return seqlz_code_literals(dst, len, st->literals);
}
