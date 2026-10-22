// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The compressor's literals: they stay raw, or are coded with one of the
 * fixed literal tables or with a table of the block's own, and at
 * SEQLZ_LEVEL_OWN they can be deltas to earlier bytes of the block first.
 */
#include "seqlz_internal.h"

/*
 * seqlz_delta_literals() prices the deltas with every DELTA_STEP-th literal.
 * Pricing every literal made blocks almost no smaller and the encoder slower.
 * Not every 4th: in fields of 4 or 8 bytes that is the same byte of each field,
 * and blocks were larger.
 */
#define DELTA_STEP 5U

/*
 * The bits of sum j in literal table k, from its lane of 16 bits in even or
 * odd, as lit_cost() sums them: even has tables 0, 2, 4 and 6. Sum j is
 * stream j in code_literals(), delta j in seqlz_delta_literals().
 */
static __always_inline unsigned int lit_bits(const u64 *even, const u64 *odd,
					     unsigned int j, unsigned int k)
{
	return (unsigned int)(((k & 1U ? odd : even)[j] >> (16U * (k / 2))) &
			      0xffffU);
}

/*
 * Adds the 8 lanes of 8 bits of the sum x to the 4 lanes of 16 bits of even
 * and odd: tables 0, 2, 4 and 6 to even, 1, 3, 5 and 7 to odd
 */
static __always_inline void lanes_spill(u64 x, u64 *even, u64 *odd)
{
	/* every second byte */
	const u64 lanes = 0x00ff00ff00ff00ffULL;

	*even += x & lanes;
	*odd += x >> 8 & lanes;
}

