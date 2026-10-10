/* ---- the kernel's interface, include/linux/seqlz.h ---- */

/*
 * The codes and decode tables, built once from the code lengths compiled in,
 * and only read after that, by every CPU.
 */
static struct seqlz_tables tables __ro_after_init;

int seqlz_compress(const void *src, void *dst, unsigned int dst_len,
		   void *wrkmem, int level)
{
	unsigned int len;

	if (level != SEQLZ_LEVEL_RAW && level != SEQLZ_LEVEL_CODED)
		return -EINVAL;
	len = seqlz_compress_page(&tables, wrkmem, src, dst, dst_len,
				  level == SEQLZ_LEVEL_CODED);
	return len ? (int)len : -E2BIG;
}
EXPORT_SYMBOL_GPL(seqlz_compress);

int seqlz_decompress(const void *src, unsigned int src_len, void *dst,
		     void *wrkmem)
{
	return seqlz_decode(&tables, src, src_len, dst, wrkmem);
}
EXPORT_SYMBOL_GPL(seqlz_decompress);

/* subsys_initcall: the tables are there before a zram that is built in */
static int __init seqlz_init(void)
{
	BUILD_BUG_ON(sizeof(struct seqlz_state) != SEQLZ_MEM_COMPRESS);
	BUILD_BUG_ON(SEQLZ_MEM_DECOMPRESS != SEQLZ_SCRATCH);
	/*
	 * The lengths are fixed, so this fails only if they are broken. Then
	 * seqlz_compress() fails for every page.
	 */
	if (WARN_ON(seqlz_tables_init(&tables, &seqlz_default_own) ||
		    !seqlz_all_symbols(&tables)))
		return -EINVAL;
	return 0;
}
subsys_initcall(seqlz_init);

static void __exit seqlz_exit(void)
{
}
module_exit(seqlz_exit);
