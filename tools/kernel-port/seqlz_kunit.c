// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for seqlz: the tables against the hashes the specification
 * gives and the codes built from them, its worked example, round trips of
 * generated blocks at every level, also with little room for the compressed
 * block, damaged blocks, and blocks made here from the specification's rules,
 * each valid or invalid by one rule.
 */
#include <crypto/sha2.h>
#include <kunit/test.h>
#include <linux/bitops.h>
#include <linux/hex.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/prandom.h>
#include <linux/seqlz.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "../seqlz_internal.h"
#include "../seqlz_lengths.h"

/*
 * The SHA-256 of each table's code lengths, in symbol order, and a block of
 * "ab" 2048 times, as the specification gives them
 */
static const char *const sha256_tok =
	"@TOK_4k@";
static const char *const sha256_ll =
	"@LL_4k@";
static const char *const sha256_ml =
	"@ML_4k@";
static const char *const sha256_lit =
	"@LIT_4k@";
static const char *const sha256_own =
	"@OWN_4k@";
static const u8 example[] = {
	@EXAMPLE@
};

static void check_sha256(struct kunit *test, const void *data, size_t len,
			 const char *hex)
{
	u8 want[SHA256_DIGEST_SIZE], got[SHA256_DIGEST_SIZE];

	KUNIT_ASSERT_EQ(test, hex2bin(want, hex, sizeof(want)), 0);
	sha256(data, len, got);
	KUNIT_EXPECT_MEMEQ(test, got, want, sizeof(want));
}

static void seqlz_test_tables(struct kunit *test)
{
	check_sha256(test, seqlz_default_lengths.token,
		     sizeof(seqlz_default_lengths.token), sha256_tok);
	check_sha256(test, seqlz_default_lengths.ll, sizeof(seqlz_default_lengths.ll),
		     sha256_ll);
	check_sha256(test, seqlz_default_lengths.ml, sizeof(seqlz_default_lengths.ml),
		     sha256_ml);
	check_sha256(test, seqlz_lit_sets, sizeof(seqlz_lit_sets), sha256_lit);
	check_sha256(test, seqlz_own_lengths, sizeof(seqlz_own_lengths),
		     sha256_own);
	/* the encoder's bound of 31 bits for a token and its offset */
	KUNIT_EXPECT_GT(test, seqlz_default_lengths.token[SEQLZ_ESCAPE], 0);
	KUNIT_EXPECT_LE(test, seqlz_default_lengths.token[SEQLZ_ESCAPE],
			SEQLZ_MAX_ESCAPE_LEN);
}

/* buffers for one test, freed by KUnit; want: the block out should hold */
struct bufs {
	u8 *block, *c, *out, *want;
	void *cmem, *dmem;
};

static void get_bufs(struct kunit *test, struct bufs *b)
{
	b->block = kunit_kmalloc(test, SEQLZ_BLOCK_SIZE, GFP_KERNEL);
	b->c = kunit_kmalloc(test, SEQLZ_COMPRESS_BOUND, GFP_KERNEL);
	b->out = kunit_kmalloc(test, SEQLZ_BLOCK_SIZE, GFP_KERNEL);
	b->want = kunit_kmalloc(test, SEQLZ_BLOCK_SIZE, GFP_KERNEL);
	b->cmem = kunit_kmalloc(test, SEQLZ_MEM_COMPRESS, GFP_KERNEL);
	b->dmem = kunit_kmalloc(test, SEQLZ_MEM_DECOMPRESS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b->block);
	KUNIT_ASSERT_NOT_NULL(test, b->c);
	KUNIT_ASSERT_NOT_NULL(test, b->out);
	KUNIT_ASSERT_NOT_NULL(test, b->want);
	KUNIT_ASSERT_NOT_NULL(test, b->cmem);
	KUNIT_ASSERT_NOT_NULL(test, b->dmem);
}

/*
 * Decompress len bytes from a buffer of exactly that size, so that KASAN sees
 * a read behind them
 */
static int decompress(struct kunit *test, struct bufs *b, const u8 *src,
		      unsigned int len)
{
	u8 *in = kunit_kmalloc(test, max(len, 1U), GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, in);
	memcpy(in, src, len);
	memset(b->out, 0xa5, SEQLZ_BLOCK_SIZE);
	ret = seqlz_decompress(in, len, b->out, b->dmem);
	kunit_kfree(test, in);
	return ret;
}

static void seqlz_test_example(struct kunit *test)
{
	struct bufs b;
	u8 longer[sizeof(example) + 1];
	unsigned int k;

	get_bufs(test, &b);
	KUNIT_ASSERT_EQ(test, decompress(test, &b, example, sizeof(example)),
			0);
	for (k = 0; k < SEQLZ_BLOCK_SIZE; k++)
		b.want[k] = k % 2 ? 'b' : 'a';
	KUNIT_ASSERT_MEMEQ(test, b.out, b.want, SEQLZ_BLOCK_SIZE);
	/* without its last byte the match is cut */
	KUNIT_EXPECT_EQ(test,
			decompress(test, &b, example, sizeof(example) - 1),
			-EINVAL);
	/* a block ends where its bits end */
	memcpy(longer, example, sizeof(example));
	longer[sizeof(example)] = 0;
	KUNIT_EXPECT_EQ(test, decompress(test, &b, longer, sizeof(longer)),
			-EINVAL);
}

/*
 * blocks of several kinds, so that every offset class and every way to code
 * the literals occur
 */
