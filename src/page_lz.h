/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_PAGE_LZ_H
#define SEQLZ_PAGE_LZ_H

/*
 * The part of seqlz that is plain LZ: the matcher, which finds the sequences
 * of a page. All of it is static inline, so that it ends up inside the loops of
 * seqlz.c.
 */

#include "seqlz.h"

/*
 * Prefetches every cache line of [p, p + size); size is a multiple of
 * PREFETCH_STEP. In a swap-in the decoder's tables are often no longer in
 * the cache: asking for all of their lines at once lets the misses overlap,
 * instead of one after the other as the decoder runs into them. 512 bytes per
 * iteration, 8 lines of 64 bytes: with one line per iteration, the loop took
 * more instructions than the prefetches.
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

/*
 * A hash of the low 5 bytes of v, as zstd computes it: the shift by 24 drops
 * the other 3, the multiply mixes them, and the top bits are the hash.
 * 889523592379 is zstd's prime5bytes.
 */
static inline unsigned int seqlz_hash5(u64 v)
{
	return (unsigned int)(((v << 24) * 889523592379ULL) >>
			      (64U - SEQLZ_HASH_BITS));
}

/*
 * How many bytes at p are the same as at q, at most up to end: the length of a
 * match, q is where it copies from.
 */
static inline unsigned int seqlz_match_len(const u8 *p, const u8 *q,
					   const u8 *end)
{
	const u8 *start = p;

	/*
	 * The first 16 bytes are compared without a branch between the two
	 * halves of 8: whether a match ends in the first 8 bytes is hard to
	 * predict, most do and many are longer. x is the first half that
	 * differs, and its lowest set bit is in the first byte that differs,
	 * the bytes are little endian. x is never 0 here, the top bit set
	 * tells the compiler so: __builtin_ctzll() of 0 is undefined.
	 */
	if (end - p >= 16) {
		u64 x1 = get_unaligned_le64(p) ^ get_unaligned_le64(q),
		    x2 = get_unaligned_le64(p + 8) ^ get_unaligned_le64(q + 8);
		u64 x = x1 ? x1 : x2;

		if (x | x2)
			return (x1 ? 0U : 8U) +
			       ((unsigned int)__builtin_ctzll(x | 1ULL << 63) >>
				3);
		p += 16;
		q += 16;
	}
	while (end - p >= 8) {
		u64 x = get_unaligned_le64(p) ^ get_unaligned_le64(q);

		if (x)
			return (unsigned int)(p - start) +
			       ((unsigned int)__builtin_ctzll(x) >> 3);
		p += 8;
		q += 8;
	}
	while (p < end && *p == *q) {
		p++;
		q++;
	}
	return (unsigned int)(p - start);
}

/*
 * The matcher calls this for every sequence it finds, with the literals before
 * the match, the match length (0 for the last sequence) and the offset.
 */
typedef void (*emit_fn)(void *ctx, const u8 *literals, unsigned int ll,
			unsigned int ml, unsigned int off);

/*
 * Finds the sequences of a page, greedy like lz4's fast mode: at each position
 * it tries two places where the next 4 bytes might have been before, and takes
 * the first match it finds. One is the offset of the match before, which
 * repeats often. The other is the last position whose 5 bytes had the same
 * hash, from the hash table. Hashing 5 bytes instead of 4 finds fewer and
 * longer matches: less time to compress for a little more memory. Matches of 4
 * bytes still come from the repeated offset.
 *
 * Unlike lz4 it tries every position, also after a long stretch without a
 * match, where lz4 starts to skip. Skipping needed more state in the loop:
 * without it, pages that compress are written faster and pages that do not
 * slower ("The matcher without its step").
 *
 * Each sequence goes to emit() as soon as it is found. With the encoder's
 * emit() inlined, finding and writing the sequences is one pass over the page.
 */
static __always_inline void seqlz_match_page(u16 *table, const u8 *src,
					     emit_fn emit, void *ctx)
{
	/*
	 * Positions in the page, not pointers: the limit is a constant, and the
	 * table stores positions. The last 8 bytes are not tried, every try
	 * reads 8 bytes.
	 */
	const unsigned int limit = SEQLZ_PAGE - 8U;
	unsigned int pos = 1, anchor = 0, last = 1, h, cand;
	/*
	 * The repeated offset as a negative index, src + pos + back, so the
	 * load needs no subtraction of its own.
	 */
	long back = -1;
	/*
	 * The 8 bytes at pos, their hash, the table's entry and the 4 bytes
	 * at that entry, each loaded one position ahead. Each of them needs
	 * the one before, and an in-order core waited for every load. The
	 * entry is still read after the position before it was stored, so the
	 * matches are the same.
	 */
	u64 v;
	u32 cand_bytes;

	memset(table, 0, sizeof(u16) << SEQLZ_HASH_BITS);
	v = get_unaligned_le64(src + pos);
	h = seqlz_hash5(v);
	cand = table[h];
	cand_bytes = get_unaligned_le32(src + cand);

	while (pos < limit) {
		u64 v_next = get_unaligned_le64(src + pos + 1);
		unsigned int h_next = seqlz_hash5(v_next), m, len;
		/* the first 4 of those 8 bytes, without another load */
		u32 cur = (u32)v;
		/*
		 * Both candidates are read before either is tested, and one
		 * branch tests both: with a branch for each, the matcher
		 * mispredicted almost twice as often as lz4. Reading both is
		 * always safe. The repeated offset starts at 1 and the search
		 * at position 1, and the table is cleared for each page, so
		 * both point into the page, before pos.
		 */
		unsigned int rep_hit = get_unaligned_le32(src + pos + back) ==
				       cur;
		unsigned int cand_hit = cand_bytes == cur;

		table[h] = (u16)pos;
		if (!(rep_hit | cand_hit)) {
			pos++;
			v = v_next;
			h = h_next;
			cand = table[h];
			cand_bytes = get_unaligned_le32(src + cand);
			continue;
		}
		m = rep_hit ? pos - last : cand;
		/*
		 * The match may have started before pos: extend it backwards
		 * into the literals, then count how far it goes forwards.
		 */
		while (pos > anchor && m > 0 && src[pos - 1] == src[m - 1]) {
			pos--;
			m--;
		}
		len = 4U + seqlz_match_len(src + pos + 4, src + m + 4,
					   src + SEQLZ_PAGE);
		last = pos - m;
		back = -(long)last;
		emit(ctx, src + anchor, pos - anchor, len, last);
		pos += len;
		anchor = pos;
		/*
		 * The positions inside the match were skipped. One of them, 2
		 * bytes before its end as lz4's fast mode does, goes into the
		 * table, so later matches can find it.
		 */
		if (pos < limit) {
			table[seqlz_hash5(get_unaligned_le64(src + pos - 2))] =
				(u16)(pos - 2);
			v = get_unaligned_le64(src + pos);
			h = seqlz_hash5(v);
			cand = table[h];
			cand_bytes = get_unaligned_le32(src + cand);
		}
	}
	emit(ctx, src + anchor, SEQLZ_PAGE - anchor, 0, 0);
}

#endif
