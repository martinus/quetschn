// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The code lengths compiled in. They are part of the format: a page decodes
 * only with the tables it was compressed with (docs/format.md). For 4 KiB pages
 * they are in seqlz_default_tables_4k.inc, which
 * tools/kernel-port/gen-seqlz-tables.py makes from the counts in
 * bench/seqlz_counts_4k.txt, counted with
 *   quetschn-seqlz-train --corpus train-rz-phone5 --counts
 * on pages that are not in the dumps the benchmarks measure. Which pages, and
 * why: docs/format.md, "Where the tables come from", and
 * docs/explored-designs.md, "The tables trained again".
 */
#include "seqlz.h"

#if QUETSCHN_PAGE_BITS != 12
#include "seqlz_default_tables_16k.inc"
#else
#include "seqlz_default_tables_4k.inc"
#endif