static void make_block(struct rnd_state *rng, unsigned int kind, u8 *p)
{
	static const char *const words[] = { "the ", "page ",   "swap ",
					     "memory ", "of ",  "zram ",
					     "kernel ", "and " };
	/* by their frequency in English, the first most often */
	static const char letters[] = "etaoinshrdlcumwfgypbvk";
	unsigned int k, i, n, off;
	const char *w;
	u64 v;

	switch (kind % 10) {
	case 0: /* random bytes: raw literals, few matches */
		prandom_bytes_state(rng, p, SEQLZ_BLOCK_SIZE);
		break;
	case 1: /* words */
		for (k = 0; k < SEQLZ_BLOCK_SIZE;)
			for (w = words[prandom_u32_state(rng) % 8];
			     *w && k < SEQLZ_BLOCK_SIZE;)
				p[k++] = *w++;
		break;
	case 2: /* pointers: offsets that are multiples of 8 */
		for (k = 0; k + 8 <= SEQLZ_BLOCK_SIZE; k += 8) {
			v = 0x7f0000001000ULL +
			    (prandom_u32_state(rng) % 64) * 16;
			put_unaligned_le64(v, p + k);
		}
		break;
	case 3: /* a short pattern with noise: offsets 2 or 4, long matches */
		for (k = 0; k < SEQLZ_BLOCK_SIZE; k++)
			p[k] = prandom_u32_state(rng) % 50 ?
				       k % (kind / 7 % 2 ? 2 : 4) :
				       prandom_u32_state(rng);
		break;
	case 4: /* copies of earlier bytes at any distance, long and short */
		for (k = 0; k < SEQLZ_BLOCK_SIZE;) {
			if (k > 16 && prandom_u32_state(rng) % 2) {
				off = 1 + prandom_u32_state(rng) % k;
				n = prandom_u32_state(rng) % 4 ? 20 : 600;
				n = min(SEQLZ_BLOCK_SIZE - k,
					4 + prandom_u32_state(rng) % n);
				for (i = 0; i < n; i++, k++)
					p[k] = p[k - off];
			} else {
				p[k++] = prandom_u32_state(rng) % 16;
			}
		}
		break;
	case 5: /* letters, the frequent ones more often: coded literals */
		for (k = 0; k < SEQLZ_BLOCK_SIZE; k++) {
			n = sizeof(letters) - 1;
			p[k] = letters[prandom_u32_state(rng) % n *
				       (prandom_u32_state(rng) % n) / n];
		}
		break;
	case 7: /* fields of 4 bytes, the high 3 kept for 2: literals as deltas */
		for (k = 0; k < SEQLZ_BLOCK_SIZE; k += 4) {
			if (k % 8 == 0)
				v = prandom_u32_state(rng);
			p[k] = prandom_u32_state(rng);
			put_unaligned_le24(v, p + k + 1);
		}
		break;
	case 8: /* records of 16 bytes, a bit flipped here and there: deltas */
		prandom_bytes_state(rng, p, 16);
		for (k = 16; k < SEQLZ_BLOCK_SIZE; k++) {
			p[k] = p[k - 16];
			if (prandom_u32_state(rng) % 12 == 0)
				p[k] ^= 1U << prandom_u32_state(rng) % 8;
		}
		break;
	case 9: /* fields of 8 bytes, each byte a bit above the one 8 before */
		prandom_bytes_state(rng, p, 8);
		for (k = 8; k < SEQLZ_BLOCK_SIZE; k++)
			p[k] = p[k - 8] + prandom_u32_state(rng) % 4;
		break;
	case 6:
	default: /* mostly zeros, as many swapped pages */
		memset(p, 0, SEQLZ_BLOCK_SIZE);
		for (k = 0; k < 40; k++)
			p[prandom_u32_state(rng) % SEQLZ_BLOCK_SIZE] =
				prandom_u32_state(rng);
	}
}

/* the compressed block c has coded literals, see SEQLZ_LIT_CODED */
static bool lits_coded(const u8 *c)
{
	return get_unaligned_le16(c) & SEQLZ_LIT_CODED;
}

/* the delta of the compressed block c, see SEQLZ_LIT_DELTA_AT; 0 for none */
static unsigned int lits_delta(const u8 *c)
{
	return lits_coded(c) && c[2] >> SEQLZ_LIT_DELTA_AT ? c[3] : 0;
}

static void seqlz_test_round_trip(struct kunit *test)
{
	struct rnd_state rng;
	struct bufs b;
	int round, level, len, coded = 0, own = 0, deltas = 0;
	/* the blocks with each delta, SEQLZ_DELTA_4 to SEQLZ_DELTA_LAST */
	int delta[SEQLZ_DELTA_LAST + 1] = {};

	get_bufs(test, &b);
	prandom_seed_state(&rng, 41);
	for (round = 0; round < 210; round++) {
		make_block(&rng, round, b.block);
		for (level = SEQLZ_LEVEL_MIN; level <= SEQLZ_LEVEL_MAX;
		     level++) {
			len = seqlz_compress(b.block, b.c, SEQLZ_COMPRESS_BOUND,
					     b.cmem, level);
			KUNIT_ASSERT_GT(test, len, 0);
			coded += lits_coded(b.c);
			own += lits_coded(b.c) &&
			       (b.c[2] & SEQLZ_LIT_CODE_MASK) == SEQLZ_LIT_OWN;
			deltas += lits_coded(b.c) &&
				  b.c[2] >> SEQLZ_LIT_DELTA_AT;
			KUNIT_ASSERT_LE(test, lits_delta(b.c),
					SEQLZ_DELTA_LAST);
			delta[lits_delta(b.c)]++;
			KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len),
					0);
			KUNIT_ASSERT_MEMEQ(test, b.out, b.block,
					   SEQLZ_BLOCK_SIZE);
			/*
			 * without work memory, only blocks without coded
			 * literals: callers that only ever see level 1 blocks
			 * may pass NULL
			 */
			memset(b.out, 0xa5, SEQLZ_BLOCK_SIZE);
			if (lits_coded(b.c)) {
				KUNIT_ASSERT_EQ(test,
						seqlz_decompress(b.c, len, b.out,
								 NULL),
						-EINVAL);
			} else {
				KUNIT_ASSERT_EQ(test,
						seqlz_decompress(b.c, len, b.out,
								 NULL),
						0);
				KUNIT_ASSERT_MEMEQ(test, b.out, b.block,
						   SEQLZ_BLOCK_SIZE);
			}
			/*
			 * one byte less does not fit, and nothing is written
			 * behind it
			 */
			memset(b.c, 0x5a, SEQLZ_COMPRESS_BOUND);
			KUNIT_ASSERT_EQ(test,
					seqlz_compress(b.block, b.c, len - 1,
						       b.cmem, level),
					-E2BIG);
			KUNIT_ASSERT_NULL(test,
					  memchr_inv(b.c + len - 1, 0x5a,
						     SEQLZ_COMPRESS_BOUND -
							     len + 1));
		}
	}
	/* the blocks have to give both layouts, own tables and each delta */
	KUNIT_EXPECT_GE(test, coded, 20);
	KUNIT_EXPECT_GE(test, own, 10);
	KUNIT_EXPECT_GE(test, deltas, 10);
	KUNIT_EXPECT_GT(test, delta[SEQLZ_DELTA_4], 0);
	KUNIT_EXPECT_GT(test, delta[SEQLZ_DELTA_8], 0);
	KUNIT_EXPECT_GT(test, delta[SEQLZ_DELTA_LAST], 0);
	KUNIT_EXPECT_EQ(test,
			seqlz_compress(b.block, b.c, SEQLZ_COMPRESS_BOUND,
				       b.cmem, SEQLZ_LEVEL_MIN - 1),
			-EINVAL);
	KUNIT_EXPECT_EQ(test,
			seqlz_compress(b.block, b.c, SEQLZ_COMPRESS_BOUND,
				       b.cmem, SEQLZ_LEVEL_MAX + 1),
			-EINVAL);
}

