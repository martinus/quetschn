// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * ch07_09.c: the facts of chapters 7 (seqlz_internal.h), 8 (seqlz_codes.c) and
 * 9 (seqlz_match.h): the entries of the tables as seqlz_tables_init() builds them,
 * first_codes() on the real lengths, the layout of struct seqlz_tables, the
 * files' lines, the counts the tables were trained on, and the matcher's steps
 * on the pages of chapter 9.
 */
// facts: also 16k
#include <stddef.h>

#include "facts.h"
#include "seqlz_match.h"

/* first_codes() and length_entry() are static: the file is included, and
 * build.py does not link it */
#include "seqlz_codes.c"

static char key[96];
#define K(...) (snprintf(key, sizeof key, __VA_ARGS__), key)

static void fact_fmt(const char* k, const char* fmt, unsigned long long v) {
    char buf[64];

    snprintf(buf, sizeof buf, fmt, v);
    fact_str(k, buf);
}

/* the repository's root, from this file's path, to read files that are not code
 */
static const char* repo(void) {
    static char root[4096];
    const char* at = strstr(__FILE__, "docs/book/facts/");

    if (!root[0]) {
        size_t n = at ? (size_t)(at - __FILE__) : 0;

        memcpy(root, __FILE__, n);
        root[n] = 0;
    }
    return root;
}

static FILE* open_repo(const char* path) {
    char name[4200];
    FILE* f;

    snprintf(name, sizeof name, "%s%s", repo(), path);
    f = fopen(name, "r");
    if (!f) {
        fprintf(stderr, "facts/ch07_09.c: cannot read %s\n", name);
        exit(1);
    }
    return f;
}

static unsigned int lines_of(const char* path) {
    FILE* f = open_repo(path);
    unsigned int n = 0;
    int c;

    while ((c = fgetc(f)) != EOF)
        n += c == '\n';
    fclose(f);
    return n;
}

/* the number of the first line that starts with s, 1 for the first line */
static unsigned int line_of(const char* path, const char* s) {
    FILE* f = open_repo(path);
    char line[512];
    unsigned int n = 0;

    while (fgets(line, sizeof line, f)) {
        n++;
        if (!strncmp(line, s, strlen(s))) {
            fclose(f);
            return n;
        }
    }
    fprintf(stderr, "facts/ch07_09.c: %s has no line %s\n", path, s);
    exit(1);
}

/* a counts file: its pages, from the head, and the fewest pages of a literal
 * table */
static void counts(const char* name, const char* path) {
    FILE* f = open_repo(path);
    char line[1 << 16];
    unsigned int pages = 0, lit_min = ~0U, n;

    while (fgets(line, sizeof line, f)) {
        const char* p = strstr(line, " pages,");

        if (line[0] == '#' && p && !pages) {
            while (p > line && p[-1] >= '0' && p[-1] <= '9')
                p--;
            pages = (unsigned int)atoi(p);
        }
        if (sscanf(line, "lit 257 %u", &n) == 1 && n < lit_min)
            lit_min = n;
    }
    fclose(f);
    fact_num(K("%s.pages", name), pages);
    fact_num(K("%s.lit_min_pages", name), lit_min);
}

/* the first 8 hex digits of a table's SHA-256 in docs/format.md */
static void format_hash(const char* name, const char* table) {
    FILE* f = open_repo("docs/format.md");
    char line[512], row[64], hash[9];

    snprintf(row, sizeof row, "| `%s` | `", table);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, row, strlen(row))) {
            memcpy(hash, line + strlen(row), 8);
            hash[8] = 0;
            fact_str(name, hash);
            fclose(f);
            return;
        }
    fprintf(stderr, "facts/ch07_09.c: docs/format.md has no hash of %s\n", table);
    exit(1);
}

/* the n bits of v from bit `at` on, highest first */
static void fact_field(const char* k, u32 v, unsigned int at, unsigned int n) {
    char buf[33];
    unsigned int i;

    for (i = 0; i < n; i++)
        buf[i] = (char)('0' + ((v >> (at + n - 1 - i)) & 1));
    buf[n] = 0;
    fact_str(k, buf);
}

