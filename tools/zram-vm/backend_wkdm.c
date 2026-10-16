// SPDX-License-Identifier: MIT OR GPL-2.0-only
/* WKdm, the page compressor of Wilson and Kaplan in Apple's C version, as a zram backend "wkdm", for the
 * VM test of run.sh. run.sh downloads WKdm.h, WKdmCompress.c and WKdmDecompress.c from Apple's xnu (APSL
 * 2.0, not in this repository) and checks their SHA-256. Their work arrays of 7200 bytes, on the stack
 * there, are static in run.sh's copy: the VM has one CPU and one zram stream, and a kernel backend would
 * hold them per CPU. */
#include <linux/kernel.h>
#include <linux/mm.h>

#include "backend_wkdm.h"
#include "WKdm.h"

static int wkdm_setup_params(struct zcomp_params *params)
{
	return 0;
}

static void wkdm_release_params(struct zcomp_params *params)
{
}

static int wkdm_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	return 0;
}

static void wkdm_destroy(struct zcomp_ctx *ctx)
{
}

static int wkdm_compress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	/* at most 16 + 256 + 4096 bytes: header, tags and every word a miss; zram's buffer has 2 pages */
	if (req->src_len != PAGE_SIZE || PAGE_SIZE != PAGE_SIZE_IN_BYTES || req->dst_len < 2 * PAGE_SIZE)
		return -EINVAL;
	req->dst_len = WKdm_compress((WK_word *)req->src, (WK_word *)req->dst, PAGE_SIZE_IN_WORDS);
	return 0;
}

/* no checks: WKdm's decoder trusts the offsets in its header */
static int wkdm_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx, struct zcomp_req *req)
{
	if (req->dst_len < PAGE_SIZE)
		return -EINVAL;
	WKdm_decompress((WK_word *)req->src, (WK_word *)req->dst, PAGE_SIZE_IN_WORDS);
	return 0;
}

const struct zcomp_ops backend_wkdm = {
	.compress	= wkdm_compress,
	.decompress	= wkdm_decompress,
#ifdef ZCOMP_RW_SPLIT
	.create_cctx	= wkdm_create,
	.destroy_cctx	= wkdm_destroy,
	.create_dctx	= wkdm_create,
	.destroy_dctx	= wkdm_destroy,
#else
	.create_ctx	= wkdm_create,
	.destroy_ctx	= wkdm_destroy,
#endif
	.setup_params	= wkdm_setup_params,
	.release_params	= wkdm_release_params,
	.name		= "wkdm",
};
