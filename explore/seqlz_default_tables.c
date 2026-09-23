// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The code lengths seqlz compiles in, from bench/seqlz_train_main.cpp on the resident pages of the
 * development machine (56559 pages, docs/explored-designs.md), not on the zram dump the benchmarks
 * measure, so that the tables have not seen the pages they are measured on. Generated with
 *   quetschn-seqlz-train --corpus resident --codec lz4
 *   quetschn-seqlz-train --corpus resident --codec lz4hc --level 3
 */
#include "seqlz.h"

const struct seqlz_lengths seqlz_default_lz4 = {
    .ll = {1, 2, 3, 5, 6, 6, 7, 7, 7, 7, 8, 8, 9, 9, 10, 10, 7, 9, 10, 10, 10, 10, 10, 10, 9},
    .ml = {3, 3, 3, 2, 3, 6, 6, 6, 6, 8, 6, 6, 5, 8, 8, 8, 4, 5, 7, 9, 10, 10, 10, 10, 9},
    .off = {4, 4, 4, 6, 4, 6, 4, 4, 4, 3, 3, 3, 4, 4, 5},
};

const struct seqlz_lengths seqlz_default_lz4hc = {
    .ll = {2, 2, 2, 4, 5, 5, 6, 6, 6, 6, 7, 7, 8, 8, 8, 8, 6, 7, 10, 10, 10, 10, 10, 10, 9},
    .ml = {4, 4, 3, 2, 4, 5, 6, 6, 6, 6, 5, 5, 5, 7, 6, 7, 3, 4, 6, 8, 9, 9, 9, 9, 8},
    .off = {2, 4, 4, 5, 7, 7, 4, 4, 4, 4, 3, 4, 4, 4, 6},
};
