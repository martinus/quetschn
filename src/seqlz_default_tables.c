// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The code lengths compiled in. They are part of the format: a page decodes
 * only with the tables it was compressed with (docs/format.md). For 4 KiB pages
 * they are in seqlz_default_tables_4k.inc, which
 * tools/kernel-port/gen-seqlz-tables.py makes from the counts in
 * bench/seqlz_counts_4k.txt. quetschn-seqlz-train counted those on 524 912
 * pages: the resident pages of the development machine, 60 132 pages of its
 * zram dump of 28 September, and the Mi 9T's first zram dump
 * (phone-mi9t-2026-10-03) five times, so that the phone does not lose what the
 * desktop's pages gain. None of these pages is in the dumps the benchmarks
 * measure, so the tables never saw the pages they are measured on
 * (docs/explored-designs.md, "The tables trained again"). Counted with
 *   quetschn-seqlz-train --corpus train-rz-phone5 --counts
 * See docs/format.md, "Where the tables come from".
 */
#include "seqlz.h"

#if QUETSCHN_PAGE_BITS != 12
#include "seqlz_default_tables_16k.inc"
#else
#include "seqlz_default_tables_4k.inc"
#endif