static void fact_u64_bin(const char* k, u64 v, unsigned int n) {
    char buf[80], *w = buf;
    unsigned int i;

    /* the top n bits, in groups of 4 */
    for (i = 0; i < n; i++) {
        if (i && i % 4 == 0)
            *w++ = ' ';
        *w++ = (char)('0' + ((v >> (63 - i)) & 1));
    }
    *w = 0;
    fact_str(k, buf);
}

/* ---- chapter 7: the entries of the tables */

static void token_facts(unsigned int s) {
    const struct seqlz_tables* t = fact_tables();
    unsigned int n = seqlz_default_lengths.token[s];
    u32 e = token_entry(s, n);

    fact_fmt(K("ch07.tok.%u.enc", s), "%#llx", t->token.enc[s]);
    fact_fmt(K("ch07.tok.%u.entry", s), "%#010llx", e);
    fact_u64_bin(K("ch07.tok.%u.bin", s), (u64)e << 32, 32);
    if (n) {
        unsigned int c = t->token.enc[s] & TOK_ENC_CODE_MASK;

        fact_num(K("ch07.tok.%u.c", s), c);
        fact_num(K("ch07.tok.%u.first", s), c << (SEQLZ_TOKEN_BITS - n));
        fact_num(K("ch07.tok.%u.last", s), ((c + 1) << (SEQLZ_TOKEN_BITS - n)) - 1);
        fact_num(K("ch07.tok.%u.entries", s), 1U << (SEQLZ_TOKEN_BITS - n));
        fact_num(K("ch07.tok.%u.rest", s), SEQLZ_TOKEN_BITS - n);
    }
    fact_num(K("ch07.tok.%u.N", s), TOK_N(e));
    fact_num(K("ch07.tok.%u.BACK", s), TOK_BACK(e));
    fact_num(K("ch07.tok.%u.SHIFT", s), TOK_SHIFT(e));
    fact_num(K("ch07.tok.%u.DROP", s), TOK_DROP(e));
    fact_num(K("ch07.tok.%u.LL", s), TOK_LL(e));
    fact_num(K("ch07.tok.%u.ML", s), TOK_ML(e));
    fact_num(K("ch07.tok.%u.value", s), (e >> TOK_VALUE_AT) & 1);
    /* the fields as bits, for the figure */
    fact_field(K("ch07.tok.%u.bits.n", s), e, 0, 6);
    fact_field(K("ch07.tok.%u.bits.back", s), e, TOK_BACK_AT, 6);
    fact_field(K("ch07.tok.%u.bits.shift", s), e, TOK_SHIFT_AT, 2);
    fact_field(K("ch07.tok.%u.bits.drop", s), e, TOK_DROP_AT, 5);
    fact_field(K("ch07.tok.%u.bits.ll", s), e, TOK_LL_AT, 4);
    fact_field(K("ch07.tok.%u.bits.ml", s), e, TOK_ML_AT, 6);
    fact_field(K("ch07.tok.%u.bits.free", s), e, 29, 1);
    fact_field(K("ch07.tok.%u.bits.value", s), e, TOK_VALUE_AT, 1);
    fact_field(K("ch07.tok.%u.bits.escape", s), e, TOK_ESCAPE_AT, 1);
}

