// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * stress.c: /init of the VM of tools/kernel-port/stress.sh. The host's root is
 * mounted read-only at /host. Prints its results as lines that start with
 * RESULT, then powers the VM off. From the kernel command line:
 *   quetschn.corpus=<file>     a corpus' .pages, a path on the host
 *   quetschn.mmstat=1          also phase 1
 *   quetschn.level=1|2         seqlz's level for the swap
 *   quetschn.minutes=<n>       how long the swap is stressed, 0 for no phase 2
 *   quetschn.selftests=1       phase 3, the kernel's selftests of zram, which
 *                              stress.sh puts into /selftests; they run in a
 *                              chroot of the host, for its shell and tools
 *
 * 1. The pages of the corpus written to a new zram device with lz4, seqlz at
 *    level 1 and at level 2: mm_stat right after, then every page read back
 *    and compared.
 * 2. zram as swap with seqlz, under memory pressure: one worker per CPU fills
 *    its part of 1.5 times the free memory with corpus pages, each with its
 *    worker, index and generation in its first 16 bytes. Then until the time
 *    is up, each takes random pages: a quarter get a new corpus page, the rest
 *    are compared with what they should hold, and every 256 steps a random
 *    range is pushed out with MADV_PAGEOUT. At the end every page is compared
 *    once more.
 * 3. The zram selftests, with seqlz instead of lzo.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE 4096
#define CHUNK 256 /* pages per read or write of phase 1 */
#define STAMP 16 /* bytes of a page that say what it is */

static char cmdline[4096];

/* the value of quetschn.<key>=, or def */
static const char *arg(const char *key, const char *def)
{
	static char vals[8][1024];
	static int next;
	char pat[64];
	const char *p;
	char *v = vals[next++ % 8];

	snprintf(pat, sizeof(pat), "quetschn.%s=", key);
	p = strstr(cmdline, pat);
	if (!p)
		return def;
	p += strlen(pat);
	snprintf(v, sizeof(vals[0]), "%.*s", (int)strcspn(p, " \n"), p);
	return v;
}

static int put(const char *path, const char *v)
{
	int fd = open(path, O_WRONLY);
	int ok = fd >= 0 && write(fd, v, strlen(v)) == (ssize_t)strlen(v);

	if (!ok)
		printf("RESULT FAIL cannot write %s to %s: %s\n", v, path,
		       strerror(errno));
	if (fd >= 0)
		close(fd);
	return ok;
}

static void put_dev(int dev, const char *attr, const char *v)
{
	char path[128];

	snprintf(path, sizeof(path), "/sys/block/zram%d/%s", dev, attr);
	put(path, v);
}

static void first_line(const char *path, char *out, size_t n)
{
	FILE *f = fopen(path, "r");

	out[0] = 0;
	if (f) {
		if (!fgets(out, (int)n, f))
			out[0] = 0;
		fclose(f);
	}
	out[strcspn(out, "\n")] = 0;
}

static unsigned long meminfo(const char *key)
{
	char line[256];
	unsigned long v = 0;
	FILE *f = fopen("/proc/meminfo", "r");

	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, key, strlen(key)))
			v = strtoul(line + strlen(key), NULL, 10) * 1024;
	if (f)
		fclose(f);
	return v;
}

static unsigned long vmstat(const char *key)
{
	char name[64];
	unsigned long v, found = 0;
	FILE *f = fopen("/proc/vmstat", "r");

	while (f && fscanf(f, "%63s %lu", name, &v) == 2)
		if (!strcmp(name, key))
			found = v;
	if (f)
		fclose(f);
	return found;
}

/* algo is "lz4" or "seqlz"; level 0 for none */
static void set_algo(int dev, const char *algo, int level)
{
	char v[64];

	put_dev(dev, "comp_algorithm", algo);
	if (level) {
		snprintf(v, sizeof(v), "algo=%s level=%d", algo, level);
		put_dev(dev, "algorithm_params", v);
	}
}

static size_t open_corpus(const char *path, int *fd)
{
	char host[1200];
	struct stat st;

	snprintf(host, sizeof(host), "/host%s", path);
	*fd = open(host, O_RDONLY);
	if (*fd < 0 || fstat(*fd, &st)) {
		printf("RESULT FAIL cannot open the corpus %s\n", host);
		return 0;
	}
	return (size_t)st.st_size / PAGE;
}

