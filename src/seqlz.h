/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef _LINUX_SEQLZ_H
#define _LINUX_SEQLZ_H

#include "seqlz_compat.h"

/*
 * seqlz compresses one memory page at a time, for zram. Like lz4, a page is a
 * list of sequences: some literal bytes, copied as they are, then a match, a
 * copy of bytes that came earlier in the same page. lz4 writes the numbers of a
 * sequence in whole bytes. seqlz writes them with Huffman codes, and the code
 * tables are fixed: trained once on real memory pages and compiled in, so a
 * page carries no table of its own. That makes pages smaller than lz4's, and
 * decoding the codes costs time.
 *
 * A sequence has three numbers:
 *   ll:     the number of literals before the match
 *   ml:     the length of the match, at least 4 bytes; 0 for the last sequence
 *   offset: how far back the match starts, 1 to the page size
 *
 * Most of them are small, and many offsets are the same as the one before. So
 * ll, ml and a class of the offset share one Huffman symbol, the token, and
 * one table lookup in the decoder gives all three:
 *   token = min(ll, 15) + 16 * min(ml - 4, 31) + 512 * class
 * which are 3072 symbols. A rare token has no code; it is sent as the code of
 * the escape, followed by the token's number in SEQLZ_ESCAPE_BITS bits.
 *
 * The offset classes, and the bits of the offset that follow the token:
 *   0:    the offset of the sequence before, 1 at the start of a page; none
 *   1:    below 16; 4 bits
 *   2:    below 256; 8 bits
 *   3:    below the page size; 12 bits for 4 KiB pages
 *   4, 5: a multiple of 8 from 16 on, below 256 or below the page size; the
 *         offset divided by 8, so 3 bits less than class 2 or 3
 * Memory pages are full of 8-byte aligned data, so many offsets are multiples
 * of 8.
 *
 * An ll of 15 or more is followed by ll - 15 as a length value, an ml - 4 of 31
 * or more by ml - 4 - 31. A length value below 16 is its own symbol. A larger
 * one is sent as the position of its highest bit, symbol 12 + bit_width(v) - 1,
 * followed by the bits below that one as they are. ll and ml have a table each.
 *
 * Everything goes into one bitstream, most significant bit first: per sequence
 * the token, the offset's bits, then the length values. A page does not store
 * how many sequences it has. The last one has no match, its token has
 * ml - 4 = 0 and class 0, and its literals end at the end of the page.
 *
 * A page starts with a u16, little endian: the number of literals of all
 * sequences. The literals follow, all of them one after the other, then the
 * bitstream. Text and code have many literals, so a page can have its literals
 * Huffman coded too, with one of SEQLZ_LIT_SETS fixed tables. Then the u16 has
 * SEQLZ_LIT_CODED set and a header follows, see SEQLZ_LIT_HEADER.
 *
 * docs/format.md is the specification. docs/explored-designs.md has the
 * measurements behind each choice, by the headings quoted below.
 */

/*
 * The bit operations and the short copies are the compiler's builtins, as in
 * lib/lz4 and lib/zstd. __builtin_ctzll() and __builtin_clz() instead of
 * __ffs64(), __fls() and fls(), which are inline assembly on x86-64 that the
 * compiler cannot look into.
 * __builtin_memcpy() for copies of 8 and 16 bytes, as LZ4_memcpy() and
 * ZSTD_memcpy(): it is always inlined, and memcpy() is not with every
 * CONFIG_FORTIFY_SOURCE, which makes each copy a call. __builtin_bswap64()
 * turns a literal stream's bits around once, in a register, see lit_flush().
 */

#ifndef QUETSCHN_PAGE_BITS
#define QUETSCHN_PAGE_BITS 12 /* 12 for 4 KiB pages, 14 for 16 KiB */
#endif
#define SEQLZ_PAGE (1U << QUETSCHN_PAGE_BITS)
/*
 * The longest code of a length value: its decode table has 256 entries, 1 KiB.
 * A table twice as large made cold reads slower.
 */
