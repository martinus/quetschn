// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * ch00_02.c: the facts of the preface and chapters 1 and 2.
 *   ch00.lines.F    lines of src/F, F with _ for . (seqlz_h)    ch00.src_lines  of all *.c and *.h
 *   ch00.inc_files  the .inc files of src/
 *   ch01.*          the work memory per CPU, as the kernel's allocators round it
 *   ch02.cls.*      the offset classes' shares in bench/seqlz_counts_4k.txt, their entropy, the
 *                   Huffman code built from them by hand in the chapter
 *   ch02.cat.K.*    sequence K of chapter 2's page, as the bitstream writes it
 *   ch02.hash.*     the matcher's hash on that page, and the page with a # in front
 *   ch02.lz4.*      the cat page's sequences in lz4's format
 * The files are found from this file's path, docs/book/facts/ in the repository.
 */
#include <dirent.h>

#include "facts.h"
#include "page_lz.h"

/* the repository, with a / at the end; FACTS_REPO for a copy of this file outside it */
static const char* repo_dir(void) {
    static char dir[4096];
    const char* at = strstr(__FILE__, "docs/book/facts/");

#ifdef FACTS_REPO
    return FACTS_REPO;
#endif

    if (!at || (size_t)(at - __FILE__) >= sizeof dir) {
        fprintf(stderr, "facts/ch00_02.c: no repository around %s\n", __FILE__);
        exit(1);
    }
    memcpy(dir, __FILE__, (size_t)(at - __FILE__));
    return dir;
}

static FILE* repo_open(const char* path) {
    char name[8192];
    FILE* f;

    snprintf(name, sizeof name, "%s%s", repo_dir(), path);
    f = fopen(name, "r");
    if (!f) {
        fprintf(stderr, "facts/ch00_02.c: cannot open %s\n", name);
        exit(1);
    }
    return f;
}

/* lines as wc -l counts them */
static unsigned int file_lines(const char* path) {
    FILE* f = repo_open(path);
    unsigned int n = 0;
    int c;

    while ((c = getc(f)) != EOF)
        n += c == '\n';
    fclose(f);
    return n;
}

static void preface(void) {
    char dir[8192], path[512], key[512];
    struct dirent* e;
    unsigned int total = 0, incs = 0, n, k;
    DIR* d;

    snprintf(dir, sizeof dir, "%ssrc", repo_dir());
    d = opendir(dir);
    if (!d)
        exit(1);
    while ((e = readdir(d))) {
        const char* dot = strrchr(e->d_name, '.');

        if (!dot || e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "src/%s", e->d_name);
        n = file_lines(path);
        snprintf(key, sizeof key, "ch00.lines.%s", e->d_name);
        for (k = strlen("ch00.lines."); key[k]; k++)
            if (key[k] == '.')
                key[k] = '_';
        fact_num(key, n);
        if (!strcmp(dot, ".c") || !strcmp(dot, ".h"))
            total += n;
        incs += !strcmp(dot, ".inc");
    }
    closedir(d);
    fact_num("ch00.src_lines", total);
    fact_num("ch00.inc_files", incs);
    fact_num("ch00.LIT_STREAMS", SEQLZ_LIT_STREAMS);
}

/* what kmalloc() hands out for n bytes, 4 KiB pages: a power of 2 */
static unsigned int kmalloc_size(unsigned int n) {
    unsigned int s = 8;

    while (s < n)
        s *= 2;
    return s;
}

static void zram(void) {
    unsigned int cctx = sizeof(struct seqlz_state), dctx = SEQLZ_SCRATCH;
    unsigned int both = cctx > dctx ? cctx : dctx;

    fact_num("ch01.LIT_CODED_MIN", SEQLZ_LIT_CODED_MIN);
    fact_num("ch01.union", both);
    fact_num("ch01.union_kib", kmalloc_size(both) / 1024);
    fact_num("ch01.dctx_kmalloc", kmalloc_size(dctx));
    fact_num("ch01.cctx_kib", kmalloc_size(cctx) / 1024);
    fact_num("ch01.dctx_kib", kmalloc_size(dctx) / 1024);
    fact_num("ch01.split_kib", (kmalloc_size(cctx) + kmalloc_size(dctx)) / 1024);
}

