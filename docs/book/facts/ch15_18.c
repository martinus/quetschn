// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * ch15_18.c: the facts of chapters 15 (the zram glue and the kernel copy), 17 (the decisions) and 18
 * (the glossary). Besides values of src/, it reads files of the repository: the line ranges of the
 * listings of tools/ and bench/ files, found by their first and last lines, so that a listing moves with
 * its code, and the output of test/seqlz_endian.c, which it runs.
 */
// facts: also 16k
#include "facts.h"
#include "seqlz_compress.c"
#include "seqlz_decompress.c"

/* the repository's root, from this file's path: <root>/docs/book/facts/ch15_18.c */
static const char* repo_path(const char* rel) {
    static char buf[4096];
    const char* tail = strstr(__FILE__, "docs/book/facts/");
    int n = tail ? (int)(tail - __FILE__) : 0;

    snprintf(buf, sizeof buf, "%.*s%s", n, __FILE__, rel);
    return buf;
}

static char file_buf[1 << 16];
static unsigned int file_len;

static void load(const char* rel) {
    FILE* f = fopen(repo_path(rel), "rb");

    if (!f) {
        fprintf(stderr, "facts: cannot open %s\n", rel);
        exit(1);
    }
    file_len = (unsigned int)fread(file_buf, 1, sizeof file_buf - 1, f);
    file_buf[file_len] = 0;
    fclose(f);
}

static unsigned int lines_of_file(void) {
    unsigned int k, n = 0;

    for (k = 0; k < file_len; k++)
        n += file_buf[k] == '\n';
    return n;
}

/* the start of line `line` (from 1) of the loaded file */
static const char* line_at(unsigned int line) {
    const char* p = file_buf;

    while (--line && (p = strchr(p, '\n')))
        p++;
    return p;
}

/* the number of the first line from line `from` on that is exactly `text`; fails the build if none is */
static unsigned int find_line(const char* text, unsigned int from) {
    size_t n = strlen(text);
    unsigned int line = from;
    const char* p = line_at(from);

    for (; p && *p; line++) {
        const char* end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len == n && !memcmp(p, text, n))
            return line;
        if (!end)
            break;
        p = end + 1;
    }
    fprintf(stderr, "facts: no line \"%s\"\n", text);
    exit(1);
}

/* whether line `line` of the loaded file ends with `text` */
static int line_ends(unsigned int line, const char* text) {
    const char* p = line_at(line);
    const char* end = p ? strchr(p, '\n') : NULL;
    size_t n = strlen(text);

    if (!end) {
        fprintf(stderr, "facts: no line ending with %s\n", text);
        exit(1);
    }
    return (size_t)(end - p) >= n && !memcmp(end - n, text, n);
}

/* lines first to last as one JSON string, newlines and tabs escaped */
static void fact_lines(const char* key, unsigned int first, unsigned int last) {
    const char* p = line_at(first);
    const char* end = line_at(last + 1);

    fact_key(key);
    putchar('"');
    for (; p < end - 1; p++) {
        if (*p == '"' || *p == '\\')
            printf("\\%c", *p);
        else if (*p == '\n')
            printf("\\n");
        else if (*p == '\t')
            printf("\\t");
        else
            putchar(*p);
    }
    putchar('"');
}

/* test/seqlz_endian.c, its main() renamed and its line kept instead of printed */
static char endian_out[256];
#define main endian_main
#define printf(...) snprintf(endian_out, sizeof endian_out, __VA_ARGS__)
#define next endian_next
#include "../../test/seqlz_endian.c" /* found through -I docs/book */
#undef printf
#undef main
#undef next

/* the statement of the loaded file that starts a line with `start`, up to the line that ends with ")" */
static void fact_statement(const char* key, const char* start) {
    unsigned int a = 1, b;
    const char* p = file_buf;

    while (strncmp(p, start, strlen(start))) {
        p = strchr(p, '\n');
        if (!p) {
            fprintf(stderr, "facts: no line starts with %s\n", start);
            exit(1);
        }
        p++;
        a++;
    }
    for (b = a; !line_ends(b, ")"); b++)
        ;
    fact_lines(key, a, b);
}