static void ch07(void) {
    static const unsigned int tokens[] = {34, 49, 1552, 1567, 1797, 2032, 2097, 2098, SEQLZ_ESCAPE};
    const struct seqlz_tables* t = fact_tables();
    unsigned int k, j, best = 0, s, c, n;
    u64 sum = t->lit_cost[0] + t->lit_cost[0x65], reg, e;

    fact_num("ch07.lines", lines_of("src/seqlz_internal.h"));
    for (k = 0; k < sizeof tokens / sizeof tokens[0]; k++)
        token_facts(tokens[k]);
    /* the escaped token 1797 and its offset 1000: the entry with n = 0 */
    e = token_entry(1797, 0);
    fact_fmt("ch07.esc1797.entry", "%#010llx", e);
    fact_num("ch07.esc1797.BACK", TOK_BACK(e));
    fact_num("ch07.esc1797.DROP", TOK_DROP(e));
    fact_num("ch07.esc1797.LL", TOK_LL(e));
    fact_num("ch07.esc1797.ML", TOK_ML(e));
    fact_field("ch07.esc1797.bits", 1797, 0, SEQLZ_ESCAPE_BITS);
    /* the decoder's top 11 bits: the escape's code, then the token */
    n = seqlz_default_lengths.token[SEQLZ_ESCAPE];
    c = (t->token.enc[SEQLZ_ESCAPE] & TOK_ENC_CODE_MASK) << (SEQLZ_TOKEN_BITS - n) |
        1797 >> (SEQLZ_ESCAPE_BITS - (SEQLZ_TOKEN_BITS - n));
    fact_num("ch07.esc1797.index", c);
    fact_field("ch07.esc1797.index_bits", c, 0, SEQLZ_TOKEN_BITS);

    /* token 2098's code, then its offset 16 / 8 = 2 in 5 bits, then ones */
    n = seqlz_default_lengths.token[2098];
    c = t->token.enc[2098] & TOK_ENC_CODE_MASK;
    reg = (u64)c << (64 - n) | (u64)2 << (64 - n - 5) | ((1ULL << (64 - n - 5)) - 1);
    e = token_entry(2098, n);
    fact_fmt("ch07.reg", "%#llx", reg);
    fact_u64_bin("ch07.reg_bin", reg, 20);
    fact_fmt("ch07.reg_shl", "%#llx", reg << TOK_N(e));
    fact_u64_bin("ch07.reg_shl_bin", reg << TOK_N(e), 20);
    fact_field("ch07.reg_shl_low", (u32)(reg << TOK_N(e)), 0, 4);
    fact_field("ch07.reg_low", (u32)reg, 0, 4);
    fact_num("ch07.reg_index", reg >> (64 - SEQLZ_TOKEN_BITS));
    fact_field("ch07.reg_index_bits", (u32)(reg >> (64 - SEQLZ_TOKEN_BITS)), 0, SEQLZ_TOKEN_BITS);
    fact_num("ch07.reg_raw", (reg << TOK_N(e)) >> TOK_BACK(e));
    fact_field("ch07.reg_raw_bits", (u32)((reg << TOK_N(e)) >> TOK_BACK(e)), 0, 64 - TOK_BACK(e));
    fact_num("ch07.reg_off", ((reg << TOK_N(e)) >> TOK_BACK(e)) << TOK_SHIFT(e));

    /* symbol 18 of the ll table: values 64 to 127 */
    s = 18;
    n = seqlz_default_lengths.ll[s];
    c = t->ll.enc[s] & 0xffffU;
    fact_num("ch07.ll18.c", c);
    fact_fmt("ch07.ll18.enc", "%#llx", t->ll.enc[s]);
    fact_fmt("ch07.ll18.entry", "%#llx", t->ll.decode[c << (SEQLZ_MAX_BITS - n)]);
    fact_num("ch07.ll18.first", c << (SEQLZ_MAX_BITS - n));
    fact_num("ch07.ll18.last", ((c + 1) << (SEQLZ_MAX_BITS - n)) - 1);
    fact_num("ch07.ll18.entries", 1U << (SEQLZ_MAX_BITS - n));
    fact_num("ch07.len_base_max", length_entry(SEQLZ_LEN_SYMBOLS - 1) >> 8);

    /* the letter e in literal table 0, and the space */
    for (k = 0; k < 2; k++) {
        const char* name = k ? "ch07.lit_e" : "ch07.lit_space";

        s = k ? 0x65 : 0x20;
        n = seqlz_lit_sets[0][s];
        c = t->lit[0].enc[s] >> 8;
        fact_num(K("%s.c", name), c);
        fact_num(K("%s.first", name), c << (SEQLZ_LIT_BITS - n));
        fact_num(K("%s.last", name), ((c + 1) << (SEQLZ_LIT_BITS - n)) - 1);
        fact_num(K("%s.entries", name), 1U << (SEQLZ_LIT_BITS - n));
        fact_fmt(K("%s.entry", name), "%#llx", t->lit[0].decode[c << (SEQLZ_LIT_BITS - n)]);
        fact_fmt(K("%s.enc", name), "%#llx", t->lit[0].enc[s]);
    }

    /* lit_cost of the bytes 0x00 and 0x65, and their sum */
    fact_fmt("ch07.lit_cost.0", "%#018llx", t->lit_cost[0]);
    fact_fmt("ch07.lit_cost.101", "%#018llx", t->lit_cost[0x65]);
    fact_fmt("ch07.lit_cost.sum", "%#018llx", sum);
    for (j = 0; j < SEQLZ_LIT_SETS; j++) {
        unsigned int lane = (unsigned int)(sum >> (8 * j)) & 255;

        fact_num(K("ch07.lit_sum.%u", j), lane);
        fact_fmt(K("ch07.lit_lane.101.%u", j), "%02llx", (t->lit_cost[0x65] >> (8 * j)) & 255);
        if (lane < ((sum >> (8 * best)) & 255))
            best = j;
    }
    /* 0x65's code lengths as a row of the text block in chapter 8, under the numbers 0 to 7 */
    {
        char row[64], *w = row;

        for (j = 0; j < SEQLZ_LIT_SETS; j++)
            w += sprintf(w, j ? "%5u" : "%7u", seqlz_lit_sets[j][0x65]);
        fact_str("ch08.lit101_row", row);
    }
    fact_num("ch07.lit_best", best);
    fact_num("ch07.lit_best_bits", (sum >> (8 * best)) & 255);
    fact_num("ch07.lit_stream_max", LIT_STREAM_MAX);

    /* struct seqlz_tables and its parts */
    fact_num("ch07.size.value_table", sizeof(struct value_table));
    fact_num("ch07.size.value_decode", sizeof t->ll.decode);
    fact_num("ch07.size.value_enc", sizeof t->ll.enc);
    fact_num("ch07.size.token_decode", sizeof t->token.decode);
    fact_num("ch07.size.token_enc", sizeof t->token.enc);
    fact_num("ch07.size.token_enc_u32", 4 * (SEQLZ_TOKEN_SYMBOLS + 1));
    fact_num("ch07.size.lit_table", sizeof(struct lit_table));
    fact_num("ch07.size.lit_decode", sizeof t->lit[0].decode);
    fact_num("ch07.size.lit_enc", sizeof t->lit[0].enc);
    fact_num("ch07.size.lit", sizeof t->lit);
    fact_num("ch07.size.lit_cost", sizeof t->lit_cost);
    fact_num("ch07.off.token_enc", offsetof(struct seqlz_tables, token.enc));
    fact_num("ch07.off.lit", offsetof(struct seqlz_tables, lit));
    fact_num("ch07.off.pad", offsetof(struct seqlz_tables, lit) + sizeof t->lit);
    fact_num("ch07.pad", offsetof(struct seqlz_tables, lit_cost) - offsetof(struct seqlz_tables, lit) - sizeof t->lit);
    fact_num("ch07.off.lit_cost", offsetof(struct seqlz_tables, lit_cost));
    fact_num("ch07.off.own", offsetof(struct seqlz_tables, own));
    fact_num("ch07.size.own", sizeof t->own);
    fact_num("ch07.pad_own", offsetof(struct seqlz_tables, ll) - offsetof(struct seqlz_tables, own) - sizeof t->own);
    fact_num("ch07.off.ll", offsetof(struct seqlz_tables, ll));
    fact_num("ch07.off.ml", offsetof(struct seqlz_tables, ml));
    fact_num("ch07.off.all_symbols", offsetof(struct seqlz_tables, all_symbols));
    fact_num("ch07.tail_pad", sizeof *t - offsetof(struct seqlz_tables, all_symbols) - 1);
}

