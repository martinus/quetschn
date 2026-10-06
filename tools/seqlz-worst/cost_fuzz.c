// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * A fuzz target that searches for slow pages, as PerfFuzz does: it counts the instructions of a call in
 * user space and calls one of COST_BUCKETS functions for that count, see mark(). To the
 * fuzzer a bucket not reached before is new coverage, so it keeps every input that costs more than any
 * before and mutates on from it. Built twice:
 *
 *   seqlz_compress_cost_fuzz  the input is a page: the compressor's instructions, and in the upper
 *                             half of the buckets the decoder's on what it wrote
 *   seqlz_decode_cost_fuzz    the input is a compressed page: the decoder's instructions if it is valid
 *   seqlz_sequence_cost_fuzz  the input is a list of sequences, 4 bytes each, that seqlz_encode() makes
 *                             a valid page of: the decoder's instructions. A mutation changes a
 *                             sequence instead of breaking the Huffman coded stream
 *
 * seqlz is linked without coverage instrumentation, so the counts are those of quetschn-seqlz-worst
 * count and count-streams. Built with -DCOST_MAIN and without the fuzzer, the same file counts inputs
 * the fuzzer found.
 */
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "seqlz.h"

#define COST_BUCKETS 2048U

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
MARK256(2)
MARK256(3)
MARK256(4)
MARK256(5)
MARK256(6)
MARK256(7)
MARK256(8)
#define REF(n) mark_##n,
#define REF4(n) REF(n##0) REF(n##1) REF(n##2) REF(n##3)
#define REF16(n) REF4(n##0) REF4(n##1) REF4(n##2) REF4(n##3)
#define REF64(n) REF16(n##0) REF16(n##1) REF16(n##2) REF16(n##3)
#define REF256(n) REF64(n##0) REF64(n##1) REF64(n##2) REF64(n##3)
static const mark_fn marks[COST_BUCKETS] = {REF256(1) REF256(2) REF256(3) REF256(4) REF256(5) REF256(6) REF256(7) REF256(8)};

/* the cost of the last call per objective, for COST_MAIN */
static unsigned long long cost_last[2];

/* Objective k (0 or 1) has 1024 buckets from 1024 * k on: 64 by steps of step up to the base in the
 * environment variable COST_BASE_<k>, if set, and 960 by steps of 16 from there on, 15 360
 * instructions, so that near the best known cost a few instructions more are new coverage too. The
 * last bucket takes all above, so the base has to move up with the best. COST_PRINT=1 prints each
 * cost. */
