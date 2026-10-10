/* ---- the kernel's interface, include/linux/seqlz.h ---- */

/*
 * The codes and decode tables, built once from the code lengths compiled in,
 * and only read after that, by every CPU.
 */
struct seqlz_tables seqlz_fixed_tables __ro_after_init;

/* subsys_initcall: the tables are there before a zram that is built in */
static int __init seqlz_init(void)
{
	BUILD_BUG_ON(sizeof(struct seqlz_state) != SEQLZ_MEM_COMPRESS);
	BUILD_BUG_ON(SEQLZ_MEM_DECOMPRESS != SEQLZ_SCRATCH);
	/*
	 * The lengths are fixed, so this fails only if they are broken. Then
	 * seqlz_compress() fails for every page.
	 */
	if (WARN_ON(seqlz_tables_init(&seqlz_fixed_tables, &seqlz_default_own) ||
		    !seqlz_all_symbols(&seqlz_fixed_tables)))
		return -EINVAL;
	return 0;
}
subsys_initcall(seqlz_init);

static void __exit seqlz_exit(void)
{
}
module_exit(seqlz_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("seqlz: LZ compression of memory pages with static Huffman codes");
