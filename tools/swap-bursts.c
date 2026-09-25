// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// How swap-ins come: alone or in bursts, and how large the bursts are (PLAN.md §1.1). Reads pswpin
// and pswpout of /proc/vmstat every interval for a while. A burst is a run of intervals with swap-ins,
// with gaps of at most --gap empty intervals. Prints the bursts by size and the share of all swap-ins
// in bursts of each size: a burst of n pages waits for n decompressions, so its wait is n times the
// mean, where a single swap-in waits for one page.
//
// usage: quetschn-swap-bursts [--interval-ms <n>] [--gap <n>] [--seconds <n>]
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "swap_bursts.h"

static int read_vmstat(unsigned long long* in, unsigned long long* out) {
    FILE* f = fopen("/proc/vmstat", "r");
    char key[64];
    unsigned long long v;
    int found = 0;

    if (!f)
        return -1;
    while (fscanf(f, "%63s %llu", key, &v) == 2) {
        if (strcmp(key, "pswpin") == 0) {
            *in = v;
            found |= 1;
        } else if (strcmp(key, "pswpout") == 0) {
            *out = v;
            found |= 2;
        }
    }
    fclose(f);
    return found == 3 ? 0 : -1;
}

int main(int argc, char** argv) {
    long interval_ms = 10, gap = 5, seconds = 3600;
    unsigned long long in0, out0, in = 0, out = 0, last_in;
    struct swap_bursts b = {0};
    long k, n;
    struct timespec next;

    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--interval-ms") == 0)
            interval_ms = atol(argv[i + 1]);
        else if (strcmp(argv[i], "--gap") == 0)
            gap = atol(argv[i + 1]);
        else if (strcmp(argv[i], "--seconds") == 0)
            seconds = atol(argv[i + 1]);
        else {
            fprintf(stderr, "usage: quetschn-swap-bursts [--interval-ms <n>] [--gap <n>] [--seconds <n>]\n");
            return 2;
        }
    }
    if (interval_ms <= 0 || gap < 0 || seconds <= 0 || read_vmstat(&in0, &out0)) {
        fprintf(stderr, "error: bad arguments, or no pswpin and pswpout in /proc/vmstat\n");
        return 2;
    }
    b.gap = gap;
    in = in0;
    out = out0;
    last_in = in0;
    n = seconds * 1000 / interval_ms;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (k = 0; k < n; k++) {
        next.tv_nsec += interval_ms * 1000000L;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        if (read_vmstat(&in, &out))
            return 1;
        swap_bursts_add(&b, in - last_in);
        last_in = in;
    }
    swap_bursts_end(&b);
    printf("%ld s, every %ld ms, gaps of up to %ld intervals: %llu swap-ins, %llu swap-outs, %.3f in per out\n",
           seconds,
           interval_ms,
           gap,
           in - in0,
           out - out0,
           out > out0 ? (double)(in - in0) / (double)(out - out0) : 0.0);
    printf("%-18s %10s %12s %10s\n", "burst of pages", "bursts", "swap-ins", "share");
    for (int c = 0; c < SWAP_BURST_CLASSES; c++) {
        if (!b.bursts[c])
            continue;
        printf("%8llu to %-6llu %10llu %12llu %9.1f%%\n",
               1ULL << c,
               (2ULL << c) - 1,
               b.bursts[c],
               b.pages[c],
               in > in0 ? 100.0 * (double)b.pages[c] / (double)(in - in0) : 0.0);
    }
    return 0;
}
