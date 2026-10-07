/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_PAGE_LZ_H
#define SEQLZ_PAGE_LZ_H

/*
 * The parts of seqlz that are plain LZ: the matcher, which finds the sequences
 * of a page, and the decoder's copies of literals and matches. All of it is
 * static inline, so that it ends up inside the loops of seqlz.c.
 */

#include "seqlz_compat.h"

/* 12 for 4 KiB pages, 14 for 16 KiB, the same for the whole build */
#ifndef QUETSCHN_PAGE_BITS
#define QUETSCHN_PAGE_BITS 12
#endif
#define PAGE_LZ_PAGE (1U << QUETSCHN_PAGE_BITS)
/*
 * The matcher's hash table has 1 << PAGE_LZ_HASH_BITS entries of 2 bytes, 8 KiB
 * for 4 KiB pages. With 2048 entries pages were 2.7 bytes larger and the
 * compressor no faster; with 8192 the A55 wrote a page 1.6 us slower ("Six
 * choices made on the PC, measured on the phone", 5). For 16 KiB pages it is
 * 16 KiB, as lz4's table.
 */
#define PAGE_LZ_HASH_BITS (QUETSCHN_PAGE_BITS == 12 ? 12U : 13U)

/*
 * Asks the CPU to load a cache line, without waiting for it. The kernel builds
 * x86-64 without SSE, and then clang drops __builtin_prefetch(), and the
 * kernel's prefetch() with it; gcc keeps it. Every x86-64 CPU has prefetcht0,
 * so here it is the instruction itself.
 */
#if defined(__x86_64__) && !defined(__SSE__)
#define PAGE_LZ_PREFETCH(p) __asm__("prefetcht0 %0" : : "m"(*(const char *)(p)))
#else
#define PAGE_LZ_PREFETCH(p) __builtin_prefetch(p)
#endif

/*
 * Prefetches every cache line of [p, p + size); size is a multiple of 512. In
 * a swap-in the decoder's tables are often no longer in the cache: asking for
 * all of their lines at once lets the misses overlap, instead of one after the
 * other as the decoder runs into them. 8 lines of 64 bytes per iteration: one
 * line per iteration took 4 instructions per line, 3% of a page's decode on
 * the Cortex-A55.
 */
static inline void prefetch_lines(const void *p, unsigned long size)
{
	const u8 *q = p;
	const u8 *const end = q + size;

	for (; q < end; q += 512) {
		PAGE_LZ_PREFETCH(q);
		PAGE_LZ_PREFETCH(q + 64);
		PAGE_LZ_PREFETCH(q + 128);
		PAGE_LZ_PREFETCH(q + 192);
		PAGE_LZ_PREFETCH(q + 256);
		PAGE_LZ_PREFETCH(q + 320);
		PAGE_LZ_PREFETCH(q + 384);
		PAGE_LZ_PREFETCH(q + 448);
	}
}

/*
 * The u16 at the start of a page, byte by byte. get_unaligned_le16() and
 * put_unaligned_le16() would do the same, but with them clang compiled
 * seqlz_decode() and code_literals() with other registers, and in the kernel VM
 * a page was read 0.04 us and written 0.08 us slower.
 */
static inline void store16(u8 *p, unsigned int v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
}