/*
 * Records of period p: a run of n literals every gap bytes, each the byte p
 * before plus 1 to 3, the others the byte p before
 */
static void make_records(struct rnd_state *rng, u8 *block, unsigned int p,
			 unsigned int gap, unsigned int n)
{
	unsigned int k;

	for (k = 0; k < p; k++)
		block[k] = prandom_u32_state(rng);
	for (; k < SEQLZ_BLOCK_SIZE; k++) {
		block[k] = block[k - p];
		if (k % gap < n)
			block[k] += 1 + prandom_u32_state(rng) % 3;
	}
}

/* how far back the byte is that a delta of block c is to, with last offset p */
static unsigned int delta_distance(const u8 *c, unsigned int p)
{
	switch (lits_delta(c)) {
	case SEQLZ_DELTA_4:
		return 4;
	case SEQLZ_DELTA_8:
		return 8;
	case SEQLZ_DELTA_LAST:
		return seqlz_delta_last(p);
	default:
		return 0;
	}
}

/*
 * Level 3 codes the literals of make_records() as deltas to the byte of the
 * last offset, at the distance seqlz_delta_last(p), or to the byte 4 or 8
 * before where that is the same. The decoder's fast path adds back the 16
 * bytes of each sequence in three ways, for distances below 8, below 16 and
 * from 16 on, and up to 14 literals make the second 8 of those bytes count
 * too. Every distance from 4 to 24 has to come.
 */
static void seqlz_test_delta_distances(struct kunit *test)
{
	/* the gap between runs of literals and their length */
	static const unsigned int runs[][2] = { { 5, 1 },
						{ 13, 8 },
						{ 20, 14 } };
	unsigned long dists = 0;
	unsigned int p, r, k;
	struct rnd_state rng;
	struct bufs b;
	int len;

	get_bufs(test, &b);
	prandom_seed_state(&rng, 240);
	for (p = 1; p <= 24; p++)
		for (r = 0; r < ARRAY_SIZE(runs); r++) {
			make_records(&rng, b.block, p, runs[r][0], runs[r][1]);
			len = seqlz_compress(b.block, b.c, SEQLZ_COMPRESS_BOUND,
					     b.cmem, SEQLZ_LEVEL_MAX);
			KUNIT_ASSERT_GT(test, len, 0);
			KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len),
					0);
			KUNIT_ASSERT_MEMEQ_MSG(test, b.out, b.block,
					       SEQLZ_BLOCK_SIZE, "p %u gap %u",
					       p, runs[r][0]);
			dists |= BIT(delta_distance(b.c, p));
		}
	for (k = 4; k <= 24; k++)
		KUNIT_EXPECT_TRUE_MSG(test, dists & BIT(k), "distance %u", k);
}

/*
 * The room seqlz_compress() needs past a compressed block while it writes it,
 * as its kernel-doc gives it
 */
#define COMPRESS_ROOM 32U

/*
 * Compresses b->block into the first dst_len bytes of b->c and returns what
 * seqlz_compress() does. Nothing may be written at or behind dst_len, and a
 * block that fits must decompress to b->block.
 */
static int compress_into(struct kunit *test, struct bufs *b,
			 unsigned int dst_len, int level)
{
	int len;

	memset(b->c, 0x5a, SEQLZ_COMPRESS_BOUND);
	len = seqlz_compress(b->block, b->c, dst_len, b->cmem, level);
	KUNIT_ASSERT_NULL(test, memchr_inv(b->c + dst_len, 0x5a,
					   SEQLZ_COMPRESS_BOUND - dst_len));
	if (len == -E2BIG)
		return len;
	KUNIT_ASSERT_GT(test, len, 0);
	KUNIT_ASSERT_LE(test, len, dst_len);
	KUNIT_ASSERT_EQ(test, decompress(test, b, b->c, len), 0);
	KUNIT_ASSERT_MEMEQ(test, b->out, b->block, SEQLZ_BLOCK_SIZE);
	return len;
}

/*
 * The compressor with less room than SEQLZ_COMPRESS_BOUND: the least room in
 * which each block fits, found by bisection, and the room around it and around
 * the block's length. The bitstream is written from the back of dst and moved
 * behind the literals at the end, in place where it overlaps them; and level 3
 * writes the block once more without what it needs for deltas where it does
 * not fit with it.
 */
