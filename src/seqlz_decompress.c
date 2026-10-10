// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The decompressor: seqlz_decode().
 */
#include "seqlz_internal.h"

/*
 * Prefetches every cache line of [p, p + size); size is a multiple of
 * PREFETCH_STEP. In a swap-in the decoder's tables are often no longer in
 * the cache: asking for all of their lines at once lets the misses overlap,
 * instead of one after the other as the decoder runs into them. 512 bytes per
 * iteration, 8 lines where they have 64 bytes: with one line per iteration,
 * the loop took more instructions than the prefetches.
 */
#define PREFETCH_STEP 512U

static inline void prefetch_lines(const void *p, unsigned long size)
{
	const u8 *q = p;
	const u8 *const end = q + size;
	unsigned int k;

	for (; q < end; q += PREFETCH_STEP)
		for (k = 0; k < PREFETCH_STEP; k += L1_CACHE_BYTES)
			prefetch(q + k);
}

/* prefetch_lines() takes multiples of PREFETCH_STEP */
static_assert(sizeof_field(struct token_table, decode) % PREFETCH_STEP == 0,
	      "the token table");
static_assert(sizeof_field(struct value_table, decode) % PREFETCH_STEP == 0,
	      "the length values' tables");
static_assert(sizeof_field(struct lit_table, decode) % PREFETCH_STEP == 0,
	      "the literal tables");

/*
 * Copies of 8 and 16 bytes with __builtin_memcpy(), as lib/lz4's LZ4_memcpy(),
 * see seqlz.h. copy8() loads before it stores, so its source may be what an
 * earlier copy8() just wrote, 8 or more bytes before d. copy16() loads both
 * halves first: its source and destination must not overlap.
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
 * bytes fit in the page and in the input: what it writes past nl, the next
 * sequence overwrites. Only near the end of the page or of the input does it
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
 * checked that it fits and that off points into the page. Like the literals, 8
 * bytes at a time as long as 8 bytes fit in the page, also past len; only the
 * last 7 bytes of a page are copied one by one. Copying byte by byte up to the
 * end of the page made the slowest pages many times slower than lz4.
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
	unsigned int step = off >= 8 ? 8U : repeat_step[off & 7U],
		     back = off >= 8 ? off : step, k = 0;

	if (off < 8) {
		if ((unsigned int)(d_end - d) >= 8U) {
			/*
			 * reads d - off up to d - off + 7, which is below
			 * d + 8 <= d_end: inside the page
			 */
			u64 pat = repeat_pattern(d, off);

			put_unaligned_le64(pat, d);
			/* the same 8 bytes every step bytes, stores only */
			for (k = step;
			     k < len && (unsigned int)(d_end - d) >= k + 8U;
			     k += step)
				put_unaligned_le64(pat, d + k);
			back = 0;
		} else {
			back = 0; /* less than 8 bytes left: byte by byte */
		}
	} else if ((unsigned int)(d_end - d) >= 16U) {
		/*
		 * Most matches are at most 16 bytes, so 16 bytes are copied
		 * without a loop. With off >= 8 the second copy reads bytes the
		 * first one wrote or that were there before, so the order is
		 * right. 32 bytes made the median page faster and the slowest
		 * ones slower: those have many short matches, and copied 32
		 * bytes for each.
		 */
		copy8(d, d - off);
		copy8(d + 8, d + 8 - off);
		k = 16;
	}
	if (back != 0) {
		while (k < len && (unsigned int)(d_end - d) >= k + 8U) {
			copy8(d + k, d + k - back);
			k += step;
		}
	}
	for (; k < len; k++)
		d[k] = *(d + k - off);
}

/*
 * Reads the sequences' bitstream, most significant bit first: bits has the
 * next bits on top, count says how many of them are real. A refill loads 8
 * bytes at once while 8 are left, and byte by byte at the end, so it never
 * reads past the stream. A page that wants more bits than it has gets zeros,
 * and count goes negative: the page is not valid. That bits are right does not
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
 * QUETSCHN_PAGE_BITS bits, 20 or 22; the caller refills before.
 */
static inline unsigned int seqlz_br_value(struct seqlz_bit_reader *r,
					  const struct value_table *t,
					  u32 *entry)
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
	seqlz_br_drop(r, n + x);
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
 * The sizes of the 8 streams of coded literals, width bits each from byte 3 of
 * the page at s, and where each starts, the first at q. Returns their sum. The
 * sizes are copied out of the page first: a 4-byte load at the last size's byte
 * would read past the header.
 */
