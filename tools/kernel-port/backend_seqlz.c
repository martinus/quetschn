// SPDX-License-Identifier: GPL-2.0-only OR MIT

#define pr_fmt(fmt) "seqlz: " fmt

#include <linux/kernel.h>
#include <linux/seqlz.h>
#include <linux/slab.h>

#include "backend_seqlz.h"

/*
 * Level 1 keeps the literals as they are, level 2, the default, Huffman codes
 * them where that saves at least 1/16 of them: smaller pages for more time.
 */
#define SEQLZ_LEVEL_RAW 1
#define SEQLZ_LEVEL_CODED 2

/* the matcher's hash table, and the scratch the literals are decoded into */
struct seqlz_ctx {
	struct seqlz_state st;
	u8 scratch[SEQLZ_SCRATCH];
};

static void seqlz_release_params(struct zcomp_params *params)
{
	kvfree(params->drv_data);
	params->drv_data = NULL;
}

static int seqlz_setup_params(struct zcomp_params *params)
{
	struct seqlz_tables *t;

	if (params->dict_sz) {
		pr_err("dictionary is not supported\n");
		return -EOPNOTSUPP;
	}
	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = SEQLZ_LEVEL_CODED;
	if (params->level != SEQLZ_LEVEL_RAW &&
	    params->level != SEQLZ_LEVEL_CODED) {
		pr_err("compression level %d is not supported\n",
		       params->level);
		return -EOPNOTSUPP;
	}

	t = kvzalloc(seqlz_tables_size(), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	/* the encoder needs a code for every symbol */
	if (seqlz_tables_init(t, &seqlz_default_own) || !seqlz_all_symbols(t)) {
		kvfree(t);
		return -EINVAL;
	}
	params->drv_data = t;
	return 0;
}

static int seqlz_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct seqlz_ctx *c = kvzalloc_obj(*c);

	if (!c)
		return -ENOMEM;
	ctx->context = c;
	return 0;
}

static void seqlz_destroy(struct zcomp_ctx *ctx)
{
	kvfree(ctx->context);
}

static int seqlz_zcomp_compress(struct zcomp_params *params,
				struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	struct seqlz_ctx *c = ctx->context;
	unsigned int len;

	if (req->src_len != SEQLZ_PAGE)
		return -EINVAL;
	len = seqlz_compress(params->drv_data, &c->st, req->src, req->dst,
			     req->dst_len, params->level == SEQLZ_LEVEL_CODED);
	if (!len)
		return -EINVAL;
	req->dst_len = len;
	return 0;
}

static int seqlz_zcomp_decompress(struct zcomp_params *params,
				  struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	struct seqlz_ctx *c = ctx->context;
	int ret;

	if (req->dst_len < SEQLZ_PAGE)
		return -EINVAL;
	ret = seqlz_decode(params->drv_data, req->src, req->src_len, req->dst,
			   c->scratch);
	if (ret)
		return ret;
	req->dst_len = SEQLZ_PAGE;
	return 0;
}

const struct zcomp_ops backend_seqlz = {
	.compress	= seqlz_zcomp_compress,
	.decompress	= seqlz_zcomp_decompress,
	.create_ctx	= seqlz_create,
	.destroy_ctx	= seqlz_destroy,
	.setup_params	= seqlz_setup_params,
	.release_params	= seqlz_release_params,
	.name		= "seqlz",
};
