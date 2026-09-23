// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* bytelz (explore/bytelz.h) as a zram backend, for the VM test of run.sh: a hash table per CPU. */
#include <linux/kernel.h>
#include <linux/slab.h>

#include "backend_bytelz.h"
#include "bytelz.h"

static int bz_setup_params(struct zcomp_params *params)
{
	return 0;
}

static void bz_release_params(struct zcomp_params *params)
{
}

static int bz_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	ctx->context = kzalloc(sizeof(struct bytelz_state), GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}

static void bz_destroy(struct zcomp_ctx *ctx)
{
	kfree(ctx->context);
	ctx->context = NULL;
}

static int bz_compress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	unsigned int len;

	if (req->src_len != BYTELZ_PAGE)
		return -EINVAL;
	len = bytelz_compress(ctx->context, req->src, req->dst, req->dst_len);
	if (!len)
		return -EINVAL;
	req->dst_len = len;
	return 0;
}

static int bz_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	if (req->dst_len < BYTELZ_PAGE || bytelz_decode(req->src, req->src_len, req->dst))
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_bytelz = {
	.compress	= bz_compress,
	.decompress	= bz_decompress,
	.create_ctx	= bz_create,
	.destroy_ctx	= bz_destroy,
	.setup_params	= bz_setup_params,
	.release_params	= bz_release_params,
	.name		= "bytelz",
};
