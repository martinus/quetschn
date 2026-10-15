/* ---- the kernel's interface, include/linux/seqlz.h ---- */

static_assert(SEQLZ_LEVEL_MIN == 1 && SEQLZ_LEVEL_MAX == SEQLZ_LEVELS,
	      "the interface's levels are the codec's");

int seqlz_compress(const void *src, void *dst, unsigned int dst_len,
		   void *wrkmem, int level)
{
	unsigned int len;

	if (level < SEQLZ_LEVEL_MIN || level > SEQLZ_LEVEL_MAX)
		return -EINVAL;
	len = seqlz_compress_page(&seqlz_fixed_tables, wrkmem, src, dst,
				  dst_len, level);
	return len ? (int)len : -E2BIG;
}
EXPORT_SYMBOL_GPL(seqlz_compress);
