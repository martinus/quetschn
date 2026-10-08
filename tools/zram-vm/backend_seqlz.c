// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* seqlz (src/seqlz.h) as zram backends, for the VM test of run.sh: seqlz with raw literals
 * (seqlz-fast) and seqlz-lit with coded ones (seqlz-fast-lit), the tables compiled in, a hash table per
 * CPU; seqlz-hc, levels 3 and 4 (seqlz-hc:4), with the hash chain instead. */
#include <linux/kernel.h>

/* zram-prefetch.patch */
extern int zram_prefetch;
#include <linux/slab.h>
#include <linux/mm.h>

#include "backend_seqlz.h"
#include "page_lz.h"
#include "seqlz.h"

/* zram-prefetch.patch, after backend_seqlz.h for the zcomp types */
extern void (*zram_warm_fn)(struct zcomp_params *params, struct zcomp_ctx *ctx, int what, const void *src,
			    unsigned int size);

/* the hash table or seqlz-hc's chain, and the scratch for coded literals */
struct sz_ctx {
	union {
		struct seqlz_state st;
		struct seqlz_hc_state hc;
	};
	unsigned char scratch[SEQLZ_SCRATCH];
};

/*
 * EXPERIMENT, zram.zram_warm of zram-prefetch.patch: before a decompression, 1 reads every cache line
 * of the tables and of the scratch, 2 decodes a page with coded literals and matches, which warms
 * the decoder's code too, and its branch predictors on another page, 5 decodes the compressed page of
 * the decompression before, a real one, and keeps a copy of this one for the next, 7 decodes this very
 * page once before, as the read benchmark does when one page is read several times in a row. For one CPU
 * only: the VM of run.sh has one.
 */
static unsigned char warm_src[2 * SEQLZ_PAGE], warm_dst[SEQLZ_PAGE], warm_prev[2 * SEQLZ_PAGE];
static unsigned int warm_len, warm_prev_len;

static void sz_warm(struct zcomp_params *params, struct zcomp_ctx *ctx, int what, const void *src,
		    unsigned int size)
{
	struct sz_ctx *c = ctx->context;

	if (what == 1) {
		const volatile unsigned char *p = params->drv_data;
		unsigned long k, sum = 0;

		/* one byte of each cache line of 64 bytes */
		for (k = 0; k < seqlz_tables_size(); k += 64)
			sum += p[k];
		for (k = 0; k < SEQLZ_SCRATCH; k += 64)
			sum += ((volatile unsigned char *)c->scratch)[k];
		(void)sum;
	} else if (what == 2 && warm_len) {
		seqlz_decode(params->drv_data, warm_src, warm_len, warm_dst, c->scratch);
	} else if (what == 5 && size <= sizeof(warm_prev)) {
		if (warm_prev_len)
			seqlz_decode(params->drv_data, warm_prev, warm_prev_len, warm_dst, c->scratch);
		memcpy(warm_prev, src, size);
		warm_prev_len = size;
	} else if (what == 7) {
		seqlz_decode(params->drv_data, src, size, warm_dst, c->scratch);
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
	/*
	 * the warm-up page: random letters, skewed, and spaces: about 100 sequences and coded literals. x is
	 * the generator of the C standard's example rand(), its upper 16 bits used: one byte in 5 a space,
	 * the others one of the first 1 to 16 letters in English order of frequency, by the top 4 bits.
	 */
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

/* seqlz-hc: level 3 by default, or 4 */
static int sz_hc_setup_params(struct zcomp_params *params)
{
	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = 3;
	if (params->level != 3 && params->level != 4)
		return -EINVAL;
	return sz_setup_params(params);
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

/*
 * With zram's contexts split into one for compression and one for decompression (Sergey Senozhatsky's
 * series of October 2026, struct zcomp_cstrm in zcomp.h; run.sh sets ZCOMP_RW_SPLIT), each is a whole
 * struct sz_ctx: the decoder and zram_warm_fn find the scratch where they found it before, so the
 * times compare with those of a tree without the split. The kernel's backend (tools/kernel-port/)
 * gives each context only what it needs.
 */
#ifdef ZCOMP_RW_SPLIT
#define SZ_CTX_OPS                       \
	.create_cctx	= sz_create,     \
	.destroy_cctx	= sz_destroy,    \
	.create_dctx	= sz_create,     \
	.destroy_dctx	= sz_destroy
#else
#define SZ_CTX_OPS                       \
	.create_ctx	= sz_create,     \
	.destroy_ctx	= sz_destroy
#endif

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

static int sz_hc_compress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	unsigned int len;

	if (req->src_len != SEQLZ_PAGE)
		return -EINVAL;
	len = seqlz_compress_hc(params->drv_data, &((struct sz_ctx *)ctx->context)->hc, req->src, req->dst,
				req->dst_len, params->level == 4);
	if (!len)
		return -EINVAL;
	req->dst_len = len;
	return 0;
}

static int sz_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	/* EXPERIMENT: the compressed data requested first, here instead of in zram_drv.c, every cache
	 * line from the second on, as quetschn_prefetch_page() of bench/kernel_codecs/zram_codec.h */
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
	SZ_CTX_OPS,
	.setup_params	= sz_setup_params,
	.release_params	= sz_release_params,
	.name		= "seqlz",
};

/* seqlz-fast with the literals coded too */
const struct zcomp_ops backend_seqlz_lit = {
	.compress	= sz_lit_compress,
	.decompress	= sz_decompress,
	SZ_CTX_OPS,
	.setup_params	= sz_setup_params,
	.release_params	= sz_release_params,
	.name		= "seqlz-lit",
};

/* levels 3 and 4: the same format, the matcher with the hash chain */
const struct zcomp_ops backend_seqlz_hc = {
	.compress	= sz_hc_compress,
	.decompress	= sz_decompress,
	SZ_CTX_OPS,
	.setup_params	= sz_hc_setup_params,
	.release_params	= sz_release_params,
	.name		= "seqlz-hc",
};