#define SEQLZ_MAX_BITS 8U
/*
 * The longest code of a token: its decode table has 2048 entries, 8 KiB. With
 * 10 bits more tokens take the escape and pages are larger, and only in-order
 * cores wrote them faster ("Six choices made on the PC, measured on the
 * phone", 1 and 7).
 */
#define SEQLZ_TOKEN_BITS 11U
/*
 * How many bits of ll and ml - 4 the token holds; larger ones need a length
 * value after the token, which costs a second table lookup. With 5 bits for the
 * match length half as many matches need one as with 4, at the same memory
 * ("Second round: branches, tails and table sizes"). At most 4 and 5:
 * the decoder's token entry has 4 bits for ll and 6 for ml, see token_entry()
 * in seqlz_internal.h.
 */
#define SEQLZ_LL_BITS 4U
#define SEQLZ_ML_BITS 5U
/*
 * The offset classes at the top. Two more would save almost no memory, by a
 * model of the offsets ("Offset classes from a histogram").
 */
#define SEQLZ_OFF_CLASSES 6U
#define SEQLZ_TOKEN_SYMBOLS \
	(SEQLZ_OFF_CLASSES << (SEQLZ_LL_BITS + SEQLZ_ML_BITS))
/*
 * The escape, the symbol after the last token. Codes of at most 11 bits have
 * room for 2048 symbols, and there are 3072 tokens, so the rare ones get no
 * code: they are sent as the escape's code and then the token in
 * SEQLZ_ESCAPE_BITS bits. A code for every token would take code space from
 * the frequent ones ("seqlz, third decoder round").
 */
#define SEQLZ_ESCAPE SEQLZ_TOKEN_SYMBOLS
/* the fewest bits that hold every token, 3072 < 4096 */
#define SEQLZ_ESCAPE_BITS 12U
/*
 * The longest code the escape may have. The encoder counts on 31 bits for a
 * token and its offset, the escape included, to know that zram's two pages are
 * always room enough for a compressed page; see the encoder in
 * seqlz_compress.c.
 */
#define SEQLZ_MAX_ESCAPE_LEN (31U - QUETSCHN_PAGE_BITS - SEQLZ_ESCAPE_BITS)
/* the largest ll and ml - 4 a token holds: 15 and 31, larger ones follow */
#define SEQLZ_LL_CAP ((1U << SEQLZ_LL_BITS) - 1U)
#define SEQLZ_ML_CAP ((1U << SEQLZ_ML_BITS) - 1U)
/* length values below 16 are their own symbol */
#define SEQLZ_LEN_DIRECT 16U
/*
 * A larger one is the symbol SEQLZ_LEN_LOG_BASE plus the position of its
 * highest bit, which is 4 or more, so these symbols follow the direct ones.
 */
#define SEQLZ_LEN_LOG_BASE (SEQLZ_LEN_DIRECT - 4U)
/*
 * The symbols of length values: 16 direct ones, then one per highest bit from 4
 * up to the page's bits, the largest length a page can need.
 */
#define SEQLZ_LEN_SYMBOLS (SEQLZ_LEN_LOG_BASE + QUETSCHN_PAGE_BITS + 1U)
/* the u16 at the start of every page */
#define SEQLZ_HEADER 2U
/*
 * Set in that u16 when the literals are Huffman coded. A page has at most
 * 16 KiB of literals, so the top bit is free.
 */
#define SEQLZ_LIT_CODED 0x8000U

/*
 * The code lengths of the token table and of the two tables of length values,
 * 0 for a symbol without a code. Canonical Huffman codes follow from the
 * lengths alone, so this is all a table is. The ones compiled in,
 * seqlz_default_own, are part of the format.
 */
struct seqlz_lengths {
	u8 token[SEQLZ_TOKEN_SYMBOLS + 1]; /* the last one is the escape */
	u8 ll[SEQLZ_LEN_SYMBOLS];
	u8 ml[SEQLZ_LEN_SYMBOLS];
};

/*
 * The symbol of a length value, and how many of its bits follow the symbol's
 * code as they are, see the top of this file.
 */