static inline unsigned int load16(const u8 *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/*
 * A hash of the low 5 bytes of v, as zstd computes it: the shift by 24 drops
 * the other 3, the multiply mixes them, and the top bits are the hash.
 * 889523592379 is zstd's prime5bytes.
 */
static inline unsigned int hash5(u64 v)
{
	return (unsigned int)(((v << 24) * 889523592379ULL) >>
			      (64U - PAGE_LZ_HASH_BITS));
}

/*
 * How many bytes at p are the same as at q, at most up to end: the length of a
 * match, q is where it copies from.
 */
static inline unsigned int count(const u8 *p, const u8 *q, const u8 *end)
{
	const u8 *start = p;

	/*
	 * The first 16 bytes are compared without a branch between the two
	 * halves of 8: whether a match ends in the first 8 bytes is hard to
	 * predict, 74% do and 24% are longer than 11 bytes. x is the first half
	 * that differs, and its lowest set bit is in the first byte that
	 * differs, the bytes are little endian. x is never 0 here, the top bit
	 * set tells the compiler so: __builtin_ctzll() of 0 is undefined.
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
 * longer matches: 5% fewer cycles to compress, 0.6% more memory, and 10% less
 * time per write at p99 in the kernel. Matches of 4 bytes still come from the
 * repeated offset.
 *
 * Unlike lz4 it tries every position, also after a long stretch without a
 * match, where lz4 starts to skip. Skipping needed more state in the loop:
 * without it, pages that compress were written 5% faster and pages that do not
 * 2.3 us slower ("The matcher without its step").
 *
 * Each sequence goes to emit() as soon as it is found. With the encoder's
 * emit() inlined, finding and writing the sequences is one pass over the page.
 */
static __always_inline void match_page(u16 *table, const u8 *src, emit_fn emit,
				       void *ctx)
{
	/*
	 * Positions in the page, not pointers: the limit is a constant, and the
	 * table stores positions. The last 8 bytes are not tried, every try
	 * reads 8 bytes.
	 */
	const unsigned int limit = PAGE_LZ_PAGE - 8U;
	unsigned int pos = 1, anchor = 0, last = 1, h, cand;
	/*
	 * The repeated offset as a negative index, src + pos + back, so the
	 * load needs no subtraction of its own.
	 */
	long back = -1;
	/*
	 * The 8 bytes at pos, their hash, the table's entry and the 4 bytes
	 * at that entry, each loaded one position ahead. Each of them needs
	 * the one before, and the in-order Cortex-A55 waited for every load,
	 * 3.4% of the compressor's cycles. The entry is still read after the
	 * position before it was stored, so the matches are the same.
	 */
	u64 v;
	u32 cand_bytes;

	memset(table, 0, sizeof(u16) << PAGE_LZ_HASH_BITS);
	v = get_unaligned_le64(src + pos);
	h = hash5(v);
	cand = table[h];
	cand_bytes = get_unaligned_le32(src + cand);

	while (pos < limit) {
		u64 v_next = get_unaligned_le64(src + pos + 1);
		unsigned int h_next = hash5(v_next), m, len;
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
		len = 4U +
		      count(src + pos + 4, src + m + 4, src + PAGE_LZ_PAGE);
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
			table[hash5(get_unaligned_le64(src + pos - 2))] =
				(u16)(pos - 2);
			v = get_unaligned_le64(src + pos);
			h = hash5(v);
			cand = table[h];
			cand_bytes = get_unaligned_le32(src + cand);
		}
	}
	emit(ctx, src + anchor, PAGE_LZ_PAGE - anchor, 0, 0);
}

/*
 * The copies below are __builtin_memcpy() of 8 bytes, as lib/lz4's
 * LZ4_memcpy(): with CONFIG_FORTIFY_SOURCE, clang did not inline the kernel's
 * memcpy() in the Mi 9T's 4.14, and every copy of 8 bytes was a call.
 */

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
		u64 a, b;

		__builtin_memcpy(&a, lit, 8);
		__builtin_memcpy(&b, lit + 8, 8);
		__builtin_memcpy(d, &a, 8);
		__builtin_memcpy(d + 8, &b, 8);
		k = 16;
	}
	while (k < nl && (unsigned int)(d_end - d) >= k + 16U &&
	       (unsigned int)(s_end - lit) >= k + 16U) {
		u64 a, b;

		__builtin_memcpy(&a, lit + k, 8);
		__builtin_memcpy(&b, lit + k + 8, 8);
		__builtin_memcpy(d + k, &a, 8);
		__builtin_memcpy(d + k + 8, &b, 8);
		k += 16;
	}
	for (; k < nl; k++)
		d[k] = lit[k];
}

/*
 * For an offset below 8, the largest multiple of it up to 8: how far a copy of
 * 8 bytes of a repeated pattern can go on, see copy_match().
 */
static const u8 page_lz_step_for[8] = { 0, 8, 8, 6, 8, 5, 6, 7 };

/*
 * Copies a match of len bytes that starts off bytes before d; the caller has
 * checked that it fits and that off points into the page. Like the literals, 8
 * bytes at a time as long as 8 bytes fit in the page, also past len; only the
 * last 7 bytes of a page are copied one by one. Copying byte by byte up to the
 * end of the page made the slowest pages 10 times slower than lz4.
 *
 * An offset below 8 overlaps the bytes it writes: off = 1 repeats one byte,
 * off = 2 two bytes, and so on. Then the off bytes before d are repeated to 8
 * bytes in a register, with shifts, and stored. The same 8 bytes fit again
 * every step bytes, a multiple of off, so the rest are stores of that register
 * only. Writing the first bytes one by one made each load wait for the stores
 * before it, and the loop over them mispredicted its end.
 */
static __always_inline void copy_match(u8 *d, const u8 *d_end, unsigned int off,
				       unsigned int len)
{
	unsigned int step = off >= 8 ? 8U : page_lz_step_for[off & 7U],
		     back = off >= 8 ? off : step, k = 0;

	if (off < 8) {
		if ((unsigned int)(d_end - d) >= 8U) {
			u64 w, pat;
			unsigned int bits = 8U * off;

			/*
			 * reads d - off up to d - off + 7, which is below
			 * d + 8 <= d_end: inside the page
			 */
			w = get_unaligned_le64(d - off);
			pat = w & ((1ULL << bits) - 1ULL);
			pat |= pat << bits;
			pat |= (pat << ((2U * bits) & 63U)) &
			       (0ULL - (u64)(2U * bits < 64U));
			pat |= (pat << ((4U * bits) & 63U)) &
			       (0ULL - (u64)(4U * bits < 64U));
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
		 * 79% of the matches are at most 16 bytes, so 16 bytes are
		 * copied without a loop. With off >= 8 the second copy reads
		 * bytes the first one wrote or that were there before, so the
		 * order is right. 32 bytes, enough for 91% of the matches, made
		 * the median page faster and the slowest ones slower: those
		 * have many short matches, and copied 32 bytes for each.
		 */
		u64 a, b;

		__builtin_memcpy(&a, d - off, 8);
		__builtin_memcpy(d, &a, 8);
		__builtin_memcpy(&b, d + 8 - off, 8);
		__builtin_memcpy(d + 8, &b, 8);
		k = 16;
	}
	if (back != 0) {
		while (k < len && (unsigned int)(d_end - d) >= k + 8U) {
			u64 w;

			__builtin_memcpy(&w, d + k - back, 8);
			__builtin_memcpy(d + k, &w, 8);
			k += step;
		}
	}
	for (; k < len; k++)
		d[k] = *(d + k - off);
}

#endif
