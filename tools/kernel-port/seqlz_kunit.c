// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * KUnit tests for seqlz: the tables against the hashes the specification
 * gives, its worked example, round trips of generated blocks at every level,
 * damaged blocks, and blocks made here from the specification's rules, each
 * valid or invalid by one rule.
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

/* buffers for one test, freed by KUnit */
struct bufs {
	u8 *block, *c, *out;
	void *cmem, *dmem;
};

static void get_bufs(struct kunit *test, struct bufs *b)
{
	b->block = kunit_kmalloc(test, SEQLZ_BLOCK_SIZE, GFP_KERNEL);
	b->c = kunit_kmalloc(test, SEQLZ_COMPRESS_BOUND, GFP_KERNEL);
	b->out = kunit_kmalloc(test, SEQLZ_BLOCK_SIZE, GFP_KERNEL);
	b->cmem = kunit_kmalloc(test, SEQLZ_MEM_COMPRESS, GFP_KERNEL);
	b->dmem = kunit_kmalloc(test, SEQLZ_MEM_DECOMPRESS, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b->block);
	KUNIT_ASSERT_NOT_NULL(test, b->c);
	KUNIT_ASSERT_NOT_NULL(test, b->out);
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
		KUNIT_ASSERT_EQ(test, b.out[k], k % 2 ? 'b' : 'a');
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

	switch (kind % 9) {
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
			p[k + 1] = v;
			p[k + 2] = v >> 8;
			p[k + 3] = v >> 16;
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

static void seqlz_test_round_trip(struct kunit *test)
{
	struct rnd_state rng;
	struct bufs b;
	int round, level, len, coded = 0, own = 0, deltas = 0;

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
			KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len),
					0);
			KUNIT_ASSERT_MEMEQ(test, b.out, b.block,
					   SEQLZ_BLOCK_SIZE);
			/*
			 * without work memory, only blocks without coded
			 * literals: zram's level 1 has no decompression context
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
	/* the blocks have to give both layouts, own tables and deltas */
	KUNIT_EXPECT_GE(test, coded, 20);
	KUNIT_EXPECT_GE(test, own, 10);
	KUNIT_EXPECT_GE(test, deltas, 10);
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
 * A block of n raw literals k * 7 and no bits: valid only for
 * n = SEQLZ_BLOCK_SIZE
 */
static unsigned int literals_only(u8 *c, unsigned int n)
{
	unsigned int k;

	c[0] = n;
	c[1] = n >> 8;
	for (k = 0; k < n; k++)
		c[2 + k] = k * 7;
	return 2 + n;
}

/*
 * A block of SEQLZ_BLOCK_SIZE raw literals and a token that takes them all, of
 * class cls, its 4 offset bits if cls is 1, then ll: invalid, the last
 * sequence has no token.
 */
static unsigned int one_sequence(struct maker *m, u8 *c, unsigned int cls)
{
	unsigned int len = literals_only(c, SEQLZ_BLOCK_SIZE);
	struct bits w = { .p = c + len };

	token(m, &w, SEQLZ_LL_CAP + (cls << (SEQLZ_LL_BITS + SEQLZ_ML_BITS)));
	if (cls == 1)
		put(&w, 1, 4);
	put_len_value(m, &w, &m->ll, SEQLZ_BLOCK_SIZE - SEQLZ_LL_CAP);
	return len + bytes(&w);
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

static void seqlz_test_one_sequence(struct kunit *test)
{
	struct maker *m = get_maker(test);
	unsigned int len, padding, k;
	struct bufs b;

	get_bufs(test, &b);
	len = one_match(m, b.c, &padding);
	KUNIT_ASSERT_EQ(test, decompress(test, &b, b.c, len), 0);
	for (k = 0; k < SEQLZ_BLOCK_SIZE; k++)
		KUNIT_ASSERT_EQ(test, b.out[k], 7);
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
		KUNIT_ASSERT_EQ(test, b.out[k], (u8)(k * 7));
	/* and as many literals as the block has bytes */
	len = literals_only(b.c, SEQLZ_BLOCK_SIZE - 1);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);
	/* the last sequence has no token, of any class */
	len = one_sequence(m, b.c, 0);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);
	len = one_sequence(m, b.c, 1);
	KUNIT_EXPECT_EQ(test, decompress(test, &b, b.c, len), -EINVAL);
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
	/* only for a block with its own table */
	OWN_OVERFULL, /* a third byte with a code of 1 bit */
	OWN_ABOVE_255, /* the second byte with a code is 256 */
	OWN_PADDING_BIT, /* the lowest bit of the table's last byte set */
	OWN_CUT, /* the block ends in the table */
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
		KUNIT_ASSERT_EQ(test, b->out[k], lit(min(k, n - 1), arg));
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

static struct kunit_case seqlz_test_cases[] = {
	KUNIT_CASE(seqlz_test_tables),
	KUNIT_CASE(seqlz_test_example),
	KUNIT_CASE(seqlz_test_round_trip),
	KUNIT_CASE(seqlz_test_damaged),
	KUNIT_CASE(seqlz_test_one_sequence),
	KUNIT_CASE(seqlz_test_coded_literals),
	KUNIT_CASE(seqlz_test_own_table),
	{}
};

static struct kunit_suite seqlz_test_suite = {
	.name = "seqlz",
	.test_cases = seqlz_test_cases,
};

kunit_test_suite(seqlz_test_suite);

MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("KUnit tests for seqlz");