static void kernel_port(void) {
    load("tools/kernel-port/seqlz.h");
    fact_num("ch15.kport.seqlz_h", lines_of_file());
    load("tools/kernel-port/api_compress.c");
    fact_num("ch15.kport.api_compress", lines_of_file());
    load("tools/kernel-port/api_codes.c");
    fact_num("ch15.kport.api_codes", lines_of_file());

    load("tools/kernel-port/backend_seqlz.c");
    fact_num("ch15.backend.head.a", find_line("#define pr_fmt(fmt) \"seqlz: \" fmt", 1));
    fact_num("ch15.backend.head.b",
             find_line("}", find_line("static int seqlz_setup_params(struct zcomp_params *params)", 1)));
    fact_num("ch15.backend.create.a",
             find_line("static int seqlz_create(struct zcomp_params *params, struct zcomp_ctx *ctx)", 1));
    fact_num("ch15.backend.create.b", find_line("}", find_line("static void seqlz_destroy(struct zcomp_ctx *ctx)", 1)));
    fact_num("ch15.backend.split.a", find_line("static int seqlz_create_cctx(struct zcomp_params *params,", 1));
    fact_num("ch15.backend.split.b",
             find_line("}", find_line("static int seqlz_create_dctx(struct zcomp_params *params,", 1)));
    fact_num("ch15.backend.pages.a", find_line("static int seqlz_zcomp_compress(struct zcomp_params *params,", 1));
    fact_num("ch15.backend.pages.b",
             find_line("}", find_line("static int seqlz_zcomp_decompress(struct zcomp_params *params,", 1)));

    load("bench/kernel_codecs/zram_codec.h");
    {
        unsigned int a = find_line("static inline void quetschn_prefetch_page(const void* src, unsigned int src_len, "
                                   "void* dst, unsigned int dst_len) {",
                                   1);

        fact_num("ch15.prefetch_page.a", a);
        fact_num("ch15.prefetch_page.b", find_line("}", a));
    }

    load("tools/kernel-port/port.py");
    fact_statement("ch15.port.harness_only", "HARNESS_ONLY = re.compile(");
    fact_statement("ch15.port.cpu_model", "CPU_MODEL = re.compile(");
    fact_statement("ch15.port.forbidden", "FORBIDDEN = re.compile(");

    load("tools/kernel-port/seqlz_kunit.c");
    {
        unsigned int cases = 0, kinds = 0;
        const char* p;

        for (p = file_buf; (p = strstr(p, "\n\tKUNIT_CASE(")); p++)
            cases++;
        p = strstr(file_buf, "switch (kind % ");
        if (!p || sscanf(p, "switch (kind %% %u)", &kinds) != 1) {
            fprintf(stderr, "facts: no switch (kind %% n) in seqlz_kunit.c\n");
            exit(1);
        }
        fact_num("ch15.kunit.cases", cases);
        fact_num("ch15.kunit.kinds", kinds);
    }

    /* the pages the tables were trained on, from the head of the counts they are built from */
    load(QUETSCHN_PAGE_BITS == 12 ? "bench/seqlz_counts_4k.txt" : "bench/seqlz_counts_16k.txt");
    {
        const char* p = strstr(file_buf, " pages, ");
        unsigned int pages;

        while (p && p > file_buf && p[-1] >= '0' && p[-1] <= '9')
            p--;
        if (!p || sscanf(p, "%u pages, ", &pages) != 1) {
            fprintf(stderr, "facts: no \"<n> pages, \" in the counts\n");
            exit(1);
        }
        fact_num("ch17.train_pages", pages);
    }

    load("src/zram_seqlz.c");
    fact_num("ch15.zram_lines", lines_of_file());
}

/*
 * What an allocation of n bytes takes, as the kernel's allocators round it: kmalloc() up to 2 pages,
 * KMALLOC_MAX_CACHE_SIZE, in caches of powers of 2, a larger one in pages, a power of 2 of them
 */
static unsigned int allocated(unsigned int n) {
    unsigned int a = n <= 2 * SEQLZ_PAGE ? 8 : SEQLZ_PAGE;

    while (a < n)
        a *= 2;
    return a;
}

