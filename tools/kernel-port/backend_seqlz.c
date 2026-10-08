// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * port.py keeps the ZCOMP_RW_SPLIT parts for a tree whose zram has separate
 * compression and decompression contexts, struct zcomp_cstrm in zcomp.h, and
 * the others for a tree without; the copy in the tree has no #ifdef left.
 */

#define pr_fmt(fmt) "seqlz: " fmt

#include <linux/kernel.h>
#include <linux/seqlz.h>
#include <linux/slab.h>

#include "backend_seqlz.h"

#ifndef ZCOMP_RW_SPLIT
/*
 * the work memory of decompression and of compression, per CPU; the latter's
 * size depends on the level
 */
struct seqlz_ctx {
	u8 dmem[SEQLZ_MEM_DECOMPRESS];
	u8 cmem[];
};
#endif

/* levels 3 and 4 need a hash chain, levels 1 and 2 a table */
static size_t seqlz_cmem_size(const struct zcomp_params *params)
{
	return params->level >= SEQLZ_LEVEL_HC ? SEQLZ_MEM_COMPRESS_HC :
						 SEQLZ_MEM_COMPRESS;
}

static void seqlz_release_params(struct zcomp_params *params)
{
}

/*
 * Level 1 keeps the literals as they are, level 2, the default, Huffman codes
 * them where that saves at least 1/16 of them: smaller pages for more time.
 * Levels 3 and 4 search longer for matches, smaller pages for more time to
 * compress, the same to decompress. The tables are the library's, built once.
 */
static int seqlz_setup_params(struct zcomp_params *params)
{
	if (params->dict_sz) {
		pr_err("dictionary is not supported\n");
		return -EOPNOTSUPP;
	}
	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = SEQLZ_LEVEL_CODED;
	if (params->level < SEQLZ_LEVEL_RAW ||
	    params->level > SEQLZ_LEVEL_HC_DEEP) {
		pr_err("compression level %d is not supported\n",
		       params->level);
		return -EINVAL;
	}
	return 0;
}

#ifdef ZCOMP_RW_SPLIT
static int seqlz_create_cctx(struct zcomp_params *params,
			     struct zcomp_ctx *ctx)
{
	ctx->context = kvzalloc(seqlz_cmem_size(params), GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}

/* level 1 decompresses without work memory, its pages have no coded literals */
static int seqlz_create_dctx(struct zcomp_params *params,
			     struct zcomp_ctx *ctx)
{
	if (params->level == SEQLZ_LEVEL_RAW)
		return 0;
	ctx->context = kvzalloc(SEQLZ_MEM_DECOMPRESS, GFP_KERNEL);
	return ctx->context ? 0 : -ENOMEM;
}
#else
static int seqlz_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct seqlz_ctx *c = kvzalloc_flex(*c, cmem, seqlz_cmem_size(params));

	if (!c)
		return -ENOMEM;
	ctx->context = c;
	return 0;
}
#endif

static void seqlz_destroy(struct zcomp_ctx *ctx)
{
	kvfree(ctx->context);
}

static int seqlz_zcomp_compress(struct zcomp_params *params,
				struct zcomp_ctx *ctx, struct zcomp_req *req)
{
#ifdef ZCOMP_RW_SPLIT
	void *wrkmem = ctx->context;
#else
	void *wrkmem = ((struct seqlz_ctx *)ctx->context)->cmem;
#endif
	int ret;

	if (req->src_len != PAGE_SIZE)
		return -EINVAL;
	ret = seqlz_compress(req->src, req->dst, req->dst_len, wrkmem,
			     params->level);
	if (ret < 0)
		return ret;
	req->dst_len = ret;
	return 0;
}

static int seqlz_zcomp_decompress(struct zcomp_params *params,
				  struct zcomp_ctx *ctx, struct zcomp_req *req)
{
#ifdef ZCOMP_RW_SPLIT
	void *wrkmem = ctx->context;
#else
	void *wrkmem = ((struct seqlz_ctx *)ctx->context)->dmem;
#endif
	int ret;

	if (req->dst_len < PAGE_SIZE)
		return -EINVAL;
	ret = seqlz_decompress(req->src, req->src_len, req->dst, wrkmem);
	if (ret)
		return ret;
	req->dst_len = PAGE_SIZE;
	return 0;
}

const struct zcomp_ops backend_seqlz = {
	.compress	= seqlz_zcomp_compress,
	.decompress	= seqlz_zcomp_decompress,
#ifdef ZCOMP_RW_SPLIT
	.create_cctx	= seqlz_create_cctx,
	.destroy_cctx	= seqlz_destroy,
	.create_dctx	= seqlz_create_dctx,
	.destroy_dctx	= seqlz_destroy,
#else
	.create_ctx	= seqlz_create,
	.destroy_ctx	= seqlz_destroy,
#endif
	.setup_params	= seqlz_setup_params,
	.release_params	= seqlz_release_params,
	.name		= "seqlz",
};