static inline unsigned int seqlz_len_symbol(unsigned int v,
					    unsigned int *extra_bits)
{
	unsigned int b;

	if (v < SEQLZ_LEN_DIRECT) {
		*extra_bits = 0;
		return v;
	}
	b = 31U - (unsigned int)__builtin_clz(v);
	*extra_bits = b;
	return SEQLZ_LEN_LOG_BASE + b; /* v >= 16, so b >= 4 */
}

/*
 * How many bits of the offset follow the token, per class: 0, 4, 8, the page's
 * bits, 5, the page's bits - 3. One nibble per class in one constant,
 * 0x95C840 for 4 KiB pages, 0xB5E840 for 16 KiB, so that it is a shift and a
 * mask without a branch: computing class 3's from the page's bits made gcc
 * branch on the class.
 */
#define SEQLZ_RAW_NIBBLES                                               \
	(0U | 4U << 4 | 8U << 8 | QUETSCHN_PAGE_BITS << 12 | 5U << 16 | \
	 (QUETSCHN_PAGE_BITS - 3U) << 20)
#define SEQLZ_RAW_BITS(cls) ((SEQLZ_RAW_NIBBLES >> (4U * (cls))) & 15U)
/* classes 4 and 5 send the offset divided by 8, a shift by 3 */
#define SEQLZ_OFF_SHIFT(cls) (((cls) >> 2) * 3U)

/*
 * The class of an offset, and how many of its bits follow the token. last is
 * the offset of the sequence before. Without a branch, because the encoder
 * calls it for every match and the classes come in no order a branch predictor
 * could learn.
 */
static inline unsigned int seqlz_off_class(unsigned int off, unsigned int last,
					   unsigned int *raw_bits)
{
	unsigned int is_new = (off == last) - 1U,
		     aligned = (off >= 16U) & ((off & 7U) == 0);
	/*
	 * 1 to 3 by size; a multiple of 8 from 16 on moves class 2 or 3
	 * to 4 or 5; the same offset as before is class 0
	 */
	unsigned int cls = (1U + (off >= 16U) + (off >= 256U) + 2U * aligned) &
			   is_new;

	*raw_bits = SEQLZ_RAW_BITS(cls);
	return cls;
}

/* a sequence as the matcher finds it, see seqlz_find() */
struct seqlz_sequence {
	u16 literals;
	u16 match; /* 0 for the last sequence */
	u16 offset;
};

/*
 * The codes and decode tables built from a struct seqlz_lengths, for the
 * encoder and the decoder. Read only once built, one for all CPUs.
 */
struct seqlz_tables;

/**
 * seqlz_tables_size() - The size of struct seqlz_tables
 *
 * Return: the bytes to allocate for seqlz_tables_init().
 */
size_t seqlz_tables_size(void);

/**
 * seqlz_all_symbols() - Whether every symbol has a code
 * @t: tables from seqlz_tables_init()
 *
 * The encoder needs a code for every symbol it may have to write. The decoder
 * does not: a complete prefix code can leave symbols out, and pages that use
 * them are not valid for these tables.
 *
 * Return: true if the encoder can use @t.
 */
bool seqlz_all_symbols(const struct seqlz_tables *t);

/**
 * seqlz_tables_init() - Build the codes and decode tables from code lengths
 * @t: seqlz_tables_size() bytes
 * @lengths: the code lengths, e.g. seqlz_default_own
 *
 * Return: 0, or -EINVAL if the lengths are not a complete prefix code: a code
 * longer than its table allows, more codes than fit, gaps, or none at all.
 */
int seqlz_tables_init(struct seqlz_tables *t,
		      const struct seqlz_lengths *lengths);

/*
 * Coded literals: the longest code, and the number of fixed tables a page can
 * choose from. The encoder takes the table that codes the page's literals in
 * the fewest bits. Codes of 9 bits made pages larger and were no faster ("Six
 * choices made on the PC, measured on the phone", 2). 16 tables saved a few
 * bytes per page, and made cold reads slower, for the larger tables
 * ("seqlz-fast-lit by the score: no budget, offsets in steps of 8").
 */