/* lanes_spill() of the 4 sums s, which are cleared */
static __always_inline void delta_spill(u64 *s, u64 *even, u64 *odd)
{
	unsigned int x;

	for (x = 0; x < 4; x++) {
		lanes_spill(s[x], &even[x], &odd[x]);
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
 * Picks the delta of the literals from the records before them, see LIT_REC and
 * SEQLZ_LIT_DELTA_AT, and moves the literals down over the records, to right
 * after the u16; *lit_end, their end, moves down with them. With a delta, their
 * deltas are also
 * written to deltas. Returns the delta, 0 for none.
 *
 * Each delta is priced as code_literals() prices the fixed tables, with
 * sums of lit_cost: the delta whose best table takes the fewest bits, with the
 * 8 bits of the byte that says which delta scaled as the sums are,
 * 8 / DELTA_STEP. The bits of the fixed tables also tell which delta suits a
 * table of the block's own.
 */
unsigned int seqlz_delta_literals(u8 *d, u8 **lit_end, const u8 *block,
				  u8 *deltas)
{
	u8 *const end = *lit_end;
	u64 s[4] = { 0 }, even[4] = { 0 }, odd[4] = { 0 };
	unsigned int k, x, j = 0, n = 0, pos, ll, last, best = ~0U, pick = 0;
	u8 *p, *out = d + SEQLZ_HEADER, *to = deltas;

	/*
	 * The sums: each literal's length in all fixed tables, as it is and
	 * with each delta, in the 8-bit lanes of s, LIT_COST_RUN at a time,
	 * then in the 16-bit lanes
	 */
	for (p = d + SEQLZ_HEADER; p < end; p += LIT_REC + ll) {
		pos = get_unaligned_le16(p);
		ll = get_unaligned_le16(p + 2);
		last = seqlz_delta_last(get_unaligned_le16(p + 4));
		for (k = j; k < ll; k += DELTA_STEP) {
			const unsigned int at = pos + k, b = block[at],
					   b4 = at >= 4U ? block[at - 4] : 0U,
					   b8 = at >= 8U ? block[at - 8] : 0U,
					   bl = at >= last ? block[at - last] :
							     0U;

			if (n == LIT_COST_RUN) {
				delta_spill(s, even, odd);
				n = 0;
			}
			s[0] += seqlz_tables.lit_cost[b];
			s[SEQLZ_DELTA_4] += seqlz_tables.lit_cost[(u8)(b - b4)];
			s[SEQLZ_DELTA_8] += seqlz_tables.lit_cost[(u8)(b - b8)];
			s[SEQLZ_DELTA_LAST] +=
				seqlz_tables.lit_cost[(u8)(b - bl)];
			n++;
		}
		/* the next sequence's literals go on with the step */
		j = k - ll;
	}
	delta_spill(s, even, odd);
	for (x = 0; x < 4; x++)
		for (k = 0; k < SEQLZ_LIT_SETS; k++) {
			unsigned int b = lit_bits(even, odd, x, k) +
					 (x ? 8U / DELTA_STEP : 0U);

			if (b < best) {
				best = b;
				pick = x;
			}
		}
	/*
	 * The literals down over the records, and their deltas into deltas: the
	 * ones near the block's start as they are. 8 bytes at a time, also past
	 * the sequence's last literal: what that writes, the next sequence's
	 * literals overwrite, only the first sequence's copy reaches the next
	 * record, its first byte, read before, and after the last literals dst
	 * has ENC_ROOM bytes more, see encode_seq(). memcpy() was a call for
	 * every sequence, and a loop over the last bytes mispredicted.
	 */
	p = d + SEQLZ_HEADER;
	if (p < end) {
		pos = get_unaligned_le16(p);
		ll = get_unaligned_le16(p + 2);
		last = seqlz_delta_last(get_unaligned_le16(p + 4));
	}
	while (p < end) {
		const u8 *in = p + LIT_REC, *pin = block + pos;
		const unsigned int n_lit = ll, at = pos, dist = last;

		p += LIT_REC + n_lit;
		if (p < end) {
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
		x = pick == SEQLZ_DELTA_4 ? 4U :
		    pick == SEQLZ_DELTA_8 ? 8U :
					    dist;
		j = x > at ? x - at : 0;
		for (k = 0; k < j && k < n_lit; k++)
			to[k] = pin[k];
		/*
		 * past the last literal only inside the block;
		 * to - deltas <= at
		 */
		if (at + n_lit + 8 <= SEQLZ_BLOCK_SIZE) {
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
	*lit_end = out;
	return pick;
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
 * starts. The bytes are swapped once, before the branch, not by
 * put_unaligned_be64() in each, which the compiler did not merge.
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

/*
 * Sums the code lengths of each stream's literals in all tables, into the
 * 16-bit lanes of even and odd: even has tables 0, 2, 4 and 6, odd tables 1,
 * 3, 5 and 7. LIT_COST_RUN literals per stream in the 8-bit lanes of x, then
 * into the 16-bit lanes, before an 8-bit lane overflows. Unless hist is NULL,
 * it counts the bytes too, for a table of the block's own: the even literals in
 * hist[0], the odd ones in hist[1], so that a run of one byte does not wait for
 * its own count.
 */
static __always_inline void lit_cost(const u8 *literals,
				     unsigned int n_literals, u64 *even,
				     u64 *odd, u16 (*hist)[256])
{
	unsigned int k, j;

	for (k = 0; k < n_literals;) {
		u64 x[SEQLZ_LIT_STREAMS] = { 0 };
		unsigned int end =
			n_literals - k < SEQLZ_LIT_STREAMS * LIT_COST_RUN ?
				n_literals :
				k + SEQLZ_LIT_STREAMS * LIT_COST_RUN;

		for (; k + SEQLZ_LIT_STREAMS <= end; k += SEQLZ_LIT_STREAMS)
			for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
				x[j] += seqlz_tables.lit_cost[literals[k + j]];
				if (hist)
					hist[j & 1U][literals[k + j]]++;
			}
		for (; k < end; k++) {
			x[k % SEQLZ_LIT_STREAMS] +=
				seqlz_tables.lit_cost[literals[k]];
			if (hist)
				hist[k & 1U][literals[k]]++;
		}
		for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
			lanes_spill(x[j], &even[j], &odd[j]);
	}
}

/*
 * Writes the n_literals literals into the 8 streams that start at q[0] to q[7],
 * q[8] the end of the last. Literal k goes into stream k % 8, most significant
 * bit first, see seqlz_decode_literals(). Four streams at a time, each with an
 * accumulator of its own, so that the four chains of shifts run side by side.
 * One stream after the other made each literal wait for the one before.
 */
static __always_inline void lit_streams(const u32 *enc, const u8 *literals,
					unsigned int n_literals, u8 *const *q)
{
	/* the literals between two flushes, 4 per stream */
	const unsigned int run = 4U * SEQLZ_LIT_STREAMS;
	unsigned int half, k, j;

	for (half = 0; half < SEQLZ_LIT_STREAMS; half += 4) {
		u8 *p0 = q[half], *p1 = q[half + 1], *p2 = q[half + 2],
		   *p3 = q[half + 3];
		u8 *e0 = p1, *e1 = p2, *e2 = p3, *e3 = q[half + 4];
		u64 a0 = 0, a1 = 0, a2 = 0, a3 = 0;
		unsigned int c0 = 0, c1 = 0, c2 = 0, c3 = 0, s0 = 0, s1 = 0,
			     s2 = 0, s3 = 0;

		/*
		 * A run of literals, then a flush: 4 codes of up to 10 bits fit
		 * the accumulator. The loop stops run - 4 literals before the
		 * end, so the rest below has at most 4 per stream.
		 */
		for (k = half; k + run - 4U < n_literals; k += run) {
			for (j = 0; j < run; j += SEQLZ_LIT_STREAMS) {
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
static void lit_sizes(u8 *p, const unsigned int *sizes, unsigned int width)
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
 * A literal table of the block's own, see SEQLZ_LIT_OWN: what the encoder needs
 * for it, in the matcher's state behind the copy of the literals. So a block
 * gets one only if its literals leave OWN_WORK bytes of the state free, which
 * all blocks of 4 KiB do.
 */
#define OWN_WORK 4096U
#define OWN_LITERALS_MAX (sizeof_field(struct seqlz_state, work) - OWN_WORK)
/*
 * The bytes of the description: at most 256 symbols of up to 8 bits, the
 * runs between the bytes with a code take fewer bits than such a byte; a
 * power of 2, own_put() masks its index with it.
 */
#define OWN_DESC_ROOM 512U
/* a block's own table only for blocks with this many literals or more */
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
	/* the table as the block sends it, and 8 bytes of room for own_put() */
	u8 desc[OWN_DESC_ROOM + 8];
};

static_assert((sizeof(struct own_work) <= OWN_WORK &&
	       sizeof_field(struct seqlz_state, work) >= OWN_WORK),
	      "the matcher's state holds the work of a block's own table");

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
	/*
	 * 2^40 / n_literals: freq * 2^l * inv >> 36 is 16 times freq * 2^l / n
	 */
	const u64 inv = div_u64(1ULL << 40, n_literals);
	const unsigned int full = 1U << SEQLZ_LIT_BITS;
	/* a counting sort by gain, 0 to 15, and its sums, one more */
	unsigned int k, space = 0, at[16 + 1] = { 0 };

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
	for (k = 1; k <= 16; k++)
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
 * The block's own code for its literals, from the counts in w->freq: lengths,
 * the encoder's codes, the description in w->desc. Returns the bytes of the
 * description, or 0 if it does not pay: fewer than 2 different bytes, or the
 * literals' entropy, or their bits with the code and the description with the
 * smallest header, say it cannot save OWN_SAVES_MIN bytes against limit, the
 * bytes of the best fixed table with its header.
 */
static unsigned int own_code(struct own_work *w, unsigned int n_literals,
			     unsigned int limit)
{
	/* the low 15 bits of each lane */
	const u64 low = 0x7fff7fff7fff7fffULL;
	unsigned int k, j, n = 0, count[CODE_LENGTHS] = { 0 },
			   next[CODE_LENGTHS], l, cnt = 0, prev, out = 0, h = 0,
			   est, bits = 0;
	u64 acc = 0, present[4] = { 0 };

	/*
	 * The two counts added, 4 bytes at a time in 16-bit lanes, which the
	 * sums fit. Each sum that is not 0 sets a bit in present, bit k for
	 * byte k: the top bit of a lane of c | (c & 0x7fff) + 0x7fff is set
	 * where the lane is not 0, and the multiply moves the 4 top bits, at 0,
	 * 16, 32 and 48 after the shift by 15, to bits 48 to 51 without
	 * carries.
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
	 * Only where the entropy and a description of about 0.45 bytes, 115 /
	 * 256, per byte with a code leave the code at least OWN_SAVES_MIN
	 * bytes smaller than limit, and small enough not to stay raw, see
	 * code_literals(), is the code built. On many blocks it is not.
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
	for (l = 1, next[0] = 0; l < CODE_LENGTHS; l++)
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

			e = seqlz_tables.own.enc[SEQLZ_OWN_RUN + nb];
			own_put(w, &acc, &cnt, &out,
				(e & 0xffU) << (nb - 1U) |
					(run & ((1U << (nb - 1U)) - 1U)),
				(e >> 8) + nb - 1U);
		}
		e = seqlz_tables.own.enc[(w->len[b] - 1U) & 15U];
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
 * The bytes of each stream with the block's own code, in w->sizes, and their
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
static unsigned int size_width(const unsigned int *sizes)
{
	unsigned int all = 1U << (SEQLZ_SIZE_BITS_MIN - 1U), j;

	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		all |= sizes[j];
	return 32U - (unsigned int)__builtin_clz(all);
}

/*
 * Turns the block of len bytes in d, with raw literals as the encoder wrote it,
 * into one with Huffman coded literals, where that saves more than 1/16 of them
 * plus SEQLZ_LIT_CODED_MIN bytes. Decoding coded literals costs time for every
 * byte, and coding them wherever they save anything at all saved almost no
 * more memory. Returns the new length, or len if the literals stay raw.
 *
 * The code is the fixed table that codes them in the fewest bits, or, with own
 * not NULL, a table of the block's own, where that and its description take at
 * least OWN_SAVES_MIN bytes less.
 *
 * The coded literals are written over the raw ones in d, so they are read from
 * a copy: raw if the caller has one, else spare, at least a block, which they
 * are copied into once it is clear they get coded. The bitstream stays behind
 * the raw literals until it moves down behind the coded ones at the end. So d
 * needs no room beyond len, and the block is coded in any dst it fits into raw.
 *
 * With delta not 0, raw has the literals' deltas, see SEQLZ_LIT_DELTA_AT,
 * which are coded instead, and the header says so; if they stay raw, d keeps
 * them as they are.
 */
static unsigned int code_literals(u8 *d, unsigned int len, const u8 *raw,
				  u8 *spare, struct own_work *own,
				  unsigned int delta)
{
	const unsigned int n_literals = get_unaligned_le16(d),
			   bitstream = len - SEQLZ_HEADER - n_literals;
	const u8 *literals = raw ? raw : d + SEQLZ_HEADER;
	/* with a delta, its byte before the sizes */
	const unsigned int delta_byte = delta != 0;
	unsigned int bits = ~0U, k, j, coded, set = 0, sizes[SEQLZ_LIT_STREAMS],
		     width, header, desc = 0;
	const u32 *enc;
	/* the bits of each stream in each table, see lit_cost() */
	u64 even[SEQLZ_LIT_STREAMS] = { 0 }, odd[SEQLZ_LIT_STREAMS] = { 0 };
	/* where each stream starts, and the end of the last */
	u8 *q[SEQLZ_LIT_STREAMS + 1];

	if (own &&
	    (n_literals < OWN_LITERALS_MIN || n_literals > OWN_LITERALS_MAX))
		own = NULL;
	if (own)
		memset(own->freq, 0, sizeof(own->freq));
	lit_cost(literals, n_literals, even, odd, own ? own->freq : NULL);
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
	header = SEQLZ_LIT_HEADER(width) + delta_byte;
	enc = seqlz_tables.lit[set].enc;
	if (own) {
		unsigned int n = own_code(own, n_literals, header + coded),
			     own_coded, own_width;

		if (n) {
			own_coded = own_sizes(own, literals, n_literals);
			own_width = size_width(own->sizes);
			if (SEQLZ_LIT_HEADER(own_width) + delta_byte + n +
				    own_coded + OWN_SAVES_MIN <=
			    header + coded) {
				desc = n;
				width = own_width;
				header = SEQLZ_LIT_HEADER(width) + delta_byte +
					 desc;
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
	 * byte 2: the literals' code in bits 0 to 3,
	 * width - SEQLZ_SIZE_BITS_MIN in bits 4 to 6, in bit 7 whether they are
	 * deltas
	 */
	d[2] = (u8)((desc ? SEQLZ_LIT_OWN : set) |
		    (width - SEQLZ_SIZE_BITS_MIN) << SEQLZ_LIT_WIDTH_AT |
		    delta_byte << SEQLZ_LIT_DELTA_AT);
	if (delta)
		d[SEQLZ_LIT_HEADER(0)] = (u8)delta;
	lit_sizes(d + SEQLZ_LIT_HEADER(0) + delta_byte, sizes, width);
	if (desc)
		memcpy(d + SEQLZ_LIT_HEADER(width) + delta_byte, own->desc,
		       desc);
	/* the bitstream, down to right after the coded literals */
	coded += header;
	memmove(d + coded, d + SEQLZ_HEADER + n_literals, bitstream);
	return coded + bitstream;
}

static_assert(SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MAX) + 1U <=
		      SEQLZ_HEADER + SEQLZ_LIT_CODED_MIN,
	      "the coded literals fit before the bitstream");
static_assert(sizeof_field(struct seqlz_state, table) >= SEQLZ_BLOCK_SIZE,
	      "the raw literals take no memory beyond the matcher's table");

/* behind the literals in the matcher's state, see OWN_WORK */
static struct own_work *own_work(struct seqlz_state *st)
{
	return (struct own_work *)(st->work + OWN_LITERALS_MAX / 8U);
}

/*
 * The literals at SEQLZ_LEVEL_OWN: the literals' deltas, as
 * seqlz_delta_literals() left them in the matcher's state, unless that is none
 * or they would stay raw; else as they are.
 */
unsigned int seqlz_code_own(u8 *d, unsigned int len, struct seqlz_state *st,
			    unsigned int delta)
{
	if (delta) {
		unsigned int n = code_literals(d, len, st->literals, NULL,
					       own_work(st), delta);

		if (n != len)
			return n;
	}
	return code_literals(d, len, NULL, st->literals, own_work(st), 0);
}

/*
 * The literals with the fixed tables only, below SEQLZ_LEVEL_OWN. Two entry
 * points, so that code_literals() is compiled for the arguments of each.
 */
unsigned int seqlz_code_literals(u8 *d, unsigned int len, const u8 *raw,
				 u8 *spare)
{
	return code_literals(d, len, raw, spare, NULL, 0);
}
