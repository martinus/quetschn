/* SPDX-License-Identifier: MIT OR GPL-2.0-only */
#ifndef QUETSCHN_TOOLS_SEQLZ_REF_H
#define QUETSCHN_TOOLS_SEQLZ_REF_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The tables of docs/format.md, "The tables": code lengths, one byte per symbol. */
struct seqlz_ref_tables {
    const unsigned char* tok;        /* 3073 symbols, the escape last */
    const unsigned char* ll;         /* 12 + block_bits symbols */
    const unsigned char* ml;         /* 12 + block_bits symbols */
    const unsigned char (*lit)[256]; /* the 8 literal tables */
    const unsigned char* own;        /* OWN, 19 symbols */
};

/* The codes of one table, docs/format.md "Prefix codes", for symbols with codes of at most 15 bits. */
struct seqlz_ref_code {
    unsigned int count[16];   /* symbols with a code of each length */
    unsigned int first[16];   /* the number of the first code of each length */
    unsigned int start[16];   /* where the symbols of each length start in sym */
    unsigned short sym[3073]; /* the symbols with a code, by length, then by number */
};

/* Everything the decoder needs: the block size and the codes of all tables. */
struct seqlz_ref {
    unsigned int block_bits;
    struct seqlz_ref_code tok, ll, ml, lit[8], own;
};

/*
 * The codes of the tables t for blocks of 1 << block_bits bytes; block_bits is 12 or 14. Returns 0, or -1
 * for another block_bits or a table that is no complete prefix code of at most 15 bits.
 */
int seqlz_ref_init(struct seqlz_ref* r, const struct seqlz_ref_tables* t, unsigned int block_bits);

/*
 * Decodes the compressed block src of len bytes into out, 1 << block_bits bytes, as docs/format.md says, bit
 * by bit and without anything from src/: a second decoder to check seqlz_decode() against. Returns
 * 0 for a valid block, -1 for an invalid one; out is undefined then.
 */
int seqlz_ref_decode(const struct seqlz_ref* r, const unsigned char* src, size_t len, unsigned char* out);

#ifdef __cplusplus
}
#endif

#endif
