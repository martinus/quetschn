// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * seqlz (src/seqlz.h) as a zram backend would call it: "seqlz-fast" with raw
 * literals, "seqlz-fast-lit" with the literals Huffman coded where that pays.
 * The tables are part of the format (docs/format.md), so zram's dictionary
 * parameter is ignored, like lzo ignores it.
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
	 * the encoder needs a code for every symbol; the tables are fixed, this
	 * keeps a bad retraining out
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

/* seqlz-fast's context is the matcher's hash table */
static int fast_create(struct quetschn_params *p, struct quetschn_stream *s)
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

/* without a scratch, pages with coded literals are invalid */
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
	"seqlz-fast", setup,	     release,	      fast_create,
	destroy,      fast_compress, fast_decompress,
};

/*
 * seqlz-fast-lit's context: the hash table, and the scratch the decoder decodes
 * the literals into
 */
struct fast_lit_ctx {
	struct seqlz_state st;
	unsigned char scratch[SEQLZ_SCRATCH];
};

static int fast_lit_create(struct quetschn_params *p, struct quetschn_stream *s)
{
	(void)p;
	s->context =
		quetschn_zalloc(sizeof(struct fast_lit_ctx), &s->allocated);
	return s->context ? 0 : -1;
}

static int fast_lit_compress(struct quetschn_params *p,
			     struct quetschn_stream *s, const void *src,
			     unsigned int src_len, void *dst,
			     unsigned int *dst_len)
{
	struct fast_lit_ctx *c = s->context;
	unsigned int len;

	if (src_len != SEQLZ_PAGE)
		return -1;
	len = seqlz_compress(p->drv_data, &c->st, src, dst, *dst_len, 1);
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
	struct fast_lit_ctx *c = s->context;

	quetschn_prefetch_page(src, src_len, dst, SEQLZ_PAGE);
	if (*dst_len < SEQLZ_PAGE ||
	    seqlz_decode(p->drv_data, src, src_len, dst, c->scratch))
		return -1;
	*dst_len = SEQLZ_PAGE;
	return 0;
}

const struct quetschn_codec quetschn_codec_seqlz_fast_lit = {
	"seqlz-fast-lit",    setup,   release,
	fast_lit_create,     destroy, fast_lit_compress,
	fast_lit_decompress,
};
