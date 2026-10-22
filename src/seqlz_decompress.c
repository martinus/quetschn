// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The decompressor: seqlz_decode().
 */
#include "seqlz.h"

/*
 * Copies of 8 and 16 bytes with __builtin_memcpy(), as lib/lz4's LZ4_memcpy(),
 * see seqlz_internal.h. copy8() loads before it stores, so its source may be
 * what an earlier copy8() just wrote, 8 or more bytes before d. copy16() loads
 * both halves first: its source and destination must not overlap.
 */
static __always_inline void copy8(u8 *d, const u8 *s)
{
	u64 v;

	__builtin_memcpy(&v, s, 8);
	__builtin_memcpy(d, &v, 8);
}

static __always_inline void copy16(u8 *d, const u8 *s)
{
	u64 a, b;

	__builtin_memcpy(&a, s, 8);
	__builtin_memcpy(&b, s + 8, 8);
	__builtin_memcpy(d, &a, 8);
	__builtin_memcpy(d + 8, &b, 8);
}

/*
 * Copies the nl literals of a sequence from lit to d; the caller has checked
 * that they fit. It copies 16 bytes at a time, also past nl, as long as 16
 * bytes fit in the block and in the input: what it writes past nl, the next
 * sequence overwrites. Only near the end of the block or of the input does it
 * copy byte by byte, at most 15 bytes.
 */
static __always_inline void copy_literals(u8 *d, const u8 *d_end, const u8 *lit,
					  const u8 *s_end, unsigned int nl)
{
	unsigned int k = 0;

	/*
	 * Most sequences have fewer than 16 literals, so the first 16 bytes are
	 * one copy without a loop, which could mispredict.
	 */
	if ((unsigned int)(d_end - d) >= 16U &&
	    (unsigned int)(s_end - lit) >= 16U) {
		copy16(d, lit);
		k = 16;
	}
	while (k < nl && (unsigned int)(d_end - d) >= k + 16U &&
	       (unsigned int)(s_end - lit) >= k + 16U) {
		copy16(d + k, lit + k);
		k += 16;
	}
	for (; k < nl; k++)
		d[k] = lit[k];
}

/*
 * For an offset below 8, the largest multiple of it up to 8: how far a copy of
 * 8 bytes of a repeated pattern can go on, see copy_match().
 */
static const u8 repeat_step[8] = { 0, 8, 8, 6, 8, 5, 6, 7 };

/*
 * For an offset below 8, 1 to 7: the off bytes before d repeated to 8 bytes in
 * one register, little endian, so that they are the low ones. repeat[off] has
 * a 1 in every byte at a multiple of off, so the multiply puts a copy of the
 * off bytes there. The same 8 bytes fit again every repeat_step[off] bytes.
 */
static __always_inline u64 repeat_pattern(const u8 *d, unsigned int off)
{
	static const u64 repeat[8] = { 0,
				       0x0101010101010101ULL,
				       0x0001000100010001ULL,
				       0x0001000001000001ULL,
				       0x0000000100000001ULL,
				       0x0000010000000001ULL,
				       0x0001000000000001ULL,
				       0x0100000000000001ULL };

	return (get_unaligned_le64(d - off) & (~0ULL >> (64U - 8U * off))) *
	       repeat[off];
}

/*
 * Copies a match of len bytes that starts off bytes before d; the caller has
 * checked that it fits and that off points into the block. Like the literals, 8
 * bytes at a time as long as 8 bytes fit in the block, also past len; only the
 * last 7 bytes of a block are copied one by one. Copying byte by byte up to the
 * end of the block made the slowest blocks many times slower than lz4.
 *
 * An offset below 8 overlaps the bytes it writes: off = 1 repeats one byte,
 * off = 2 two bytes, and so on. Then the off bytes before d are repeated to 8
 * bytes in a register, see repeat_pattern(), and stored. The same 8 bytes fit
 * again every step bytes, a multiple of off, so the rest are stores of that
 * register only. Writing the first bytes one by one made each load wait for
 * the stores before it, and the loop over them mispredicted its end.
 */
