// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The code lengths compiled in. They are part of the format: a block decodes
 * only with the tables it was compressed with (docs/format.md). For blocks of
 * 4 KiB they are in seqlz_default_tables_4k.inc, which
 * tools/kernel-port/gen-seqlz-lengths.py makes from the counts in
 * bench/seqlz_counts_4k.txt, counted with
 *   seqlz-train --corpus train-mix-1012 --counts
 * on pages that are not in the dumps the benchmarks measure. docs/format.md
 * says which pages, in "Where the tables come from".
 */
#include "seqlz.h"

#if SEQLZ_PAGE_BITS != 12
#include "seqlz_default_tables_16k.inc"
#else
#include "seqlz_default_tables_4k.inc"
#endif
