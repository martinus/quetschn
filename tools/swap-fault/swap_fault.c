// SPDX-License-Identifier: MIT OR GPL-2.0-only
// zram as swap, the whole page fault, on a running Linux, e.g. a rooted phone. Run as root:
//
//   swap_fault <pages file> <cpu> <first zram index> <algo> [<algo> ...]
//
// zram<first + k> gets algo k, 512 MiB and a swap header; the devices must exist and be unused (on
// Linux 4.14 /sys/class/zram-control/hot_add makes them). The algorithms take turns, in a rotating
// order, each swapped on alone while it runs, at the highest priority; other swap devices should be
// off. The pages of the file and N_SAME same-filled pages, which zram stores without the codec, go into
// one anonymous mapping, are swapped out and read back by touching them, one page fault each, in a
// random order. Per algorithm and run, each on a new mapping:
//
// - swap-out of the whole mapping in one call, timed: the mean per page with reclaim's batching, then
//   swap-in of every page, timed, after touching another page
// - swap-out of every page in its own call, timed, with the cost of the call, then swap-in of every
//   page after reading other data, outside the timed window, once for each size in EVICT, a comma
//   separated list in bytes (default 2 MiB); the last size is reported as "cold"
// - write faults on new anonymous pages, the page fault without swap
//
// Swap-out is MADV_PAGEOUT, or /proc/self/reclaim where the kernel has that instead (Android's 4.14).
// page-cluster is 0 while it runs, so a fault reads one page. Prints p50 / p90 / p99 and the mean over
// the pages of the median of 3 runs per page, and the medians of every page.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/swap.h>
#include <time.h>
#include <unistd.h>

#ifndef MADV_PAGEOUT
#    define MADV_PAGEOUT 21
#endif

#define MAX_ALGOS 8
#define REPS 3
#define N_SAME 2000
#define N_FRESH 2000

static volatile char sink;
static int reclaim_fd = -1;

