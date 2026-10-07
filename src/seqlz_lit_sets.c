// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The literal tables a page with coded literals chooses one of, part of the
 * format (docs/format.md): for 4 KiB pages in seqlz_lit_sets_4k.inc, for 16 KiB
 * pages in seqlz_lit_sets_16k.inc. Made by k-means over the literal histograms
 * of the training pages, with the compressor's matcher: each page goes to the
 * table that codes its literals in the fewest bits, then each table becomes the
 * Huffman code of its pages' literals. Of 32 runs from other starting tables,
 * the one with the fewest bits is kept. The most used table comes first.
 */
#include "seqlz.h"

#if QUETSCHN_PAGE_BITS != 12
#include "seqlz_lit_sets_16k.inc"
#else
#include "seqlz_lit_sets_4k.inc"
#endif
