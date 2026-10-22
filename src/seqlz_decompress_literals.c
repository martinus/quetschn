// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The decompressor's literals: the 8 streams of coded literals, with one of
 * the fixed literal tables or with the block's own table, which it reads first.
 */
#include "seqlz_internal.h"

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
 * the 8 bytes at ip, zeros after end; only the last refills of a block need it,
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
 * seqlz_decode_literals().
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
 * is 0 the literal is decoded but not taken, see the end of
 * seqlz_decode_literals().
 */
static __always_inline u64 lit_decode(u8 *out, u64 bits, const u16 *lt,
				      unsigned int mask)
{
	unsigned int e = lt[bits >> (64U - SEQLZ_LIT_BITS)];

	*out = (u8)(e >> 8);
	return bits << (e & mask);
}

/*
 * The sizes of the 8 streams of coded literals, width bits each from p, and
 * where each starts, the first at q. Returns their sum. The sizes are copied
 * out of the block first: a 4-byte load at the last size's byte would read past
 * the header.
 */
static __always_inline u64 lit_stream_sizes(const u8 *p, unsigned int width,
					    const u8 *q, unsigned int *sz,
					    const u8 **start)
{
	u8 h[SEQLZ_SIZE_BITS_MAX + 4U] = { 0 };
	u64 total = 0;
	unsigned int k;

	__builtin_memcpy(h, p, width);
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
 * block, not from b: when the stream's last 8 bytes are in it, its last bit is
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

/* the most bits one step of own_table() takes: a run's code and 8 bits more */
#define OWN_STEP_MAX (2U * SEQLZ_OWN_CODE_BITS)
static_assert(((OWN_QUAD_NONE + 1U) * 256U <= SEQLZ_SCRATCH &&
	       (sizeof(u16) << SEQLZ_LIT_BITS) + 1U <= SEQLZ_BLOCK_SIZE),
	      "own_table()'s buckets fit the scratch, its table the block");

/*
 * A block's own literal table, see SEQLZ_LIT_OWN: reads it from p and writes
 * its decode table to lt, 1 << SEQLZ_LIT_BITS entries as those of the fixed
 * tables. Returns where the table ends, or NULL if it is not valid: codes that
 * overfill the code space, more than 256 bytes, bits that are not zero after
 * its last symbol, or fewer bytes than it needs.
 *
 * The bytes go into one bucket per code length, by value, in bucket: 256 bytes
 * per length from 1 to OWN_QUAD_NONE, the last for none. Then the entries are
 * written length by length, which is the order of the canonical codes. Each
 * step decodes up to two lengths with one lookup of own->quad[]; the checks of
 * the bytes wait until the end, nothing reads the table before. Each step moves
 * on by at least one code, so the loop ends for any input: past its end the
 * block reads as zeros, and a code of zeros is a length. Not inlined: in
 * seqlz_decode_literals() its locals made the stack frame larger than the
 * kernel allows with KASAN.
 */
static noinline const u8 *own_table(const struct own_table *own, const u8 *p,
				    const u8 *end, u16 *lt, u8 *bucket)
{
	const unsigned int full = 1U << SEQLZ_LIT_BITS,
			   len = (unsigned int)(end - p);
	unsigned int at = 0, avail = 0, space = 0, b = 0, l, k, used,
		     count[OWN_QUAD_NONE + 1] = { 0 };
	u64 bits = 0;
	u16 *o = lt;

	while (space < full) {
		unsigned int top, e, l1, l2, next, two;

		if (avail < OWN_STEP_MAX) {
			/* whole bytes, as many as fit below 64 bits */
			unsigned int n = (63U - avail) >> 3;
			u64 w = at + 8U <= len ?
					get_unaligned_be64(p + at) :
					lit_load_tail(p + min(at, len), end);

			bits = (bits | w >> avail) &
			       ~(~0ULL >> (avail + 8U * n));
			at += n;
			avail += 8U * n;
		}
		top = (unsigned int)(bits >> (64U - SEQLZ_OWN_CODE_BITS));
		e = own->quad[top];
		if (e >> OWN_QUAD_ALONE_AT) {
			/*
			 * a run alone: its code, then nb - 1 bits below its top
			 */
			unsigned int d = own->decode[top],
				     nb = (d & 0xffU) - SEQLZ_OWN_RUN;

			bits <<= d >> 8;
			b += (1U << (nb - 1U)) +
			     (unsigned int)((bits >> 1) >> (64U - nb));
			bits <<= nb - 1U;
			avail -= (d >> 8) + nb - 1U;
			continue;
		}
		l1 = e & 15U;
		b += e >> OWN_QUAD_RUN1_AT & OWN_QUAD_RUN_MAX;
		next = space + (full >> l1);
		/* the second length only while the first leaves room */
		two = next < full;
		l2 = two ? e >> 4 & 15U : OWN_QUAD_NONE;
		k = e >> (two ? OWN_QUAD_END2_AT : OWN_QUAD_END1_AT) & 15U;
		bits <<= k;
		avail -= k;
		bucket[l1 * 256U + (count[l1]++ & 255U)] = (u8)b;
		b += 1U + (two ? e >> OWN_QUAD_RUN2_AT & OWN_QUAD_RUN_MAX : 0U);
		bucket[l2 * 256U + (count[l2]++ & 255U)] = (u8)b;
		space = next + (full >> l2);
		b += l2 != OWN_QUAD_NONE;
	}
	used = 8U * at - avail;
	if (space != full || b > 256U || (used + 7U) / 8U > len ||
	    ((used & 7U) && bits >> (64U - (8U - (used & 7U)))))
		return NULL;
	/*
	 * Each byte's entries, all the same, so the order of their bytes does
	 * not matter: 8 bytes at a time down to codes of 8 bits, which have 4
	 * entries; codes of 9 bits have 2, codes of 10 one. A loop per length
	 * without a branch per byte read faster than one loop for all.
	 */
	for (l = 1; l <= SEQLZ_LIT_BITS - 2U; l++) {
		const unsigned int run = full >> l;
		const u8 *bk = bucket + l * 256U;

		for (k = 0; k < count[l]; k++) {
			u64 v = (u64)(l | (unsigned int)bk[k] << 8) *
				0x0001000100010001ULL;
			u16 *q;

			for (q = o; q < o + run; q += 4)
				__builtin_memcpy(q, &v, 8);
			o += run;
		}
	}
	for (k = 0; k < count[SEQLZ_LIT_BITS - 1U]; k++) {
		u32 v = ((SEQLZ_LIT_BITS - 1U) |
			 (u32)bucket[(SEQLZ_LIT_BITS - 1U) * 256U + k] << 8) *
			0x00010001U;

		__builtin_memcpy(o, &v, 4);
		o += 2;
	}
	for (k = 0; k < count[SEQLZ_LIT_BITS]; k++)
		*o++ = (u16)(SEQLZ_LIT_BITS |
			     (unsigned int)bucket[SEQLZ_LIT_BITS * 256U + k]
				     << 8);
	return p + (used + 7U) / 8U;
}

/*
 * Decodes the coded literals of a block into out, the scratch, see
 * SEQLZ_LIT_HEADER for the layout, and their delta into *delta, 0 for none.
 * Returns where the sequences' bitstream starts, or NULL if the block is not
 * valid. Not inlined: inside the loop over the sequences its registers made
 * blocks with raw literals slower too.
 */
noinline const u8 *seqlz_decode_literals(const u8 *s, unsigned int src_len,
					 unsigned int n_lit, u8 *out, u8 *block,
					 unsigned int *delta)
{
	const u8 *const end = s + src_len;
	/*
	 * byte 2: the literals' code in bits 0 to 3,
	 * width - SEQLZ_SIZE_BITS_MIN in bits 4 to 6, in bit 7 whether they are
	 * deltas, then their byte
	 */
	const unsigned int width =
		SEQLZ_SIZE_BITS_MIN +
		(src_len > 2U ?
			 (s[2] >> SEQLZ_LIT_WIDTH_AT) &
				 (SEQLZ_SIZE_BITS_MAX - SEQLZ_SIZE_BITS_MIN) :
			 0U);
	const unsigned int deltas = src_len > 2U ? s[2] >> SEQLZ_LIT_DELTA_AT :
						   0U;
	const u8 *q = s + SEQLZ_LIT_HEADER(width) + deltas;
	const u8 *ip[SEQLZ_LIT_STREAMS];
	const u8 *start[SEQLZ_LIT_STREAMS];
	unsigned int sz[SEQLZ_LIT_STREAMS];
	u64 b[SEQLZ_LIT_STREAMS];
	unsigned int i, k, j, rest, steps, r;
	u64 total = 0;
	const u16 *lt;

	if (src_len < SEQLZ_LIT_HEADER(width) + deltas ||
	    (s[2] & SEQLZ_LIT_CODE_MASK) > SEQLZ_LIT_OWN ||
	    n_lit > SEQLZ_BLOCK_SIZE)
		return NULL;
	/* the delta, SEQLZ_DELTA_4 to SEQLZ_DELTA_LAST, in byte 3 */
	*delta = deltas ? s[SEQLZ_LIT_HEADER(0)] : 0U;
	if (deltas && *delta - 1U >= SEQLZ_DELTA_LAST)
		return NULL;
	if ((s[2] & SEQLZ_LIT_CODE_MASK) == SEQLZ_LIT_OWN) {
		/*
		 * the block's own table, built in the block itself, from an
		 * even address: the caller's block may start at an odd one
		 */
		u16 *own = (u16 *)(block + ((unsigned long)block & 1U));

		q = own_table(&seqlz_tables.own, q, end, own, out);
		if (!q)
			return NULL;
		lt = own;
	} else {
		lt = seqlz_tables.lit[s[2] & (SEQLZ_LIT_SETS - 1U)].decode;
		prefetch_lines(lt, sizeof(seqlz_tables.lit[0].decode));
	}
	total = lit_stream_sizes(s + SEQLZ_LIT_HEADER(0) + deltas, width, q, sz,
				 start);
	for (i = 0; i < SEQLZ_LIT_STREAMS; i++) {
		ip[i] = start[i];
		b[i] = 1;
	}
	if ((u64)(q - s) + total > src_len)
		return NULL;
	/*
	 * Full rounds of SEQLZ_LIT_ROUNDS literals per stream, all 8 streams
	 * side by side. The rest below.
	 */
	for (k = 0; k + SEQLZ_LIT_STREAMS * SEQLZ_LIT_ROUNDS <= n_lit;
	     k += SEQLZ_LIT_STREAMS * SEQLZ_LIT_ROUNDS) {
		for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
			b[i] = lit_refill(&ip[i], b[i], end);
		for (j = 0; j < SEQLZ_LIT_ROUNDS; j++)
			for (i = 0; i < SEQLZ_LIT_STREAMS; i++) {
				unsigned int at = k + SEQLZ_LIT_STREAMS * j + i;

				b[i] = lit_decode(&out[at], b[i], lt, 63U);
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
	 * after a different number of literals on every block, and
	 * mispredict.
	 */
	rest = n_lit - k;
	steps = rest / SEQLZ_LIT_STREAMS;
	r = rest % SEQLZ_LIT_STREAMS;
	for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
		b[i] = lit_refill(&ip[i], b[i], end);
	for (j = 0; j < steps; j++)
		for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
			b[i] = lit_decode(&out[k + SEQLZ_LIT_STREAMS * j + i],
					  b[i], lt, 63U);
	k += SEQLZ_LIT_STREAMS * steps;
	for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
		b[i] = lit_decode(&out[k + i], b[i], lt, (0U - (r > i)) & 63U);
	for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
		if (!lit_stream_ends(start[i], sz[i], ip[i], b[i]))
			return NULL;
	return q + total;
}