#define SEQLZ_LIT_BITS 10U
#define SEQLZ_LIT_SETS 8U
/*
 * The coded literals are 8 bitstreams, literal k in stream k % 8. A literal's
 * code length is known only after its table lookup, so one stream is one long
 * chain of lookups; the decoder runs 8 such chains side by side. 4 streams made
 * pages a little smaller and decoding no faster ("Six choices made on the PC,
 * measured on the phone", 3).
 */
#define SEQLZ_LIT_STREAMS 8U
extern const u8 seqlz_lit_sets[SEQLZ_LIT_SETS][256];
/*
 * How many literals the decoder takes from each stream after loading 8 bytes
 * of it: at least 56 bits are then left, enough for 5 codes of 10 bits.
 */
#define SEQLZ_LIT_ROUNDS (56U / SEQLZ_LIT_BITS)
/*
 * The header of a page with coded literals, 3 + w bytes: the u16 of every page,
 * then a byte with the literal table in bits 0 to 2 and w - SEQLZ_SIZE_BITS_MIN
 * in bits 3 to 5, bits 6 and 7 zero, then the sizes of the 8 streams in bytes,
 * w bits each, lowest bit first. The decoder needs the sizes to know where each
 * stream starts.
 *
 * w is the fewest bits that hold the largest size, but at least 5. A stream
 * holds every 8th literal of at most 10 bits, at most 640 bytes in a 4 KiB page
 * and 2560 in a 16 KiB page, so w is at most 12. Sizes of 2 bytes each made
 * pages larger; widths of 3 and 4 bits are left out, few pages would use them.
 */
#define SEQLZ_LIT_HEADER(w) (3U + (w))
#define SEQLZ_SIZE_BITS_MIN 5U
#define SEQLZ_SIZE_BITS_MAX 12U
/*
 * Where the fields of byte 2 start: the table at bit 0, w - SEQLZ_SIZE_BITS_MIN
 * at bit 3, the two bits that must be zero at bit 6.
 */
#define SEQLZ_LIT_WIDTH_AT 3
#define SEQLZ_LIT_ZERO_AT 6
/*
 * The encoder codes a page's literals only where that saves more than 1/16 of
 * them plus this many bytes. Decoding coded literals has a cost that does not
 * depend on how many there are: the literal table has to be read from memory,
 * and the literals decoded into a buffer first. A smaller minimum made pages a
 * few bytes smaller and writes slower, most on in-order cores ("Coded literals
 * only where they save 51 bytes: 2.7 µs per page written less on the A55,
 * kept").
 */
#define SEQLZ_LIT_CODED_MIN 51U
/*
 * The buffer the decoder decodes coded literals into, one per CPU. 16 bytes
 * more than a page, because the decoder copies literals 16 bytes at a time and
 * may read past the last one.
 */
#define SEQLZ_SCRATCH (SEQLZ_PAGE + 16U)

/**
 * seqlz_encode() - Write a compressed page from given sequences
 * @t: tables with a code for every symbol, see seqlz_all_symbols()
 * @seq: the sequences, the last one with match 0
 * @n: how many
 * @literals: the literals of all sequences, one after the other
 * @n_literals: how many
 * @dst: where the compressed page goes
 * @dst_cap: the bytes at @dst
 * @coded: whether to Huffman code the literals where that saves enough
 *
 * For tests and tools, which make their own sequences; zram uses
 * seqlz_compress().
 *
 * The sequences describe a page if each but the last has a match of at least
 * 4 bytes, with an offset from 1 to the bytes of the page before the match,
 * and the literals and matches add up to the page, with all @n_literals
 * literals used.
 *
 * Return: the length of the compressed page, or 0 if it does not fit into
 * @dst_cap bytes, @t lacks a code, or the sequences do not describe a page.
 */
unsigned int seqlz_encode(const struct seqlz_tables *t,
			  const struct seqlz_sequence *seq, unsigned int n,
			  const u8 *literals, unsigned int n_literals,
			  void *dst, unsigned int dst_cap, bool coded);

