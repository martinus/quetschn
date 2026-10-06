// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * Pages made to be slow for seqlz, for its worst-case time (PLAN.md, Phase 4).
 *
 * Counts instructions and branch misses in user space with perf_event_open: they do not change with the
 * load on the machine, as time does.
 *
 *   quetschn-seqlz-worst pages <base>
 *       writes made-up pages of the kinds below as a corpus, <base>.pages and <base>.tsv, with the kind of
 *       each page in the .tsv, for count or for the harness.
 *   quetschn-seqlz-worst count <pages file>
 *       compresses and decodes each page as zram would, one line per page: compressed length,
 *       instructions and branch misses of both. For the made-up pages and for real ones alike.
 *   quetschn-seqlz-worst decode
 *       compressed pages written by seqlz_encode() from chosen sequences, valid pages the matcher never
 *       writes: every token escaped, as many sequences as fit, offsets below 8, ... Per kind the most
 *       instructions and branch misses of the decode of its 16 pages.
 *   quetschn-seqlz-worst streams <dir>
 *       writes 4 of those per kind into dir, one file each, as seeds for cost_fuzz.c.
 *   quetschn-seqlz-worst count-streams <file>...
 *       the decoder on compressed pages in files, e.g. what cost_fuzz.c found.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/perf_event.h>
#include <sys/syscall.h>