/* log2(x) for x > 0, without libm: the integer part, then the bits of the fraction by squaring */
static double lg2(double x) {
    double r = 0, f = 0.5;
    int k;

    while (x >= 2) {
        x /= 2;
        r++;
    }
    while (x < 1) {
        x *= 2;
        r--;
    }
    for (k = 0; k < 48; k++, f /= 2) {
        x *= x;
        if (x >= 2) {
            x /= 2;
            r += f;
        }
    }
    return r;
}

/* hundredths as "8.17", or "100" for a whole number */
static void fact_cents(const char* key, unsigned long long c) {
    char buf[32];

    if (c % 100)
        snprintf(buf, sizeof buf, "%llu.%02llu", c / 100, c % 100);
    else
        snprintf(buf, sizeof buf, "%llu", c / 100);
    fact_str(key, buf);
}

static void fact_fixed2(const char* key, double v) {
    char buf[32];

    snprintf(buf, sizeof buf, "%.2f", v);
    fact_str(key, buf);
}

/* the offset classes in the training counts, and the Huffman code of chapter 2 built from them */
static void classes(void) {
    enum { N = SEQLZ_OFF_CLASSES };
    unsigned long long count[N] = {0}, total = 0, cents[N], rem[N], sum = 0, w[2 * N];
    unsigned int depth[2 * N] = {0}, parent[2 * N], alive[2 * N], nodes = N, k, j, step;
    char members[2 * N][32], key[64], text[128];
    FILE* f = repo_open("bench/seqlz_counts_4k.txt");
    char* line = NULL;
    size_t cap = 0;
    double h = 0;

    while (getline(&line, &cap, f) > 0) {
        char *p, *at = strstr(line, " pages,");

        /* "# 524912 pages, ..." in the head */
        if (line[0] == '#' && at && at > line && at[-1] >= '0' && at[-1] <= '9') {
            while (at > line && at[-1] >= '0' && at[-1] <= '9')
                at--;
            fact_num("ch02.train_pages", strtoll(at, NULL, 10));
        }
        if (strncmp(line, "token ", 6))
            continue;
        p = line + 6;
        if (strtoul(p, &p, 10) != SEQLZ_TOKEN_SYMBOLS)
            exit(1);
        for (k = 0; k < SEQLZ_TOKEN_SYMBOLS; k++)
            count[k >> (SEQLZ_LL_BITS + SEQLZ_ML_BITS)] += strtoull(p, &p, 10);
    }
    free(line);
    fclose(f);
    for (k = 0; k < N; k++)
        total += count[k];
    if (!total)
        exit(1);
    /* shares in hundredths of a percent that sum to 100%: the largest remainders round up */
    for (k = 0; k < N; k++) {
        cents[k] = count[k] * 10000 / total;
        rem[k] = count[k] * 10000 % total;
        sum += cents[k];
    }
    for (; sum < 10000; sum++) {
        unsigned int best = 0;

        for (k = 1; k < N; k++)
            if (rem[k] > rem[best])
                best = k;
        cents[best]++;
        rem[best] = 0;
    }
    for (k = 0; k < N; k++) {
        double p = (double)count[k] / (double)total;

        snprintf(key, sizeof key, "ch02.cls.%u.share", k);
        fact_cents(key, cents[k]);
        snprintf(key, sizeof key, "ch02.cls.%u.info", k);
        fact_fixed2(key, -lg2(p));
        h -= p * lg2(p);
        w[k] = cents[k];
        alive[k] = 1;
        snprintf(members[k], sizeof members[k], "%u", k);
    }
    fact_fixed2("ch02.cls.entropy", h);
    /* Huffman on the shares as the chapter shows them, the smaller node of a join first */
    for (step = 1; step < N; step++) {
        unsigned int a = 2 * N, b = 2 * N;
        char name[2][48];

        for (k = 0; k < nodes; k++) {
            if (!alive[k])
                continue;
            if (a == 2 * N || w[k] < w[a]) {
                b = a;
                a = k;
            } else if (b == 2 * N || w[k] < w[b])
                b = k;
        }
        for (j = 0; j < 2; j++) {
            unsigned int n = j ? b : a;

            if (n < N)
                snprintf(name[j], sizeof name[j], "class %s", members[n]);
            else
                snprintf(name[j], sizeof name[j], "{%s}", members[n]);
        }
        if (step == N - 1)
            snprintf(text, sizeof text, "the two that are left");
        else
            snprintf(text, sizeof text, "%s and %s", name[0], name[1]);
        snprintf(key, sizeof key, "ch02.join.%u.what", step);
        fact_str(key, text);
        snprintf(key, sizeof key, "ch02.join.%u.a", step);
        fact_cents(key, w[a]);
        snprintf(key, sizeof key, "ch02.join.%u.b", step);
        fact_cents(key, w[b]);
        w[nodes] = w[a] + w[b];
        snprintf(key, sizeof key, "ch02.join.%u.sum", step);
        fact_cents(key, w[nodes]);
        snprintf(members[nodes], sizeof members[nodes], "%s, %s", members[a], members[b]);
        alive[a] = alive[b] = 0;
        alive[nodes] = 1;
        parent[a] = parent[b] = nodes;
        nodes++;
    }
    /* a class's code length is its depth: the joins above it */
    for (k = 0; k < N; k++) {
        for (j = k; j != nodes - 1; j = parent[j])
            depth[k]++;
        snprintf(key, sizeof key, "ch02.cls.%u.len", k);
        fact_num(key, depth[k]);
    }
    /* the average code length, from the shares as shown */
    sum = 0;
    for (k = 0; k < N; k++)
        sum += depth[k] * cents[k];
    fact_cents("ch02.cls.avg", (sum + 50) / 100);
}