/**
 * seqlz_decode() - Decompress one page
 * @t: the tables the page was compressed with
 * @src: the compressed page
 * @src_len: its length
 * @dst: SEQLZ_PAGE bytes for the page
 * @scratch: SEQLZ_SCRATCH bytes for coded literals, or NULL
 *
 * Safe for any input: it never reads outside @src and @src_len, and never
 * writes outside @dst and @scratch. Without @scratch, pages with coded literals
 * are not valid.
 *
 * Return: 0, or -EINVAL if @src is not a valid page for @t.
 */
int seqlz_decode(const struct seqlz_tables *t, const void *src,
		 unsigned int src_len, void *dst, void *scratch);

/* the token of a sequence, see the top of this file */
static inline unsigned int seqlz_token(unsigned int ll, unsigned int ml,
				       unsigned int cls)
{
	unsigned int a = min(ll, SEQLZ_LL_CAP);
	unsigned int b = ml ? min(ml - 4, SEQLZ_ML_CAP) : 0;

	return a + (b << SEQLZ_LL_BITS) +
	       (cls << (SEQLZ_LL_BITS + SEQLZ_ML_BITS));
}

/*
 * The compressor finds the matches with its own matcher and writes each
 * sequence as soon as it is found, in one pass over the page.
 */
/*
 * The matcher's hash table has 1 << SEQLZ_HASH_BITS entries of 2 bytes, 8 KiB
 * for 4 KiB pages. With 2048 entries pages were larger and the compressor no
 * faster; with 8192 in-order cores wrote pages slower ("Six choices made on the
 * PC, measured on the phone", 5). For 16 KiB pages it is 16 KiB, as lz4's
 * table.
 */
#define SEQLZ_HASH_BITS (QUETSCHN_PAGE_BITS == 12 ? 12U : 13U)
/* every sequence but the last covers at least 4 bytes of the page */
#define SEQLZ_MAX_SEQUENCES (SEQLZ_PAGE / 4U + 1U)

/*
 * The matcher's hash table, one per CPU, cleared for each page. Once the
 * matcher is done it holds the raw literals while they are coded, see
 * code_literals() in seqlz_compress.c.
 */
struct seqlz_state {
	union {
		u16 table[1U << SEQLZ_HASH_BITS];
		u8 literals[SEQLZ_PAGE];
	};
};

/**
 * seqlz_find() - The sequences of a page, as the compressor finds them
 * @st: the matcher's state
 * @src: the page, SEQLZ_PAGE bytes
 * @seq: room for SEQLZ_MAX_SEQUENCES sequences
 *
 * For tests and tools. The last sequence has no match; the literals are the
 * bytes of the page no match covers.
 *
 * Return: the number of sequences.
 */
unsigned int seqlz_find(struct seqlz_state *st, const void *src,
			struct seqlz_sequence *seq);

/**
 * seqlz_compress() - Compress one page
 * @t: tables with a code for every symbol, see seqlz_all_symbols()
 * @st: the matcher's state, one per CPU
 * @src: the page, SEQLZ_PAGE bytes
 * @dst: where the compressed page goes
 * @dst_cap: the bytes at @dst
 * @coded: whether to Huffman code the literals where that saves enough
 *
 * Two pages, as zram's buffer has, are always enough, even for random data,
 * see the encoder in seqlz_compress.c. A smaller @dst works too: a page that
 * does not fit is an error, which can happen up to 32 bytes before @dst_cap is
 * full.
 * The page is written with raw literals first, so it has to fit that way; the
 * coded literals then go over the raw ones, which @st holds meanwhile. The same
 * bytes as seqlz_encode() for the sequences of seqlz_find().
 *
 * Return: the length of the compressed page, or 0 if it does not fit into
 * @dst_cap bytes or @t lacks a code.
 */
unsigned int seqlz_compress(const struct seqlz_tables *t,
			    struct seqlz_state *st, const void *src, void *dst,
			    unsigned int dst_cap, bool coded);

/* the code lengths compiled in, see seqlz_default_tables.c */
extern const struct seqlz_lengths seqlz_default_own;

#endif
