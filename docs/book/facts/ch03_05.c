// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * ch03_05.c: the facts of chapter 3 (what makes code slow on a CPU) and chapter 5 (seqlz_compat.h):
 * the tables' sizes in entries, bytes and cache lines, which entries and lines the codes of tokens
 * 49 and 4 fill, the bits the decoder's fast path counts on, and what the chapters count in the
 * files of the repository. Includes seqlz_decompress.c for PREFETCH_STEP.
 */
// facts: also 16k
#include <dirent.h>
#include <stddef.h>

#include "facts.h"
#include "seqlz_decompress.c"

#define LINE 64U /* the cache line of chapter 3 */

static void sizes(const char* name, unsigned int entries, unsigned int entry_bytes) {
    char key[64];

    snprintf(key, sizeof key, "ch03.%s.entries", name);
    fact_num(key, entries);
    snprintf(key, sizeof key, "ch03.%s.bytes", name);
    fact_num(key, entries * entry_bytes);
    snprintf(key, sizeof key, "ch03.%s.lines", name);
    fact_num(key, (entries * entry_bytes + LINE / 2) / LINE);
    snprintf(key, sizeof key, "ch03.%s.kib", name);
    fact_num(key, (entries * entry_bytes + 512) / 1024);
}

/*
 * The entries of the token table that token s fills, read most significant bit first as seqlz does,
 * and least significant bit first, the code's bits reversed into the low bits of the index.
 */
static void token_lines(unsigned int s) {
    static char code[SEQLZ_TOKEN_SYMBOLS + 1][17];
    const unsigned int per_line = LINE / sizeof(u32), size = 1U << SEQLZ_TOKEN_BITS;
    unsigned int l, c = 0, rev = 0, k, n, idx, lines = 0, last_line = ~0U;
    char key[64];

    fact_canonical(seqlz_default_own.token, SEQLZ_TOKEN_SYMBOLS + 1, code);
    l = seqlz_default_own.token[s];
    if (!l) {
        fprintf(stderr, "facts/ch03_05.c: token %u has no code\n", s);
        exit(1);
    }
    for (k = 0; k < l; k++) {
        c = c << 1 | (unsigned int)(code[s][k] - '0');
        rev |= (unsigned int)(code[s][k] - '0') << k;
    }
    n = size >> l;
#define K(what) (snprintf(key, sizeof key, "ch03.t%u.%s", s, what), key)
    fact_num(K("entries"), n);
    fact_num(K("first"), c << (SEQLZ_TOKEN_BITS - l));
    fact_num(K("last"), (c << (SEQLZ_TOKEN_BITS - l)) + n - 1);
    fact_num(K("line_first"), (c << (SEQLZ_TOKEN_BITS - l)) / per_line);
    fact_num(K("line_last"), ((c << (SEQLZ_TOKEN_BITS - l)) + n - 1) / per_line);
    /* least significant bit first: every 2^l-th entry from rev on */
    for (idx = rev; idx < size; idx += 1U << l)
        if (idx / per_line != last_line) {
            last_line = idx / per_line;
            lines++;
        }
    fact_num(K("lsb.lines"), lines);
    fact_num(K("lsb.step"), (1U << l) / per_line);
    for (k = 0; k < 3; k++) {
        char what[32];

        snprintf(what, sizeof what, "lsb.at.%u", k);
        fact_num(K(what), rev + (k << l));
        snprintf(what, sizeof what, "lsb.line.%u", k);
        fact_num(K(what), (rev + (k << l)) / per_line);
    }
#undef K
}

/* the repository's root, from where this file is: docs/book/facts/ch03_05.c */
static void repo_path(char* out, size_t n, const char* rel) {
    const char* f = __FILE__;
    const char* end = strstr(f, "docs/book/facts/");

    if (!end) {
        fprintf(stderr, "facts/ch03_05.c: %s is not in docs/book/facts/\n", f);
        exit(1);
    }
    snprintf(out, n, "%.*s%s", (int)(end - f), f, rel);
}

/* lines of a file of the repository that start with prefix */
static unsigned int lines_starting(const char* rel, const char* prefix) {
    char path[4096], line[4096];
    unsigned int n = 0;
    FILE* f;

    repo_path(path, sizeof path, rel);
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "facts/ch03_05.c: cannot read %s\n", path);
        exit(1);
    }
    while (fgets(line, sizeof line, f))
        n += !strncmp(line, prefix, strlen(prefix));
    fclose(f);
    return n;
}