static void seqlz_test_small_dst(struct kunit *test)
{
	/* less than the u16 and the room: no block fits */
	static const unsigned int too_small[] = {
		0, 1, 2, SEQLZ_HEADER + COMPRESS_ROOM - 1
	};
	unsigned int kind, k, lo, hi, least, least1 = 0, dst_len, full_len;
	unsigned int stream;
	int level, len, in_place = 0, retried = 0;
	struct rnd_state rng;
	struct bufs b;
	u8 *full, *big;

	get_bufs(test, &b);
	full = kunit_kmalloc(test, SEQLZ_COMPRESS_BOUND, GFP_KERNEL);
	big = kunit_kmalloc(test, 2 * SEQLZ_COMPRESS_BOUND, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, full);
	KUNIT_ASSERT_NOT_NULL(test, big);
	prandom_seed_state(&rng, 47);
	for (kind = 0; kind < 10; kind++) {
		make_block(&rng, kind, b.block);
		for (level = SEQLZ_LEVEL_MIN; level <= SEQLZ_LEVEL_MAX;
		     level++) {
			for (k = 0; k < ARRAY_SIZE(too_small); k++)
				KUNIT_ASSERT_EQ(test,
						compress_into(test, &b,
							      too_small[k],
							      level),
						-E2BIG);
			len = compress_into(test, &b, SEQLZ_COMPRESS_BOUND,
					    level);
			full_len = len;
			memcpy(full, b.c, full_len);
			/*
			 * more room gives the same block, and the bytes behind
			 * SEQLZ_COMPRESS_BOUND stay as they were
			 */
			memset(big, 0x5a, 2 * SEQLZ_COMPRESS_BOUND);
			KUNIT_ASSERT_EQ(test,
					seqlz_compress(b.block, big,
						       2 * SEQLZ_COMPRESS_BOUND,
						       b.cmem, level),
					len);
			KUNIT_ASSERT_MEMEQ(test, big, full, full_len);
			KUNIT_ASSERT_NULL(test,
					  memchr_inv(big + SEQLZ_COMPRESS_BOUND,
						     0x5a, SEQLZ_COMPRESS_BOUND));
			/* lo does not fit, hi does */
			lo = too_small[ARRAY_SIZE(too_small) - 1];
			hi = SEQLZ_COMPRESS_BOUND;
			while (hi - lo > 1) {
				dst_len = (lo + hi) / 2;
				if (compress_into(test, &b, dst_len, level) > 0)
					hi = dst_len;
				else
					lo = dst_len;
			}
			least = hi;
			/*
			 * Level 1: raw literals, then the bitstream of stream
			 * bytes. In the least room the bitstream, written at
			 * the back of dst, is least - full_len bytes behind the
			 * literals, at most COMPRESS_ROOM; a longer bitstream
			 * overlaps where it goes and is moved in place. Level 2
			 * writes the block of level 1 first, so it needs the
			 * same room.
			 */
			if (level == SEQLZ_LEVEL_MIN) {
				KUNIT_EXPECT_LE(test, least,
						full_len + COMPRESS_ROOM);
				least1 = least;
				stream = full_len - SEQLZ_HEADER -
					 get_unaligned_le16(full);
				in_place += stream > least - full_len;
			} else if (level == SEQLZ_LEVEL_MIN + 1) {
				KUNIT_EXPECT_EQ(test, least, least1);
			}
			/*
			 * Level 3 writes the block first with what picking a
			 * delta needs, 6 bytes per sequence with literals, and
			 * once more without where it does not fit: in the least
			 * room no block has deltas, also one that has them in
			 * all the room
			 */
			len = compress_into(test, &b, least, level);
			if (level == SEQLZ_LEVEL_MAX && lits_delta(full)) {
				KUNIT_EXPECT_EQ(test, lits_delta(b.c), 0);
				retried++;
			}
			for (dst_len = least; dst_len < least + 8; dst_len++) {
				len = compress_into(test, &b, dst_len, level);
				KUNIT_ASSERT_GT(test, len, 0);
				/* below level 3 the same block in any room */
				if (level < SEQLZ_LEVEL_MAX) {
					KUNIT_ASSERT_EQ(test, len, full_len);
					KUNIT_ASSERT_MEMEQ(test, b.c, full,
							   full_len);
				}
			}
			for (dst_len = full_len;
			     dst_len <= full_len + COMPRESS_ROOM; dst_len++)
				KUNIT_EXPECT_EQ(test,
						compress_into(test, &b, dst_len,
							      level) > 0,
						dst_len >= least);
		}
	}
	KUNIT_EXPECT_GT(test, in_place, 0);
	KUNIT_EXPECT_GT(test, retried, 0);
}

/*
 * Damaged blocks: bits flipped or a header byte replaced give a block or
 * -EINVAL, a block cut short or with bytes appended is invalid
 */
static void seqlz_test_damaged(struct kunit *test)
{
	struct rnd_state rng;
	struct bufs b;
	u8 *d;
	int round, len, f, ret;

	get_bufs(test, &b);
	d = kunit_kmalloc(test, SEQLZ_COMPRESS_BOUND + 9, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, d);
	prandom_seed_state(&rng, 43);
	for (round = 0; round < 1000; round++) {
		make_block(&rng, round, b.block);
		len = seqlz_compress(b.block, b.c, SEQLZ_COMPRESS_BOUND, b.cmem,
				     SEQLZ_LEVEL_MIN + round % (SEQLZ_LEVEL_MAX -
								SEQLZ_LEVEL_MIN + 1));
		KUNIT_ASSERT_GT(test, len, 0);

		memcpy(d, b.c, len);
		for (f = 0; f < 1 + round % 3; f++)
			d[prandom_u32_state(&rng) % len] ^=
				1U << (prandom_u32_state(&rng) % 8);
		ret = decompress(test, &b, d, len);
		KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EINVAL);

		memcpy(d, b.c, len);
		d[prandom_u32_state(&rng) % min(len, 1 + round % 24)] =
			prandom_u32_state(&rng);
		ret = decompress(test, &b, d, len);
		KUNIT_EXPECT_TRUE(test, ret == 0 || ret == -EINVAL);

		KUNIT_EXPECT_EQ(test,
				decompress(test, &b, b.c,
					   prandom_u32_state(&rng) % len),
				-EINVAL);

		memcpy(d, b.c, len);
		f = 1 + round % 9;
		prandom_bytes_state(&rng, d + len, f);
		KUNIT_EXPECT_EQ(test, decompress(test, &b, d, len + f),
				-EINVAL);
	}
}

/*
 * Blocks made here, from the rules of the specification. A code table's
 * canonical codes: shorter codes first, the symbols of one length in the
 * order of their number.
 */
struct code {
	u16 code[SEQLZ_TOKEN_SYMBOLS + 1];
	u8 len[SEQLZ_TOKEN_SYMBOLS + 1];
};

static void canonical(struct code *c, const u8 *len, unsigned int n)
{
	unsigned int count[16] = {}, next[16], first = 0, l, s;

	for (s = 0; s < n; s++)
		count[len[s]]++;
	count[0] = 0;
	for (l = 1; l < 16; l++) {
		first = (first + count[l - 1]) << 1;
		next[l] = first;
	}
	for (s = 0; s < n; s++) {
		c->len[s] = len[s];
		c->code[s] = len[s] ? next[len[s]]++ : 0;
	}
}

/* bits, most significant first */
struct bits {
	u8 *p;
	unsigned int n;
};

static void put(struct bits *w, u32 v, unsigned int n)
{
	while (n--) {
		if (!(w->n & 7))
			w->p[w->n >> 3] = 0;
		w->p[w->n >> 3] |= ((v >> n) & 1) << (7 - (w->n & 7));
		w->n++;
	}
}

static unsigned int bytes(const struct bits *w)
{
	return DIV_ROUND_UP(w->n, 8);
}

