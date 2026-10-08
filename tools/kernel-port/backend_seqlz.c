// SPDX-License-Identifier: GPL-2.0-only OR MIT

#define pr_fmt(fmt) "seqlz: " fmt

#include <linux/kernel.h>
#include <linux/seqlz.h>
#include <linux/slab.h>

#include "backend_seqlz.h"

/* the work memory of compression and of decompression, per CPU */
struct seqlz_ctx {
	u8 cmem[SEQLZ_MEM_COMPRESS];
	u8 dmem[SEQLZ_MEM_DECOMPRESS];
};

static void seqlz_release_params(struct zcomp_params *params)
{
}

/*
 * Level 1 keeps the literals as they are, level 2, the default, Huffman codes
 * them where that saves at least 1/16 of them: smaller pages for more time.
 * The tables are the library's, built once.
 */
static int seqlz_setup_params(struct zcomp_params *params)
{
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
		return -EINVAL;
	}
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
	int ret;

	if (req->src_len != PAGE_SIZE)
		return -EINVAL;
	ret = seqlz_compress(req->src, req->dst, req->dst_len, c->cmem,
			     params->level);
	if (ret < 0)
		return ret;
	req->dst_len = ret;
	return 0;
}

static int seqlz_zcomp_decompress(struct zcomp_params *params,
				  struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	struct seqlz_ctx *c = ctx->context;
	int ret;

	if (req->dst_len < PAGE_SIZE)
		return -EINVAL;
	ret = seqlz_decompress(req->src, req->src_len, req->dst, c->dmem);
	if (ret)
		return ret;
	req->dst_len = PAGE_SIZE;
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
