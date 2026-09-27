// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* seqlz-fast (explore/seqlz.h) as a zram backend, for the VM test of run.sh: the tables compiled in,
 * a hash table per CPU. */
#include <linux/kernel.h>

/* zram-prefetch.patch */
extern int zram_prefetch;
#include <linux/slab.h>
#include <linux/mm.h>

#include "backend_seqlz.h"
#include "page_lz.h"
#include "seqlz.h"

static int sz_setup_params(struct zcomp_params *params)
{
	struct seqlz_tables *t = kzalloc(seqlz_tables_size(), GFP_KERNEL);

	if (!t)
		return -ENOMEM;
	if (seqlz_tables_init(t, &seqlz_default_own) || !seqlz_all_symbols(t)) {
		kfree(t);
		return -EINVAL;
	}
	params->drv_data = t;
	return 0;
}

static void sz_release_params(struct zcomp_params *params)
{
	kfree(params->drv_data);
	params->drv_data = NULL;
}

/* the hash table, and the scratch for seqlz-fast-lit's literals */
struct sz_ctx {
	struct seqlz_state st;
	unsigned char scratch[SEQLZ_SCRATCH];
};

static int sz_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	ctx->context = kzalloc(sizeof(struct sz_ctx), GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}

static void sz_destroy(struct zcomp_ctx *ctx)
{
	kfree(ctx->context);
	ctx->context = NULL;
}

static int sz_compress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	unsigned int len;

	if (req->src_len != SEQLZ_PAGE)
		return -EINVAL;
	len = seqlz_compress(params->drv_data, &((struct sz_ctx *)ctx->context)->st, req->src, req->dst,
			     req->dst_len);
	if (!len)
		return -EINVAL;
	req->dst_len = len;
	return 0;
}

static int sz_lit_compress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	unsigned int len;

	if (req->src_len != SEQLZ_PAGE)
		return -EINVAL;
	len = seqlz_compress_coded(params->drv_data, &((struct sz_ctx *)ctx->context)->st, req->src, req->dst,
				   req->dst_len);
	if (!len)
		return -EINVAL;
	req->dst_len = len;
	return 0;
}

static int sz_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	/* EXPERIMENT: the compressed data requested first, here instead of in zram_drv.c */
	if (READ_ONCE(zram_prefetch) & 8)
		for (unsigned int q = 64; q < req->src_len; q += 64)
			PAGE_LZ_PREFETCH((const char *)req->src + q);
	if (req->dst_len < SEQLZ_PAGE || seqlz_decode_scratch(params->drv_data, req->src, req->src_len, req->dst,
							      ((struct sz_ctx *)ctx->context)->scratch))
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_seqlz = {
	.compress	= sz_compress,
	.decompress	= sz_decompress,
	.create_ctx	= sz_create,
	.destroy_ctx	= sz_destroy,
	.setup_params	= sz_setup_params,
	.release_params	= sz_release_params,
	.name		= "seqlz",
};

/* seqlz-fast with the literals coded too */
const struct zcomp_ops backend_seqlz_lit = {
	.compress	= sz_lit_compress,
	.decompress	= sz_decompress,
	.create_ctx	= sz_create,
	.destroy_ctx	= sz_destroy,
	.setup_params	= sz_setup_params,
	.release_params	= sz_release_params,
	.name		= "seqlz-lit",
};