static void bits_str(char* buf, unsigned int v, unsigned int n) {
    unsigned int k;

    for (k = 0; k < n; k++)
        buf[k] = (char)('0' + ((v >> (n - 1 - k)) & 1));
    buf[n] = 0;
}

/* a length value: its symbol's code, then its extra bits */
static unsigned int value_facts(const char prefix[32], const u8* lens, unsigned int v) {
    static char code[SEQLZ_LEN_SYMBOLS][17];
    unsigned int extra, s = seqlz_len_symbol(v, &extra);
    char key[128], buf[32];

    fact_canonical(lens, SEQLZ_LEN_SYMBOLS, code);
    snprintf(key, sizeof key, "%s.value", prefix);
    fact_num(key, v);
    snprintf(key, sizeof key, "%s.vcode", prefix);
    fact_str(key, code[s]);
    bits_str(buf, v & ((1U << extra) - 1U), extra);
    snprintf(key, sizeof key, "%s.vextra", prefix);
    fact_str(key, buf);
    return lens[s] + extra;
}

/* chapter 2's page in the bitstream, sequence by sequence, and in lz4's format */
static void cat(void) {
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    static char code[SEQLZ_TOKEN_SYMBOLS + 1][17];
    static u8 page[SEQLZ_BLOCK_SIZE];
    unsigned int n, i, last = 1, lz4 = 0, ml_bytes = 0, ml_255 = 0, bits = 0;
    char key[64], buf[32];

    fact_canonical(seqlz_default_lengths.token, SEQLZ_TOKEN_SYMBOLS + 1, code);
    cat_page(page);
    fact_num("ch02.hash.dog", seqlz_hash5(get_unaligned_le64(page + 5)));
    fact_num("ch02.hash.dog17", seqlz_hash5(get_unaligned_le64(page + 17)));
    n = seqlz_find(&st, page, seq, 2);
    fact_num("ch02.cat.n", n);
    for (i = 0; i < n; i++) {
        unsigned int raw, cls, tok, mlm4 = seq[i].match - 4U;

#define K(suffix) (snprintf(key, sizeof key, "ch02.cat.%u.%s", i, suffix), key)
        fact_num(K("ll"), seq[i].literals);
        fact_num(K("ml"), seq[i].match);
        lz4 += 1 + (seq[i].literals >= 15 ? (seq[i].literals - 15) / 255 + 1 : 0);
        if (!seq[i].match)
            continue;
        cls = seqlz_off_class(seq[i].offset, last, &raw);
        tok = seqlz_token(seq[i].literals, seq[i].match, cls);
        fact_num(K("mlm4"), mlm4);
        fact_num(K("off"), seq[i].offset);
        fact_num(K("cls"), cls);
        fact_num(K("tok"), tok);
        fact_num(K("escaped"), !seqlz_default_lengths.token[tok]);
        bits += seqlz_default_lengths.token[tok] ? seqlz_default_lengths.token[tok]
                                                 : seqlz_default_lengths.token[SEQLZ_ESCAPE] + SEQLZ_ESCAPE_BITS;
        bits += raw;
        fact_str(K("code"), seqlz_default_lengths.token[tok] ? code[tok] : code[SEQLZ_ESCAPE]);
        bits_str(buf, tok, SEQLZ_ESCAPE_BITS);
        fact_str(K("esc"), seqlz_default_lengths.token[tok] ? "" : buf);
        bits_str(buf, seq[i].offset >> SEQLZ_OFF_SHIFT(cls), raw);
        fact_str(K("raw"), buf);
        fact_num(K("rawbits"), raw);
        fact_num(K("tml"), mlm4 < SEQLZ_ML_CAP ? mlm4 : SEQLZ_ML_CAP);
        if (mlm4 >= SEQLZ_ML_CAP) {
            snprintf(key, sizeof key, "ch02.cat.%u", i);
            bits += value_facts(key, seqlz_default_lengths.ml, mlm4 - SEQLZ_ML_CAP);
        }
#undef K
        lz4 += 2;
        if (mlm4 >= 15) {
            unsigned int b = (mlm4 - 15) / 255 + 1;

            lz4 += b;
            if (b > ml_bytes) {
                ml_bytes = b;
                ml_255 = (mlm4 - 15) / 255;
            }
        }
        last = seq[i].offset;
    }
    for (i = 0; i < (8 - bits % 8) % 8; i++)
        buf[i] = '0';
    buf[i] = 0;
    fact_str("ch02.cat.fill", buf);
    fact_num("ch02.ml_base", SEQLZ_ML_CAP + 4U);
    fact_num("ch02.lz4.bytes", lz4);
    fact_num("ch02.lz4.ml_bytes", ml_bytes);
    fact_num("ch02.lz4.ml_255", ml_255);

    /* the page with a # in front */
    memmove(page + 1, page, 24);
    page[0] = '#';
    n = seqlz_find(&st, page, seq, 2);
    fact_num("ch02.hash.sharp.ll", seq[0].literals);
    fact_num("ch02.hash.sharp.ml", seq[0].match);
    fact_num("ch02.hash.sharp.off", seq[0].offset);
    fact_num("ch02.hash.sharp.n", n);
}

