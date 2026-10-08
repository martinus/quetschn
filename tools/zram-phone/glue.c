// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz-fast and seqlz-fast-lit, levels 1 and 2, and seqlz-fast-hc and
 * seqlz-fast-hc4, levels 3 and 4, as crypto compressors for zram on Linux
 * 4.14, which takes any registered compressor by name. Only for measuring on
 * the Mi 9T; the tables are compiled in. build.sh renames them to
 * seqlz-<name>, seqlz-<name>-lit, seqlz-<name>-hc and seqlz-<name>-hc4, so that
 * several builds can be loaded at the same time.
 */
#include <linux/crypto.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "page_lz.h"
#include "seqlz.h"

static struct seqlz_tables *tables;

/*
 * per tfm, so per CPU in zram: the matcher's hash table or the chain of levels
 * 3 and 4, and the scratch for coded literals
 */
struct q_ctx {
	union {
		struct seqlz_state st;
		struct seqlz_hc_state hc;
	};
	unsigned char scratch[SEQLZ_SCRATCH];
};

static int q_compress(struct crypto_tfm *tfm, const u8 *src, unsigned int slen,
		      u8 *dst, unsigned int *dlen, int coded)
{
	struct q_ctx *c = crypto_tfm_ctx(tfm);
	unsigned int len;

	if (slen != SEQLZ_PAGE)
		return -EINVAL;
	len = seqlz_compress(tables, &c->st, src, dst, *dlen, coded);
	if (!len)
		return -EINVAL;
	*dlen = len;
	return 0;
}

static int q_compress_fast(struct crypto_tfm *tfm, const u8 *src,
			   unsigned int slen, u8 *dst, unsigned int *dlen)
{
	return q_compress(tfm, src, slen, dst, dlen, 0);
}

static int q_compress_lit(struct crypto_tfm *tfm, const u8 *src,
			  unsigned int slen, u8 *dst, unsigned int *dlen)
{
	return q_compress(tfm, src, slen, dst, dlen, 1);
}

static int q_compress_hc(struct crypto_tfm *tfm, const u8 *src,
			 unsigned int slen, u8 *dst, unsigned int *dlen,
			 bool deep)
{
	struct q_ctx *c = crypto_tfm_ctx(tfm);
	unsigned int len;

	if (slen != SEQLZ_PAGE)
		return -EINVAL;
	len = seqlz_compress_hc(tables, &c->hc, src, dst, *dlen, deep);
	if (!len)
		return -EINVAL;
	*dlen = len;
	return 0;
}

static int q_compress_hc3(struct crypto_tfm *tfm, const u8 *src,
			  unsigned int slen, u8 *dst, unsigned int *dlen)
{
	return q_compress_hc(tfm, src, slen, dst, dlen, false);
}

static int q_compress_hc4(struct crypto_tfm *tfm, const u8 *src,
			  unsigned int slen, u8 *dst, unsigned int *dlen)
{
	return q_compress_hc(tfm, src, slen, dst, dlen, true);
}

static int q_decompress(struct crypto_tfm *tfm, const u8 *src,
			unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct q_ctx *c = crypto_tfm_ctx(tfm);

	if (*dlen < SEQLZ_PAGE ||
	    seqlz_decode(tables, src, slen, dst, c->scratch))
		return -EINVAL;
	*dlen = SEQLZ_PAGE;
	return 0;
}

static struct crypto_alg algs[] = {
	{
		.cra_name = "seqlz-fast",
		.cra_driver_name = "seqlz-fast-generic",
		.cra_flags = CRYPTO_ALG_TYPE_COMPRESS,
		.cra_ctxsize = sizeof(struct q_ctx),
		.cra_module = THIS_MODULE,
		.cra_u = { .compress = { .coa_compress = q_compress_fast,
					 .coa_decompress = q_decompress } },
	},
	{
		.cra_name = "seqlz-fast-lit",
		.cra_driver_name = "seqlz-fast-lit-generic",
		.cra_flags = CRYPTO_ALG_TYPE_COMPRESS,
		.cra_ctxsize = sizeof(struct q_ctx),
		.cra_module = THIS_MODULE,
		.cra_u = { .compress = { .coa_compress = q_compress_lit,
					 .coa_decompress = q_decompress } },
	},
	{
		.cra_name = "seqlz-fast-hc",
		.cra_driver_name = "seqlz-fast-hc-generic",
		.cra_flags = CRYPTO_ALG_TYPE_COMPRESS,
		.cra_ctxsize = sizeof(struct q_ctx),
		.cra_module = THIS_MODULE,
		.cra_u = { .compress = { .coa_compress = q_compress_hc3,
					 .coa_decompress = q_decompress } },
	},
	{
		.cra_name = "seqlz-fast-hc4",
		.cra_driver_name = "seqlz-fast-hc4-generic",
		.cra_flags = CRYPTO_ALG_TYPE_COMPRESS,
		.cra_ctxsize = sizeof(struct q_ctx),
		.cra_module = THIS_MODULE,
		.cra_u = { .compress = { .coa_compress = q_compress_hc4,
					 .coa_decompress = q_decompress } },
	},
};

static int __init q_init(void)
{
	int ret;

	tables = vzalloc(seqlz_tables_size());
	if (!tables)
		return -ENOMEM;
	if (seqlz_tables_init(tables, &seqlz_default_own) ||
	    !seqlz_all_symbols(tables)) {
		vfree(tables);
		return -EINVAL;
	}
	ret = crypto_register_algs(algs, ARRAY_SIZE(algs));
	if (ret)
		vfree(tables);
	return ret;
}

static void __exit q_exit(void)
{
	crypto_unregister_algs(algs, ARRAY_SIZE(algs));
	vfree(tables);
}

module_init(q_init);
module_exit(q_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("seqlz's levels 1 to 4 for zram, for measuring");
