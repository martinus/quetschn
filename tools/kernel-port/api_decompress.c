/* ---- the kernel's interface, include/linux/seqlz.h ---- */

int seqlz_decompress(const void *src, unsigned int src_len, void *dst,
		     void *wrkmem)
{
	return seqlz_decode(&seqlz_fixed_tables, src, src_len, dst, wrkmem);
}
EXPORT_SYMBOL_GPL(seqlz_decompress);
