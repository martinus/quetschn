// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* lz4page (explore/lz4page.h) as a zram backend, for the VM test of run.sh: lz4's block format from a
 * compressor for pages, read by LZ4_decompress_safe() as backend_lz4.c reads it, without a prefetch. */
#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/slab.h>

#include "backend_lz4page.h"
#include "lz4page.h"

static int lp_setup_params(struct zcomp_params *params)
{
	return 0;
}

static void lp_release_params(struct zcomp_params *params)
{
}

static int lp_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	ctx->context = kzalloc(sizeof(unsigned short) << LZ4PAGE_HASH_BITS, GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}

static void lp_destroy(struct zcomp_ctx *ctx)
{
	kfree(ctx->context);
	ctx->context = NULL;
}

static int lp_compress_with(struct zcomp_ctx *ctx, struct zcomp_req *req, unsigned int flags)
{
	unsigned int len;

	if (req->src_len != PAGE_SIZE)
		return -EINVAL;
	len = lz4page_compress(ctx->context, req->src, req->dst, req->dst_len, flags);
	if (!len)
		return -EINVAL;
	req->dst_len = len;
	return 0;
}

static int lp_compress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	return lp_compress_with(ctx, req, 0);
}

static int lp_lazy_compress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	return lp_compress_with(ctx, req, LZ4PAGE_LAZY);
}

static int lp_both_compress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	return lp_compress_with(ctx, req, LZ4PAGE_TWO_WAY | LZ4PAGE_LAZY);
}

static int lp_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	int ret = LZ4_decompress_safe(req->src, req->dst, req->src_len, req->dst_len);

	return ret < 0 ? -EINVAL : 0;
}

const struct zcomp_ops backend_lz4page = {
	.compress	= lp_compress,
	.decompress	= lp_decompress,
	.create_ctx	= lp_create,
	.destroy_ctx	= lp_destroy,
	.setup_params	= lp_setup_params,
	.release_params	= lp_release_params,
	.name		= "lz4page",
};

const struct zcomp_ops backend_lz4page_lazy = {
	.compress	= lp_lazy_compress,
	.decompress	= lp_decompress,
	.create_ctx	= lp_create,
	.destroy_ctx	= lp_destroy,
	.setup_params	= lp_setup_params,
	.release_params	= lp_release_params,
	.name		= "lz4page-lazy",
};

const struct zcomp_ops backend_lz4page_both = {
	.compress	= lp_both_compress,
	.decompress	= lp_decompress,
	.create_ctx	= lp_create,
	.destroy_ctx	= lp_destroy,
	.setup_params	= lp_setup_params,
	.release_params	= lp_release_params,
	.name		= "lz4page-both",
};
