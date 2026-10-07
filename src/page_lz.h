/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef SEQLZ_PAGE_LZ_H
#define SEQLZ_PAGE_LZ_H

/*
 * seqlz's matcher, seqlz_find, and the literal and match copies of its decoder.
 * Everything static inline, so that it is inlined into the loops of seqlz.c.
 */

#include "seqlz_compat.h"

/*
 * 12 for 4 KiB pages, 14 for 16 KiB, set for the whole build (CMake's
 * QUETSCHN_PAGE_BITS)
 */
#ifndef QUETSCHN_PAGE_BITS
#define QUETSCHN_PAGE_BITS 12
#endif
#define PAGE_LZ_PAGE (1U << QUETSCHN_PAGE_BITS)
/*
 * The matcher's table has 1 << PAGE_LZ_HASH_BITS entries of 2 bytes: 8 KiB for
 * 4 KiB pages, where 2048 slots were 2.7 bytes per page larger and no faster,
 * and 8192 cost the A55 1.6 us per write ("Six choices made on the PC, measured
 * on the phone", 5); 16 KiB for 16 KiB pages, lz4's size.
 */
#define PAGE_LZ_HASH_BITS (QUETSCHN_PAGE_BITS == 12 ? 12U : 13U)

/*
 * Prefetch for reading. Without SSE, as the kernel builds x86-64, clang drops
 * __builtin_prefetch and the kernel's prefetch() with it, gcc does not. Every
 * x86-64 CPU has prefetcht0.
 */
#if defined(__x86_64__) && !defined(__SSE__)
#define PAGE_LZ_PREFETCH(p) __asm__("prefetcht0 %0" : : "m"(*(const char *)(p)))
#else
#define PAGE_LZ_PREFETCH(p) __builtin_prefetch(p)
#endif

/*
 * Every cache line of [p, p + size), size a multiple of 512: 8 lines of 64
 * bytes per iteration, the line size of x86-64, the Cortex-A55 and the A76. A
 * loop of one line per iteration was 4 instructions per line, about 500 per
 * page for seqlz's tables, 3% of a page's decode on a Cortex-A55.
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
 * The u16 at the start of a page, from bytes. Not get_unaligned_le16() and
 * put_unaligned_le16(): with them clang gave seqlz_decode() and code_literals()
 * other registers, and in the kernel VM a page read 0.04 us and written 0.08 us
 * slower (#108).
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
 * a hash of the low 5 bytes of v, as zstd's: << 24 keeps only them,
 * 889523592379 is zstd's prime5bytes
 */
static inline unsigned int hash5(u64 v)
{
	return (unsigned int)(((v << 24) * 889523592379ULL) >>
			      (64U - PAGE_LZ_HASH_BITS));
}