static void consts(void) {
    fact_num("ch02.HEADER", SEQLZ_HEADER);
    fact_num("ch02.TOKEN_BITS", SEQLZ_TOKEN_BITS);
    fact_num("ch02.LIT_BITS", SEQLZ_LIT_BITS);
    fact_num("ch02.MAX_BITS", SEQLZ_MAX_BITS);
    fact_num("ch02.ESCAPE_BITS", SEQLZ_ESCAPE_BITS);
    fact_num("ch02.OFF_CLASSES", SEQLZ_OFF_CLASSES);
    fact_num("ch02.LIT_SETS", SEQLZ_LIT_SETS);
    fact_num("ch02.lit_set_bits", SEQLZ_LIT_WIDTH_AT);
    fact_num("ch02.HASH_SLOTS", 1U << SEQLZ_HASH_BITS);
    fact_num("ch02.tok_entries", sizeof(((struct token_table*)0)->decode) / sizeof(u32));
    fact_num("ch02.tok_entry_bytes", sizeof(u32));
    fact_num("ch02.tok_table_kib", sizeof(((struct token_table*)0)->decode) / 1024);
    fact_num("ch02.tok49", seqlz_token(1, 7, 0));
    fact_num("ch02.tok49_entries", 1U << (SEQLZ_TOKEN_BITS - seqlz_default_lengths.token[49]));
}

int main(void) {
    preface();
    zram();
    classes();
    cat();
    consts();
    facts_end();
    return 0;
}
