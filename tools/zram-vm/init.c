// SPDX-License-Identifier: MIT OR GPL-2.0-only
// /init of the VM of run.sh. One zram device per algorithm of quetschn.algos= on the kernel command
// line; writes /pages to each and reads each page back with O_DIRECT, timed, so that zram decompresses
// straight into this program's page. Per page all algorithms and the prefetches of zram-prefetch.patch
// (0 none, 2 the compressed data in zram, 8 the same in the backend, which lz4's ignores) in turn, one
// device after the other, warm and with the compressed data or also the destination flushed from the
// cache first. Each timed read comes after a read of the same page, or of another one, which does not
// let the branch predictor learn the page first. Before that, the writes of each page, timed, after a
// write of the same or another page. Prints p50 / p90 / p99 and the mean over the pages of the median
// of 3 runs per page.
//
// With quetschn.mode=swap on the kernel command line it measures zram as swap instead, the whole page
// fault: swap_main().
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/swap.h>
#include <time.h>
#include <unistd.h>

#define MAX_ALGOS 8
#define REPS 3

static void put(const char* path, const char* v) {
    int fd = open(path, O_WRONLY);
    if (fd < 0 || write(fd, v, strlen(v)) < 0)
        printf("cannot write %s\n", path);
    close(fd);
}

static long long now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int cmp(const void* a, const void* b) {
    long long x = *(const long long*)a, y = *(const long long*)b;
    return x < y ? -1 : x > y;
}

/* the mean over the pages of their median times: what a burst of swap-ins waits for, per page */
static long long mean(const long long* x, size_t n) {
    long long sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += x[i];
    return n ? sum / (long long)n : 0;
}

static void flush(void* p, size_t n) {
    for (size_t k = 0; k < n; k += 64)
        __builtin_ia32_clflush((char*)p + k);
    __builtin_ia32_mfence();
}

static unsigned long read_ulong(const char* path) {
    char v[64] = {0};
    int fd = open(path, O_RDONLY);
    read(fd, v, sizeof v - 1);
    close(fd);
    return strtoul(v, NULL, 10);
}

static void print_stats(const char* algo, const char* what, long long* med, size_t n) {
    long long m = mean(med, n);
    qsort(med, n, sizeof med[0], cmp);
    printf("RESULT %-9s %-34s n %6zu: p50 %lld p90 %lld p99 %lld mean %lld ns\n",
           algo,
           what,
           n,
           med[n / 2],
           med[n * 9 / 10],
           med[n * 99 / 100],
           m);
}

/* the median of the REPS runs of each page in the list */
static void print_medians(const char* algo, const char* what, const long long* t, const size_t* list, size_t n_list, size_t n) {
    long long* med = malloc(sizeof(long long) * n_list);
    for (size_t k = 0; k < n_list; k++) {
        long long v[REPS];
        for (int r = 0; r < REPS; r++)
            v[r] = t[(size_t)r * n + list[k]];
        qsort(v, REPS, sizeof v[0], cmp);
        med[k] = v[REPS / 2];
    }
    print_stats(algo, what, med, n_list);
    free(med);
}

static int same_filled(const char* p) {
    const unsigned long* w = (const unsigned long*)p;
    for (size_t k = 1; k < 4096 / sizeof *w; k++)
        if (w[k] != w[0])
            return 0;
    return 1;
}

/*
 * zram as swap, the whole page fault: the pages of the corpus and N_SAME same-filled pages, which zram
 * stores without the codec, in one anonymous mapping, swapped out with MADV_PAGEOUT and read back by
 * touching them, one page fault each, in a random order. Per algorithm and run:
 *
 * - swap-out of the whole mapping in one call, timed: the mean per page with reclaim's batching
 * - swap-in of every page, timed, compressed data as it is
 * - swap-out of every page in its own call, timed: per page, with the cost of the call
 * - swap-in of every page with the compressed data flushed from the cache first; the flush is in the
 *   timed window, the mean time of the flushes, which zram_flush_ns counts, is subtracted from each
 *   page that zram decompressed
 * - swap-in again, flushed, with zcomp_decompress() timed alone in zram (zram_decomp_ns): the
 *   kernel's part, zram's and zsmalloc's, and the decompression, means only
 * - write faults on new anonymous pages, the page fault without swap
 *
 * page-cluster 0 and the synchronous swap-in of zram read one page per fault. The algorithms take
 * turns, in a rotating order, one zram device each, only one of them swapped on.
 */
#define N_SAME 2000
#define N_FRESH 2000

static volatile char sink;