/* phase 1: a new device, all pages written, mm_stat, all read back */
static void write_all(const char *corpus, int dev, const char *algo, int level)
{
	char v[64], stat[256], label[32];
	size_t n, k, i, bad = 0;
	int cf, zf;
	char *buf, *back;

	n = open_corpus(corpus, &cf);
	if (!n)
		return;
	snprintf(label, sizeof(label), level ? "%s:%d" : "%s", algo, level);
	set_algo(dev, algo, level);
	snprintf(v, sizeof(v), "%zu", n * PAGE);
	put_dev(dev, "disksize", v);
	snprintf(v, sizeof(v), "/dev/zram%d", dev);
	zf = open(v, O_RDWR | O_DIRECT);
	if (posix_memalign((void **)&buf, PAGE, CHUNK * PAGE) ||
	    posix_memalign((void **)&back, PAGE, CHUNK * PAGE) || zf < 0) {
		printf("RESULT FAIL %s: cannot open %s\n", label, v);
		return;
	}
	for (k = 0; k < n; k += CHUNK) {
		size_t len = (n - k < CHUNK ? n - k : CHUNK) * PAGE;

		if (pread(cf, buf, len, (off_t)(k * PAGE)) != (ssize_t)len ||
		    pwrite(zf, buf, len, (off_t)(k * PAGE)) != (ssize_t)len) {
			printf("RESULT FAIL %s: write at page %zu\n", label, k);
			return;
		}
	}
	snprintf(v, sizeof(v), "/sys/block/zram%d/mm_stat", dev);
	first_line(v, stat, sizeof(stat));
	printf("RESULT mm_stat %-8s %zu pages: %s\n", label, n, stat);
	for (k = 0; k < n; k += CHUNK) {
		size_t len = (n - k < CHUNK ? n - k : CHUNK) * PAGE;

		if (pread(cf, buf, len, (off_t)(k * PAGE)) != (ssize_t)len ||
		    pread(zf, back, len, (off_t)(k * PAGE)) != (ssize_t)len) {
			printf("RESULT FAIL %s: read at page %zu\n", label, k);
			return;
		}
		for (i = 0; i < len / PAGE; i++)
			bad += memcmp(buf + i * PAGE, back + i * PAGE, PAGE) != 0;
	}
	printf("RESULT %s %s read back: %zu of %zu pages differ\n",
	       bad ? "FAIL" : "ok", label, bad, n);
	close(zf);
	close(cf);
	free(buf);
	free(back);
	put_dev(dev, "reset", "1");
}

