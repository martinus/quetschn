// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The literal tables of pages with coded literals, one chosen per page, part of the format (docs/format.md):
 * for 4 KiB pages in seqlz_lit_sets_4k.inc, for 16 KiB pages in seqlz_lit_sets_16k.inc. k-means over the
 * literal histograms of the pages with seqlz-fast's matcher: each page goes to the table that codes its
 * literals in the fewest bits, each table is the code of its pages' literals; 32 starts, the one with the
 * fewest bits wins. The most used table first.
 */
#include "seqlz.h"

#if QUETSCHN_PAGE_BITS != 12
#    include "seqlz_lit_sets_16k.inc"
#else
#    include "seqlz_lit_sets_4k.inc"
#endif
