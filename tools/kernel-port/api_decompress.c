EXPORT_SYMBOL_GPL(seqlz_decompress);

/* the interface's work memory is the buffer of coded literals */
static_assert(SEQLZ_MEM_DECOMPRESS == SEQLZ_SCRATCH);
static_assert(SEQLZ_BLOCK_SIZE == 1U << SEQLZ_BLOCK_BITS);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("seqlz: LZ compression of 4 KiB blocks with static Huffman codes");