static uint64_t next(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

/* what page p of worker w holds in generation g: corpus page idx, stamped */
static void make(int cf, size_t idx, int w, size_t p, uint32_t g, char *out)
{
	uint64_t stamp[2] = { ((uint64_t)w << 48) | p, g };

	if (pread(cf, out, PAGE, (off_t)(idx * PAGE)) != PAGE)
		memset(out, 0x5a, PAGE);
	memcpy(out, stamp, STAMP);
}

/* phase 2, one worker: returns 0, or 1 if a page held something else */
static int worker(const char *corpus, int w, size_t pages, long seconds)
{
	size_t n, p, steps = 0, checks = 0, writes = 0, bad = 0;
	uint64_t s = 0x9e3779b97f4a7c15ULL * (uint64_t)(w + 1);
	time_t end = time(NULL) + seconds;
	char want[PAGE];
	uint32_t *gen;
	size_t *idx;
	char *m;
	int cf;

	n = open_corpus(corpus, &cf);
	m = mmap(NULL, pages * PAGE, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	idx = calloc(pages, sizeof(*idx));
	gen = calloc(pages, sizeof(*gen));
	if (!n || m == MAP_FAILED || !idx || !gen) {
		printf("RESULT FAIL worker %d: no memory\n", w);
		return 1;
	}
	madvise(m, pages * PAGE, MADV_NOHUGEPAGE);
	for (p = 0; p < pages; p++) {
		idx[p] = next(&s) % n;
		make(cf, idx[p], w, p, 0, m + p * PAGE);
	}
	while (time(NULL) < end) {
		p = next(&s) % pages;
		if (next(&s) % 4 == 0) {
			idx[p] = next(&s) % n;
			make(cf, idx[p], w, p, ++gen[p], m + p * PAGE);
			writes++;
		} else {
			make(cf, idx[p], w, p, gen[p], want);
			if (memcmp(m + p * PAGE, want, PAGE)) {
				if (bad++ < 5)
					printf("RESULT FAIL worker %d page %zu generation %u differs\n",
					       w, p, gen[p]);
			}
			checks++;
		}
		if (++steps % 256 == 0) {
			size_t at = next(&s) % pages, len = 1 + next(&s) % 512;

			if (at + len > pages)
				len = pages - at;
			madvise(m + at * PAGE, len * PAGE, MADV_PAGEOUT);
		}
	}
	for (p = 0; p < pages; p++) {
		make(cf, idx[p], w, p, gen[p], want);
		if (memcmp(m + p * PAGE, want, PAGE) && bad++ < 5)
			printf("RESULT FAIL worker %d page %zu differs at the end\n",
			       w, p);
	}
	printf("RESULT %s worker %d: %zu pages, %zu steps, %zu compared, %zu written, %zu differ\n",
	       bad ? "FAIL" : "ok", w, pages, steps, checks, writes, bad);
	return bad != 0;
}

static void write_swap_header(int dev, size_t bytes)
{
	static union {
		char page[PAGE];
		struct {
			char bootbits[1024];
			unsigned int version, last_page, nr_badpages;
		} info;
	} hdr;
	char path[64];
	int fd;

	memset(&hdr, 0, sizeof(hdr));
	hdr.info.version = 1;
	hdr.info.last_page = (unsigned int)(bytes / PAGE - 1);
	memcpy(hdr.page + PAGE - 10, "SWAPSPACE2", 10);
	snprintf(path, sizeof(path), "/dev/zram%d", dev);
	fd = open(path, O_WRONLY);
	if (fd < 0 || pwrite(fd, hdr.page, PAGE, 0) != PAGE)
		printf("RESULT FAIL cannot write the swap header of %s\n", path);
	if (fd >= 0)
		close(fd);
}

/* phase 2 */
static void thrash(const char *corpus, int dev, int level, long minutes)
{
	unsigned long free_bytes = meminfo("MemAvailable:");
	unsigned long in0 = vmstat("pswpin"), out0 = vmstat("pswpout");
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	size_t per = free_bytes / 2 * 3 / PAGE / (size_t)cpus;
	char v[64], stat[256];
	int w, status, failed = 0;

	set_algo(dev, "seqlz", level);
	snprintf(v, sizeof(v), "%lu", free_bytes * 4);
	put_dev(dev, "disksize", v);
	write_swap_header(dev, free_bytes * 4);
	snprintf(v, sizeof(v), "/dev/zram%d", dev);
	if (swapon(v, 0)) {
		printf("RESULT FAIL swapon %s: %s\n", v, strerror(errno));
		return;
	}
	printf("RESULT swap: seqlz:%d, %ld workers, %zu pages each, %lu MiB available, %ld minutes\n",
	       level, cpus, per, free_bytes >> 20, minutes);
	fflush(stdout);
	for (w = 0; w < cpus; w++)
		if (fork() == 0) {
			fflush(stdout);
			_exit(worker(corpus, w, per, minutes * 60));
		}
	for (w = 0; w < cpus; w++) {
		if (wait(&status) < 0)
			break;
		if (!WIFEXITED(status)) {
			printf("RESULT FAIL a worker died, signal %d\n",
			       WIFSIGNALED(status) ? WTERMSIG(status) : 0);
			failed = 1;
		}
	}
	snprintf(v, sizeof(v), "/sys/block/zram%d/mm_stat", dev);
	first_line(v, stat, sizeof(stat));
	printf("RESULT swap: %lu pages swapped out, %lu in, mm_stat %s%s\n",
	       vmstat("pswpout") - out0, vmstat("pswpin") - in0, stat,
	       failed ? ", a worker died" : "");
	snprintf(v, sizeof(v), "/dev/zram%d", dev);
	if (swapoff(v))
		printf("RESULT FAIL swapoff %s: %s\n", v, strerror(errno));
	put_dev(dev, "reset", "1");
}

/*
 * phase 3: the selftests in a chroot of the host, with /tmp of its own and
 * /selftests as /tmp/zram, where they can write
 */
static void selftests(void)
{
	const char *cmd =
		"cd /tmp/zram && "
		"sed -i 's/^zram_algs=.*/zram_algs=\"seqlz\"/' zram01.sh zram02.sh && "
		"./zram.sh 2>&1 | sed 's/^/selftest: /'";
	pid_t pid;
	int status;

	mount("tmpfs", "/host/tmp", "tmpfs", 0, 0);
	mkdir("/host/tmp/zram", 0755);
	mount("/selftests", "/host/tmp/zram", NULL, MS_BIND, NULL);
	mount("devtmpfs", "/host/dev", "devtmpfs", 0, 0);
	mount("proc", "/host/proc", "proc", 0, 0);
	mount("sysfs", "/host/sys", "sysfs", 0, 0);
	pid = fork();
	if (pid == 0) {
		if (chroot("/host") || chdir("/"))
			_exit(127);
		execl("/bin/bash", "bash", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	waitpid(pid, &status, 0);
	printf("RESULT selftests exited with %d\n",
	       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

int main(void)
{
	const char *corpus;
	char version[256];
	int cf, level;

	mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
	mount("sysfs", "/sys", "sysfs", 0, 0);
	mount("proc", "/proc", "proc", 0, 0);
	mkdir("/host", 0755);
	if (mount("host", "/host", "9p", MS_RDONLY,
		  "trans=virtio,version=9p2000.L,cache=loose"))
		printf("RESULT FAIL cannot mount the host: %s\n",
		       strerror(errno));
	setvbuf(stdout, NULL, _IOLBF, 0);
	first_line("/proc/cmdline", cmdline, sizeof(cmdline));
	first_line("/proc/version", version, sizeof(version));
	printf("RESULT kernel %s\n", version);
	corpus = arg("corpus", "");
	level = atoi(arg("level", "2"));

	if (*corpus && open_corpus(corpus, &cf)) {
		close(cf);
		if (atoi(arg("mmstat", "0"))) {
			write_all(corpus, 0, "lz4", 0);
			write_all(corpus, 1, "seqlz", 1);
			write_all(corpus, 2, "seqlz", 2);
		}
		if (atol(arg("minutes", "10")) > 0)
			thrash(corpus, 3, level, atol(arg("minutes", "10")));
	}
	if (atoi(arg("selftests", "0")))
		selftests();
	printf("RESULT done\n");
	fflush(stdout);
	sync();
	reboot(RB_POWER_OFF);
	return 0;
}
