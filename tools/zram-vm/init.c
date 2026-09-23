// SPDX-License-Identifier: MIT OR GPL-2.0-only
// /init of the VM of run.sh: writes /pages to /dev/zram0 and reads each page back with O_DIRECT, timed, with the
// prefetch of zram-prefetch.patch off and on (0 none, 1 destination, 2 source, 3 both), warm and with the
// destination or the compressed source flushed from the cache first.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

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
    mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
    mount("sysfs", "/sys", "sysfs", 0, 0);
    mount("proc", "/proc", "proc", 0, 0);
    put("/sys/block/zram0/comp_algorithm", "lz4");
    put("/sys/block/zram0/disksize", "512M");
    int pf = open("/pages", O_RDONLY);
    off_t size = lseek(pf, 0, SEEK_END);
    size_t n = (size_t)size / 4096;
    char* pages;
    posix_memalign((void**)&pages, 4096, (size_t)size);
    pread(pf, pages, (size_t)size, 0);
    int fd = open("/dev/zram0", O_RDWR | O_DIRECT);
    printf("INFO pages %zu, fd %d, size %lld\n", n, fd, (long long)size);
    for (size_t i = 0; i < n; i++)
        pwrite(fd, pages + i * 4096, 4096, (off_t)(i * 4096));
    char* buf;
    posix_memalign((void**)&buf, 4096, 4096);
    {
        ssize_t got = pread(fd, buf, 4096, 4096 * 7);
        printf("INFO read %zd, same %d\n", got, memcmp(buf, pages + 7 * 4096, 4096) == 0);
        char st[256] = {0};
        int sf = open("/sys/block/zram0/mm_stat", O_RDONLY);
        read(sf, st, sizeof st - 1);
        printf("INFO mm_stat %s", st);
    }
    size_t other_n = 2u << 20;
    char* other = malloc(other_n);
    memset(other, 1, other_n);
    int reps = 3;
    long long* t = malloc(sizeof(long long) * n * reps * 4);
    const char* names[4] = {"warm", "destination flushed", "source flushed", "source and destination flushed"};
    volatile long long sink = 0;
    for (int cond = 0; cond < 4; cond++) {
        put("/sys/module/zram/parameters/zram_flush_src", cond >= 2 ? "1" : "0");
        for (int r = 0; r < reps; r++) {
            for (size_t i = 0; i < n; i++) {
                for (int m = 0; m < 4; m++) {
                    int mode = (int)((i + r + m) & 3);
                    char mv[2] = {(char)('0' + mode), 0};
                    put("/sys/module/zram/parameters/zram_prefetch", mv);
                    put("/sys/module/zram/parameters/zram_flush_src", "0");
                    pread(fd, buf, 4096, (off_t)(i * 4096)); /* warm up the path */
                    put("/sys/module/zram/parameters/zram_flush_src", cond >= 2 ? "1" : "0");
                    if (cond == 1 || cond == 3)
                        flush(buf, 4096);
                    long long t0 = now();
                    pread(fd, buf, 4096, (off_t)(i * 4096));
                    t[((size_t)mode * reps + (size_t)r) * n + i] = now() - t0;
                }
            }
        }
        for (int mode = 0; mode < 4; mode++) {
            /* median over the repetitions per page, then percentiles over the pages */
            long long* med = malloc(sizeof(long long) * n);
            for (size_t i = 0; i < n; i++) {
                long long v[3];
                for (int r = 0; r < reps; r++)
                    v[r] = t[((size_t)mode * reps + (size_t)r) * n + i];
                qsort(v, (size_t)reps, sizeof v[0], cmp);
                med[i] = v[reps / 2];
            }
            qsort(med, n, sizeof med[0], cmp);
            printf("RESULT %-32s prefetch %d: p50 %lld p90 %lld p99 %lld ns\n", names[cond], mode, med[n / 2],
                   med[n * 9 / 10], med[n * 99 / 100]);
            free(med);
        }
    }
    fflush(stdout);
    reboot(RB_POWER_OFF);
    return 0;
}