/* number of equal bytes at p and q, p after q, up to end */
static inline unsigned int count(const u8 *p, const u8 *q, const u8 *end)
{
	const u8 *start = p;

	/*
	 * The first 16 bytes without a branch: whether the first 8 are equal
	 * mispredicted, 24% of the matches are longer than 11 bytes. One ctz,
	 * of the first word that differs: two, combined, took more
	 * instructions, and 74% of the matches end in the first 8 bytes. With
	 * the top bit set, a ctz of 7 bytes stands for 8.
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
 * What the matcher hands on per sequence: the literals before the match, the
 * match length, 0 for the last sequence, and the offset.
 */
typedef void (*emit_fn)(void *ctx, const u8 *literals, unsigned int ll,
			unsigned int ml, unsigned int off);

/*
 * Greedy, like lz4's fast mode: at every position the last offset and one
 * candidate from a hash of 5 bytes. With 5 instead of 4 the matcher finds fewer
 * sequences: 5% fewer compress cycles for 0.6 points of memory, and in the
 * kernel 10% less write time at p99 (docs/explored-designs.md). Matches of 4
 * bytes still come from the last offset. Every position is tried, without lz4's
 * growing step after a long run without a match: the step needed the start of
 * the literals in the loop, and without it the pages that compress were 5%
 * faster, the ones zram stores raw 2.3 us slower (docs/explored-designs.md,
 * "The matcher without its step"). Each sequence goes to emit as soon as it is
 * found; inlined with the encoder that is one pass over the page, and the same
 * matcher feeds seqlz_find.
 */
static __always_inline void match_page(u16 *table, const u8 *src, emit_fn emit,
				       void *ctx)
{
	/*
	 * positions, not pointers: the end is a constant, and the position for
	 * the table is at hand
	 */
	/* 8 bytes readable for the hash and the comparison */
	const unsigned int limit = PAGE_LZ_PAGE - 8U;
	unsigned int pos = 1, anchor = 0, last = 1, h, cand;
	/*
	 * -last as an index, so the load at the last offset needs no
	 * subtraction of its own
	 */
	long back = -1;
	/*
	 * This position's 8 bytes, hash, table entry and the 4 bytes there, all
	 * loaded one position ahead: on the in-order Cortex-A55 each step
	 * waited for the one before, 3.4% of the compressor's cycles. The entry
	 * is read after the position before was stored, as before, so the
	 * matches are the same.
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
		/*
		 * the first 4 bytes of the 8 for the hash, without a
		 * second load
		 */
		u32 cur = (u32)v;
		/*
		 * One branch for both candidates, not three: the last offset
		 * always points into the page (it starts at 1, the search at
		 * position 1), and so does a table entry, so both can be read
		 * before it is known whether they count. Three branches
		 * mispredicted almost twice as often as lz4's one. The table is
		 * cleared for each page, so every entry is before pos.
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
		/* backwards into the literals, then forwards */
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
		 * a position near the end of the match, 2 bytes before it as in
		 * lz4's fast mode, for the next matches
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
 * The copies of a fixed 8 or 16 bytes are __builtin_memcpy(), as lib/lz4's
 * LZ4_memcpy(): with CONFIG_FORTIFY_SOURCE, clang does not inline the kernel's
 * memcpy() in the Mi 9T's 4.14, and every copy was a call.
 */

/*
 * The literals of a sequence, nl bytes from lit to d; the caller has checked
 * that they fit in both. 16 bytes at a time while there are 16 bytes of room
 * behind, in the page and in the input; may write and read past nl, which is
 * overwritten or ignored. The rest one by one, at most 15 bytes at the end of
 * the page or of the input.
 */
static __always_inline void copy_literals(u8 *d, const u8 *d_end, const u8 *lit,
					  const u8 *s_end, unsigned int nl)
{
	unsigned int k = 0;

	/*
	 * most literal runs are shorter than 16 bytes: one unconditional copy,
	 * no loop to mispredict
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
 * per offset below 8 the largest multiple of off up to 8, for copy_match() and
 * seqlz.c's fast path
 */
static const u8 page_lz_step_for[8] = { 0, 8, 8, 6, 8, 5, 6, 7 };

/*
 * A match of len bytes, off back from d; the caller has checked that 0 < off <=
 * d - start of the page and that len fits. 8 bytes at a time while there are 8
 * bytes of room behind in the page; may write past len, which the next sequence
 * overwrites. The rest one by one, at most 7 bytes at the end of the page; a
 * run to the end of the page byte by byte made the slowest pages 10 times
 * slower than lz4.
 *
 * For an offset below 8 the first 8 bytes of the match are built in a register:
 * the off bytes before the match, repeated with shifts, then one 8-byte store.
 * From there each step copies from step bytes back, the largest multiple of off
 * up to 8, which is exactly what the previous store wrote, so the load gets it
 * from that store. Writing the first bytes one by one made the load wait for
 * four stores, and a loop over them mispredicted its exit.
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

			/* d - off + 7 < d + 8 <= d_end: inside the page */
			w = get_unaligned_le64(d - off);
			pat = w & ((1ULL << bits) - 1ULL);
			pat |= pat << bits;
			pat |= (pat << ((2U * bits) & 63U)) &
			       (0ULL - (u64)(2U * bits < 64U));
			pat |= (pat << ((4U * bits) & 63U)) &
			       (0ULL - (u64)(4U * bits < 64U));
			put_unaligned_le64(pat, d);
			/*
			 * step bytes on, a multiple of off, it is the same 8
			 * bytes again: stores only, without a load that waits
			 * for the store before it
			 */
			for (k = step;
			     k < len && (unsigned int)(d_end - d) >= k + 8U;
			     k += step)
				put_unaligned_le64(pat, d + k);
			back = 0;
		} else {
			back = 0; /* at the end of the page: one by one below */
		}
	} else if ((unsigned int)(d_end - d) >= 16U) {
		/*
		 * 79% of the matches are at most 16 bytes: two unconditional
		 * copies, the second reads only bytes the first wrote or that
		 * were there before, because off >= 8. Four copies, for 91% of
		 * them, were faster at p50 and slower at p99: the slowest pages
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