/* ---- chapter 8: the codes from the lengths */

static void next_facts(const char* name, const u8* len, unsigned int n, unsigned int bits) {
    unsigned int next[16], l;

    if (first_codes(len, n, bits, next)) {
        fprintf(stderr, "facts/ch07_09.c: %s is no complete code\n", name);
        exit(1);
    }
    for (l = 1; l <= bits; l++)
        fact_num(K("ch08.%s.next.%u", name, l), next[l]);
}

static void ch08(void) {
    static const unsigned int symbols[] = {5, 16, 18, SEQLZ_LEN_SYMBOLS - 1};
    unsigned int k, j, l, n16 = 0, sum[16] = {0}, min, max, min2 = 0, min34 = 0;
    char lengths[3 * SEQLZ_LEN_SYMBOLS + 1], *w = lengths;

    fact_num("ch08.lines", lines_of("src/seqlz_codes.c"));
    fact_num("ch08.length_bytes", sizeof(struct seqlz_lengths) + sizeof seqlz_lit_sets + sizeof seqlz_own_lengths);
    for (k = 0; k < sizeof symbols / sizeof symbols[0]; k++)
        fact_fmt(K("ch08.le.%u", symbols[k]), "%#llx", length_entry(symbols[k]));
    fact_num("ch08.le_last_symbol", SEQLZ_LEN_SYMBOLS - 1);
    fact_num("ch08.le_last_extra", (length_entry(SEQLZ_LEN_SYMBOLS - 1) >> 4) & 15);

    /* the ll table: its lengths, the sum k of first_codes(), next[] */
    for (k = 0; k < SEQLZ_LEN_SYMBOLS; k++)
        w += sprintf(w, "%s%u", k ? " " : "", seqlz_default_lengths.ll[k]);
    fact_str("ch08.ll.lengths", lengths);
    for (k = 0; k < SEQLZ_LEN_SYMBOLS; k++)
        sum[seqlz_default_lengths.ll[k]] += 1U << (SEQLZ_MAX_BITS - seqlz_default_lengths.ll[k]);
    for (l = 3; l <= SEQLZ_MAX_BITS; l++)
        fact_num(K("ch08.ll.fill.%u", l), sum[l]);
    next_facts("ll", seqlz_default_lengths.ll, SEQLZ_LEN_SYMBOLS, SEQLZ_MAX_BITS);
    next_facts("tok", seqlz_default_lengths.token, SEQLZ_TOKEN_SYMBOLS + 1, SEQLZ_TOKEN_BITS);

    /* the token table: how many entries the codes of each length fill */
    for (l = 1; l <= SEQLZ_TOKEN_BITS; l++) {
        unsigned int count = 0;

        for (k = 0; k <= SEQLZ_TOKEN_SYMBOLS; k++)
            count += seqlz_default_lengths.token[k] == l;
        fact_num(K("ch08.tok.fill.%u", l), count << (SEQLZ_TOKEN_BITS - l));
    }
    for (k = 0; k < SEQLZ_TOKEN_SYMBOLS; k++)
        n16 += seqlz_default_lengths.token[k] != 0;
    fact_num("ch08.tok.coded", n16);
    fact_num("ch08.tok.coded_64", n16 / 64);
    fact_num("ch08.tok.escaped", SEQLZ_TOKEN_SYMBOLS - n16);
    /* token 49's entries in bytes of the decode table, and in cache lines of 64
     * bytes */
    {
        unsigned int c = fact_tables()->token.enc[49] & TOK_ENC_CODE_MASK, n = seqlz_default_lengths.token[49];
        unsigned int first = (c << (SEQLZ_TOKEN_BITS - n)) * 4, end = ((c + 1) << (SEQLZ_TOKEN_BITS - n)) * 4;

        fact_num("ch08.tok49.byte_first", first);
        fact_num("ch08.tok49.byte_last", end - 1);
        fact_num("ch08.tok49.lines", (end - first) / 64);
    }
    /* the entries of the 4-bit codes, the first of the table, in bytes and cache
     * lines */
    for (k = 0, j = 0; k <= SEQLZ_TOKEN_SYMBOLS; k++)
        j += seqlz_default_lengths.token[k] == 4;
    fact_num("ch08.tok.len4.last", (j << (SEQLZ_TOKEN_BITS - 4)) - 1);
    fact_num("ch08.tok.len4.bytes", 4 * (j << (SEQLZ_TOKEN_BITS - 4)));
    fact_num("ch08.tok.len4.lines", 4 * (j << (SEQLZ_TOKEN_BITS - 4)) / 64);

    /* the literal tables: shortest and longest codes */
    for (j = 0; j < SEQLZ_LIT_SETS; j++) {
        min = 99;
        max = 0;
        for (k = 0; k < 256; k++) {
            min = seqlz_lit_sets[j][k] < min ? seqlz_lit_sets[j][k] : min;
            max = seqlz_lit_sets[j][k] > max ? seqlz_lit_sets[j][k] : max;
        }
        fact_num(K("ch08.lit.%u.min", j), min);
        fact_num(K("ch08.lit.%u.max", j), max);
        min2 += min == 2;
        min34 += min == 3 || min == 4;
    }
    fact_num("ch08.lit.min2", min2);
    fact_num("ch08.lit.min34", min34);

    /* the generated files: where token s and literal b are */
    k = line_of("src/seqlz_default_tables_4k.inc", "\t.token = {") + 1;
    fact_num("ch08.inc.tok_line", k);
    fact_num("ch08.inc.tok34_line", k + 34 / 16);
    fact_num("ch08.inc.tok34_col", 34 % 16 + 1);
    fact_num("ch08.inc.tok49_line", k + 49 / 16);
    fact_num("ch08.inc.esc_line", k + SEQLZ_ESCAPE / 16);
    k = line_of("src/seqlz_lit_sets_4k.inc", "\t{") + 1;
    fact_num("ch08.litinc.line", k);
    fact_num("ch08.litinc.line20", k + 0x20 / 16);
    fact_num("ch08.litinc.line60", k + 0x60 / 16);

    counts("ch08.counts4k", "bench/seqlz_counts_4k.txt");
    counts("ch08.counts16k", "bench/seqlz_counts_16k.txt");
    format_hash("ch08.tok4k_sha", "TOK_4k");
}

