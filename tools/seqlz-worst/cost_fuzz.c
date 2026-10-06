// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * A fuzz target that searches for slow pages, as PerfFuzz does: it counts the instructions of a call in
 * user space and calls one of COST_BUCKETS functions for that count, by steps of 256 to 1024. To the
 * fuzzer a bucket not reached before is new coverage, so it keeps every input that costs more than any
 * before and mutates on from it. Built twice:
 *
 *   seqlz_compress_cost_fuzz  the input is a page: the compressor's instructions, and in the upper
 *                             half of the buckets the decoder's on what it wrote
 *   seqlz_decode_cost_fuzz    the input is a compressed page: the decoder's instructions if it is valid
 *
 * The counts include the coverage instrumentation; quetschn-seqlz-worst count and count-stream count
 * what the fuzzer found without it.
 */
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "seqlz.h"

#define COST_BUCKETS 1024U

/* COST_BUCKETS functions of their own, each one edge the fuzzer sees */
static volatile unsigned int sink;
typedef void (*mark_fn)(void);
#define MARK(n)                                            \
    static __attribute__((noinline)) void mark_##n(void) { \
        sink = n;                                          \
    }
#define MARK4(n) MARK(n##0) MARK(n##1) MARK(n##2) MARK(n##3)
#define MARK16(n) MARK4(n##0) MARK4(n##1) MARK4(n##2) MARK4(n##3)
#define MARK64(n) MARK16(n##0) MARK16(n##1) MARK16(n##2) MARK16(n##3)
#define MARK256(n) MARK64(n##0) MARK64(n##1) MARK64(n##2) MARK64(n##3)
MARK256(1)
MARK256(2) MARK256(3) MARK256(4)
#define REF(n) mark_##n,
#define REF4(n) REF(n##0) REF(n##1) REF(n##2) REF(n##3)
#define REF16(n) REF4(n##0) REF4(n##1) REF4(n##2) REF4(n##3)
#define REF64(n) REF16(n##0) REF16(n##1) REF16(n##2) REF16(n##3)
#define REF256(n) REF64(n##0) REF64(n##1) REF64(n##2) REF64(n##3)
    static const mark_fn marks[COST_BUCKETS] = {REF256(1) REF256(2) REF256(3) REF256(4)};

/* the bucket of cost by steps of step among n buckets from first on, the last for all above */
static void mark(unsigned long long cost, unsigned int step, unsigned int first, unsigned int n) {
    unsigned long long b = cost / step;

    marks[first + (b < n ? (unsigned int)b : n - 1U)]();
}

static int insn_fd = -1;

static unsigned long long instructions(void) {
    unsigned long long v = 0;

    if (read(insn_fd, &v, sizeof v) != (ssize_t)sizeof v)
        abort();
    return v;
}

static struct seqlz_tables* tables;
static unsigned char c[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned long long i0, i1;

    if (insn_fd < 0) {
        struct perf_event_attr a;

        memset(&a, 0, sizeof a);
        a.type = PERF_TYPE_HARDWARE;
        a.size = sizeof a;
        a.config = PERF_COUNT_HW_INSTRUCTIONS;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        insn_fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
        tables = malloc(seqlz_tables_size());
        if (insn_fd < 0 || !tables || seqlz_tables_init(tables, &seqlz_default_own))
            abort();
    }
#ifdef COST_COMPRESS
    static struct seqlz_state st;
    unsigned int len;

    if (size != SEQLZ_PAGE)
        return 0;
    i0 = instructions();
    len = seqlz_compress(tables, &st, data, c, sizeof c, 1);
    i1 = instructions();
    if (!len)
        abort();
    mark(i1 - i0, 1024, 0, COST_BUCKETS / 2U);
    /* zram stores larger pages as they are and never decodes them */
    if (len < 3625U) {
        i0 = instructions();
        if (seqlz_decode(tables, c, len, out, scratch) || memcmp(out, data, SEQLZ_PAGE))
            abort();
        i1 = instructions();
        mark(i1 - i0, 512, COST_BUCKETS / 2U, COST_BUCKETS / 2U);
    }
#else
    if (size > 2U * SEQLZ_PAGE)
        return 0;
    memcpy(c, data, size);
    i0 = instructions();
    if (seqlz_decode(tables, c, (unsigned int)size, out, scratch) == 0) {
        i1 = instructions();
        mark(i1 - i0, 256, 0, COST_BUCKETS);
    }
#endif
    return 0;
}