int main(void) {
    const unsigned char* tok = seqlz_default_own.token;
    unsigned int s, own = 0, shortest = 99, longest = 0;
    char buf[32];

    kernel_port();

    endian_main();
    endian_out[strcspn(endian_out, "\n")] = 0;
    fact_str("ch15.endian.line", endian_out);
    if (sscanf(endian_out, "pages %*u, coded %u", &s) != 1)
        exit(1);
    fact_num("ch15.endian.coded", s);
    if (sscanf(endian_out, "pages %*u, coded %*u, own table %u", &s) != 1)
        exit(1);
    fact_num("ch15.endian.own", s);
    fact_num("ch15.endian.runs", 600 * SEQLZ_LEVELS);

    fact_num("ch15.state_plus_scratch", sizeof(struct seqlz_state) + SEQLZ_SCRATCH);
    fact_num("ch15.mem.union", sizeof(struct seqlz_state) > SEQLZ_SCRATCH ? sizeof(struct seqlz_state) : SEQLZ_SCRATCH);
    fact_num("ch15.mem.union_gets",
             allocated(sizeof(struct seqlz_state) > SEQLZ_SCRATCH ? sizeof(struct seqlz_state) : SEQLZ_SCRATCH));
    fact_num("ch15.mem.struct_gets", allocated(sizeof(struct seqlz_state) + SEQLZ_SCRATCH));
    fact_num("ch15.mem.cctx_gets", allocated(sizeof(struct seqlz_state)));
    fact_num("ch15.mem.dctx_gets", allocated(SEQLZ_SCRATCH));
    fact_num("ch15.mem.split_gets", allocated(sizeof(struct seqlz_state)) + allocated(SEQLZ_SCRATCH));
    fact_num("ch15.mem.tables_gets", allocated(sizeof(struct seqlz_tables)));
    fact_num("ch15.tables_kb", (sizeof(struct seqlz_tables) + 500) / 1000);
    snprintf(buf, sizeof buf, "0x%xU", SEQLZ_RAW_NIBBLES);
    fact_str("ch15.raw_nibbles", buf);
    fact_num("ch15.coded_min", SEQLZ_LIT_CODED_MIN);
    fact_num("ch15.enc_room", ENC_ROOM);

    for (s = 0; s < SEQLZ_TOKEN_SYMBOLS; s++) {
        if (!tok[s])
            continue;
        own++;
        shortest = tok[s] < shortest ? tok[s] : shortest;
        longest = tok[s] > longest ? tok[s] : longest;
    }
    fact_num("ch17.tok_own", own);
    fact_num("ch17.tok_shortest", shortest);
    fact_num("ch17.tok_longest", longest);
    fact_num("ch17.escape_len", tok[SEQLZ_ESCAPE]);
    fact_num("ch17.escape_bits", SEQLZ_ESCAPE_BITS);
    fact_num("ch17.rare_token_bits", tok[SEQLZ_ESCAPE] + SEQLZ_ESCAPE_BITS);
    fact_num("ch17.tables", 4 + SEQLZ_LIT_SETS);
    fact_num("ch17.ll_alone_max", SEQLZ_LL_CAP - 1);
    fact_num("ch17.ll_value_from", SEQLZ_LL_CAP);
    fact_num("ch17.ml_alone_max", SEQLZ_ML_CAP - 1 + 4);
    fact_num("ch17.ml_value_from", SEQLZ_ML_CAP + 4);
    fact_num("ch17.tok_entries", 1U << SEQLZ_TOKEN_BITS);
    fact_num("ch17.tok_entry_bytes", sizeof(((struct token_table*)0)->decode[0]));
    fact_num("ch17.tok_lines", sizeof_field(struct token_table, decode) / 64);
    fact_num("ch17.len_lines", sizeof_field(struct value_table, decode) / 64);
    fact_num("ch17.lit_entries", 1U << SEQLZ_LIT_BITS);
    fact_num("ch17.lit_entry_bytes", sizeof(((struct lit_table*)0)->decode[0]));
    fact_num("ch17.lit_lines", sizeof_field(struct lit_table, decode) / 64);
    fact_num("ch17.prefetch_lines", PREFETCH_STEP / 64);
    fact_num("ch17.lit_cost_run", LIT_COST_RUN);
    fact_num("ch17.refill_below", SEQLZ_TOKEN_BITS + QUETSCHN_PAGE_BITS);
    fact_num("ch17.width_min", SEQLZ_SIZE_BITS_MIN);
    fact_num("ch17.width_max", SEQLZ_SIZE_BITS_MAX);
    fact_num("ch17.hash_slots", 1U << SEQLZ_HASH_BITS);
    fact_num("ch18.slot_max", (1U << SEQLZ_HASH_BITS) - 1);
    fact_num("ch18.bits.token", SEQLZ_TOKEN_BITS);
    fact_num("ch18.bits.lit", SEQLZ_LIT_BITS);
    fact_num("ch18.bits.len", SEQLZ_MAX_BITS);
    for (s = 1; s < SEQLZ_OFF_CLASSES; s++) {
        snprintf(buf, sizeof buf, "ch18.class_bits.%u", s);
        fact_num(buf, SEQLZ_RAW_BITS(s));
    }
    facts_end();
    return 0;
}
