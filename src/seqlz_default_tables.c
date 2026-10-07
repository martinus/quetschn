// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The code lengths seqlz compiles in, part of the format (docs/format.md). For 4 KiB pages they are in
 * seqlz_default_tables_4k.inc, from bench/seqlz_train_main.cpp on 524912 pages: the resident pages of
 * the development machine, 60132 pages of its zram dump of 28 September, and 5 times the first zram
 * dump of the Mi 9T (phone-mi9t-2026-10-03), so that the phone does not lose what the swapped desktop
 * pages gain. None of them is in the dumps the benchmarks measure, so that the tables have not seen
 * the pages they are measured on (docs/explored-designs.md, "The tables trained again"). Generated with
 *   quetschn-seqlz-train --corpus train-rz-phone5
 */
#include "seqlz.h"

#if QUETSCHN_PAGE_BITS != 12
#    include "seqlz_default_tables_16k.inc"
#else
#    include "seqlz_default_tables_4k.inc"
#endif
