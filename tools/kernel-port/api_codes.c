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
	 * Does not fail: before scripts/gen-seqlz-tables.py writes
	 * seqlz_tables.c, it checks the lengths as seqlz_tables_init() does,
	 * every table a complete prefix code with a code for every symbol, and
	 * the KUnit tests check that the lengths are the format's.
	 */
	return seqlz_tables_init(&seqlz_fixed_tables, &seqlz_default_own);
}
subsys_initcall(seqlz_init);

static void __exit seqlz_exit(void)
{
}
module_exit(seqlz_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("seqlz: LZ compression of memory pages with static Huffman codes");
