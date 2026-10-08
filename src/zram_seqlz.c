// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz as a zram backend calls it, for the benchmarks: "seqlz-fast" with raw
 * literals, "seqlz-fast-lit" with the literals Huffman coded where that pays.
 * The tables are fixed, part of the format (docs/format.md), so zram's
 * dictionary is ignored, as lzo ignores it.
 */
#include "seqlz.h"
#include "zram_codec.h"

static int setup(struct quetschn_params *p)
{
	struct seqlz_tables *t;

	if (p->page_size != SEQLZ_PAGE)
		return -1;
	t = quetschn_zalloc(seqlz_tables_size(), &p->allocated);
	if (!t)
		return -1;
	/*
	 * the encoder needs a code for every symbol; the tables are fixed, so
	 * this only catches a training that went wrong
	 */
	if (seqlz_tables_init(t, &seqlz_default_own) || !seqlz_all_symbols(t)) {
		quetschn_free(t, &p->allocated);
		return -1;
	}
	p->drv_data = t;
	return 0;
}

static void release(struct quetschn_params *p)
{
	quetschn_free(p->drv_data, &p->allocated);
	p->drv_data = NULL;
}

static void destroy(struct quetschn_stream *s)
{
	quetschn_free(s->context, &s->allocated);
	s->context = NULL;
}

/*
 * The compression context is the matcher's hash table, for both. seqlz-fast
 * decompresses without a context.
 */
static int create_cctx(struct quetschn_params *p, struct quetschn_stream *s)
{
	(void)p;
	s->context = quetschn_zalloc(sizeof(struct seqlz_state), &s->allocated);
	return s->context ? 0 : -1;
}

static int fast_compress(struct quetschn_params *p, struct quetschn_stream *s,
			 const void *src, unsigned int src_len, void *dst,
			 unsigned int *dst_len)
{
	unsigned int len;

	if (src_len != SEQLZ_PAGE)
		return -1;
	len = seqlz_compress(p->drv_data, s->context, src, dst, *dst_len, 0);
	if (!len)
		return -1;
	*dst_len = len;
	return 0;
}

/*
 * without a scratch, pages with coded literals are invalid; s is the
 * compression context, which it does not touch
 */
static int fast_decompress(struct quetschn_params *p, struct quetschn_stream *s,
			   const void *src, unsigned int src_len, void *dst,
			   unsigned int *dst_len)
{
	(void)s;
	quetschn_prefetch_page(src, src_len, dst, SEQLZ_PAGE);
	if (*dst_len < SEQLZ_PAGE ||
	    seqlz_decode(p->drv_data, src, src_len, dst, NULL))
		return -1;
	*dst_len = SEQLZ_PAGE;
	return 0;
}

const struct quetschn_codec quetschn_codec_seqlz_fast = {
	.name = "seqlz-fast",
	.setup_params = setup,
	.release_params = release,
	.create_cctx = create_cctx,
	.destroy_cctx = destroy,
	.compress = fast_compress,
	.decompress = fast_decompress,
};

/*
 * seqlz-fast-lit's decompression context: the scratch the decoder decodes the
 * literals into
 */
static int create_dctx(struct quetschn_params *p, struct quetschn_stream *s)
{
	(void)p;
	s->context = quetschn_zalloc(SEQLZ_SCRATCH, &s->allocated);
	return s->context ? 0 : -1;
}

static int fast_lit_compress(struct quetschn_params *p,
			     struct quetschn_stream *s, const void *src,
			     unsigned int src_len, void *dst,
			     unsigned int *dst_len)
{
	unsigned int len;

	if (src_len != SEQLZ_PAGE)
		return -1;
	len = seqlz_compress(p->drv_data, s->context, src, dst, *dst_len, 1);
	if (!len)
		return -1;
	*dst_len = len;
	return 0;
}

static int fast_lit_decompress(struct quetschn_params *p,
			       struct quetschn_stream *s, const void *src,
			       unsigned int src_len, void *dst,
			       unsigned int *dst_len)
{
	quetschn_prefetch_page(src, src_len, dst, SEQLZ_PAGE);
	if (*dst_len < SEQLZ_PAGE ||
	    seqlz_decode(p->drv_data, src, src_len, dst, s->context))
		return -1;
	*dst_len = SEQLZ_PAGE;
	return 0;
}

const struct quetschn_codec quetschn_codec_seqlz_fast_lit = {
	.name = "seqlz-fast-lit",
	.setup_params = setup,
	.release_params = release,
	.create_cctx = create_cctx,
	.destroy_cctx = destroy,
	.create_dctx = create_dctx,
	.destroy_dctx = destroy,
	.compress = fast_lit_compress,
	.decompress = fast_lit_decompress,
};