/* ---- chapter 9: the matcher */

/* chapter 9's second page: random bytes, 0 to 63 again at 64, and 124 to 137
 * again at 398 */
static void random_page(u8* p) {
    unsigned int k, seed = 12345;

    for (k = 0; k < SEQLZ_BLOCK_SIZE; k++) {
        seed = seed * 1103515245U + 12345U;
        p[k] = (u8)(seed >> 16);
    }
    memcpy(p + 64, p, 64);
    memcpy(p + 398, p + 124, 14);
}

/*
 * The loop of seqlz_match_block() again, with what each position sees: its hash,
 * the table's candidate, the repeat offset and the two hits. The sequences it
 * finds must be those of seqlz_find(), or this program fails, so the steps are
 * those of the real matcher.
 */
struct step {
    unsigned int h, cand, last, rep_hit, cand_hit;
};

static void replay(const u8* src, struct step* at) {
    static u16 table[1U << SEQLZ_HASH_BITS];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    unsigned int pos = 1, anchor = 0, last = 1, n = seqlz_find(&st, src, seq, 2), i = 0;

    memset(table, 0, sizeof table);
    memset(at, 0, SEQLZ_BLOCK_SIZE * sizeof *at);
    while (pos < SEQLZ_BLOCK_SIZE - 8U) {
        u32 cur = get_unaligned_le32(src + pos);
        unsigned int h = seqlz_hash5(get_unaligned_le64(src + pos)), cand = table[h], m, len;
        unsigned int rep_hit = get_unaligned_le32(src + pos - last) == cur;
        unsigned int cand_hit = get_unaligned_le32(src + cand) == cur;

        at[pos] = (struct step){h, cand, last, rep_hit, cand_hit};
        table[h] = (u16)pos;
        if (!(rep_hit | cand_hit)) {
            pos++;
            continue;
        }
        m = rep_hit ? pos - last : cand;
        len = 4U + seqlz_match_len(src + pos + 4, src + m + 4, src + SEQLZ_BLOCK_SIZE);
        last = pos - m;
        if (i >= n || seq[i].literals != pos - anchor || seq[i].match != len || seq[i].offset != last) {
            fprintf(stderr, "facts/ch07_09.c: the replay of the matcher differs at sequence %u\n", i);
            exit(1);
        }
        i++;
        pos += len;
        anchor = pos;
        if (pos < SEQLZ_BLOCK_SIZE - 8U)
            table[seqlz_hash5(get_unaligned_le64(src + pos - 2))] = (u16)(pos - 2);
    }
    if (i + 1 != n) {
        fprintf(stderr,
                "facts/ch07_09.c: the replay of the matcher found %u sequences, "
                "not %u\n",
                i + 1,
                n);
        exit(1);
    }
}