static int swap_main(char* pages, size_t n_corpus, char algos[][32], int n_algos) {
    size_t n = n_corpus + N_SAME;
    char* src = malloc((size_t)n * 4096);
    size_t *perm = malloc(sizeof(size_t) * n), *codec = malloc(sizeof(size_t) * n), *same = malloc(sizeof(size_t) * n);
    size_t n_codec = 0, n_same = 0;
    long long *out_batch = calloc((size_t)n_algos * REPS, sizeof(long long)), *t[MAX_ALGOS][3], *fresh[MAX_ALGOS];

    put("/proc/sys/vm/page-cluster", "0");
    memcpy(src, pages, n_corpus * 4096);
    for (size_t i = 0; i < N_SAME; i++)
        memset(src + (n_corpus + i) * 4096, (int)(i % 255 + 1), 4096);
    for (size_t i = 0; i < n; i++)
        if (same_filled(src + i * 4096))
            same[n_same++] = i;
        else
            codec[n_codec++] = i;
    printf("RESULT swap: %zu pages, %zu of them same-filled (%d added)\n", n, n_same, N_SAME);
    srand(1);
    for (size_t i = 0; i < n; i++)
        perm[i] = i;
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)rand() % (i + 1), x = perm[i];
        perm[i] = perm[j];
        perm[j] = x;
    }

    for (int a = 0; a < n_algos; a++) {
        char path[128], dev[64];
        static union {
            char page[4096];
            struct {
                char bootbits[1024];
                unsigned int version, last_page, nr_badpages;
            } info;
        } hdr;

        for (int c = 0; c < 3; c++)
            t[a][c] = malloc(sizeof(long long) * n * REPS);
        fresh[a] = malloc(sizeof(long long) * N_FRESH * REPS);
        snprintf(path, sizeof path, "/sys/block/zram%d/comp_algorithm", a);
        put(path, algos[a]);
        snprintf(path, sizeof path, "/sys/block/zram%d/disksize", a);
        put(path, "512M");
        memset(&hdr, 0, sizeof hdr);
        hdr.info.version = 1;
        hdr.info.last_page = 512 * 256 - 1;
        memcpy(hdr.page + 4096 - 10, "SWAPSPACE2", 10);
        snprintf(dev, sizeof dev, "/dev/zram%d", a);
        int fd = open(dev, O_WRONLY);
        if (fd < 0 || pwrite(fd, hdr.page, 4096, 0) != 4096)
            printf("RESULT cannot write the swap header of %s\n", dev);
        close(fd);
    }

    /* per algorithm and run, the swap-in of pass 2: same-filled, the others, zcomp_decompress() alone */
    long long split[MAX_ALGOS][REPS][3];
    long long* x3 = malloc(sizeof(long long) * n);
    for (int r = 0; r < REPS; r++) {
        for (int k = 0; k < n_algos; k++) {
            int a = (r + k) % n_algos;
            char dev[64], stat[128];
            snprintf(dev, sizeof dev, "/dev/zram%d", a);
            snprintf(stat, sizeof stat, "/sys/block/zram%d/stat", a);
            if (swapon(dev, SWAP_FLAG_PREFER | 32767) != 0) {
                printf("RESULT swapon %s failed\n", dev);
                return 1;
            }
            char* m = mmap(NULL, n * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            madvise(m, n * 4096, MADV_NOHUGEPAGE);
            memcpy(m, src, n * 4096);
            for (int c = 0; c < 3; c++) {
                /* c 0: one call for all, then swap-in warm; 1: one call per page, then swap-in with
                 * the compressed data flushed; 2: as 1 with zcomp_decompress() timed alone, apart
                 * from the others because the timing takes time too */
                long long* tc = c < 2 ? t[a][c] + (size_t)r * n : x3;
                long long t0 = now();
                if (c != 1)
                    madvise(m, n * 4096, MADV_PAGEOUT);
                else
                    for (size_t i = 0; i < n; i++) {
                        long long t1 = now();
                        madvise(m + perm[i] * 4096, 4096, MADV_PAGEOUT);
                        t[a][2][(size_t)r * n + perm[i]] = now() - t1;
                    }
                if (c == 0)
                    out_batch[(size_t)a * REPS + (size_t)r] = (now() - t0) / (long long)n;
                if (c == 0 && r == 0) {
                    char path[128], st[256] = {0};
                    snprintf(path, sizeof path, "/sys/block/zram%d/mm_stat", a);
                    int sf = open(path, O_RDONLY);
                    read(sf, st, sizeof st - 1);
                    close(sf);
                    printf("RESULT %-15s mm_stat %s", algos[a], st);
                }
                unsigned char* vec = malloc(n);
                size_t resident = 0;
                mincore(m, n * 4096, vec);
                for (size_t i = 0; i < n; i++)
                    resident += vec[i] & 1;
                free(vec);
                put("/sys/module/zram/parameters/zram_flush_src", c >= 1 ? "1" : "0");
                put("/sys/module/zram/parameters/zram_time_decomp", c == 2 ? "1" : "0");
                unsigned long d0 = read_ulong("/sys/module/zram/parameters/zram_decomp_ns");
                unsigned long dn0 = read_ulong("/sys/module/zram/parameters/zram_decomp_n");
                char st0[256] = {0}, st1[256] = {0};
                int fd = open(stat, O_RDONLY);
                read(fd, st0, sizeof st0 - 1);
                close(fd);
                unsigned long f0 = read_ulong("/sys/module/zram/parameters/zram_flush_ns");
                for (size_t i = 0; i < n; i++) {
                    char* p = m + perm[i] * 4096;
                    long long t1 = now();
                    sink += *(volatile char*)p;
                    tc[perm[i]] = now() - t1;
                }
                /* the flushes' mean, off every page that zram decompressed */
                long long flush = (long long)(read_ulong("/sys/module/zram/parameters/zram_flush_ns") - f0) / (long long)n_codec;
                for (size_t k = 0; k < n_codec; k++)
                    tc[codec[k]] -= flush;
                put("/sys/module/zram/parameters/zram_flush_src", "0");
                put("/sys/module/zram/parameters/zram_time_decomp", "0");
                if (c == 2) {
                    unsigned long dn = read_ulong("/sys/module/zram/parameters/zram_decomp_n") - dn0;
                    long long sc = 0, ss = 0;
                    for (size_t k = 0; k < n_codec; k++)
                        sc += tc[codec[k]];
                    for (size_t k = 0; k < n_same; k++)
                        ss += tc[same[k]];
                    split[a][r][0] = ss / (long long)n_same;
                    split[a][r][1] = sc / (long long)n_codec;
                    split[a][r][2] = dn ? (long long)((read_ulong("/sys/module/zram/parameters/zram_decomp_ns") - d0) / dn) : 0;
                }
                fd = open(stat, O_RDONLY);
                read(fd, st1, sizeof st1 - 1);
                close(fd);
                if (memcmp(m, src, n * 4096) != 0)
                    printf("RESULT %s: the pages came back different\n", algos[a]);
                /* the first field of stat: reads completed */
                printf("RESULT %-9s run %d %s: %zu of %zu pages still resident, %lu reads, flush %lld ns per page\n",
                       algos[a],
                       r,
                       c == 0 ? "batch, warm" : c == 1 ? "per page, flushed" : "batch, flushed, timed",
                       resident,
                       n,
                       strtoul(st1, NULL, 10) - strtoul(st0, NULL, 10),
                       flush);
            }
            munmap(m, n * 4096);
            if (swapoff(dev) != 0)
                printf("RESULT swapoff %s failed\n", dev);
            /* the page fault without swap: a write to a new anonymous page */
            char* f = mmap(NULL, N_FRESH * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            madvise(f, N_FRESH * 4096, MADV_NOHUGEPAGE);
            for (size_t i = 0; i < N_FRESH; i++) {
                long long t1 = now();
                *(volatile char*)(f + i * 4096) = 1;
                fresh[a][(size_t)r * N_FRESH + i] = now() - t1;
            }
            munmap(f, N_FRESH * 4096);
        }
    }

    for (int a = 0; a < n_algos; a++) {
        long long batch[REPS];
        size_t all[N_FRESH];
        for (int r = 0; r < REPS; r++)
            batch[r] = out_batch[(size_t)a * REPS + (size_t)r];
        qsort(batch, REPS, sizeof batch[0], cmp);
        printf("RESULT %-9s swap-out, one call for all pages: median of %d runs %lld ns per page\n", algos[a], REPS, batch[REPS / 2]);
        print_medians(algos[a], "swap-out, one call per page", t[a][2], codec, n_codec, n);
        print_medians(algos[a], "swap-out, same-filled", t[a][2], same, n_same, n);
        print_medians(algos[a], "swap-in, warm", t[a][0], codec, n_codec, n);
        print_medians(algos[a], "swap-in, warm, same-filled", t[a][0], same, n_same, n);
        print_medians(algos[a], "swap-in, flushed", t[a][1], codec, n_codec, n);
        print_medians(algos[a], "swap-in, flushed, same-filled", t[a][1], same, n_same, n);
        for (size_t i = 0; i < N_FRESH; i++)
            all[i] = i;
        print_medians(algos[a], "write fault, new page", fresh[a], all, N_FRESH, N_FRESH);
        long long sp[3][REPS];
        for (int r = 0; r < REPS; r++)
            for (int j = 0; j < 3; j++)
                sp[j][r] = split[a][r][j];
        for (int j = 0; j < 3; j++)
            qsort(sp[j], REPS, sizeof sp[j][0], cmp);
        printf("RESULT %-9s swap-in, flushed, decompress timed: same-filled %lld, others %lld, of it zcomp_decompress() %lld ns, means, median of %d runs\n",
               algos[a],
               sp[0][REPS / 2],
               sp[1][REPS / 2],
               sp[2][REPS / 2],
               REPS);
    }
    /* per page the medians, for plots: index, same-filled, then per algorithm swap-out, swap-in warm, flushed */
    for (size_t i = 0; i < n; i++) {
        printf("RESULT PAGE %zu %d", i, same_filled(src + i * 4096));
        for (int a = 0; a < n_algos; a++)
            for (int k = 0; k < 3; k++) {
                static const int col[3] = {2, 0, 1};
                long long v[REPS];
                for (int r = 0; r < REPS; r++)
                    v[r] = t[a][col[k]][(size_t)r * n + i];
                qsort(v, REPS, sizeof v[0], cmp);
                printf(" %lld", v[REPS / 2]);
            }
        printf("\n");
    }
    return 0;
}

int main(void) {
    static const int modes[3] = {0, 2, 8};
    static const char* const conds[4] = {"warm", "compressed data flushed", "both flushed", "flushed, other page first"};
    char cmdline[4096] = {0}, algos[MAX_ALGOS][32];
    int n_algos = 0, fds[MAX_ALGOS], swap;

    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("proc", "/proc", "proc", 0, 0);
    {
        int cf = open("/proc/cmdline", O_RDONLY);
        read(cf, cmdline, sizeof cmdline - 1);
        close(cf);
        swap = strstr(cmdline, "quetschn.mode=swap") != NULL;
        char* a = strstr(cmdline, "quetschn.algos=");
        if (a) {
            a += strlen("quetschn.algos=");
            a[strcspn(a, " \n")] = 0;
        } else {
            a = "lz4";
        }
        for (char* tok = strtok(a, ","); tok && n_algos < MAX_ALGOS; tok = strtok(NULL, ","))
            snprintf(algos[n_algos++], sizeof algos[0], "%s", tok);
    }

    int pf = open("/pages", O_RDONLY);
    off_t size = lseek(pf, 0, SEEK_END);
    size_t n = (size_t)size / 4096;
    char* pages;
    posix_memalign((void**)&pages, 4096, (size_t)size);
    pread(pf, pages, (size_t)size, 0);
    if (swap) {
        swap_main(pages, n, algos, n_algos);
        fflush(stdout);
        reboot(RB_POWER_OFF);
        return 0;
    }
    for (int a = 0; a < n_algos; a++) {
        char path[128], dev[64], primary[32];
        /* primary+secondary: the secondary for zram's recompression of idle pages, before the reads */
        char* plus = strchr(algos[a], '+');
        snprintf(primary, sizeof primary, "%.*s", plus ? (int)(plus - algos[a]) : (int)strlen(algos[a]), algos[a]);
        snprintf(path, sizeof path, "/sys/block/zram%d/comp_algorithm", a);
        put(path, primary);
        if (plus) {
            char arg[64];
            snprintf(path, sizeof path, "/sys/block/zram%d/recomp_algorithm", a);
            snprintf(arg, sizeof arg, "algo=%s priority=1", plus + 1);
            put(path, arg);
        }
        snprintf(path, sizeof path, "/sys/block/zram%d/disksize", a);
        put(path, "512M");
        snprintf(dev, sizeof dev, "/dev/zram%d", a);
        fds[a] = open(dev, O_RDWR | O_DIRECT);
        for (size_t i = 0; i < n; i++)
            pwrite(fds[a], pages + i * 4096, 4096, (off_t)(i * 4096));
        snprintf(path, sizeof path, "/sys/block/zram%d/mm_stat", a);
        char st[256] = {0};
        int sf = open(path, O_RDONLY);
        read(sf, st, sizeof st - 1);
        close(sf);
        printf("RESULT %-8s mm_stat %s", algos[a], st);
    }

    char* buf;
    posix_memalign((void**)&buf, 4096, 4096);
    for (int v = 0; v < 2; v++) {
        /* writes: each page again to every device, in turn, timed: compression and zsmalloc. In the
         * first variant all devices write page i one after the other, so the write before was the same
         * page; in the second device a writes page i + a n / n_algos, so it was another page */
        long long* w = malloc(sizeof(long long) * n * REPS * (size_t)n_algos);
        for (int r = 0; r < REPS; r++)
            for (size_t i = 0; i < n; i++)
                for (int k = 0; k < n_algos; k++) {
                    int a = (int)((i + (size_t)r + (size_t)k) % (size_t)n_algos);
                    size_t j = v == 0 ? i : (i + (size_t)a * (n / (size_t)n_algos)) % n;
                    long long t0 = now();
                    pwrite(fds[a], pages + j * 4096, 4096, (off_t)(j * 4096));
                    w[((size_t)a * REPS + (size_t)r) * n + j] = now() - t0;
                }
        for (int a = 0; a < n_algos; a++) {
            long long* med = malloc(sizeof(long long) * n);
            for (size_t i = 0; i < n; i++) {
                long long x[REPS];
                for (int r = 0; r < REPS; r++)
                    x[r] = w[((size_t)a * REPS + (size_t)r) * n + i];
                qsort(x, REPS, sizeof x[0], cmp);
                med[i] = x[REPS / 2];
            }
            qsort(med, n, sizeof med[0], cmp);
            printf("RESULT %-8s write, %-20s: p50 %lld p90 %lld p99 %lld mean %lld ns\n",
                   algos[a],
                   v == 0 ? "same page before" : "other page before",
                   med[n / 2],
                   med[n * 9 / 10],
                   med[n * 99 / 100],
                   mean(med, n));
            free(med);
        }
        free(w);
    }
    /* recompression of all pages as idle ones, where a device has a secondary algorithm */
    for (int a = 0; a < n_algos; a++) {
        char path[128], st[256] = {0};
        long long t0;

        if (!strchr(algos[a], '+'))
            continue;
        snprintf(path, sizeof path, "/sys/block/zram%d/idle", a);
        put(path, "all");
        snprintf(path, sizeof path, "/sys/block/zram%d/recompress", a);
        t0 = now();
        put(path, "type=idle priority=1");
        printf("RESULT %-8s recompress: %lld ns per page\n", algos[a], (now() - t0) / (long long)n);
        /* the old objects leave holes in zsmalloc's pages until they are compacted */
        snprintf(path, sizeof path, "/sys/block/zram%d/compact", a);
        put(path, "1");
        snprintf(path, sizeof path, "/sys/block/zram%d/mm_stat", a);
        int sf = open(path, O_RDONLY);
        read(sf, st, sizeof st - 1);
        close(sf);
        printf("RESULT %-8s mm_stat after recompress %s", algos[a], st);
    }
    size_t per = (size_t)n_algos * 3;
    long long* t = malloc(sizeof(long long) * n * REPS * per);
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < REPS; r++) {
            for (size_t i = 0; i < n; i++) {
                for (size_t k = 0; k < per; k++) {
                    size_t which = (i + (size_t)r + k) % per;
                    int a = (int)(which % (size_t)n_algos), mode = modes[which / (size_t)n_algos];
                    char mv[4];
                    snprintf(mv, sizeof mv, "%d", mode);
                    put("/sys/module/zram/parameters/zram_prefetch", mv);
                    put("/sys/module/zram/parameters/zram_flush_src", "0");
                    /* warm up the path; with the same page the branch predictor may learn its branches */
                    pread(fds[a], buf, 4096, (off_t)((c == 3 ? (i + n / 2) % n : i) * 4096));
                    put("/sys/module/zram/parameters/zram_flush_src", c >= 1 ? "1" : "0");
                    if (c == 2)
                        flush(buf, 4096);
                    long long t0 = now();
                    pread(fds[a], buf, 4096, (off_t)(i * 4096));
                    t[(which * REPS + (size_t)r) * n + i] = now() - t0;
                }
            }
        }
        for (size_t which = 0; which < per; which++) {
            long long* med = malloc(sizeof(long long) * n);
            for (size_t i = 0; i < n; i++) {
                long long v[REPS];
                for (int r = 0; r < REPS; r++)
                    v[r] = t[(which * REPS + (size_t)r) * n + i];
                qsort(v, REPS, sizeof v[0], cmp);
                med[i] = v[REPS / 2];
            }
            qsort(med, n, sizeof med[0], cmp);
            printf("RESULT %-8s %-26s prefetch %d: p50 %lld p90 %lld p99 %lld mean %lld ns\n",
                   algos[which % (size_t)n_algos],
                   conds[c],
                   modes[which / (size_t)n_algos],
                   med[n / 2],
                   med[n * 9 / 10],
                   med[n * 99 / 100],
                   mean(med, n));
            free(med);
        }
    }
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 0;
}