static __always_inline void copy_match(u8 *d, const u8 *d_end, unsigned int off,
				       unsigned int len)
{
	unsigned int k = 0;

	if (off < 8) {
		/* with less than 8 bytes left, byte by byte below */
		if ((unsigned int)(d_end - d) >= 8U) {
			unsigned int step = repeat_step[off & 7U];
			/*
			 * reads d - off up to d - off + 7, which is below
			 * d + 8 <= d_end: inside the block
			 */
			u64 pat = repeat_pattern(d, off);

			put_unaligned_le64(pat, d);
			/* the same 8 bytes every step bytes, stores only */
			for (k = step;
			     k < len && (unsigned int)(d_end - d) >= k + 8U;
			     k += step)
				put_unaligned_le64(pat, d + k);
		}
	} else {
		if ((unsigned int)(d_end - d) >= 16U) {
			/*
			 * Most matches are at most 16 bytes, so 16 bytes are
			 * copied without a loop. The second copy reads bytes
			 * the first one wrote or that were there before, so the
			 * order is right. 32 bytes made the median block faster
			 * and the slowest ones slower: those have many short
			 * matches, and copied 32 bytes for each.
			 */
			copy8(d, d - off);
			copy8(d + 8, d + 8 - off);
			k = 16;
		}
		while (k < len && (unsigned int)(d_end - d) >= k + 8U) {
			copy8(d + k, d + k - off);
			k += 8;
		}
	}
	for (; k < len; k++)
		d[k] = *(d + k - off);
}

/*
 * Reads the sequences' bitstream, most significant bit first: bits has the
 * next bits on top, count says how many of them are real. A refill loads 8
 * bytes at once while 8 are left, and byte by byte at the end, so it never
 * reads past the stream. A block that wants more bits than it has gets zeros,
 * and count goes negative: the block is not valid. That bits are right does not
 * matter for memory safety, every length and offset is checked where it is
 * used.
 */
struct seqlz_bit_reader {
	const u8 *p;
	const u8 *end;
	u64 bits;
	int count; /* negative once more bits were taken than there are */
};

static inline void seqlz_br_refill(struct seqlz_bit_reader *r)
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
static inline void seqlz_br_drop(struct seqlz_bit_reader *r, unsigned int n)
{
	r->bits <<= n;
	r->count -= (int)n;
}

/*
 * Reads one length value. The entry of its code has the base value and the
 * number of extra bits, which follow the code. Takes up to SEQLZ_MAX_BITS +
 * SEQLZ_BLOCK_BITS bits, 20 or 22; the caller refills before.
 */
static inline unsigned int seqlz_br_value(struct seqlz_bit_reader *r,
					  const struct value_table *t)
{
	u32 e = t->decode[r->bits >> (64U - SEQLZ_MAX_BITS)];
	unsigned int n = e & 15U, x = (e >> 4) & 15U;
	/*
	 * the x extra bits after the code; two shifts, because a shift by 64 -
	 * x is undefined for x = 0
	 */
	unsigned int v = ((e >> 8) & 0xffffU) +
			 (unsigned int)(((r->bits << n) >> 1) >> (63U - x));

	seqlz_br_drop(r, n + x);
	return v;
}

/*
 * Adds the 4 (8) bytes dist before p to the 4 (8) bytes at p, each byte apart,
 * modulo 256: the top bit of each byte is added apart, so that no carry goes
 * from one byte into the next
 */
static __always_inline void add4(u8 *p, unsigned int dist)
{
	const u32 low = 0x7f7f7f7fU; /* bits 0 to 6 of each byte */
	u32 a = get_unaligned_le32(p), b = get_unaligned_le32(p - dist);

	put_unaligned_le32(((a & low) + (b & low)) ^ ((a ^ b) & ~low), p);
}

static __always_inline void add8(u8 *p, unsigned int dist)
{
	const u64 low = 0x7f7f7f7f7f7f7f7fULL; /* bits 0 to 6 of each byte */
	u64 a = get_unaligned_le64(p), b = get_unaligned_le64(p - dist);

	put_unaligned_le64(((a & low) + (b & low)) ^ ((a ^ b) & ~low), p);
}

/*
 * Turns the nl deltas just copied to d back into literals, see
 * SEQLZ_LIT_DELTA_AT, in the block's order: the byte a delta is to is before
 * it, so it is the block's own again by then. last is the offset of the
 * sequence before.
 * Whole words while a word fits before the literals' end: as the byte
 * referred to is at least 4 (8) bytes before each byte of the word, all of
 * them are done when it is read.
 */