static void step_facts(const char* name, const u8* page, const struct step* at, unsigned int pos) {
    fact_hex(K("%s.%u.bytes", name, pos), page + pos, 5);
    fact_num(K("%s.%u.h", name, pos), at[pos].h);
    fact_num(K("%s.%u.cand", name, pos), at[pos].cand);
    fact_num(K("%s.%u.last", name, pos), at[pos].last);
    fact_num(K("%s.%u.rep_hit", name, pos), at[pos].rep_hit);
    fact_num(K("%s.%u.cand_hit", name, pos), at[pos].cand_hit);
}

static void hash_facts(const char* name, const u8* bytes) {
    u64 v = get_unaligned_le64(bytes), prod = (v << 24) * 889523592379ULL;
    unsigned int k, slot = seqlz_hash5(v);

    if (slot != prod >> (64 - SEQLZ_HASH_BITS)) {
        fprintf(stderr,
                "facts/ch07_09.c: seqlz_hash5() is no longer the product's "
                "top 12 bits\n");
        exit(1);
    }
    fact_fmt(K("%s.prod", name), "%#018llx", prod);
    for (k = 0; k < 8; k++)
        fact_fmt(K("%s.prod.%u", name, k), "%02llx", (prod >> (56 - 8 * k)) & 255);
    fact_fmt(K("%s.prod.1hi", name), "%llx", (prod >> 52) & 15);
    fact_fmt(K("%s.prod.1lo", name), "%llx", (prod >> 48) & 15);
    fact_num(K("%s.slot", name), slot);
    fact_fmt(K("%s.slot_hex", name), "%#llx", slot);
}

