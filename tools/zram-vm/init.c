// SPDX-License-Identifier: MIT OR GPL-2.0-only
// /init of the VM of run.sh. One zram device per algorithm of quetschn.algos= on the kernel command
// line; writes /pages to each and reads each page back with O_DIRECT, timed, so that zram decompresses
// straight into this program's page. Per page all algorithms and both prefetches of zram-prefetch.patch
// (0 none, 2 the compressed data) in turn, warm and with the compressed data or also the destination
// flushed from the cache first; before that, the writes of each page, timed. Prints p50 / p90 / p99 over
// the pages of the median of 3 runs per page.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

#define MAX_ALGOS 4
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

static void flush(void* p, size_t n) {
    for (size_t k = 0; k < n; k += 64)
        __builtin_ia32_clflush((char*)p + k);
    __builtin_ia32_mfence();
}

int main(void) {
    static const int modes[2] = {0, 2};
    static const char* const conds[3] = {"warm", "compressed data flushed", "both flushed"};
    char cmdline[4096] = {0}, algos[MAX_ALGOS][32];
    int n_algos = 0, fds[MAX_ALGOS];

    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("proc", "/proc", "proc", 0, 0);
    {
        int cf = open("/proc/cmdline", O_RDONLY);
        read(cf, cmdline, sizeof cmdline - 1);
        close(cf);
        char* a = strstr(cmdline, "quetschn.algos=");
        a = a ? a + strlen("quetschn.algos=") : "lz4";
        for (char* tok = strtok(a, ", \n"); tok && n_algos < MAX_ALGOS; tok = strtok(NULL, ", \n"))
            snprintf(algos[n_algos++], sizeof algos[0], "%s", tok);
    }

    int pf = open("/pages", O_RDONLY);
    off_t size = lseek(pf, 0, SEEK_END);
    size_t n = (size_t)size / 4096;
    char* pages;
    posix_memalign((void**)&pages, 4096, (size_t)size);
    pread(pf, pages, (size_t)size, 0);
    for (int a = 0; a < n_algos; a++) {
        char path[128], dev[64];
        snprintf(path, sizeof path, "/sys/block/zram%d/comp_algorithm", a);
        put(path, algos[a]);
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
    {
        /* writes: each page again to every device, in turn, timed: compression and zsmalloc */
        long long* w = malloc(sizeof(long long) * n * REPS * (size_t)n_algos);
        for (int r = 0; r < REPS; r++)
            for (size_t i = 0; i < n; i++)
                for (int k = 0; k < n_algos; k++) {
                    int a = (int)((i + (size_t)r + (size_t)k) % (size_t)n_algos);
                    long long t0 = now();
                    pwrite(fds[a], pages + i * 4096, 4096, (off_t)(i * 4096));
                    w[((size_t)a * REPS + (size_t)r) * n + i] = now() - t0;
                }
        for (int a = 0; a < n_algos; a++) {
            long long* med = malloc(sizeof(long long) * n);
            for (size_t i = 0; i < n; i++) {
                long long v[REPS];
                for (int r = 0; r < REPS; r++)
                    v[r] = w[((size_t)a * REPS + (size_t)r) * n + i];
                qsort(v, REPS, sizeof v[0], cmp);
                med[i] = v[REPS / 2];
            }
            qsort(med, n, sizeof med[0], cmp);
            printf("RESULT %-8s write                                : p50 %lld p90 %lld p99 %lld ns\n", algos[a],
                   med[n / 2], med[n * 9 / 10], med[n * 99 / 100]);
            free(med);
        }
        free(w);
    }
    size_t per = (size_t)n_algos * 2;
    long long* t = malloc(sizeof(long long) * n * REPS * per);
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < REPS; r++) {
            for (size_t i = 0; i < n; i++) {
                for (size_t k = 0; k < per; k++) {
                    size_t which = (i + (size_t)r + k) % per;
                    int a = (int)(which / 2), mode = modes[which % 2];
                    char mv[2] = {(char)('0' + mode), 0};
                    put("/sys/module/zram/parameters/zram_prefetch", mv);
                    put("/sys/module/zram/parameters/zram_flush_src", "0");
                    pread(fds[a], buf, 4096, (off_t)(i * 4096)); /* warm up the path */
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
            printf("RESULT %-8s %-24s prefetch %d: p50 %lld p90 %lld p99 %lld ns\n", algos[which / 2], conds[c],
                   modes[which % 2], med[n / 2], med[n * 9 / 10], med[n * 99 / 100]);
            free(med);
        }
    }
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 0;
}
