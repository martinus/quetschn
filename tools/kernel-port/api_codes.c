/* ---- the kernel's interface, include/linux/seqlz.h ---- */

/* the interface's sizes are the codec's */
static_assert(sizeof(struct seqlz_state) == SEQLZ_MEM_COMPRESS);
static_assert(SEQLZ_MEM_DECOMPRESS == SEQLZ_SCRATCH);
static_assert(SEQLZ_PAGE == 1U << SEQLZ_PAGE_BITS);

/* subsys_initcall: the tables are there before a zram that is built in */
static int __init seqlz_init(void)
{
	/*
	 * Does not fail: before scripts/gen-seqlz-tables.py writes
	 * seqlz_tables.c, it checks the lengths as seqlz_tables_init() does,
	 * every table a complete prefix code with a code for every symbol, and
	 * the KUnit tests check that the lengths are the format's.
	 */
	return seqlz_tables_init(&seqlz_default_lengths);
}
subsys_initcall(seqlz_init);

static void __exit seqlz_exit(void)
{
}
module_exit(seqlz_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("seqlz: LZ compression of memory pages with static Huffman codes");