static void ch09(void) {
    static const unsigned int ptr_pos[] = {1, 2, 8, 9, 16, 17, 24, 25};
    static const unsigned int ptr_seq[] = {0, 1, 13, 14, 15, 29, 30};
    static const u8 h1[8] = {0x10, 0x00, 0x56, 0x34, 0x12, 0x7f, 0, 0}, h2[8] = {0x20, 0x00, 0x56, 0x34, 0x12, 0x7f, 0, 0};
    static const u8 p[] = "ABCDEFGHIJKLMNOP", q[] = "ABCDEFGHIJKxMNOP";
    static u8 page[SEQLZ_BLOCK_SIZE];
    static struct step at[SEQLZ_BLOCK_SIZE];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    unsigned int k, n, pos, odd = 0, cand0 = 0, first = 0;
    u64 x2 = get_unaligned_le64(p + 8) ^ get_unaligned_le64(q + 8);

    fact_num("ch09.lines", lines_of("src/seqlz_match.h"));
    fact_num("ch09.limit", SEQLZ_BLOCK_SIZE - 8);
    fact_num("ch09.limit_last", SEQLZ_BLOCK_SIZE - 9);
    fact_num("ch09.table_bytes", sizeof(u16) << SEQLZ_HASH_BITS);
    hash_facts("ch09.h1", h1);
    hash_facts("ch09.h2", h2);

    /* seqlz_match_len() on 16 bytes that differ in byte 11 */
    fact_fmt("ch09.mlen.x2", "%#018llx", x2);
    fact_num("ch09.mlen.ctz", __builtin_ctzll(x2 | 1ULL << 63));
    fact_num("ch09.mlen.len", seqlz_match_len(p, q, p + 16));

    /* the page of pointers */
    pointer_page(page);
    replay(page, at);
    for (k = 0; k < sizeof ptr_pos / sizeof ptr_pos[0]; k++)
        step_facts("ch09.ptr", page, at, ptr_pos[k]);
    for (pos = 3; pos <= 7; pos++)
        cand0 += at[pos].cand != 0 || at[pos].rep_hit || at[pos].cand_hit;
    fact_num("ch09.ptr.miss3to7", cand0);
    n = seqlz_find(&st, page, seq, 2);
    for (k = 0, pos = 0; k < n; k++) {
        unsigned int i;

        for (i = 0; i < sizeof ptr_seq / sizeof ptr_seq[0]; i++)
            if (ptr_seq[i] == k || k + 2 == n || k + 1 == n) {
                fact_num(K("ch09.ptr.seq.%u.at", k), pos);
                fact_num(K("ch09.ptr.seq.%u.ll", k), seq[k].literals);
                fact_num(K("ch09.ptr.seq.%u.ml", k), seq[k].match);
                fact_num(K("ch09.ptr.seq.%u.off", k), seq[k].offset);
                break;
            }
        /* all but the first and the last: 1 literal and 7 bytes, or 2 and 6 at a
         * carry */
        if (k && k + 1 < n &&
            !(seq[k].offset == 8 && seq[k].literals + seq[k].match == 8 && seq[k].literals == (k % 16 == 14 ? 2U : 1U)))
            odd++;
        pos += seq[k].literals + seq[k].match;
    }
    fact_num("ch09.ptr.seqs", n);
    fact_num("ch09.ptr.last", n - 1);
    fact_num("ch09.ptr.before_last", n - 2);
    fact_num("ch09.ptr.odd", odd);

    /* the random page: a match found late, not extended backwards */
    random_page(page);
    replay(page, at);
    for (pos = 398; pos <= 400; pos++)
        step_facts("ch09.rnd", page, at, pos);
    n = seqlz_find(&st, page, seq, 2);
    for (k = 0, pos = 0; k < n; k++) {
        unsigned int at_match = pos + seq[k].literals;

        if (seq[k].match && at_match >= 64 && pos <= 64 && !first) {
            first = 1;
            fact_num("ch09.rnd.first.at", at_match);
            fact_num("ch09.rnd.first.ml", seq[k].match);
            fact_num("ch09.rnd.first.off", seq[k].offset);
            fact_num("ch09.rnd.first.inserted", at_match + seq[k].match - 2);
        }
        /* the match found in the copy at 398 */
        if (seq[k].match && at_match <= 400 && at_match + seq[k].match > 400) {
            fact_num("ch09.rnd.seq.ll", seq[k].literals);
            fact_num("ch09.rnd.seq.ml", seq[k].match);
            fact_num("ch09.rnd.seq.off", seq[k].offset);
            fact_num("ch09.rnd.seq.at", at_match);
        }
        pos += seq[k].literals + seq[k].match;
    }
    /* level 3 on the same page: the match at 400 extended backwards over 398 and 399 */
    n = seqlz_find(&st, page, seq, SEQLZ_LEVEL_OWN);
    for (k = 0, pos = 0, first = 0; k < n; k++) {
        unsigned int at_match = pos + seq[k].literals;

        if (seq[k].match && at_match <= 400 && at_match + seq[k].match > 400) {
            fact_num("ch09.rnd3.seq.ll", seq[k].literals);
            fact_num("ch09.rnd3.seq.ml", seq[k].match);
            fact_num("ch09.rnd3.seq.off", seq[k].offset);
            fact_num("ch09.rnd3.seq.at", at_match);
            first = 1;
        }
        pos += seq[k].literals + seq[k].match;
    }
    if (!first)
        exit(1);
    fact_num("ch09.lazy_len", SEQLZ_LAZY_LEN);

    /*
     * the page of the lazy step: random bytes as random_page() without its copies; 100 to 104 the
     * byte 0x5a and the 4 bytes at 21, 105 not the byte at 25; at 199 the byte 0x5a, then the 31
     * bytes of 21 to 51
     */
    for (k = 0, pos = 12345; k < SEQLZ_BLOCK_SIZE; k++) {
        pos = pos * 1103515245U + 12345U;
        page[k] = (u8)(pos >> 16);
    }
    page[100] = 0x5a;
    memcpy(page + 101, page + 21, 4);
    page[105] = (u8)(page[25] ^ 0xff);
    page[199] = 0x5a;
    memcpy(page + 200, page + 21, 31);
    if (page[20] == 0x5a)
        page[20] ^= 1;
    for (pos = 2; pos <= 3; pos++) {
        unsigned int level = pos, i;

        n = seqlz_find(&st, page, seq, level);
        fact_num(K("ch09.lazy.l%u.seqs", level), n);
        for (k = 0, i = 0; k + 1 < n; k++) {
            fact_num(K("ch09.lazy.l%u.%u.at", level, k), i + seq[k].literals);
            fact_num(K("ch09.lazy.l%u.%u.ll", level, k), seq[k].literals);
            fact_num(K("ch09.lazy.l%u.%u.ml", level, k), seq[k].match);
            fact_num(K("ch09.lazy.l%u.%u.off", level, k), seq[k].offset);
            i += seq[k].literals + seq[k].match;
        }
    }
}

int main(void) {
    ch07();
    ch08();
    ch09();
    facts_end();
    return 0;
}