static void mark(unsigned long long cost, unsigned int k, unsigned int step) {
    static unsigned long long base[2];
    static int have_base[2];
    unsigned long long b;

    cost_last[k] = cost;
    if (!have_base[k]) {
        char name[] = "COST_BASE_0";
        const char* v;

        name[10] = (char)('0' + k);
        v = getenv(name);
        base[k] = v ? strtoull(v, 0, 10) : ~0ULL;
        have_base[k] = 1;
    }
    if (getenv("COST_PRINT"))
        fprintf(stderr, "cost %u %llu\n", k, cost);
    if (cost < base[k])
        b = cost / step < 63U ? cost / step : 63U;
    else
        b = 64U + ((cost - base[k]) / 16U < 959U ? (cost - base[k]) / 16U : 959U);
    marks[1024U * k + (unsigned int)b]();
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
    mark(i1 - i0, 0, 1024);
    /* zram stores larger pages as they are and never decodes them */
    if (len < 3625U) {
        i0 = instructions();
        if (seqlz_decode(tables, c, len, out, scratch))
            abort();
        i1 = instructions();
        if (memcmp(out, data, SEQLZ_PAGE))
            abort();
        mark(i1 - i0, 1, 512);
    }
#elif defined(COST_SEQUENCES)
    /* byte 0: the literals, random bytes or ones with long codes; then per sequence 4 bytes: ll, ml - 4,
     * and how the offset is chosen with its value */
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    static unsigned char page[SEQLZ_PAGE], lits[SEQLZ_PAGE], pool[256];
    static unsigned int n_pool;
    unsigned int n = 0, n_lit = 0, pos = 0, last = 1, k, len;
    uint64_t rng = 0x9e3779b97f4a7c15ULL;

    if (size < 1)
        return 0;
    if (!n_pool)
        for (k = 0; k < 256; k++)
            if (seqlz_lit_sets[0][k] >= 6 && seqlz_lit_sets[0][k] <= 8)
                pool[n_pool++] = (unsigned char)k;
    for (k = 1; k + 4 <= size && n + 1 < SEQLZ_MAX_SEQUENCES; k += 4) {
        unsigned int ll = data[k] % 40U, ml = 4U + data[k + 1] % 64U, sel = data[k + 2] >> 6, off, i;
        unsigned int v = (unsigned int)(data[k + 2] & 63U) << 8 | data[k + 3];

        if (pos + ll == 0)
            ll = 1;
        if (pos + ll + ml > SEQLZ_PAGE - 1U)
            break;
        if (sel == 0 && last <= pos + ll)
            off = last;
        else if (sel == 1)
            off = 1 + v % 8U;
        else if (sel == 2)
            off = 1 + v % (pos + ll);
        else
            off = 8U * (1 + v % ((pos + ll) / 8U + 1U));
        if (off > pos + ll)
            off = pos + ll;
        for (i = 0; i < ll; i++) {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            page[pos++] = lits[n_lit++] = data[0] & 1 ? pool[rng % n_pool] : (unsigned char)rng;
        }
        for (i = 0; i < ml; i++, pos++)
            page[pos] = page[pos - off];
        seq[n].literals = (unsigned short)ll;
        seq[n].match = (unsigned short)ml;
        seq[n].offset = (unsigned short)off;
        n++;
        last = off;
    }
    /* the last sequence: literals to the end of the page */
    seq[n].literals = (unsigned short)(SEQLZ_PAGE - pos);
    seq[n].match = 0;
    seq[n].offset = 0;
    while (pos < SEQLZ_PAGE)
        page[pos++] = lits[n_lit++] = (unsigned char)(rng += 0x9e37);
    n++;
    len = seqlz_encode(tables, seq, n, lits, n_lit, c, sizeof c, 1);
    if (!len)
        abort();
    /* COST_DUMP=<file>: the compressed page this input makes, for quetschn-seqlz-worst count-streams */
    if (getenv("COST_DUMP")) {
        FILE* f = fopen(getenv("COST_DUMP"), "wb");

        if (!f || fwrite(c, 1, len, f) != len || fclose(f))
            abort();
    }
    i0 = instructions();
    if (seqlz_decode(tables, c, len, out, scratch))
        abort();
    i1 = instructions();
    if (memcmp(out, page, SEQLZ_PAGE))
        abort();
    mark(i1 - i0, 0, 2048);
#else
    if (size > 2U * SEQLZ_PAGE)
        return 0;
    memcpy(c, data, size);
    i0 = instructions();
    if (seqlz_decode(tables, c, (unsigned int)size, out, scratch) == 0) {
        i1 = instructions();
        mark(i1 - i0, 0, 2048);
    }
#endif
    return 0;
}

#ifdef COST_MAIN
/* Built without -fsanitize=fuzzer and with -DCOST_MAIN: each file through the target, the cost of each
 * objective, for compress the compressor's and the decoder's on its output. */
int main(int argc, char** argv) {
    static unsigned char buf[2 * SEQLZ_PAGE];
    int i;

    for (i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        size_t n;

        if (!f)
            continue;
        n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        cost_last[0] = cost_last[1] = 0;
        LLVMFuzzerTestOneInput(buf, n);
        printf("%llu\t%llu\t%s\n", cost_last[0], cost_last[1], argv[i]);
    }
    return 0;
}
#endif