static void put(const char* path, const char* v) {
    int fd = open(path, O_WRONLY);
    if (fd < 0 || write(fd, v, strlen(v)) < 0) {
        printf("cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    close(fd);
}

static long long now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int cmp(const void* a, const void* b) {
    long long x = *(const long long*)a, y = *(const long long*)b;
    return (x > y) - (x < y);
}

static unsigned long vmstat(const char* name) {
    char line[128];
    size_t len = strlen(name);
    unsigned long v = 0;
    FILE* f = fopen("/proc/vmstat", "r");
    while (f && fgets(line, sizeof line, f))
        if (strncmp(line, name, len) == 0 && line[len] == ' ')
            v = strtoul(line + len + 1, NULL, 10);
    if (f)
        fclose(f);
    return v;
}

static void pageout(char* p, size_t len) {
    if (reclaim_fd < 0) {
        if (madvise(p, len, MADV_PAGEOUT) == 0)
            return;
        if (errno != EINVAL) {
            printf("MADV_PAGEOUT: %s\n", strerror(errno));
            exit(1);
        }
        reclaim_fd = open("/proc/self/reclaim", O_WRONLY);
        if (reclaim_fd < 0) {
            printf("neither MADV_PAGEOUT nor /proc/self/reclaim\n");
            exit(1);
        }
    }
    char arg[64];
    int n = snprintf(arg, sizeof arg, "%lu %zu", (unsigned long)p, len);
    if (write(reclaim_fd, arg, (size_t)n) != n) {
        printf("/proc/self/reclaim: %s\n", strerror(errno));
        exit(1);
    }
}

static int same_filled(const char* p) {
    const unsigned long* w = (const unsigned long*)p;
    for (size_t k = 1; k < 4096 / sizeof *w; k++)
        if (w[k] != w[0])
            return 0;
    return 1;
}

/* the median of the REPS runs of each page in the list, then p50 / p90 / p99 and their mean */
static void report(const char* algo, const char* what, const long long* t, const size_t* list, size_t n_list, size_t n) {
    long long* med = malloc(sizeof(long long) * n_list);
    long long sum = 0;
    for (size_t k = 0; k < n_list; k++) {
        long long v[REPS];
        for (int r = 0; r < REPS; r++)
            v[r] = t[(size_t)r * n + list[k]];
        qsort(v, REPS, sizeof v[0], cmp);
        med[k] = v[REPS / 2];
        sum += med[k];
    }
    qsort(med, n_list, sizeof med[0], cmp);
    printf("RESULT %-15s %-34s n %6zu: p50 %lld p90 %lld p99 %lld mean %lld ns\n",
           algo,
           what,
           n_list,
           med[n_list / 2],
           med[n_list * 9 / 10],
           med[n_list * 99 / 100],
           sum / (long long)n_list);
    free(med);
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: swap_fault <pages file> <cpu> <first zram index> <algo> [<algo> ...]\n");
        return 2;
    }
    int cpu = atoi(argv[2]), first = atoi(argv[3]), n_algos = argc - 4;
    if (n_algos > MAX_ALGOS)
        n_algos = MAX_ALGOS;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0)
        printf("cannot pin to cpu %d\n", cpu);

    int pf = open(argv[1], O_RDONLY);
    if (pf < 0) {
        perror(argv[1]);
        return 1;
    }
    size_t n_corpus = (size_t)lseek(pf, 0, SEEK_END) / 4096, n = n_corpus + N_SAME;
    char* src = malloc(n * 4096);
    if (pread(pf, src, n_corpus * 4096, 0) != (ssize_t)(n_corpus * 4096)) {
        perror("pread");
        return 1;
    }
    close(pf);
    for (size_t i = 0; i < N_SAME; i++)
        memset(src + (n_corpus + i) * 4096, (int)(i % 255 + 1), 4096);
    size_t *perm = malloc(sizeof(size_t) * n), *codec = malloc(sizeof(size_t) * n), *same = malloc(sizeof(size_t) * n);
    size_t n_codec = 0, n_same = 0;
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
    /* the sizes of other data to read before a cold swap-in, a comma separated list in EVICT */
    size_t evict[8], n_evict = 0, max_evict = 0;
    {
        const char* e = getenv("EVICT") ? getenv("EVICT") : "2097152";
        char* end;
        while (*e && n_evict < 8) {
            evict[n_evict] = strtoul(e, &end, 0);
            if (evict[n_evict] > max_evict)
                max_evict = evict[n_evict];
            n_evict++;
            e = *end == ',' ? end + 1 : end;
            if (end == e && *e != 0)
                break;
        }
    }
    char* other = malloc(max_evict + 64);
    memset(other, 1, max_evict + 64);

    char cluster[16] = {0};
    {
        int fd = open("/proc/sys/vm/page-cluster", O_RDONLY);
        if (fd < 0 || read(fd, cluster, sizeof cluster - 1) <= 0)
            strcpy(cluster, "3");
        close(fd);
    }
    put("/proc/sys/vm/page-cluster", "0");

    char devs[MAX_ALGOS][64];
    /* per algorithm: 0 swap-in warm, 1 swap-out per page, 2 + e swap-in after evict[e] bytes */
    long long *t[MAX_ALGOS][2 + 8], *fresh[MAX_ALGOS], batch[MAX_ALGOS][REPS];
    for (int a = 0; a < n_algos; a++) {
        char path[128];
        static union {
            char page[4096];
            struct {
                char bootbits[1024];
                unsigned int version, last_page, nr_badpages;
            } info;
        } hdr;
        int z = first + a;

        for (size_t c = 0; c < 2 + n_evict; c++)
            t[a][c] = malloc(sizeof(long long) * n * REPS);
        fresh[a] = malloc(sizeof(long long) * N_FRESH * REPS);
        snprintf(path, sizeof path, "/sys/block/zram%d/reset", z);
        put(path, "1");
        snprintf(path, sizeof path, "/sys/block/zram%d/comp_algorithm", z);
        put(path, argv[4 + a]);
        snprintf(path, sizeof path, "/sys/block/zram%d/disksize", z);
        put(path, "536870912");
        memset(&hdr, 0, sizeof hdr);
        hdr.info.version = 1;
        hdr.info.last_page = 512 * 256 - 1;
        memcpy(hdr.page + 4096 - 10, "SWAPSPACE2", 10);
        /* Android has /dev/block/zram<n>, a plain Linux /dev/zram<n> */
        snprintf(devs[a], sizeof devs[a], "/dev/block/zram%d", z);
        if (access(devs[a], F_OK) != 0)
            snprintf(devs[a], sizeof devs[a], "/dev/zram%d", z);
        int fd = open(devs[a], O_WRONLY);
        if (fd < 0 || pwrite(fd, hdr.page, 4096, 0) != 4096) {
            printf("cannot write the swap header of %s\n", devs[a]);
            return 1;
        }
        close(fd);
    }

    for (int r = 0; r < REPS; r++) {
        for (int k = 0; k < n_algos; k++) {
            int a = (r + k) % n_algos;
            if (swapon(devs[a], SWAP_FLAG_PREFER | 32767) != 0) {
                printf("swapon %s: %s\n", devs[a], strerror(errno));
                return 1;
            }
            for (size_t c = 0; c < 1 + n_evict; c++) {
                /* c 0: one call for all, then swap-in warm; c > 0: one call per page, then swap-in
                 * after evict[c - 1] bytes of other data */
                long long* tc = t[a][c == 0 ? 0 : 1 + c] + (size_t)r * n;
                char* m = mmap(NULL, n * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                madvise(m, n * 4096, MADV_NOHUGEPAGE);
                memcpy(m, src, n * 4096);
                unsigned long out0 = vmstat("pswpout");
                long long t0 = now();
                if (c == 0)
                    pageout(m, n * 4096);
                else
                    for (size_t i = 0; i < n; i++) {
                        long long t1 = now();
                        pageout(m + perm[i] * 4096, 4096);
                        if (c == 1)
                            t[a][1][(size_t)r * n + perm[i]] = now() - t1;
                    }
                if (c == 0)
                    batch[a][r] = (now() - t0) / (long long)n;
                unsigned long out1 = vmstat("pswpout");
                if (c == 0) {
                    char path[128], st[256] = {0};
                    snprintf(path, sizeof path, "/sys/block/zram%d/mm_stat", first + a);
                    int sf = open(path, O_RDONLY);
                    read(sf, st, sizeof st - 1);
                    close(sf);
                    printf("RESULT %-15s %s mm_stat %s", argv[4 + a], r == 0 ? "" : "again", st);
                }
                unsigned char* vec = malloc(n);
                size_t resident = 0;
                mincore(m, n * 4096, vec);
                for (size_t i = 0; i < n; i++)
                    resident += vec[i] & 1;
                free(vec);
                unsigned long in0 = vmstat("pswpin");
                for (size_t i = 0; i < n; i++) {
                    char* p = m + perm[i] * 4096;
                    if (c > 0) {
                        unsigned long s = 0;
                        for (size_t j = 0; j < evict[c - 1]; j += 64)
                            s += (unsigned char)other[j];
                        sink += (char)s;
                    }
                    long long t1 = now();
                    sink += *(volatile char*)p;
                    tc[perm[i]] = now() - t1;
                }
                unsigned long in1 = vmstat("pswpin");
                if (memcmp(m, src, n * 4096) != 0)
                    printf("RESULT %s: the pages came back different\n", argv[4 + a]);
                printf("RESULT %-15s run %d %-6s %8zu: %lu pages out, %zu of %zu still resident, %lu in\n",
                       argv[4 + a],
                       r,
                       c == 0 ? "warm" : "evict",
                       c == 0 ? 0 : evict[c - 1],
                       out1 - out0,
                       resident,
                       n,
                       in1 - in0);
                munmap(m, n * 4096);
            }
            if (swapoff(devs[a]) != 0)
                printf("swapoff %s: %s\n", devs[a], strerror(errno));
            /* the page fault without swap: a write to a new anonymous page */
            char* f = mmap(NULL, N_FRESH * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            madvise(f, N_FRESH * 4096, MADV_NOHUGEPAGE);
            for (size_t i = 0; i < N_FRESH; i++) {
                long long t1 = now();
                *(volatile char*)(f + i * 4096) = 1;
                fresh[a][(size_t)r * N_FRESH + i] = now() - t1;
            }
            munmap(f, N_FRESH * 4096);
            fflush(stdout);
        }
    }
    put("/proc/sys/vm/page-cluster", cluster);

    for (int a = 0; a < n_algos; a++) {
        size_t all[N_FRESH];
        qsort(batch[a], REPS, sizeof batch[a][0], cmp);
        printf("RESULT %-15s swap-out, one call for all pages: median of %d runs %lld ns per page\n",
               argv[4 + a],
               REPS,
               batch[a][REPS / 2]);
        report(argv[4 + a], "swap-out, one call per page", t[a][1], codec, n_codec, n);
        report(argv[4 + a], "swap-out, same-filled", t[a][1], same, n_same, n);
        report(argv[4 + a], "swap-in, warm", t[a][0], codec, n_codec, n);
        report(argv[4 + a], "swap-in, warm, same-filled", t[a][0], same, n_same, n);
        /* the last size of EVICT is "cold" */
        for (size_t e = 0; e < n_evict; e++) {
            char what[64];
            if (e + 1 == n_evict)
                snprintf(what, sizeof what, "swap-in, cold");
            else
                snprintf(what, sizeof what, "swap-in, after %zu KiB", evict[e] >> 10);
            report(argv[4 + a], what, t[a][2 + e], codec, n_codec, n);
            strcat(what, ", same-filled");
            report(argv[4 + a], what, t[a][2 + e], same, n_same, n);
        }
        for (size_t i = 0; i < N_FRESH; i++)
            all[i] = i;
        report(argv[4 + a], "write fault, new page", fresh[a], all, N_FRESH, N_FRESH);
    }
    /* per page the medians, for plots: index, same-filled, then per algorithm swap-out, swap-in warm, cold */
    for (size_t i = 0; i < n; i++) {
        printf("PAGE %zu %d", i, same_filled(src + i * 4096));
        for (int a = 0; a < n_algos; a++)
            for (int k = 0; k < 3; k++) {
                const size_t col[3] = {1, 0, 1 + n_evict};
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