static __always_inline u64 lit_stream_sizes(const u8 *s, unsigned int width,
					    const u8 *q, unsigned int *sz,
					    const u8 **start)
{
	u8 h[SEQLZ_SIZE_BITS_MAX + 4U] = { 0 };
	u64 total = 0;
	unsigned int k;

	__builtin_memcpy(h, s + SEQLZ_LIT_HEADER(0), width);
	for (k = 0; k < SEQLZ_LIT_STREAMS; k++) {
		sz[k] = (get_unaligned_le32(h + k * width / 8U) >>
			 (k * width % 8U)) &
			((1U << width) - 1U);
		start[k] = q + total;
		total += sz[k];
	}
	return total;
}

/*
 * Whether a stream of sz bytes from start ends in its last byte, read up to ip
 * with bits left in the 64 bits of b, the last of them marked by a 1: the bits
 * used, the bytes it moved past plus the 1's position, are 0 to 7 fewer than
 * its size, and the bits after its last code are 0. Those are read from the
 * page, not from b: when the stream's last 8 bytes are in it, its last bit is
 * where the 1 is. A stream that read into the next one fails here, and so does
 * one with bytes after its codes, also one without literals that has a size.
 */
static __always_inline bool lit_stream_ends(const u8 *start, unsigned int sz,
					    const u8 *ip, u64 b)
{
	long left = 8L * sz - (8L * (ip - start) + __builtin_ctzll(b));

	return left >= 0 && left <= 7 &&
	       (left == 0 || !(start[sz - 1U] & ((1U << left) - 1U)));
}

/*
 * Decodes the coded literals of a page into out, the scratch, see
 * SEQLZ_LIT_HEADER for the layout. Returns where the sequences' bitstream
 * starts, or NULL if the page is not valid. Not inlined: inside the loop over
 * the sequences its registers made pages with raw literals slower too.
 */
static noinline const u8 *decode_literals(const struct seqlz_tables *t,
					  const u8 *s, unsigned int src_len,
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
	const u8 *i0, *i1, *i2, *i3, *i4, *i5, *i6, *i7;
	unsigned int k, j, rest, steps, r;
	u64 total = 0;
	const u16 *lt;

	if (src_len < SEQLZ_LIT_HEADER(width) || s[2] >> SEQLZ_LIT_ZERO_AT ||
	    n_lit > SEQLZ_PAGE)
		return NULL;
	lt = t->lit[s[2] & (SEQLZ_LIT_SETS - 1U)].decode;
	prefetch_lines(lt, sizeof(t->lit[0].decode));
	total = lit_stream_sizes(s, width, q, sz, start);
	for (k = 0; k < SEQLZ_LIT_STREAMS; k++)
		ip[k] = start[k];
	if (SEQLZ_LIT_HEADER(width) + total > src_len)
		return NULL;
	/*
	 * Full rounds of SEQLZ_LIT_ROUNDS literals per stream, all 8 streams
	 * side by side. The rest below.
	 */
	for (k = 0; k + SEQLZ_LIT_STREAMS * SEQLZ_LIT_ROUNDS <= n_lit;
	     k += SEQLZ_LIT_STREAMS * SEQLZ_LIT_ROUNDS) {
		i0 = ip[0];
		i1 = ip[1];
		i2 = ip[2];
		i3 = ip[3];
		i4 = ip[4];
		i5 = ip[5];
		i6 = ip[6];
		i7 = ip[7];
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
	rest = n_lit - k;
	steps = rest / SEQLZ_LIT_STREAMS;
	r = rest % SEQLZ_LIT_STREAMS;
	i0 = ip[0];
	i1 = ip[1];
	i2 = ip[2];
	i3 = ip[3];
	i4 = ip[4];
	i5 = ip[5];
	i6 = ip[6];
	i7 = ip[7];
	b0 = lit_refill(&i0, b0, end);
	b1 = lit_refill(&i1, b1, end);
	b2 = lit_refill(&i2, b2, end);
	b3 = lit_refill(&i3, b3, end);
	b4 = lit_refill(&i4, b4, end);
	b5 = lit_refill(&i5, b5, end);
	b6 = lit_refill(&i6, b6, end);
	b7 = lit_refill(&i7, b7, end);
	for (j = 0; j < steps; j++) {
		b0 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j], b0, lt, 63U);
		b1 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 1], b1, lt,
				63U);
		b2 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 2], b2, lt,
				63U);
		b3 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 3], b3, lt,
				63U);
		b4 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 4], b4, lt,
				63U);
		b5 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 5], b5, lt,
				63U);
		b6 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 6], b6, lt,
				63U);
		b7 = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + 7], b7, lt,
				63U);
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
	if (!lit_stream_ends(start[0], sz[0], ip[0], b0) ||
	    !lit_stream_ends(start[1], sz[1], ip[1], b1) ||
	    !lit_stream_ends(start[2], sz[2], ip[2], b2) ||
	    !lit_stream_ends(start[3], sz[3], ip[3], b3) ||
	    !lit_stream_ends(start[4], sz[4], ip[4], b4) ||
	    !lit_stream_ends(start[5], sz[5], ip[5], b5) ||
	    !lit_stream_ends(start[6], sz[6], ip[6], b6) ||
	    !lit_stream_ends(start[7], sz[7], ip[7], b7))
		return NULL;
	return q + total;
}