static __always_inline void undelta_literals(unsigned int delta, u8 *d,
					     const u8 *dst, unsigned int nl,
					     unsigned int last)
{
	const unsigned int at = (unsigned int)(d - dst);
	unsigned int dist = delta == SEQLZ_DELTA_4 ? 4U :
			    delta == SEQLZ_DELTA_8 ? 8U :
						     seqlz_delta_last(last);
	/* the first delta: the first literal dist bytes or more into dst */
	unsigned int k = at >= dist ? 0 : dist - at;

	if (dist >= 8U) {
		for (; k + 8 <= nl; k += 8)
			add8(d + k, dist);
	} else {
		for (; k + 4 <= nl; k += 4)
			add4(d + k, dist);
	}
	for (; k < nl; k++)
		d[k] = (u8)(d[k] + *(d + k - dist));
}

/*
 * undelta_literals() for the fast path, which copied 16 bytes to d, of which nl
 * are literals: all 16 bytes, without a loop. What it writes past nl, the match
 * after the literals overwrites, and that match reads only bytes before them.
 * Near the block's start, the loops above.
 */
static __always_inline void undelta_fast(unsigned int delta, u8 *d,
					 const u8 *dst, unsigned int nl,
					 unsigned int last)
{
	unsigned int dist = delta == SEQLZ_DELTA_4 ? 4U :
			    delta == SEQLZ_DELTA_8 ? 8U :
						     seqlz_delta_last(last);

	if ((unsigned long)(d - dst) < dist) {
		undelta_literals(delta, d, dst, nl, last);
	} else if (dist >= 8U) {
		add8(d, dist);
		add8(d + 8, dist);
	} else {
		add4(d, dist);
		add4(d + 4, dist);
		add4(d + 8, dist);
		add4(d + 12, dist);
	}
}

/*
 * The fast path's match of len bytes, at most 34, off bytes before d: with room
 * for 40 bytes at d, see seqlz_decode(). Without loops for all but long matches
 * with an offset below 8.
 */
static __always_inline void copy_match_fast(u8 *d, unsigned int off,
					    unsigned int len)
{
	if (off >= 8U) {
		/*
		 * With 8 <= off < 16 a load reads what the store before it
		 * wrote, as an overlapping match needs. From m, not d + 8 -
		 * off: clang computed that sum again for every load.
		 */
		const u8 *m = d - off;

		copy8(d, m);
		copy8(d + 8, m + 8);
		if (len > 16U) {
			copy8(d + 16, m + 16);
			copy8(d + 24, m + 24);
			if (len > 32U)
				copy8(d + 32, m + 32);
		}
	} else {
		/*
		 * off < 8: five stores step bytes apart cover at least 28
		 * bytes, the loop the longer matches
		 */
		unsigned int step = repeat_step[off], k;
		u64 pat = repeat_pattern(d, off);

		put_unaligned_le64(pat, d);
		put_unaligned_le64(pat, d + step);
		put_unaligned_le64(pat, d + 2U * step);
		put_unaligned_le64(pat, d + 3U * step);
		put_unaligned_le64(pat, d + 4U * step);
		for (k = 5U * step; k < len; k += step)
			put_unaligned_le64(pat, d + k);
	}
}

/*
 * The next token's entry. A refill only when token and offset might not fit,
 * 11 + 12 bits: then the token's lookup does not wait for the refill's load,
 * and a refill leaves 56 bits for two or three sequences. The escape and the
 * length values refill before they read. The token's entry knows the offset's
 * class, so the offset's bits are known without a second lookup.
 */
static __always_inline u32 next_token(struct seqlz_bit_reader *br,
				      const struct token_table *t)
{
	if (br->count < (int)(SEQLZ_TOKEN_BITS + SEQLZ_BLOCK_BITS))
		seqlz_br_refill(br);
	return t->decode[br->bits >> (64U - SEQLZ_TOKEN_BITS)];
}

/*
 * The next token in the fast path, read before the sequence's copies: its entry
 * takes a while to load, and on an in-order core such as the Cortex-A55 the
 * copies fill that time. No length value follows, so the next token's code
 * comes right after the offset.
 *
 * The refill: every second fast sequence, without checking the bits left.
 * Whether they run short depends on the codes before, which the branch
 * predictor learns only for a block it has seen, and a swap-in decodes a block
 * once. A refill leaves at least 56 bits, and two fast sequences take at most
 * 50, 11 for each token and up to 14 for each offset; after a slow one it
 * always refills, *skip is 0 then. 8 bytes of input are there, and count is at
 * least 0.
 */
