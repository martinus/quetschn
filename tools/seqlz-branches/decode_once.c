// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * decode_once <pages file> <mode>: where seqlz's decoder mispredicts branches when it sees a page for
 * the first time, as in a swap-in. Compresses every page as seqlz-fast-lit, then decodes each page once,
 * in order (mode 0), or twice in a row, timing the second, which the branch predictor learned (mode 1),
 * or not at all (mode 2, the baseline to subtract in perf stat). Same-filled pages and pages zram stores
 * as they are, of 3625 bytes and more, are left out, as zram does. Prints the time per timed decode, in
 * TSC ticks on x86-64, in ns elsewhere.
 * With seqlz_decompress.c built with the kernel's flags, as cmake/kernel_codecs.cmake builds it, see run.sh.
 */
#include "seqlz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__)
#    include <x86intrin.h>
/* TSC ticks */
static unsigned long long ticks(void) {
    return __rdtsc();
}
#    define TICKS "TSC ticks"
#else
#    include <time.h>
/* ns; arm64's counter ticks at only 19.2 MHz on the phones, too coarse for one decode */
static unsigned long long ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}
#    define TICKS "ns"
#endif

int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    int mode = atoi(argv[2]);
    fseek(f, 0, SEEK_END);
    size_t n = (size_t)ftell(f) / 4096;
    fseek(f, 0, SEEK_SET);
    unsigned char* pages = malloc(n * 4096);
    if (fread(pages, 4096, n, f) != n)
        return 1;
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    static struct seqlz_state st;
    seqlz_tables_init(t, &seqlz_default_own);
    unsigned char** c = malloc(n * sizeof *c);
    unsigned* len = malloc(n * sizeof *len);
    size_t m = 0;
    static unsigned char buf[2 * 4096], out[4096], scratch[SEQLZ_SCRATCH];
    for (size_t i = 0; i < n; i++) {
        unsigned char* p = pages + i * 4096;
        size_t k = 1;
        while (k < 4096 && p[k] == p[0])
            k++;
        if (k == 4096)
            continue;
        unsigned l = seqlz_compress(t, &st, p, buf, sizeof buf, 1);
        if (!l || l >= 3625)
            continue;
        c[m] = malloc(l);
        memcpy(c[m], buf, l);
        len[m++] = l;
    }
    unsigned long long cyc = 0;
    if (mode == 2) /* compress only, for perf stat's baseline */
        m = 0;
    for (size_t i = 0; i < m; i++) {
        if (mode == 1)
            seqlz_decode(t, c[i], len[i], out, scratch);
        unsigned long long t0 = ticks();
        if (seqlz_decode(t, c[i], len[i], out, scratch))
            return 2;
        cyc += ticks() - t0;
    }
    printf("%zu pages decoded, mode %d, %llu " TICKS " per timed decode\n", m, mode, m ? cyc / m : 0);
    return 0;
}