/*
 * The fast path's match of len bytes, at most 34, off bytes before d: with room
 * for 40 bytes at d, see decode_page(). Without loops for all but long matches
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
	if (br->count < (int)(SEQLZ_TOKEN_BITS + QUETSCHN_PAGE_BITS))
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
 * predictor learns only for a page it has seen, and a swap-in decodes a page
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
 * The room the fast path of decode_page() needs. In the page: it writes at most
 * 54 bytes, 14 literals and 40 bytes of match. In the literals: one copy of 16
 * bytes. In the input: the 8 bytes of a refill.
 */
#define FAST_PAGE_ROOM 64U
#define FAST_LIT_ROOM 16U
#define FAST_IN_ROOM 8U

/* seqlz_decode(), see seqlz.h */
static __always_inline int decode_page(const struct seqlz_tables *t,
				       const void *src, unsigned int src_len,
				       void *dst, void *scratch)
{
	const u8 *s = src;
	const u8 *const s_end = s + src_len;
	u8 *d = dst;
	u8 *const d_end = d + SEQLZ_PAGE;
	const u8 *lit, *lit_end, *lit_bound;
	u8 *d_fast;
	unsigned long lit_fast, in_fast;
	struct seqlz_bit_reader br;
	unsigned int n_lit, last = 1, tok, skip = 0;

	if (src_len < SEQLZ_HEADER)
		return -EINVAL;
	/*
	 * The decoder's tables first, so that their cache misses overlap. An
	 * in-order core stops at every miss it did not see coming.
	 */
	prefetch_lines(t->token.decode, sizeof(t->token.decode));
	prefetch_lines(t->ll.decode, sizeof(t->ll.decode));
	prefetch_lines(t->ml.decode, sizeof(t->ml.decode));
	n_lit = get_unaligned_le16(s);
	if (n_lit & SEQLZ_LIT_CODED) {
		/* decoded into the scratch first, the bitstream follows them */
		const u8 *q;

		n_lit &= SEQLZ_LIT_CODED - 1U;
		if (!scratch)
			return -EINVAL;
		q = decode_literals(t, s, src_len, n_lit, scratch);
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
	 * FAST_PAGE_ROOM. lit_fast and in_fast are numbers, not pointers: with
	 * less input, lit_bound - FAST_LIT_ROOM would point before it.
	 */
	d_fast = d_end - FAST_PAGE_ROOM;
	lit_fast = (unsigned long)lit_bound - FAST_LIT_ROOM;
	/* 0 if the input is shorter than a refill: no refill without a check */
	in_fast = src_len >= FAST_IN_ROOM ?
			  (unsigned long)s_end - FAST_IN_ROOM :
			  0;

	tok = next_token(&br, &t->token);
	for (;;) {
		unsigned int nl, len, off, back, raw;
		u32 e;

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
		 * without loops. The last sequence never takes it: its literals
		 * end at the end of the page, too close.
		 */
		if (!(tok & (1U << TOK_VALUE_AT)) && d <= d_fast &&
		    (unsigned long)lit <= lit_fast &&
		    (unsigned long)br.p <= in_fast) {
			tok = next_token_fast(&br, &t->token, &skip);
			/*
			 * No check of nl against the literals left, which saved
			 * 3 instructions per sequence. The 16 bytes read are
			 * inside lit_bound, so lit stays inside it. If nl goes
			 * past lit_end, bytes of the bitstream were taken as
			 * literals, and the slow path below, where every page
			 * ends, rejects the page because lit > lit_end.
			 */
			copy16(d, lit);
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
			nl += seqlz_br_value(&br, &t->ll, &e);
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
			if (len != 4U || TOK_BACK(tok) != 0U)
				return -EINVAL;
			break;
		}

		if (len == SEQLZ_ML_CAP + 4U) {
			seqlz_br_refill(&br);
			len += seqlz_br_value(&br, &t->ml, &e);
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
	return decode_page(t, src, src_len, dst, scratch);
}
