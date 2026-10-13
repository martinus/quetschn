// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * swapmap [swap type]: as root, for every page of every process that is in swap (type 0, the first
 * swap device, by default), one line: swap offset, pid, process name, address, mapping. A zram dump's
 * page n is the page of swap offset n, so this gives each page of a dump its process and its mapping.
 * Only the mappings that smaps shows with Swap > 0 are read from pagemap. A page that is shared, e.g.
 * of zygote and an app forked from it, has a line for each process.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE 4096

static uint64_t buf[1 << 16];

static void name_of(int pid, char* out, size_t n) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    ssize_t len = fd >= 0 ? read(fd, out, n - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (len <= 0) {
        snprintf(path, sizeof path, "/proc/%d/comm", pid);
        fd = open(path, O_RDONLY);
        len = fd >= 0 ? read(fd, out, n - 1) : -1;
        if (fd >= 0)
            close(fd);
    }
    if (len < 0)
        len = 0;
    out[len] = 0;
    for (char* p = out; *p; p++)
        if (*p == '\n' || *p == '\t' || *p == ' ')
            *p = 0;
    if (!*out)
        strcpy(out, "?");
}

static void scan(int pid, unsigned type) {
    char path[64], line[4096], name[256], map[4096];
    snprintf(path, sizeof path, "/proc/%d/smaps", pid);
    FILE* f = fopen(path, "r");
    if (!f)
        return;
    snprintf(path, sizeof path, "/proc/%d/pagemap", pid);
    int pm = open(path, O_RDONLY);
    if (pm < 0) {
        fclose(f);
        return;
    }
    name_of(pid, name, sizeof name);
    unsigned long start = 0, end = 0;
    map[0] = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        int off = 0;
        if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %n", &a, &b, &off) == 2 && off > 0) {
            start = a;
            end = b;
            snprintf(map, sizeof map, "%s", line + off);
            map[strcspn(map, "\n")] = 0;
            continue;
        }
        unsigned long kb;
        if (sscanf(line, "Swap: %lu kB", &kb) != 1 || kb == 0)
            continue;
        for (unsigned long v = start; v < end;) {
            size_t n = (end - v) / PAGE;
            if (n > sizeof buf / sizeof buf[0])
                n = sizeof buf / sizeof buf[0];
            ssize_t got = pread(pm, buf, n * 8, (off_t)(v / PAGE * 8));
            if (got <= 0)
                break;
            n = (size_t)got / 8;
            for (size_t i = 0; i < n; i++) {
                uint64_t e = buf[i];
                // bit 62: swapped; bits 0-4 the swap type, 5-54 the offset
                if (!(e >> 62 & 1) || (e & 31) != type)
                    continue;
                printf("%llu\t%d\t%s\t%lx\t%s\n",
                       (unsigned long long)(e >> 5 & ((1ULL << 50) - 1)),
                       pid,
                       name,
                       v + i * PAGE,
                       map);
            }
            v += n * PAGE;
        }
    }
    close(pm);
    fclose(f);
}

int main(int argc, char** argv) {
    unsigned type = argc > 1 ? (unsigned)atoi(argv[1]) : 0;
    DIR* d = opendir("/proc");
    struct dirent* e;
    while ((e = readdir(d)))
        if (e->d_name[0] >= '1' && e->d_name[0] <= '9')
            scan(atoi(e->d_name), type);
    closedir(d);
    return 0;
}