struct maker {
	struct kunit *test;
	/* block_own: a block's own literal table, see read_own_table() */
	struct code tok, ll, ml, lit, own, block_own;
	u8 streams[SEQLZ_LIT_STREAMS][SEQLZ_BLOCK_SIZE / 8];
	/* the bits after the last symbol of the block's own table */
	unsigned int own_padding;
};

static void symbol(struct maker *m, struct bits *w, const struct code *c,
		   unsigned int s)
{
	KUNIT_ASSERT_NE(m->test, c->len[s], 0);
	put(w, c->code[s], c->len[s]);
}

/* a token, escaped if it has no code */
static void token(struct maker *m, struct bits *w, unsigned int t)
{
	if (m->tok.len[t]) {
		symbol(m, w, &m->tok, t);
	} else {
		symbol(m, w, &m->tok, SEQLZ_ESCAPE);
		put(w, t, SEQLZ_ESCAPE_BITS);
	}
}

static void put_len_value(struct maker *m, struct bits *w,
			  const struct code *c, unsigned int v)
{
	unsigned int b;

	if (v < 16) {
		symbol(m, w, c, v);
		return;
	}
	b = fls(v) - 1;
	symbol(m, w, c, 12 + b);
	put(w, v - (1U << b), b);
}

static struct maker *get_maker(struct kunit *test)
{
	struct maker *m = kunit_kzalloc(test, sizeof(*m), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, m);
	m->test = test;
	canonical(&m->tok, seqlz_default_lengths.token, SEQLZ_TOKEN_SYMBOLS + 1);
	canonical(&m->ll, seqlz_default_lengths.ll, SEQLZ_LEN_SYMBOLS);
	canonical(&m->ml, seqlz_default_lengths.ml, SEQLZ_LEN_SYMBOLS);
	canonical(&m->lit, seqlz_lit_sets[0], 256);
	canonical(&m->own, seqlz_own_lengths, SEQLZ_OWN_SYMBOLS);
	return m;
}

/*
 * The encoder's codes in seqlz_tables, which a program for the build host
 * writes, against the canonical codes made here from the code lengths: of the
 * tokens, the length values, all literal tables and the code of a block's own
 * table. A symbol without a code has the entry 0.
 */
static void seqlz_test_codes(struct kunit *test)
{
	struct maker *m = get_maker(test);
	struct code *lit = kunit_kmalloc(test, sizeof(*lit), GFP_KERNEL);
	u32 *want = kunit_kcalloc(test, SEQLZ_TOKEN_SYMBOLS + 1, sizeof(u32),
				  GFP_KERNEL);
	u16 *want16 = kunit_kcalloc(test, SEQLZ_TOKEN_SYMBOLS + 1, sizeof(u16),
				    GFP_KERNEL);
	unsigned int k, s;

	KUNIT_ASSERT_NOT_NULL(test, lit);
	KUNIT_ASSERT_NOT_NULL(test, want);
	KUNIT_ASSERT_NOT_NULL(test, want16);
	/* code | length << TOK_ENC_LEN_AT */
	for (s = 0; s <= SEQLZ_TOKEN_SYMBOLS; s++)
		want16[s] = m->tok.code[s] | m->tok.len[s] << TOK_ENC_LEN_AT;
	KUNIT_EXPECT_MEMEQ(test, seqlz_tables.token.enc, want16,
			   sizeof(seqlz_tables.token.enc));
	/* code | length << 16, 0 behind the last symbol */
	memset(want, 0, sizeof(seqlz_tables.ll.enc));
	for (s = 0; s < SEQLZ_LEN_SYMBOLS; s++)
		want[s] = m->ll.code[s] | m->ll.len[s] << 16;
	KUNIT_EXPECT_MEMEQ(test, seqlz_tables.ll.enc, want,
			   sizeof(seqlz_tables.ll.enc));
	for (s = 0; s < SEQLZ_LEN_SYMBOLS; s++)
		want[s] = m->ml.code[s] | m->ml.len[s] << 16;
	KUNIT_EXPECT_MEMEQ(test, seqlz_tables.ml.enc, want,
			   sizeof(seqlz_tables.ml.enc));
	/* length | code << 8 */
	for (k = 0; k < SEQLZ_LIT_SETS; k++) {
		canonical(lit, seqlz_lit_sets[k], 256);
		for (s = 0; s < 256; s++)
			want[s] = lit->len[s] | lit->code[s] << 8;
		KUNIT_EXPECT_MEMEQ_MSG(test, seqlz_tables.lit[k].enc, want,
				       sizeof(seqlz_tables.lit[k].enc),
				       "literal table %u", k);
	}
	/* code | length << 8 */
	for (s = 0; s < SEQLZ_OWN_SYMBOLS; s++)
		want16[s] = m->own.code[s] | m->own.len[s] << 8;
	KUNIT_EXPECT_MEMEQ(test, seqlz_tables.own.enc, want16,
			   sizeof(seqlz_tables.own.enc));
}

/*
 * A block of n raw literals k * 7; without bits after them valid only for
 * n = SEQLZ_BLOCK_SIZE
 */
static unsigned int literals_only(u8 *c, unsigned int n)
{
	unsigned int k;

	put_unaligned_le16(n, c);
	for (k = 0; k < n; k++)
		c[2 + k] = k * 7;
	return 2 + n;
}

/*
 * A block of one literal, 7, and a match of the rest of the block from 1 back,
 * the offset a block starts with. *padding gets the bits after the last code.
 */
static unsigned int one_match(struct maker *m, u8 *c, unsigned int *padding)
{
	struct bits w = { .p = c + 3 };

	c[0] = 1;
	c[1] = 0;
	c[2] = 7;
	token(m, &w, 1 + (SEQLZ_ML_CAP << SEQLZ_LL_BITS));
	put_len_value(m, &w, &m->ml, SEQLZ_BLOCK_SIZE - 1 - 4 - SEQLZ_ML_CAP);
	*padding = 8 * bytes(&w) - w.n;
	return 3 + bytes(&w);
}

/*
 * A sequence's token, the bits of its offset, raw, which are those of class
 * cls, and its length values, as the specification orders them
 */
