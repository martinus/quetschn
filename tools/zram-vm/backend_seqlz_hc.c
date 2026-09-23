// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* seqlz-hc as a zram backend, for the VM test of run.sh: lz4hc's matches at level 3, coded again with
 * seqlz's tables for lz4hc. Slow to compress, as a candidate for zram's recompression. */
#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "backend_seqlz_hc.h"
#include "seqlz.h"

struct szhc_ctx {
	void *mem;
	unsigned char *lz4;
	struct seqlz_sequence *seq;
	unsigned char *literals;
};

static int szhc_setup_params(struct zcomp_params *params)
{
	struct seqlz_tables *t = kzalloc(seqlz_tables_size(), GFP_KERNEL);

	if (!t)
		return -ENOMEM;
	if (seqlz_tables_init(t, &seqlz_default_lz4hc) || !seqlz_all_symbols(t)) {
		kfree(t);
		return -EINVAL;
	}
	params->drv_data = t;
	return 0;
}

static void szhc_release_params(struct zcomp_params *params)
{
	kfree(params->drv_data);
	params->drv_data = NULL;
}

static void szhc_destroy(struct zcomp_ctx *ctx)
{
	struct szhc_ctx *c = ctx->context;

	if (!c)
		return;
	vfree(c->mem);
	kfree(c->lz4);
	kfree(c->seq);
	kfree(c->literals);
	kfree(c);
	ctx->context = NULL;
}

static int szhc_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct szhc_ctx *c = kzalloc(sizeof(*c), GFP_KERNEL);

	if (!c)
		return -ENOMEM;
	ctx->context = c;
	c->mem = vmalloc(LZ4HC_MEM_COMPRESS);
	c->lz4 = kmalloc(2 * SEQLZ_PAGE, GFP_KERNEL);
	c->seq = kmalloc_array(SEQLZ_MAX_SEQUENCES, sizeof(*c->seq), GFP_KERNEL);
	c->literals = kmalloc(SEQLZ_PAGE, GFP_KERNEL);
	if (!c->mem || !c->lz4 || !c->seq || !c->literals) {
		szhc_destroy(ctx);
		return -ENOMEM;
	}
	return 0;
}

/* an lz4 block split into sequences and literals, as explore/zram_seqlz.c does */
static int split(const unsigned char *p, unsigned int n, struct szhc_ctx *c, unsigned int *n_seq,
		 unsigned int *n_lit)
{
	unsigned int i = 0, k = 0, lits = 0;

	while (i < n) {
		unsigned int token = p[i++], ll = token >> 4, ml, b;

		if (ll == 15) {
			do {
				if (i >= n)
					return -1;
				b = p[i++];
				ll += b;
			} while (b == 255);
		}
		if (ll > n - i || ll > SEQLZ_PAGE - lits || k == SEQLZ_MAX_SEQUENCES)
			return -1;
		memcpy(c->literals + lits, p + i, ll);
		lits += ll;
		i += ll;
		c->seq[k].literals = ll;
		c->seq[k].match = 0;
		c->seq[k].offset = 0;
		if (i == n) {
			k++;
			break;
		}
		if (n - i < 2)
			return -1;
		c->seq[k].offset = p[i] | (p[i + 1] << 8);
		i += 2;
		ml = (token & 15U) + 4U;
		if ((token & 15U) == 15) {
			do {
				if (i >= n)
					return -1;
				b = p[i++];
				ml += b;
			} while (b == 255);
		}
		c->seq[k].match = ml;
		k++;
	}
	*n_seq = k;
	*n_lit = lits;
	return 0;
}

static int szhc_compress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	struct szhc_ctx *c = ctx->context;
	unsigned int n_seq, n_lit, len;
	int ret;

	if (req->src_len != SEQLZ_PAGE)
		return -EINVAL;
	ret = LZ4_compress_HC(req->src, c->lz4, SEQLZ_PAGE, 2 * SEQLZ_PAGE, 3, c->mem);
	if (ret <= 0 || split(c->lz4, ret, c, &n_seq, &n_lit))
		return -EINVAL;
	len = seqlz_encode(params->drv_data, c->seq, n_seq, c->literals, n_lit, req->dst, req->dst_len);
	if (!len)
		return -EINVAL;
	req->dst_len = len;
	return 0;
}

static int szhc_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	if (req->dst_len < SEQLZ_PAGE || seqlz_decode(params->drv_data, req->src, req->src_len, req->dst))
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_seqlz_hc = {
	.compress	= szhc_compress,
	.decompress	= szhc_decompress,
	.create_ctx	= szhc_create,
	.destroy_ctx	= szhc_destroy,
	.setup_params	= szhc_setup_params,
	.release_params	= szhc_release_params,
	.name		= "seqlz-hc",
};