#include "seqlz.h"

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t next(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static unsigned int below(unsigned int n) {
    return (unsigned int)(next() % n);
}

/* ---- pages for the compressor and the decoder as zram runs them ---- */

/* kind k of the pages, with a parameter v; the names go into the .tsv */
static const char* const kinds[] = {
    "random",   /* bytes at random: no matches, every position tried, zram stores it raw */
    "alphabet", /* bytes from v symbols: many short matches at small offsets */
    "words",    /* 4-byte words from v random ones: as many sequences of 4 bytes as fit */
    "period",   /* runs of a period v below 8, one byte apart: the decoder's pattern copy */
    "skewed",   /* skewed bytes, few repeats: many literals that code well */
    "sparse",   /* random bytes, every v-th group of 8 a copy of an earlier one: long literal runs */
    "zeros",    /* zeros with v random bytes: long matches */
};

static void make_page(unsigned int kind, unsigned int v, unsigned char* p) {
    unsigned int k, i;

    memset(p, 0, SEQLZ_PAGE);
    switch (kind) {
    case 0:
        for (k = 0; k < SEQLZ_PAGE; k++)
            p[k] = (unsigned char)next();
        break;
    case 1:
        for (k = 0; k < SEQLZ_PAGE; k++)
            p[k] = (unsigned char)below(v);
        break;
    case 2: {
        uint32_t words[1024];

        for (i = 0; i < v; i++)
            words[i] = (uint32_t)next();
        for (k = 0; k + 4 <= SEQLZ_PAGE; k += 4)
            memcpy(p + k, &words[below(v)], 4);
        break;
    }
    case 3:
        for (k = 0; k < SEQLZ_PAGE;) {
            unsigned int run = 8 + below(56);

            p[k++] = (unsigned char)next();
            for (i = 0; i < run && k < SEQLZ_PAGE; i++, k++)
                p[k] = k >= v ? p[k - v] : (unsigned char)next();
        }
        break;
    case 4:
        for (k = 0; k < SEQLZ_PAGE; k++) {
            unsigned int z = (unsigned int)__builtin_ctzll(next() | 1ULL << 40);

            p[k] = (unsigned char)(32U + (z < 30 ? z : 30) * 3U + below(3));
        }
        break;
    case 5:
        for (k = 0; k < SEQLZ_PAGE; k += 8) {
            if (k >= 64 && below(v) == 0)
                memcpy(p + k, p + 8 * below(k / 8), 8);
            else
                for (i = 0; i < 8; i++)
                    p[k + i] = (unsigned char)next();
        }
        break;
    default:
        for (i = 0; i < v; i++)
            p[below(SEQLZ_PAGE)] = (unsigned char)next();
    }
}

static int write_pages(const char* base) {
    /* per kind its parameters, 16 pages each */
    static const unsigned int params[][6] = {
        {0},
        {2, 3, 4, 6, 8, 16},
        {16, 64, 256, 1024},
        {1, 2, 3, 4, 5, 7},
        {0},
        {2, 4, 16},
        {1, 16, 64, 256},
    };
    static const unsigned int n_params[] = {1, 6, 4, 6, 1, 3, 4};
    char path[4096];
    unsigned char page[SEQLZ_PAGE];
    unsigned int kind, j, s, n = 0;
    FILE *pages, *tsv;

    snprintf(path, sizeof path, "%s.pages", base);
    pages = fopen(path, "wb");
    snprintf(path, sizeof path, "%s.tsv", base);
    tsv = fopen(path, "w");
    if (!pages || !tsv)
        return 1;
    fprintf(tsv, "# page_size=%u\npage\tkind\tparameter\n", SEQLZ_PAGE);
    for (kind = 0; kind < sizeof kinds / sizeof kinds[0]; kind++)
        for (j = 0; j < n_params[kind]; j++)
            for (s = 0; s < 16; s++) {
                make_page(kind, params[kind][j], page);
                if (fwrite(page, 1, SEQLZ_PAGE, pages) != SEQLZ_PAGE)
                    return 1;
                fprintf(tsv, "%u\t%s\t%u\n", n++, kinds[kind], params[kind][j]);
            }
    fprintf(stderr, "%u pages\n", n);
    return fclose(pages) || fclose(tsv);
}

/* ---- valid pages the matcher never writes, for the decoder ---- */

struct built {
    unsigned char page[SEQLZ_PAGE];
    unsigned char lits[SEQLZ_PAGE];
    struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    unsigned int n_seq, n_lit, pos, last;
};

/* a sequence of ll literals, from lit(), and a match of ml bytes at off, applied to the page */
static void add(struct built* b, unsigned int ll, unsigned int ml, unsigned int off, unsigned char (*lit)(void)) {
    unsigned int k;

    for (k = 0; k < ll; k++)
        b->page[b->pos++] = b->lits[b->n_lit++] = lit();
    for (k = 0; k < ml; k++, b->pos++)
        b->page[b->pos] = b->page[b->pos - off];
    b->seq[b->n_seq].literals = (unsigned short)ll;
    b->seq[b->n_seq].match = (unsigned short)ml;
    b->seq[b->n_seq].offset = (unsigned short)off;
    b->n_seq++;
    if (ml)
        b->last = off;
}

static unsigned char lit_random(void) {
    return (unsigned char)next();
}

/* bytes with codes of 6 to 8 bits in literal table 0: coded, but each code long */
static unsigned char lit_long_code(void) {
    static unsigned char pool[256];
    static unsigned int n_pool;
    unsigned int s;

    if (!n_pool)
        for (s = 0; s < 256; s++)
            if (seqlz_lit_sets[0][s] >= 6 && seqlz_lit_sets[0][s] <= 8)
                pool[n_pool++] = (unsigned char)s;
    return pool[below(n_pool)];
}

/* an offset of class cls (seqlz.h) up to pos that is not the last one, 0 if there is none */
static unsigned int offset_of_class(const struct built* b, unsigned int cls) {
    unsigned int tries, off, raw;

    for (tries = 0; tries < 1000; tries++) {
        off = 1 + below(b->pos < 4095 ? b->pos : 4095);
        if (off != b->last && seqlz_off_class(off, b->last, &raw) == cls)
            return off;
    }
    return 0;
}

/* the matches' ll and ml for the first token of class cls without a code, the one of fewest bytes */
static int escaped_token(unsigned int cls, unsigned int* ll, unsigned int* ml) {
    unsigned int best = ~0U, l, m;

    for (l = 0; l < 15; l++)
        for (m = 0; m < 31; m++)
            if (seqlz_default_own.token[seqlz_token(l, m + 4, cls)] == 0 && l + m + 4 < best) {
                best = l + m + 4;
                *ll = l;
                *ml = m + 4;
            }
    return best == ~0U ? -1 : 0;
}

/* kind k of the made-up streams into b; returns its name */
static const char* build(unsigned int kind, struct built* b) {
    unsigned int ll = 0, ml = 4, off;

    memset(b, 0, sizeof *b);
    b->last = 1;
    switch (kind) {
    case 0: /* as many sequences as fit, ml 4, offsets at random */
        add(b, 4, 4, 1 + below(4), lit_random);
        while (b->pos + 4 < SEQLZ_PAGE)
            add(b, 0, 4, 1 + below(b->pos), lit_random);
        add(b, SEQLZ_PAGE - b->pos, 0, 0, lit_random);
        return "most sequences, ml 4";
    case 1: /* the same with offsets of class 3, 12 raw bits, where there are such */
        add(b, 4, 4, 1, lit_random);
        while (b->pos + 4 < SEQLZ_PAGE) {
            off = offset_of_class(b, 3);
            add(b, 0, 4, off ? off : 1 + below(b->pos), lit_random);
        }
        add(b, SEQLZ_PAGE - b->pos, 0, 0, lit_random);
        return "most sequences, offsets of 12 bits";
    case 2: /* every token escaped: the token of class 3 without a code that takes fewest bytes */
        if (escaped_token(3, &ll, &ml))
            return 0;
        /* offsets of class 3 need 256 bytes before them */
        add(b, 4, 4, 1, lit_random);
        while (b->pos < 300)
            add(b, 0, 4, 1 + below(b->pos), lit_random);
        while (b->pos + ll + ml < SEQLZ_PAGE && (off = offset_of_class(b, 3)) != 0)
            add(b, ll, ml, off, lit_random);
        add(b, SEQLZ_PAGE - b->pos, 0, 0, lit_random);
        return "every token escaped";
    case 3: /* offsets 1 to 7 by turns, ml 4: the decoder's pattern copy for each, and escaped tokens, as
             * no token of class 1 with ml 4 has a code */
        add(b, 8, 4, 1, lit_random);
        for (off = 2; b->pos + 4 < SEQLZ_PAGE; off = off % 7 + 1)
            add(b, 0, 4, off, lit_random);
        add(b, SEQLZ_PAGE - b->pos, 0, 0, lit_random);
        return "offsets below 8, ml 4, escaped";
    case 4: /* all literals, coded with long codes */
        add(b, SEQLZ_PAGE, 0, 0, lit_long_code);
        return "only literals, long codes";
    case 5: /* half the page literals with long codes, the rest sequences of ml 4 */
        add(b, 4, 4, 1, lit_long_code);
        while (b->pos + 8 < SEQLZ_PAGE)
            add(b, 4, 4, 1 + below(b->pos), lit_long_code);
        add(b, SEQLZ_PAGE - b->pos, 0, 0, lit_long_code);
        return "4 coded literals, then ml 4";
    case 6: /* lengths that need a length value each, the shortest such */
        add(b, 15, 35, 1 + below(8), lit_random);
        while (b->pos + 50 < SEQLZ_PAGE)
            add(b, 15, 35, 1 + below(b->pos), lit_random);
        add(b, SEQLZ_PAGE - b->pos, 0, 0, lit_random);
        return "length values for ll and ml";
    case 7: /* each sequence's shape at random, for the branches: ll 0 to 2, ml 4 to 6, offsets below 8
             * or not, escaped tokens or not */
        add(b, 8, 4, 1, lit_random);
        while (b->pos + 8 < SEQLZ_PAGE) {
            unsigned int l = below(3), m = 4 + below(3);

            off = below(2) ? 1 + below(7) : 1 + below(b->pos + l);
            add(b, l, m, off > b->pos + l ? b->pos + l : off, lit_random);
        }
        add(b, SEQLZ_PAGE - b->pos, 0, 0, lit_random);
        return "short sequences of random shape";
    default:
        return 0;
    }
}

/* Instructions, branch misses and cycles in user space, of this thread. Instructions are the same on
 * a busy machine, unlike time; they are the fewest of a few runs, which takes out what an interrupt
 * adds. Branch misses and cycles are those of the first run, after another page, as the branches of the
 * runs after it are learned; cycles only mean something on an idle machine. */
struct counters {
    int insn, miss, cycles;
};

static int perf_open(unsigned long long config) {
    struct perf_event_attr a;

    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_HARDWARE;
    a.size = sizeof a;
    a.config = config;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}

static int counters_open(struct counters* c) {
    c->insn = perf_open(PERF_COUNT_HW_INSTRUCTIONS);
    c->miss = perf_open(PERF_COUNT_HW_BRANCH_MISSES);
    c->cycles = perf_open(PERF_COUNT_HW_CPU_CYCLES);
    if (c->insn < 0 || c->miss < 0 || c->cycles < 0) {
        perror("perf_event_open");
        return -1;
    }
    return 0;
}

static unsigned long long counter(int fd) {
    unsigned long long v = 0;

    if (read(fd, &v, sizeof v) != (ssize_t)sizeof v)
        abort();
    return v;
}

/* what one call of f(arg) costs, see above */
struct cost {
    unsigned long long insn, miss, cycles;
};

static struct cost measure(const struct counters* c, int (*f)(void*), void* arg) {
    struct cost best = {~0ULL, ~0ULL, ~0ULL};
    int r;

    for (r = 0; r < 5; r++) {
        unsigned long long i0 = counter(c->insn), m0 = counter(c->miss), c0 = counter(c->cycles), i1, m1, c1;

        if (f(arg) != 0)
            abort();
        c1 = counter(c->cycles);
        m1 = counter(c->miss);
        i1 = counter(c->insn);
        if (i1 - i0 < best.insn)
            best.insn = i1 - i0;
        if (r == 0) {
            best.miss = m1 - m0;
            best.cycles = c1 - c0;
        }
    }
    return best;
}

struct job {
    const struct seqlz_tables* t;
    struct seqlz_state* st;
    const unsigned char* page;
    unsigned char *c, *out, *scratch;
    unsigned int len;
};

static int do_compress(void* arg) {
    struct job* j = arg;

    j->len = seqlz_compress(j->t, j->st, j->page, j->c, 2 * SEQLZ_PAGE, 1);
    return j->len ? 0 : -1;
}

static int do_decode(void* arg) {
    struct job* j = arg;

    return seqlz_decode(j->t, j->c, j->len, j->out, j->scratch);
}

static int do_nothing(void* arg) {
    (void)arg;
    return 0;
}

static int cmp_u64(const void* a, const void* b) {
    unsigned long long x = *(const unsigned long long*)a, y = *(const unsigned long long*)b;

    return (x > y) - (x < y);
}

static struct seqlz_tables* tables(void) {
    struct seqlz_tables* t = malloc(seqlz_tables_size());

    if (!t || seqlz_tables_init(t, &seqlz_default_own))
        abort();
    return t;
}

/* The decoder on the made-up streams, per kind the most instructions and branch misses of its 16 pages,
 * and the median of their cycles: a single run's cycles can be far off on a phone. */
static int count_decode(void) {
    static struct built b;
    static unsigned char c[3 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    struct job j = {tables(), 0, 0, c, out, scratch, 0};
    struct counters ctr;
    struct cost empty;
    unsigned int kind, k;

    if (counters_open(&ctr))
        return 1;
    empty = measure(&ctr, do_nothing, 0);
    /* one decode first, so that the first page measured does not pay for the code and tables alone */
    build(0, &b);
    j.len = seqlz_encode(j.t, b.seq, b.n_seq, b.lits, b.n_lit, c, sizeof c, 1);
    if (!j.len || do_decode(&j))
        return 1;
    printf(
        "%-36s %6s %5s %6s %12s %12s %12s\n", "kind", "bytes", "seqs", "coded", "instructions", "branch miss", "cycles p50");
    for (kind = 0;; kind++) {
        struct cost most = {0, 0, 0};
        unsigned long long cycles[16];
        const char* name = build(kind, &b);

        if (!name)
            break;
        for (k = 0; k < 16; k++) {
            struct cost x;

            if (k)
                build(kind, &b);
            j.len = seqlz_encode(j.t, b.seq, b.n_seq, b.lits, b.n_lit, c, sizeof c, 1);
            if (!j.len || do_decode(&j) || memcmp(out, b.page, SEQLZ_PAGE)) {
                fprintf(stderr, "%s: page %u does not come back\n", name, k);
                return 1;
            }
            x = measure(&ctr, do_decode, &j);
            if (x.insn - empty.insn > most.insn)
                most.insn = x.insn - empty.insn;
            if (x.miss > most.miss)
                most.miss = x.miss;
            cycles[k] = x.cycles;
        }
        qsort(cycles, 16, sizeof cycles[0], cmp_u64);
        most.cycles = cycles[8];
        printf("%-36s %6u %5u %6s %12llu %12llu %12llu\n",
               name,
               j.len,
               b.n_seq,
               c[1] >> 7 ? "yes" : "no",
               most.insn,
               most.miss,
               most.cycles);
    }
    return 0;
}

/* Each page of a corpus compressed and decoded as zram would, one line per page: compressed length,
 * instructions and branch misses of the compression and of the decode. */
static int count_pages(const char* path) {
    static unsigned char page[SEQLZ_PAGE], c[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    struct job j = {tables(), &st, page, c, out, scratch, 0};
    struct counters ctr;
    struct cost empty;
    FILE* f = fopen(path, "rb");
    unsigned int n = 0;

    if (!f || counters_open(&ctr))
        return 1;
    empty = measure(&ctr, do_nothing, 0);
    printf("page\tlength\tcompress_insn\tcompress_miss\tcompress_cycles\tdecode_insn\tdecode_miss\tdecode_cycles\n");
    while (fread(page, 1, SEQLZ_PAGE, f) == SEQLZ_PAGE) {
        struct cost cc = measure(&ctr, do_compress, &j), dc = measure(&ctr, do_decode, &j);

        if (memcmp(out, page, SEQLZ_PAGE))
            return 1;
        printf("%u\t%u\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",
               n++,
               j.len,
               cc.insn - empty.insn,
               cc.miss,
               cc.cycles,
               dc.insn - empty.insn,
               dc.miss,
               dc.cycles);
    }
    return fclose(f);
}

/* the made-up streams, one file per page, as seeds for seqlz_decode_cost_fuzz */
static int write_streams(const char* dir) {
    static struct built b;
    static unsigned char c[3 * SEQLZ_PAGE];
    const struct seqlz_tables* t = tables();
    unsigned int kind, k, len;
    char path[4096];

    for (kind = 0; build(kind, &b); kind++)
        for (k = 0; k < 4; k++) {
            FILE* f;

            build(kind, &b);
            len = seqlz_encode(t, b.seq, b.n_seq, b.lits, b.n_lit, c, sizeof c, 1);
            snprintf(path, sizeof path, "%s/stream-%u-%u", dir, kind, k);
            if (!len || !(f = fopen(path, "wb")) || fwrite(c, 1, len, f) != len || fclose(f))
                return 1;
        }
    return 0;
}

/* the decoder on compressed pages in files, as the fuzzer found them: instructions, branch misses and
 * cycles, or invalid */
static int count_streams(int n, char** paths) {
    static unsigned char c[2 * SEQLZ_PAGE], out[SEQLZ_PAGE], scratch[SEQLZ_SCRATCH];
    struct job j = {tables(), 0, 0, c, out, scratch, 0};
    struct counters ctr;
    struct cost empty;
    int i;

    if (counters_open(&ctr))
        return 1;
    empty = measure(&ctr, do_nothing, 0);
    for (i = 0; i < n; i++) {
        FILE* f = fopen(paths[i], "rb");
        struct cost x;

        if (!f)
            return 1;
        j.len = (unsigned int)fread(c, 1, sizeof c, f);
        fclose(f);
        if (do_decode(&j)) {
            printf("%s\t%u\tinvalid\n", paths[i], j.len);
            continue;
        }
        x = measure(&ctr, do_decode, &j);
        printf("%s\t%u\t%llu\t%llu\t%llu\n", paths[i], j.len, x.insn - empty.insn, x.miss, x.cycles);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "pages"))
        return write_pages(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "decode"))
        return count_decode();
    if (argc == 3 && !strcmp(argv[1], "count"))
        return count_pages(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "streams"))
        return write_streams(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "count-streams"))
        return count_streams(argc - 2, argv + 2);
    fprintf(stderr,
            "usage: quetschn-seqlz-worst pages <base> | count <pages file> | decode | streams <dir> | "
            "count-streams <file>...\n");
    return 1;
}