static void put_seq(struct maker *m, struct bits *w, unsigned int ll,
		    unsigned int ml, unsigned int cls, unsigned int raw)
{
	token(m, w,
	      min(ll, SEQLZ_LL_CAP) |
		      min(ml - SEQLZ_MIN_MATCH, SEQLZ_ML_CAP) << SEQLZ_LL_BITS |
		      cls << (SEQLZ_LL_BITS + SEQLZ_ML_BITS));
	put(w, raw, SEQLZ_RAW_BITS(cls));
	if (ll >= SEQLZ_LL_CAP)
		put_len_value(m, w, &m->ll, ll - SEQLZ_LL_CAP);
	if (ml - SEQLZ_MIN_MATCH >= SEQLZ_ML_CAP)
		put_len_value(m, w, &m->ml,
			      ml - SEQLZ_MIN_MATCH - SEQLZ_ML_CAP);
}

/*
 * A block of one literal and a match of ml from 1 back, then the last
 * sequence's last literals; *w gets its bitstream, for more bits after it.
 * Valid where the three fill the block.
 */
static unsigned int match_then_literals(struct maker *m, u8 *c, unsigned int ml,
					unsigned int last, struct bits *w)
{
	unsigned int len = literals_only(c, 1 + last);

	*w = (struct bits){ .p = c + len };
	put_seq(m, w, 1, ml, 0, 0);
	return len + bytes(w);
}

static void seqlz_test_one_sequence(struct kunit *test)
{
	struct maker *m = get_maker(test);
	unsigned int len, padding, k, before;
	struct bufs b;
	struct bits w;

	get_bufs(test, &b);
	len = one_match(m, b.c, &padding);
	KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
	memset(b.want, 7, SEQLZ_BLOCK_SIZE);
	KUNIT_ASSERT_MEMEQ(test, b.out, b.want, SEQLZ_BLOCK_SIZE);
	/* a byte more, and if there is room, a bit after the last code */
	b.c[len] = 0;
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len + 1), -EINVAL);
	if (padding) {
		b.c[len - 1] |= 1;
		KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);
	}

	/* a block of literals only has no bits */
	len = literals_only(b.c, SEQLZ_BLOCK_SIZE);
	KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
	for (k = 0; k < SEQLZ_BLOCK_SIZE; k++)
		b.want[k] = k * 7;
	KUNIT_ASSERT_MEMEQ(test, b.out, b.want, SEQLZ_BLOCK_SIZE);
	/*
	 * so anything after its literals, even a 0 byte, is invalid: the
	 * decoder reads no token there, the end check finds the byte unread
	 */
	b.c[len] = 0;
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len + 1), -EINVAL);
	/* and as many literals as the block has bytes */
	len = literals_only(b.c, SEQLZ_BLOCK_SIZE - 1);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);

	/*
	 * A match that ends where the literals left fill the block, then a
	 * token of one sequence more, with 12 bits of offset so that the block
	 * gets at least a byte longer: the last sequence has no token.
	 */
	len = match_then_literals(m, b.c, SEQLZ_BLOCK_SIZE - 2, 1, &w);
	KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
	before = bytes(&w);
	put_seq(m, &w, 1, SEQLZ_MIN_MATCH, 3, 1);
	KUNIT_ASSERT_GT(test, bytes(&w), before);
	KUNIT_EXPECT_EQ(test,
			decompress(test, &b, b.c, len - before + bytes(&w)),
			-EINVAL);
}

/*
 * A block of 2 literals: a sequence of 1 literal and a match of 4, then the
 * escape and the token idx in SEQLZ_ESCAPE_BITS bits for a sequence of
 * covers bytes, then 1 literal and a match up to the block's end
 */
static unsigned int escaped(struct maker *m, u8 *c, unsigned int idx,
			    unsigned int covers)
{
	unsigned int len = literals_only(c, 2);
	struct bits w = { .p = c + len };

	put_seq(m, &w, 1, SEQLZ_MIN_MATCH, 0, 0);
	symbol(m, &w, &m->tok, SEQLZ_ESCAPE);
	put(&w, idx, SEQLZ_ESCAPE_BITS);
	put_seq(m, &w, 1, SEQLZ_BLOCK_SIZE - 2 - SEQLZ_MIN_MATCH - covers, 0,
		0);
	return len + bytes(&w);
}

/*
 * A block whose first sequence has ll literals, a match of 4 and the offset
 * raw of class cls; then 20 sequences of 1 literal and a match of 4 from 5
 * back, which give the bitstream the length the decoder's fast path needs,
 * one sequence with a match up to 8 bytes before the block's end, and 8
 * literals. With ll below SEQLZ_LL_CAP the first sequence has no length value
 * and takes the fast path, else the slow one.
 */
static unsigned int first_offset(struct maker *m, u8 *c, unsigned int ll,
				 unsigned int cls, unsigned int raw)
{
	const unsigned int fill = 20, rest = 8;
	unsigned int len = literals_only(c, ll + fill + 1 + rest), k,
		     at = ll + SEQLZ_MIN_MATCH + fill * (1 + SEQLZ_MIN_MATCH);
	struct bits w = { .p = c + len };

	put_seq(m, &w, ll, SEQLZ_MIN_MATCH, cls, raw);
	for (k = 0; k < fill; k++)
		put_seq(m, &w, 1, SEQLZ_MIN_MATCH, 2, 5);
	put_seq(m, &w, 1, SEQLZ_BLOCK_SIZE - at - 1 - rest, 0, 0);
	return len + bytes(&w);
}

/* what coded_literals() does to the block it makes */
enum damage {
	GOOD,
	DELTA_0, /* deltas, their byte 0 */
	DELTA_4, /* deltas, their byte 4 */
	DELTA_CUT, /* deltas, the block ends 1 byte before its header does */
	CODE_9, /* the literals' code 9, above a block's own table */
	STREAM_0_LONGER, /* stream 0 a 0 byte longer than its codes */
	BYTE_TO_STREAM_1, /* stream 0's last byte counted in stream 1 */
	PADDING_BIT, /* the lowest bit of stream 0's last byte set */
	SIZES_PAST_END, /* stream 7's size the largest, past the block's end */
	/* only for a block with its own table */
	OWN_OVERFULL, /* a third byte with a code of 1 bit */
	OWN_ABOVE_255, /* the second byte with a code is 256 */
	OWN_PADDING_BIT, /* the lowest bit of the table's last byte set */
	OWN_CUT, /* the block ends in the table */
	OWN_RUNS, /* two runs of 200 bytes first: past the 256 byte values */
	DAMAGES,
};

/* in a block's own table: a run of n byte values without a code, if n > 0 */
static void own_run(struct maker *m, struct bits *w, unsigned int n)
{
	unsigned int k;

	if (!n)
		return;
	k = fls(n);
	symbol(m, w, &m->own, SEQLZ_OWN_RUN + k);
	put(w, n - (1U << (k - 1)), k - 1);
}

