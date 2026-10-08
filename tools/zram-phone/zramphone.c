// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * zram's write and read latency on the phone, per algorithm, like
 * tools/zram-vm/init.c without its kernel patch:
 *
 *   zramphone <pages file> <first zram index> <algo> [<algo> ...]
 *
 * zram<first + k> gets algo k. Every page is written to every device and read
 * back with O_DIRECT, so that zram decompresses straight into this program's
 * buffer. All devices take turns per page, so for differences below 1 us give
 * it one algo per run, see run.sh. Reads after a read of another page and then
 * reading EVICT bytes of other data, for each size in the environment's EVICT,
 * a comma separated list, default 0 ("warm") and 2 MiB ("cold"). The median of
 * 3 runs per page, then p50 / p90 / p99 and the mean over the pages, and zram's
 * mm_stat per device.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_ALGOS 8
#define REPS 3
#define OTHER (2u << 20)

static void put(const char *path, const char *v)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0 || write(fd, v, strlen(v)) < 0) {
		printf("cannot write %s\n", path);
		exit(1);
	}
	close(fd);
}

static long long now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int cmp(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;
	return (x > y) - (x < y);
}

static void report(const char *algo, const char *what, long long *t, size_t n)
{
	long long *med = malloc(sizeof(long long) * n);
	long long sum = 0;
	for (size_t i = 0; i < n; i++) {
		long long x[REPS];
		for (int r = 0; r < REPS; r++)
			x[r] = t[(size_t)r * n + i];
		qsort(x, REPS, sizeof x[0], cmp);
		med[i] = x[REPS / 2];
		sum += med[i];
	}
	qsort(med, n, sizeof med[0], cmp);
	printf("RESULT %-15s %-7s p50 %lld p90 %lld p99 %lld mean %lld ns\n",
	       algo, what, med[n / 2], med[n * 9 / 10], med[n * 99 / 100],
	       sum / (long long)n);
	free(med);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr,
			"usage: zramphone <pages> <first zram index> <algo> [<algo> ...]\n");
		return 2;
	}
	int first = atoi(argv[2]), n_algos = argc - 3, fds[MAX_ALGOS];
	struct stat sb;
	int pf = open(argv[1], O_RDONLY);
	if (pf < 0 || fstat(pf, &sb) || n_algos > MAX_ALGOS)
		return 1;
	size_t n = (size_t)sb.st_size / 4096;
	char *pages;
	posix_memalign((void **)&pages, 4096, n * 4096);
	if (read(pf, pages, n * 4096) != (ssize_t)(n * 4096))
		return 1;
	/* same-filled pages never reach the compressor, zram keeps them itself */
	size_t m = 0;
	for (size_t i = 0; i < n; i++) {
		const unsigned long *w =
			(const unsigned long *)(pages + i * 4096);
		int same = 1;
		for (size_t k = 1; k < 4096 / sizeof *w; k++)
			same &= w[k] == w[0];
		if (!same)
			memmove(pages + m++ * 4096, pages + i * 4096, 4096);
	}
	printf("RESULT pages %zu of %zu, same-filled ones left out\n", m, n);
	n = m;
	for (int a = 0; a < n_algos; a++) {
		char path[128], dev[64], size[32];
		snprintf(path, sizeof path, "/sys/block/zram%d/reset",
			 first + a);
		put(path, "1");
		snprintf(path, sizeof path, "/sys/block/zram%d/comp_algorithm",
			 first + a);
		put(path, argv[3 + a]);
		snprintf(path, sizeof path, "/sys/block/zram%d/disksize",
			 first + a);
		snprintf(size, sizeof size, "%zu", (n + 256) * 4096);
		put(path, size);
		snprintf(dev, sizeof dev, "/dev/block/zram%d", first + a);
		fds[a] = open(dev, O_RDWR | O_DIRECT);
		if (fds[a] < 0) {
			printf("cannot open %s\n", dev);
			return 1;
		}
	}
	char *buf, *other;
	posix_memalign((void **)&buf, 4096, 4096);
	other = malloc(OTHER);
	memset(other, 1, OTHER);
	size_t evict[16];
	int n_evict = 0;
	{
		const char *e = getenv("EVICT");
		char *end;

		if (!e)
			e = "0,2097152";
		while (*e && n_evict < 16) {
			evict[n_evict] = strtoul(e, &end, 0);
			if (evict[n_evict] > OTHER)
				evict[n_evict] = OTHER;
			n_evict++;
			e = *end == ',' ? end + 1 : end;
			if (end == e && *e != ',')
				break;
		}
	}
	long long *t = malloc(sizeof(long long) * n * REPS * (size_t)n_algos);
	/* writes, compression and zsmalloc: all devices page i, then page i + 1 */
	for (int r = 0; r < REPS; r++)
		for (size_t i = 0; i < n; i++)
			for (int k = 0; k < n_algos; k++) {
				int a = (int)((i + (size_t)r + (size_t)k) %
					      (size_t)n_algos);
				long long t0 = now();
				if (pwrite(fds[a], pages + i * 4096, 4096,
					   (off_t)(i * 4096)) != 4096)
					return 1;
				t[((size_t)a * REPS + (size_t)r) * n + i] =
					now() - t0;
			}
	for (int a = 0; a < n_algos; a++)
		report(argv[3 + a], "write", t + (size_t)a * REPS * n, n);
	for (int a = 0; a < n_algos; a++) {
		char path[128], st[256] = { 0 };
		snprintf(path, sizeof path, "/sys/block/zram%d/mm_stat",
			 first + a);
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			ssize_t got = read(fd, st, sizeof st - 1);
			(void)got;
			close(fd);
		}
		printf("RESULT %-15s mm_stat %s", argv[3 + a], st);
	}
	for (int ev = 0; ev < n_evict; ev++) {
		size_t cold = evict[ev];
		volatile unsigned long sink = 0;
		for (int r = 0; r < REPS; r++)
			for (size_t i = 0; i < n; i++)
				for (int k = 0; k < n_algos; k++) {
					int a = (int)((i + (size_t)r +
						       (size_t)k) %
						      (size_t)n_algos);
					/*
					 * another page first, so that the branch
					 * predictor does not see this one twice
					 */
					if (pread(fds[a], buf, 4096,
						  (off_t)(((i + n / 2) % n) *
							  4096)) != 4096)
						return 1;
					if (cold)
						for (size_t o = 0; o < cold;
						     o += 64)
							sink += (unsigned char)
								other[o];
					long long t0 = now();
					if (pread(fds[a], buf, 4096,
						  (off_t)(i * 4096)) != 4096)
						return 1;
					t[((size_t)a * REPS + (size_t)r) * n +
					  i] = now() - t0;
					if (memcmp(buf, pages + i * 4096,
						   4096)) {
						printf("RESULT %s page %zu does not come back\n",
						       argv[3 + a], i);
						return 1;
					}
				}
		for (int a = 0; a < n_algos; a++) {
			char what[32];

			snprintf(what, sizeof what, "r%zuK", cold >> 10);
			report(argv[3 + a], what, t + (size_t)a * REPS * n, n);
		}
	}
	for (int a = 0; a < n_algos; a++) {
		char path[128];
		close(fds[a]);
		snprintf(path, sizeof path, "/sys/block/zram%d/reset",
			 first + a);
		put(path, "1");
	}
	return 0;
}
