// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* seqlz (src/seqlz.h) as zram backends, for the VM test of run.sh: seqlz with raw literals
 * (seqlz-fast) and seqlz-lit with coded ones (seqlz-fast-lit), the tables compiled in, a hash table per
 * CPU. */
#include <linux/kernel.h>

/* zram-prefetch.patch */
extern int zram_prefetch;
#include <linux/slab.h>
#include <linux/mm.h>

#include "backend_seqlz.h"
#include "page_lz.h"
#include "seqlz.h"

/* zram-prefetch.patch, after backend_seqlz.h for the zcomp types */
extern void (*zram_warm_fn)(struct zcomp_params *params, struct zcomp_ctx *ctx, int what);

/* the hash table, and the scratch for seqlz-fast-lit's literals */
struct sz_ctx {
	struct seqlz_state st;
	unsigned char scratch[SEQLZ_SCRATCH];
};

/*
 * EXPERIMENT, zram.zram_warm of zram-prefetch.patch: before a decompression, 1 reads every cache line
 * of the tables and of the scratch, 2 decodes a page with coded literals and matches, which warms
 * the decoder's code too, and its branch predictors on another page. For one CPU only: the VM of run.sh
 * has one.
 */
static unsigned char warm_src[2 * SEQLZ_PAGE], warm_dst[SEQLZ_PAGE];
static unsigned int warm_len;

static void sz_warm(struct zcomp_params *params, struct zcomp_ctx *ctx, int what)
{
	struct sz_ctx *c = ctx->context;

	if (what == 1) {
		const volatile unsigned char *p = params->drv_data;
		unsigned long k, sum = 0;

		for (k = 0; k < seqlz_tables_size(); k += 64)
			sum += p[k];
		for (k = 0; k < SEQLZ_SCRATCH; k += 64)
			sum += ((volatile unsigned char *)c->scratch)[k];
		(void)sum;
	} else if (what == 2 && warm_len) {
		seqlz_decode(params->drv_data, warm_src, warm_len, warm_dst, c->scratch);
	}
}

static int sz_setup_params(struct zcomp_params *params)
{
	struct seqlz_tables *t = kzalloc(seqlz_tables_size(), GFP_KERNEL);
	struct seqlz_state *st;
	unsigned int k;

	if (!t)
		return -ENOMEM;
	if (seqlz_tables_init(t, &seqlz_default_own) || !seqlz_all_symbols(t)) {
		kfree(t);
		return -EINVAL;
	}
	params->drv_data = t;
	/* the warm-up page: random letters, skewed, and spaces: about 100 sequences and coded literals */
	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (st) {
		unsigned int x = 1;

		for (k = 0; k < SEQLZ_PAGE; k++) {
			x = x * 1103515245U + 12345U;
			warm_dst[k] = (x >> 16) % 5 == 0 ? ' ' :
				      "etaoinshrdlucmfwypvbgkqjxz"[((x >> 16) % 89) % 26 % (1 + (x >> 28))];
		}
		warm_len = seqlz_compress(t, st, warm_dst, warm_src, sizeof(warm_src), 1);
		kfree(st);
	}
	zram_warm_fn = sz_warm;
	return 0;
}

static void sz_release_params(struct zcomp_params *params)
{
	kfree(params->drv_data);
	params->drv_data = NULL;
}

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
			     req->dst_len, 0);
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
	len = seqlz_compress(params->drv_data, &((struct sz_ctx *)ctx->context)->st, req->src, req->dst,
				   req->dst_len, 1);
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
	if (req->dst_len < SEQLZ_PAGE || seqlz_decode(params->drv_data, req->src, req->src_len, req->dst,
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