/*
 * A block's own table into w, in which only the bytes x and y, x < y, have a
 * code, of 1 bit each; their codes into m->block_own
 */
static void own_table(struct maker *m, struct bits *w, unsigned int x,
		      unsigned int y, enum damage damage)
{
	u8 len[256] = {};

	if (damage == OWN_RUNS) {
		own_run(m, w, 200);
		own_run(m, w, 200);
	}
	own_run(m, w, x);
	symbol(m, w, &m->own, damage == OWN_OVERFULL ? 1 : 0);
	own_run(m, w, damage == OWN_ABOVE_255 ? 255 - x : y - x - 1);
	symbol(m, w, &m->own, 0);
	if (damage == OWN_OVERFULL)
		symbol(m, w, &m->own, 0);
	m->own_padding = 8 * bytes(w) - w->n;
	if (damage == OWN_PADDING_BIT)
		w->p[bytes(w) - 1] |= 1;
	len[x] = 1;
	len[y] = 1;
	canonical(&m->block_own, len, 256);
}

/*
 * A block of n literals lit(k, arg), coded with literal table 0, or with own a
 * table of the block's own for the bytes arg & 0xff and arg >> 8, then a match
 * of offset 1 up to the end of the block: valid, as the specification has it,
 * unless damaged. *padding gets the bits after stream 0's last code.
 */
static unsigned int coded_literals(struct maker *m, u8 *c, unsigned int n,
				   u8 (*lit)(unsigned int k, unsigned int arg),
				   unsigned int arg, bool own,
				   enum damage damage, unsigned int *padding)
{
	struct bits s[SEQLZ_LIT_STREAMS], w, t;
	/* the bytes of each stream, and the sizes the header gives */
	unsigned int size[SEQLZ_LIT_STREAMS], told[SEQLZ_LIT_STREAMS];
	unsigned int width = 5, at, j, k, q;
	/* the byte that says which delta, with deltas */
	unsigned int e = damage == DELTA_0 || damage == DELTA_4 ||
			 damage == DELTA_CUT;
	u8 desc[64];

	t = (struct bits){ .p = desc };
	if (own)
		own_table(m, &t, arg & 0xff, arg >> 8, damage);
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		s[j] = (struct bits){ .p = m->streams[j] };
	for (k = 0; k < n; k++)
		symbol(m, &s[k % SEQLZ_LIT_STREAMS],
		       own ? &m->block_own : &m->lit, lit(k, arg));
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		size[j] = bytes(&s[j]);
	*padding = 8 * size[0] - s[0].n;
	if (damage == STREAM_0_LONGER)
		m->streams[0][size[0]++] = 0;
	if (damage == PADDING_BIT)
		m->streams[0][size[0] - 1] |= 1;
	memcpy(told, size, sizeof(told));
	if (damage == BYTE_TO_STREAM_1) {
		told[0]--;
		told[1]++;
	}
	if (damage == SIZES_PAST_END)
		told[SEQLZ_LIT_STREAMS - 1] = BIT(SEQLZ_SIZE_BITS_MAX) - 1;
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		width = max_t(unsigned int, width, fls(told[j]));

	put_unaligned_le16(SEQLZ_LIT_CODED | n, c);
	c[2] = (width - SEQLZ_SIZE_BITS_MIN) << SEQLZ_LIT_WIDTH_AT |
	       (own ? SEQLZ_LIT_OWN : 0) | e << SEQLZ_LIT_DELTA_AT;
	c[3] = damage == DELTA_0 ? 0 : damage == DELTA_4 ? 4 : SEQLZ_DELTA_4;
	if (damage == CODE_9)
		c[2] = (c[2] & ~SEQLZ_LIT_CODE_MASK) | 9;
	/* the sizes are one number of 8 * width bits, little endian */
	memset(c + 3 + e, 0, width);
	for (q = 0; q < 8 * width; q++)
		c[3 + e + q / 8] |= ((told[q / width] >> (q % width)) & 1)
				    << (q % 8);
	at = 3 + e + width;
	if (damage == DELTA_CUT)
		return at - 1;
	memcpy(c + at, desc, bytes(&t));
	at += bytes(&t);
	if (damage == OWN_CUT)
		return at - 1;
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
		memcpy(c + at, m->streams[j], size[j]);
		at += size[j];
	}

	/*
	 * ll and ml - 4 above what the token holds, class 0, so offset 1; the
	 * last sequence, without literals, has no bits
	 */
	w = (struct bits){ .p = c + at };
	token(m, &w, SEQLZ_LL_CAP + (SEQLZ_ML_CAP << SEQLZ_LL_BITS));
	put_len_value(m, &w, &m->ll, n - SEQLZ_LL_CAP);
	put_len_value(m, &w, &m->ml, SEQLZ_BLOCK_SIZE - n - 4 - SEQLZ_ML_CAP);
	return at + bytes(&w);
}

static u8 same(unsigned int k, unsigned int b)
{
	return b;
}

/*
 * Codes of 9 and of 10 bits: 57 bits after a stream's last load are then 5
 * codes of 10 bits and 7 bits of the first byte, which codes of one even
 * length never give
 */
static u8 mixed(unsigned int k, unsigned int seed)
{
	return ((k + 1) * 2654435761U + seed * 40503U) >> 31 ? 0xff : 0x71;
}

static void check_coded(struct maker *m, struct bufs *b, unsigned int n,
			u8 (*lit)(unsigned int k, unsigned int arg),
			unsigned int arg, bool own)
{
	struct kunit *test = m->test;
	unsigned int len, padding, k;
	enum damage d;

	len = coded_literals(m, b->c, n, lit, arg, own, GOOD, &padding);
	KUNIT_ASSERT_EQ_MSG(test, decompress(test, b, b->c, len), 0,
			    "n %u arg %u", n, arg);
	for (k = 0; k < SEQLZ_BLOCK_SIZE; k++)
		b->want[k] = lit(min(k, n - 1), arg);
	KUNIT_ASSERT_MEMEQ_MSG(test, b->out, b->want, SEQLZ_BLOCK_SIZE,
			       "n %u arg %u", n, arg);
	for (d = DELTA_0; d < (own ? DAMAGES : OWN_OVERFULL); d++) {
		if ((d == PADDING_BIT && !padding) ||
		    (d == OWN_PADDING_BIT && !m->own_padding))
			continue;
		len = coded_literals(m, b->c, n, lit, arg, own, d, &padding);
		KUNIT_EXPECT_EQ_MSG(test, decompress(test, b, b->c, len),
				    -EINVAL, "n %u arg %u damage %d", n, arg,
				    d);
	}
}

