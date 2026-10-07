// SPDX-License-Identifier: MIT OR GPL-2.0-only
// The grouping of quetschn-swap-bursts: swap-ins per interval into bursts, bursts by size.
#pragma once

#define SWAP_BURST_CLASSES 24

struct swap_bursts {
    long gap;                 /* empty intervals a burst may have inside it */
    long idle;                /* empty intervals since the last swap-in */
    unsigned long long burst; /* swap-ins of the burst going on, 0 for none */
    unsigned long long bursts[SWAP_BURST_CLASSES];
    unsigned long long pages[SWAP_BURST_CLASSES];
};

/* the size class of a burst of n pages: 0 for 1, 1 for 2 to 3, 2 for 4 to 7, ... */
static inline int swap_burst_class(unsigned long long n) {
    int c = 0;

    while (n > 1 && c < SWAP_BURST_CLASSES - 1) {
        n >>= 1;
        c++;
    }
    return c;
}

static inline void swap_bursts_end(struct swap_bursts* s) {
    if (s->burst) {
        s->bursts[swap_burst_class(s->burst)]++;
        s->pages[swap_burst_class(s->burst)] += s->burst;
        s->burst = 0;
    }
}

/* one interval with n swap-ins */
static inline void swap_bursts_add(struct swap_bursts* s, unsigned long long n) {
    if (n) {
        s->burst += n;
        s->idle = 0;
    } else if (s->burst && ++s->idle > s->gap) {
        swap_bursts_end(s);
    }
}
