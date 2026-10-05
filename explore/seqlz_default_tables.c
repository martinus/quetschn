// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The code lengths seqlz compiles in, part of the format (FORMAT.md). For 4 KiB pages they are in
 * seqlz_default_tables_4k.inc, from bench/seqlz_train_main.cpp on 138268 pages: the resident pages of
 * the development machine and the first zram dump of the Mi 9T (phone-mi9t-2026-10-03), not on the
 * dumps the benchmarks measure, so that the tables have not seen the pages they are measured on.
 * Tables trained on one phone's pages saved at most 1% on another dump of that phone and cost about 1%
 * on the desktop (docs/explored-designs.md). Generated with
 *   quetschn-seqlz-train --corpus train-resident-phone1003
 */
#include "seqlz.h"

#if QUETSCHN_PAGE_BITS != 12
#    include "seqlz_default_tables_16k.inc"
#else
#    include "seqlz_default_tables_4k.inc"
#endif
