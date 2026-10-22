// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz-1 to seqlz-3 as crypto compressors for zram on Linux 4.14,
 * which takes any registered compressor by name. Only for measuring on the
 * Mi 9T; the tables are compiled in. build.sh renames them to seqlz-<name>-1
 * and so on, so that several builds can be loaded at the same time.
 */
#include <linux/crypto.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "seqlz.h"

/*
 * per tfm, so per CPU in zram: the matcher's hash table, and the scratch for
 * coded literals
 */
struct q_ctx {
	struct seqlz_state st;
	unsigned char scratch[SEQLZ_SCRATCH];
};

static int q_compress(struct crypto_tfm *tfm, const u8 *src, unsigned int slen,
		      u8 *dst, unsigned int *dlen, unsigned int level)
{
	struct q_ctx *c = crypto_tfm_ctx(tfm);
	unsigned int len;

	if (slen != SEQLZ_BLOCK_SIZE)
		return -EINVAL;
	len = seqlz_compress(&c->st, src, dst, *dlen, level);
	if (!len)
		return -EINVAL;
	*dlen = len;
	return 0;
}

/* the compressor of one level */
#define Q_COMPRESS(level)                                                    \
	static int q_compress_##level(struct crypto_tfm *tfm, const u8 *src, \
				      unsigned int slen, u8 *dst,            \
				      unsigned int *dlen)                    \
	{                                                                    \
		return q_compress(tfm, src, slen, dst, dlen, level);         \
	}
Q_COMPRESS(1)
Q_COMPRESS(2)
Q_COMPRESS(3)

static int q_decompress(struct crypto_tfm *tfm, const u8 *src,
			unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct q_ctx *c = crypto_tfm_ctx(tfm);

	if (*dlen < SEQLZ_BLOCK_SIZE ||
	    seqlz_decode(src, slen, dst, c->scratch))
		return -EINVAL;
	*dlen = SEQLZ_BLOCK_SIZE;
	return 0;
}

/* one per level; build.sh puts the build's name into each */
#define Q_ALG(level)                                                         \
	{                                                                    \
		.cra_name = "seqlz-" #level,                                 \
		.cra_driver_name = "seqlz-" #level "-generic",               \
		.cra_flags = CRYPTO_ALG_TYPE_COMPRESS,                       \
		.cra_ctxsize = sizeof(struct q_ctx),                         \
		.cra_module = THIS_MODULE,                                   \
		.cra_u = { .compress = { .coa_compress = q_compress_##level, \
					 .coa_decompress = q_decompress } }, \
	}

static struct crypto_alg algs[] = { Q_ALG(1), Q_ALG(2), Q_ALG(3) };

static int __init q_init(void)
{
	return crypto_register_algs(algs, ARRAY_SIZE(algs));
}

static void __exit q_exit(void)
{
	crypto_unregister_algs(algs, ARRAY_SIZE(algs));
}

module_init(q_init);
module_exit(q_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("seqlz-1 to seqlz-3 for zram, for measuring");
