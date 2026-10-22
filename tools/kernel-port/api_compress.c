/* the interface's work memory and levels are the matcher's state and levels */
static_assert(sizeof(struct seqlz_state) == SEQLZ_MEM_COMPRESS);
static_assert(SEQLZ_LEVEL_MIN == 1 && SEQLZ_LEVEL_MAX == SEQLZ_LEVELS);

int seqlz_compress(const void *src, void *dst, unsigned int dst_len,
		   void *wrkmem, int level)
{
	unsigned int len;

	if (level < SEQLZ_LEVEL_MIN || level > SEQLZ_LEVEL_MAX)
		return -EINVAL;
	len = seqlz_compress_block(wrkmem, src, dst, dst_len, level);
	return len ? (int)len : -E2BIG;
}
EXPORT_SYMBOL_GPL(seqlz_compress);
