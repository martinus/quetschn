// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz as a zram backend calls it, for the benchmarks: "seqlz-1" to "seqlz-3",
 * one per level of seqlz_compress(). The tables are fixed, part of the format
 * (docs/format.md), so zram's dictionary is ignored, as lzo ignores it.
 */
#include "seqlz.h"
#include "zram_codec.h"

/* the tables are one object for all devices, built in, as the kernel's */
static int setup(struct bench_params *p)
{
	if (p->page_size != SEQLZ_BLOCK_SIZE)
		return -1;
	return 0;
}

static void release(struct bench_params *p)
{
	(void)p;
}

static void destroy(struct bench_stream *s)
{
	bench_free(s->context, &s->allocated);
	s->context = NULL;
}

/*
 * The compression context is the matcher's hash table, for all levels.
 * seqlz-1 decompresses without a context.
 */
static int create_cctx(struct bench_params *p, struct bench_stream *s)
{
	(void)p;
	s->context = bench_zalloc(sizeof(struct seqlz_state), &s->allocated);
	return s->context ? 0 : -1;
}

static int compress(struct bench_params *p, struct bench_stream *s,
		    const void *src, unsigned int src_len, void *dst,
		    unsigned int *dst_len, unsigned int level)
{
	unsigned int len;

	if (src_len != SEQLZ_BLOCK_SIZE)
		return -1;
	len = seqlz_compress(s->context, src, dst, *dst_len, level);
	if (!len)
		return -1;
	*dst_len = len;
	return 0;
}

static int compress_1(struct bench_params *p, struct bench_stream *s,
		      const void *src, unsigned int src_len, void *dst,
		      unsigned int *dst_len)
{
	return compress(p, s, src, src_len, dst, dst_len, 1);
}

/*
 * without a scratch, pages with coded literals are invalid; s is the
 * compression context, which it does not touch
 */
static int decompress_1(struct bench_params *p, struct bench_stream *s,
			const void *src, unsigned int src_len, void *dst,
			unsigned int *dst_len)
{
	(void)s;
	bench_prefetch_page(src, src_len, dst, SEQLZ_BLOCK_SIZE);
	if (*dst_len < SEQLZ_BLOCK_SIZE ||
	    seqlz_decode(src, src_len, dst, NULL))
		return -1;
	*dst_len = SEQLZ_BLOCK_SIZE;
	return 0;
}

const struct bench_codec bench_codec_seqlz_1 = {
	.name = "seqlz-1",
	.setup_params = setup,
	.release_params = release,
	.create_cctx = create_cctx,
	.destroy_cctx = destroy,
	.compress = compress_1,
	.decompress = decompress_1,
};

/*
 * The decompression context from level 2 on: the scratch the decoder decodes
 * the literals into
 */
static int create_dctx(struct bench_params *p, struct bench_stream *s)
{
	(void)p;
	s->context = bench_zalloc(SEQLZ_SCRATCH, &s->allocated);
	return s->context ? 0 : -1;
}

static int compress_2(struct bench_params *p, struct bench_stream *s,
		      const void *src, unsigned int src_len, void *dst,
		      unsigned int *dst_len)
{
	return compress(p, s, src, src_len, dst, dst_len, 2);
}

static int compress_3(struct bench_params *p, struct bench_stream *s,
		      const void *src, unsigned int src_len, void *dst,
		      unsigned int *dst_len)
{
	return compress(p, s, src, src_len, dst, dst_len, 3);
}

static int decompress(struct bench_params *p, struct bench_stream *s,
		      const void *src, unsigned int src_len, void *dst,
		      unsigned int *dst_len)
{
	bench_prefetch_page(src, src_len, dst, SEQLZ_BLOCK_SIZE);
	if (*dst_len < SEQLZ_BLOCK_SIZE ||
	    seqlz_decode(src, src_len, dst, s->context))
		return -1;
	*dst_len = SEQLZ_BLOCK_SIZE;
	return 0;
}

const struct bench_codec bench_codec_seqlz_2 = {
	.name = "seqlz-2",
	.setup_params = setup,
	.release_params = release,
	.create_cctx = create_cctx,
	.destroy_cctx = destroy,
	.create_dctx = create_dctx,
	.destroy_dctx = destroy,
	.compress = compress_2,
	.decompress = decompress,
};

const struct bench_codec bench_codec_seqlz_3 = {
	.name = "seqlz-3",
	.setup_params = setup,
	.release_params = release,
	.create_cctx = create_cctx,
	.destroy_cctx = destroy,
	.create_dctx = create_dctx,
	.destroy_dctx = destroy,
	.compress = compress_3,
	.decompress = decompress,
};