/* the number after the first "needle" in a file of the repository */
static unsigned long number_after(const char* rel, const char* needle) {
    char path[4096], line[4096];
    FILE* f;

    repo_path(path, sizeof path, rel);
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "facts/ch03_05.c: cannot read %s\n", path);
        exit(1);
    }
    while (fgets(line, sizeof line, f)) {
        const char* at = strstr(line, needle);

        if (at) {
            fclose(f);
            return strtoul(at + strlen(needle), NULL, 10);
        }
    }
    fprintf(stderr, "facts/ch03_05.c: no \"%s\" in %s\n", needle, rel);
    exit(1);
}

int main(void) {
    const char* inl = "static __always_inline";
    unsigned int dec, cls, raw_max = 0, n_inline = 0;
    char path[4096];
    struct dirent* e;
    DIR* d;

    /* chapter 3: the tables, and the code lengths that decide their sizes */
    fact_num("ch03.tok_bits", SEQLZ_TOKEN_BITS);
    fact_num("ch03.len_bits", SEQLZ_MAX_BITS);
    fact_num("ch03.lit_bits", SEQLZ_LIT_BITS);
    sizes("tok_dec", 1U << SEQLZ_TOKEN_BITS, sizeof(u32));
    sizes("len_dec", 1U << SEQLZ_MAX_BITS, sizeof(u32));
    sizes("lit_dec", 1U << SEQLZ_LIT_BITS, sizeof(u16));
    sizes("tok_enc", SEQLZ_TOKEN_SYMBOLS + 1, sizeof(u16));
    sizes("hash", sizeof_field(struct seqlz_state, table) / sizeof(u16), sizeof(u16));
    dec = sizeof_field(struct token_table, decode) + 2 * sizeof_field(struct value_table, decode) +
          sizeof_field(struct lit_table, decode);
    fact_num("ch03.dec_kib", (dec + 512) / 1024);
    fact_num("ch03.prefetch_lines", PREFETCH_STEP / L1_CACHE_BYTES);
    /* the literal tables start off a cache line, and all at the same offset in it */
    fact_num("ch03.lit_at_mod", offsetof(struct seqlz_tables, lit) % LINE);
    fact_num("ch03.lit_size_mod", sizeof(struct lit_table) % LINE);
    token_lines(49);
    token_lines(4);

    /* the bits of the fast path: a token and its offset, the largest offset of any class */
    for (cls = 0; cls < SEQLZ_OFF_CLASSES; cls++)
        if (SEQLZ_RAW_BITS(cls) > raw_max)
            raw_max = SEQLZ_RAW_BITS(cls);
    fact_num("ch03.off_max", raw_max);
    fact_num("ch03.refill_at", SEQLZ_TOKEN_BITS + QUETSCHN_PAGE_BITS);
    fact_num("ch03.two_fast", 2 * (SEQLZ_TOKEN_BITS + raw_max));
    fact_num("ch03.lit_rounds", SEQLZ_LIT_ROUNDS);
    fact_num("ch03.lit_cost_run", LIT_COST_RUN);
    fact_num("ch03.token_ll_max", SEQLZ_LL_CAP - 1);
    fact_num("ch03.token_ml_max", SEQLZ_ML_CAP - 1 + 4);
    fact_num("ch03.enc_len_symbols", ENC_LEN_SYMBOLS);

    /* chapter 5 */
    fact_num("ch05.prefetch_step", PREFETCH_STEP);
    fact_num("ch05.prefetches_128", PREFETCH_STEP / 128);
    fact_num("ch05.ll_cap", SEQLZ_LL_CAP);
    fact_num("ch05.ml_cap", SEQLZ_ML_CAP);
    /* the helpers marked __always_inline: in the .c files and page_lz.h, and in seqlz_compat.h */
    repo_path(path, sizeof path, "src");
    d = opendir(path);
    if (!d) {
        fprintf(stderr, "facts/ch03_05.c: cannot read %s\n", path);
        exit(1);
    }
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        char rel[300];

        if (!(n > 2 && !strcmp(e->d_name + n - 2, ".c")) && strcmp(e->d_name, "page_lz.h"))
            continue;
        snprintf(rel, sizeof rel, "src/%s", e->d_name);
        n_inline += lines_starting(rel, inl);
    }
    closedir(d);
    fact_num("ch05.inline_c", n_inline);
    fact_num("ch05.inline_compat", lines_starting("src/seqlz_compat.h", inl));
    fact_num("ch05.endian_pages", (long long)number_after("test/seqlz_endian.c", "printf(\"pages "));
    facts_end();
    return 0;
}
