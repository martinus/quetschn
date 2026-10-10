/* ---- the kernel's interface, include/linux/seqlz.h ---- */

int seqlz_compress(const void *src, void *dst, unsigned int dst_len,
		   void *wrkmem, int level)
{
	unsigned int len;

	if (level != SEQLZ_LEVEL_RAW && level != SEQLZ_LEVEL_CODED)
		return -EINVAL;
	len = seqlz_compress_page(&seqlz_fixed_tables, wrkmem, src, dst,
				  dst_len, level == SEQLZ_LEVEL_CODED);
	return len ? (int)len : -E2BIG;
}
EXPORT_SYMBOL_GPL(seqlz_compress);
