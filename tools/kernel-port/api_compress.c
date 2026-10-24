/* the interface's work memory is the matcher's state */
static_assert(sizeof(struct seqlz_state) == SEQLZ_MEM_COMPRESS);
/* its levels are those of seqlz_compress_block() */
static_assert(SEQLZ_LEVEL_MIN == 1 && SEQLZ_LEVEL_CODED == 2 &&
	      SEQLZ_LEVEL_OWN == SEQLZ_LEVEL_MAX);
/* the room the interface's kernel-doc names, see ENC_ROOM */
static_assert(ENC_ROOM == 32);

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
