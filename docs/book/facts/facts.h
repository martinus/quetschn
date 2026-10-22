// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * facts.h: what the facts programs share. Each C file of facts/ is built with src/ when the book is built
 * and prints one JSON object; a chapter quotes a value as {{key}}, see build.py. Keys are flat and
 * dotted, e.g. "ab.bytes", so that a chapter can name any of them without nesting.
 */
#ifndef BOOK_FACTS_H
#define BOOK_FACTS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pages.h"
#include "seqlz.h"

static int fact_n;

static inline void fact_key(const char* key) {
    printf("%s\n\"%s\":", fact_n++ ? "," : "{", key);
}

static inline void fact_num(const char* key, long long v) {
    fact_key(key);
    printf("%lld", v);
}

static inline void fact_str(const char* key, const char* s) {
    fact_key(key);
    putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            putchar('\\');
        putchar(*s);
    }
    putchar('"');
}

/* n bytes as "02 00 61 62" */
static inline void fact_hex(const char* key, const unsigned char* p, unsigned int n) {
    static char buf[3 * 2 * SEQLZ_BLOCK_SIZE + 1];
    char* w = buf;
    unsigned int k;

    *w = 0;
    for (k = 0; k < n; k++)
        w += sprintf(w, "%s%02x", k ? " " : "", p[k]);
    fact_str(key, buf);
}

/* the bits from bit `first` on, n of them, most significant bit of each byte first, as "0110" */
static inline void fact_bits(const char* key, const unsigned char* p, unsigned int first, unsigned int n) {
    static char buf[8 * 2 * SEQLZ_BLOCK_SIZE + 1];
    unsigned int k;

    for (k = 0; k < n; k++)
        buf[k] = (char)('0' + ((p[(first + k) / 8] >> (7 - (first + k) % 8)) & 1));
    buf[n] = 0;
    fact_str(key, buf);
}

static inline void facts_end(void) {
    printf(fact_n ? "\n}\n" : "{}\n");
}

/* the tables compiled in */
static inline const struct seqlz_tables* fact_tables(void) {
    return &seqlz_tables;
}

/* the page compressed as seqlz_compress() writes it at a level, 1 to SEQLZ_LEVELS, and decoded again */
static inline unsigned int fact_compress_level(const unsigned char* page, unsigned char* dst, unsigned int level) {
    static struct seqlz_state st;
    static unsigned char out[SEQLZ_BLOCK_SIZE], scratch[SEQLZ_SCRATCH];
    unsigned int n;

    fact_tables(); /* built once, for seqlz_compress() and seqlz_decode() */
    n = seqlz_compress(&st, page, dst, 2 * SEQLZ_BLOCK_SIZE, level);

    if (!n || seqlz_decode(dst, n, out, scratch) || memcmp(out, page, SEQLZ_BLOCK_SIZE)) {
        fprintf(stderr, "facts: a page does not come back\n");
        exit(1);
    }
    return n;
}

/* the page compressed as seqlz_compress() writes it, raw (coded = 0, level 1) or with coded literals (level 2) */
static inline unsigned int fact_compress(const unsigned char* page, unsigned char* dst, int coded) {
    return fact_compress_level(page, dst, coded ? 2U : 1U);
}

/*
 * The canonical code of each symbol from its length, as docs/format.md's "Prefix codes" gives them:
 * shorter codes first, those of one length by symbol. code[s] is "" for a symbol without a code.
 */
static inline void fact_canonical(const unsigned char* len, unsigned int n, char (*code)[17]) {
    unsigned int count[17] = {0}, next[17] = {0}, c = 0, s, l, k;

    for (s = 0; s < n; s++)
        count[len[s]]++;
    count[0] = 0;
    for (l = 1; l <= 16; l++) {
        c = (c + count[l - 1]) << 1;
        next[l] = c;
    }
    for (s = 0; s < n; s++) {
        l = len[s];
        code[s][l] = 0;
        if (!l)
            continue;
        for (k = 0; k < l; k++)
            code[s][k] = (char)('0' + ((next[l] >> (l - 1 - k)) & 1));
        next[l]++;
    }
}

#endif