static __always_inline u32 next_token_fast(struct seqlz_bit_reader *br,
					   const struct token_table *t,
					   unsigned int *skip)
{
	if (!*skip) {
		u64 v = get_unaligned_be64(br->p);

		br->bits |= v >> br->count;
		/* as in seqlz_br_refill() */
		br->p += (63 - br->count) >> 3;
		br->count |= 56;
	}
	*skip ^= 1U;
	return t->decode[br->bits >> (64U - SEQLZ_TOKEN_BITS)];
}

/*
 * The room the fast path of seqlz_decode() needs. In the block: it writes at
 * most 54 bytes, 14 literals and 40 bytes of match. In the literals: one copy
 * of 16 bytes. In the input: the 8 bytes of a refill.
 */
#define FAST_BLOCK_ROOM 64U
#define FAST_LIT_ROOM 16U
#define FAST_IN_ROOM 8U

/*
 * seqlz_decode() for a block whose literals are deltas, see SEQLZ_LIT_DELTA_AT,
 * or not, deltas a constant: two copies of the loop over the sequences, so
 * that a block without deltas runs none of their code. A check of it for every
 * sequence made those blocks slower to read.
 */
static __always_inline int decode_block(const void *src, unsigned int src_len,
					void *dst, void *scratch, bool deltas)
{
	const u8 *s = src;
	const u8 *const s_end = s + src_len;
	u8 *d = dst;
	u8 *const d_end = d + SEQLZ_BLOCK_SIZE;
	const u8 *lit, *lit_end, *lit_bound;
	u8 *d_fast;
	unsigned long lit_fast, in_fast, end_gap;
	struct seqlz_bit_reader br;
	unsigned int n_lit, last = 1, tok, skip = 0, delta = 0;

	if (src_len < SEQLZ_HEADER)
		return -EINVAL;
	/*
	 * The decoder's tables first, so that their cache misses overlap. An
	 * in-order core stops at every miss it did not see coming.
	 */
	prefetch_lines(seqlz_tables.token.decode,
		       sizeof(seqlz_tables.token.decode));
	prefetch_lines(seqlz_tables.ll.decode, sizeof(seqlz_tables.ll.decode));
	prefetch_lines(seqlz_tables.ml.decode, sizeof(seqlz_tables.ml.decode));
	n_lit = get_unaligned_le16(s);
	if (n_lit & SEQLZ_LIT_CODED) {
		/* decoded into the scratch first, the bitstream follows them */
		const u8 *q;

		n_lit &= SEQLZ_LIT_CODED - 1U;
		if (!scratch)
			return -EINVAL;
		q = seqlz_decode_literals(s, src_len, n_lit, scratch, d,
					  &delta);
		if (!q)
			return -EINVAL;
		br = (struct seqlz_bit_reader){ .p = q, .end = s_end };
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
		br = (struct seqlz_bit_reader){ .p = lit_end, .end = s_end };
	}

	/*
	 * The fast path below copies without checking the room left, so it is
	 * taken only where there is enough, one compare each, see
	 * FAST_BLOCK_ROOM. lit_fast and in_fast are numbers, not pointers: with
	 * less input, lit_bound - FAST_LIT_ROOM would point before it.
	 */
	d_fast = d_end - FAST_BLOCK_ROOM;
	lit_fast = (unsigned long)lit_bound - FAST_LIT_ROOM;
	/* 0 if the input is shorter than a refill: no refill without a check */
	in_fast = src_len >= FAST_IN_ROOM ?
			  (unsigned long)s_end - FAST_IN_ROOM :
			  0;

	/*
	 * The literals left fill the rest of the block when d_end - d is
	 * lit_end - lit, that is when d - lit is end_gap: one compare of the
	 * two pointers the loop moves. Numbers, as d and lit point into
	 * different buffers.
	 */
	end_gap = (unsigned long)d_end - (unsigned long)lit_end;
	/* a block of literals only has no sequence with a match, no token */
	if (n_lit == SEQLZ_BLOCK_SIZE)
		goto last_literals;
	tok = next_token(&br, &seqlz_tables.token);
	for (;;) {
		unsigned int nl, len, off, back, raw;

		if (tok >> TOK_ESCAPE_AT) {
			/* escape: SEQLZ_ESCAPE_BITS bits of the token follow */
			unsigned int idx;

			seqlz_br_drop(&br, TOK_DROP(tok));
			seqlz_br_refill(&br);
			idx = (unsigned int)(br.bits >>
					     (64U - SEQLZ_ESCAPE_BITS));
			seqlz_br_drop(&br, SEQLZ_ESCAPE_BITS);
			if (idx >= SEQLZ_TOKEN_SYMBOLS)
				return -EINVAL;
			tok = token_entry(idx, 0);
		}
		/*
		 * The offset: shift the token's code out on top, then the
		 * offset's bits down. Class 0 has none, back is 0, and the
		 * offset is the one before.
		 */
		back = TOK_BACK(tok);
		raw = (unsigned int)((br.bits << TOK_N(tok)) >> back)
		      << TOK_SHIFT(tok);
		seqlz_br_drop(&br, TOK_DROP(tok));
		off = back ? raw : last;
		nl = TOK_LL(tok);
		len = TOK_ML(tok);
		/*
		 * The fast path: no length value follows, and there is room,
		 * see d_fast. Then nl is at most 14 and len at most 34, and the
		 * copies are 16 literal bytes and 16 to 40 bytes of match,
		 * without loops. The block's last match never takes it: after
		 * it fewer than 8 bits are left, and the fast path needs 8
		 * bytes of input, so it never misses the end below.
		 */
		if (!(tok & (1U << TOK_VALUE_AT)) && d <= d_fast &&
		    (unsigned long)lit <= lit_fast &&
		    (unsigned long)br.p <= in_fast) {
			tok = next_token_fast(&br, &seqlz_tables.token, &skip);
			/*
			 * No check of nl against the literals left, which saved
			 * 3 instructions per sequence. The 16 bytes read are
			 * inside lit_bound, so lit stays inside it. If nl goes
			 * past lit_end, bytes of the bitstream were taken as
			 * literals, and the slow path below, where every block
			 * ends, rejects the block because lit > lit_end.
			 */
			copy16(d, lit);
			if (deltas)
				undelta_fast(delta, d, dst, nl, last);
			d += nl;
			lit += nl;
			last = off;
			if (off - 1U >= (unsigned int)(d - (u8 *)dst))
				return -EINVAL;
			copy_match_fast(d, off, len);
			d += len;
			continue;
		}
		/* the slow path: length values, and every check */
		if (nl == SEQLZ_LL_CAP) {
			seqlz_br_refill(&br);
			nl += seqlz_br_value(&br, &seqlz_tables.ll);
		}
		if (lit > lit_end || nl > (unsigned int)(lit_end - lit) ||
		    nl > (unsigned int)(d_end - d))
			return -EINVAL;
		copy_literals(d, d_end, lit, lit_bound, nl);
		if (deltas)
			undelta_literals(delta, d, dst, nl, last);
		d += nl;
		lit += nl;
		if (len == SEQLZ_ML_CAP + 4U) {
			seqlz_br_refill(&br);
			len += seqlz_br_value(&br, &seqlz_tables.ml);
		}
		last = off;
		/* off must point into the block; off - 1 wraps for 0 */
		if (off - 1U >= (unsigned int)(d - (u8 *)dst) ||
		    len > (unsigned int)(d_end - d))
			return -EINVAL;
		copy_match(d, d_end, off, len);
		d += len;
		/*
		 * The literals left fill the rest of the block, see end_gap:
		 * every match has at least 4 bytes, so none can follow. They
		 * are the last sequence, which has no token.
		 */
		if ((unsigned long)d - (unsigned long)lit == end_gap)
			break;
		tok = next_token(&br, &seqlz_tables.token);
		skip = 0;
	}
last_literals:
	/*
	 * the last sequence: the literals left, as many as the block has room
	 */
	copy_literals(d, d_end, lit, lit_bound, (unsigned int)(lit_end - lit));
	if (deltas)
		undelta_literals(delta, d, dst, (unsigned int)(lit_end - lit),
				 last);
	/*
	 * The block must end where its bits end: every byte read, fewer than 8
	 * bits left in the last one, and those 0, as the compressor writes
	 * them. zram has no checksum, so this is what tells a damaged block.
	 */
	if (br.count < 0 || br.p != br.end || br.count >= 8 ||
	    (br.count > 0 && (br.end[-1] & ((1U << br.count) - 1U))))
		return -EINVAL;
	return 0;
}

int seqlz_decode(const void *src, unsigned int src_len, void *dst,
		 void *scratch)
{
	const u8 *s = src;

	/* bit 7 of byte 2, with coded literals */
	if (src_len > SEQLZ_HEADER &&
	    (get_unaligned_le16(s) & SEQLZ_LIT_CODED) &&
	    s[2] >> SEQLZ_LIT_DELTA_AT)
		return decode_block(src, src_len, dst, scratch, true);
	return decode_block(src, src_len, dst, scratch, false);
}