/*
 * Literal streams whose codes end at every place of a byte. The decoder once
 * read a stream's last bits after its codes from its register instead of the
 * block, where the 1 that marks how far it read can be.
 */
static void seqlz_test_coded_literals(struct kunit *test)
{
	static const u8 values[] = { 0x65, 0x71, 0x00, 0xff };
	struct maker *m = get_maker(test);
	unsigned int n, i;
	struct bufs b;

	get_bufs(test, &b);
	for (i = 0; i < ARRAY_SIZE(values); i++)
		for (n = 16; n < 600; n++)
			check_coded(m, &b, n, same, values[i], false);
	for (i = 0; i < 4; i++)
		for (n = 300; n < 600; n++)
			check_coded(m, &b, n, mixed, i, false);
}

/* the byte arg & 0xff or arg >> 8, by a hash of k */
static u8 two(unsigned int k, unsigned int arg)
{
	return (k + 1) * 2654435761U >> 31 ? arg >> 8 : arg & 0xff;
}

/*
 * Blocks with a table of their own: two bytes with a code of 1 bit, with runs
 * before them that the decoder takes two at a time, 3 and 1, or alone, 97 and
 * 254, and tables that are invalid by one rule each
 */
static void seqlz_test_own_table(struct kunit *test)
{
	static const unsigned int pairs[] = { 3 | 5 << 8, 97 | 98 << 8,
					      0 | 255 << 8 };
	struct maker *m = get_maker(test);
	unsigned int n, i;
	struct bufs b;

	get_bufs(test, &b);
	for (i = 0; i < ARRAY_SIZE(pairs); i++)
		for (n = 16; n < 200; n++)
			check_coded(m, &b, n, two, pairs[i], true);
}

/*
 * Blocks invalid by one rule each, which seqlz_test_damaged() may or may not
 * make; where it is simple, also the block with the least change that makes it
 * valid, so that the invalid one fails at the check meant
 */
static void seqlz_test_invalid(struct kunit *test)
{
	static const unsigned int lls[] = { 2, SEQLZ_LL_CAP + 1 };
	struct maker *m = get_maker(test);
	unsigned int len, padding, ll, k;
	struct bufs b;
	struct bits w;

	get_bufs(test, &b);
	/* shorter than the u16 every block starts with */
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, 0), -EINVAL);
	b.c[0] = 0;
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, 1), -EINVAL);

	/*
	 * An escaped token the escape has no room for: SEQLZ_TOKEN_SYMBOLS and
	 * above. The decoder would take it as a sequence without literals and
	 * match, so only its check of the number rejects the block.
	 */
	len = escaped(m, b.c, 0, SEQLZ_MIN_MATCH);
	KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
	len = escaped(m, b.c, SEQLZ_TOKEN_SYMBOLS, 0);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);
	len = escaped(m, b.c, BIT(SEQLZ_ESCAPE_BITS) - 1, 0);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);

	/*
	 * Offset 0, class 1 with its bit 0, and an offset past the bytes
	 * written, on the fast path and with a literal length value on the
	 * slow path. Offset 2, and an offset of all the bytes written, are
	 * valid.
	 */
	for (k = 0; k < ARRAY_SIZE(lls); k++) {
		ll = lls[k];
		len = first_offset(m, b.c, ll, 1, 1);
		KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
		len = first_offset(m, b.c, ll, 1, 0);
		KUNIT_EXPECT_EQ_MSG(test, decompress(test, &b, b.c, len),
				    -EINVAL, "ll %u", ll);
		len = first_offset(m, b.c, ll, 2, ll);
		KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
		len = first_offset(m, b.c, ll, 2, ll + 1);
		KUNIT_EXPECT_EQ_MSG(test, decompress(test, &b, b.c, len),
				    -EINVAL, "ll %u", ll);
	}

	/* a match one byte past the block's end */
	len = match_then_literals(m, b.c, SEQLZ_BLOCK_SIZE - 1, 0, &w);
	KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
	len = match_then_literals(m, b.c, SEQLZ_BLOCK_SIZE, 0, &w);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);

	/*
	 * A block that wants more bits than it has: its match length value
	 * 2048 ends in 11 bits 0, so its last byte is 0. Without that byte the
	 * decoder reads the same zeros, the block decodes to the same bytes,
	 * and only the bits it took past the end make it invalid.
	 */
	len = match_then_literals(m, b.c, SEQLZ_MIN_MATCH + SEQLZ_ML_CAP + 2048,
				  SEQLZ_BLOCK_SIZE - 1 - SEQLZ_MIN_MATCH -
					  SEQLZ_ML_CAP - 2048,
				  &w);
	KUNIT_ASSERT_EQ(test, b.c[len - 1], 0);
	KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len - 1), -EINVAL);

	/* raw literals: more than the block has bytes, or than it holds */
	len = literals_only(b.c, SEQLZ_BLOCK_SIZE);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len - 1), -EINVAL);
	len = literals_only(b.c, SEQLZ_BLOCK_SIZE + 1);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);

	/* coded literals, more than the block holds */
	len = coded_literals(m, b.c, 100, same, 0x65, false, GOOD, &padding);
	KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
	put_unaligned_le16(SEQLZ_LIT_CODED | (SEQLZ_BLOCK_SIZE + 1), b.c);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);
}

static struct kunit_case seqlz_test_cases[] = {
	KUNIT_CASE(seqlz_test_tables),
	KUNIT_CASE(seqlz_test_codes),
	KUNIT_CASE(seqlz_test_example),
	KUNIT_CASE(seqlz_test_round_trip),
	KUNIT_CASE(seqlz_test_delta_distances),
	KUNIT_CASE(seqlz_test_small_dst),
	KUNIT_CASE(seqlz_test_damaged),
	KUNIT_CASE(seqlz_test_one_sequence),
	KUNIT_CASE(seqlz_test_invalid),
	KUNIT_CASE(seqlz_test_coded_literals),
	KUNIT_CASE(seqlz_test_own_table),
	{}
};

static struct kunit_suite seqlz_test_suite = {
	.name = "seqlz",
	.test_cases = seqlz_test_cases,
};

kunit_test_suite(seqlz_test_suite);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("KUnit tests for seqlz");
