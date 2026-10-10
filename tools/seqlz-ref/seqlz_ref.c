// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * A seqlz decoder written from docs/format.md alone, slow on purpose: every bit is read on its own, every
 * code is found by its canonical number, and every rule of the format is a check where the format
 * states it. Nothing from src/, so that a misreading in src/ is not shared. fuzz/seqlz_diff_fuzz.c
 * and test/seqlz_ref_test.cpp hold it against seqlz_decode().
 */
#include "seqlz_ref.h"

#include <string.h>

#define MAX_CODE 15U /* longer than the codes of any table, see "The tables" */
#define TOKENS 3072U /* "Tokens": 16 * 32 * 6, the escape is the symbol after them */

/* "Prefix codes", rules 1 to 3; 0, or -1 if the lengths are no complete prefix code */
static int make_code(struct seqlz_ref_code* c, const unsigned char* len, unsigned int n) {
    unsigned long space = 0;
    unsigned int l, s, k = 0;

    memset(c, 0, sizeof *c);
    for (s = 0; s < n; s++) {
        if (len[s] > MAX_CODE)
            return -1;
        c->count[len[s]]++;
    }
    c->count[0] = 0;
    for (l = 2; l <= MAX_CODE; l++)
        c->first[l] = (c->first[l - 1] + c->count[l - 1]) * 2U;
    for (l = 1; l <= MAX_CODE; l++) {
        c->start[l] = k;
        for (s = 0; s < n; s++)
            if (len[s] == l)
                c->sym[k++] = (unsigned short)s;
        space += (unsigned long)c->count[l] << (MAX_CODE - l);
    }
    /* "Every table is complete": 2^-l summed over all codes is 1 */
    return space == 1UL << MAX_CODE ? 0 : -1;
}

int seqlz_ref_init(struct seqlz_ref* r, const struct seqlz_ref_tables* t, unsigned int page_bits) {
    unsigned int k;

    if (page_bits != 12U && page_bits != 14U)
        return -1;
    r->page_bits = page_bits;
    if (make_code(&r->tok, t->tok, TOKENS + 1U) || make_code(&r->ll, t->ll, 13U + page_bits) ||
        make_code(&r->ml, t->ml, 13U + page_bits))
        return -1;
    for (k = 0; k < 8U; k++)
        if (make_code(&r->lit[k], t->lit[k], 256U))
            return -1;
    return 0;
}

/* bits read most significant bit first, at most nbits of them; reading more makes the page invalid */
struct bits {
    const unsigned char* p;
    size_t nbits, pos;
};

static int bit(struct bits* b, unsigned int* v) {
    if (b->pos >= b->nbits)
        return -1;
    *v = ((unsigned int)b->p[b->pos / 8U] >> (7U - b->pos % 8U)) & 1U;
    b->pos++;
    return 0;
}

/* "The end of a page": fewer than 8 bits left, and they are 0; 0 if so, else -1 */
static int ends_here(struct bits* b) {
    unsigned int v;

    if (b->nbits - b->pos >= 8U)
        return -1;
    while (b->pos < b->nbits)
        if (bit(b, &v) || v != 0)
            return -1;
    return 0;
}

/* read(n): the next n bits as a number, the first one the most significant */
static int read_n(struct bits* b, unsigned int n, unsigned int* v) {
    unsigned int k, x;

    *v = 0;
    for (k = 0; k < n; k++) {
        if (bit(b, &x))
            return -1;
        *v = *v * 2U + x;
    }
    return 0;
}

/* symbol(table): bit by bit until they are a code, rule 4 read backwards */
static int symbol(struct bits* b, const struct seqlz_ref_code* c, unsigned int* sym) {
    unsigned int l, x, number = 0;

    for (l = 1; l <= MAX_CODE; l++) {
        if (bit(b, &x))
            return -1;
        number = number * 2U + x;
        if (number >= c->first[l] && number - c->first[l] < c->count[l]) {
            *sym = c->sym[c->start[l] + number - c->first[l]];
            return 0;
        }
    }
    return -1;
}

/* "Length values" */
static int value(struct bits* b, const struct seqlz_ref_code* c, unsigned int* v) {
    unsigned int s, x;

    if (symbol(b, c, &s))
        return -1;
    if (s < 16U) {
        *v = s;
        return 0;
    }
    if (read_n(b, s - 12U, &x))
        return -1;
    *v = (1U << (s - 12U)) + x;
    return 0;
}

int seqlz_ref_decode(const struct seqlz_ref* r, const unsigned char* src, size_t len, unsigned char* out) {
    unsigned char lits[1U << 14];
    const unsigned int P = r->page_bits, page = 1U << P;
    /* "Offsets": the offset bits of each class, and what the number read is multiplied by */
    const unsigned int class_bits[6] = {0, 4, 8, P, 5, P - 3U};
    const unsigned int class_scale[6] = {0, 1, 1, 1, 8, 8};
    struct bits b;
    unsigned int h, n, used = 0, size = 0, last = 1;

    /* "Layout of a compressed page" */
    if (len < 2U)
        return -1;
    h = src[0] | (unsigned int)src[1] << 8;
    n = h & 0x7fffU;
    if (n > page)
        return -1;
    if (!(h & 0x8000U)) {
        if (2U + (size_t)n > len)
            return -1;
        memcpy(lits, src + 2, n);
        b = (struct bits){src + 2 + n, 8U * (len - 2U - n), 0};
    } else {
        unsigned int t, w, j, k;
        size_t sizes[8], start, total = 0;

        if (len < 3U)
            return -1;
        t = src[2];
        w = 5U + ((t >> 3) & 7U);
        if (len < 3U + w || t >> 6)
            return -1;
        /* S is the 8 * w bits from byte 3 on, little endian, s[j] = (S >> (j * w)) & ((1 << w) - 1):
         * bit q of S is bit q mod 8 of byte 3 + q div 8. Bit by bit, as S has up to 96 bits. */
        for (j = 0; j < 8U; j++) {
            unsigned int x = 0, i;

            for (i = 0; i < w; i++) {
                unsigned int q = j * w + i;

                x |= ((unsigned int)(src[3 + q / 8U] >> (q % 8U)) & 1U) << i;
            }
            sizes[j] = x;
            total += x;
        }
        if (3U + w + total > len)
            return -1;
        /* "Coded literals": stream j holds the literals j, j + 8, ..., their codes within s[j] bytes */
        start = 3U + w;
        for (j = 0; j < 8U; j++) {
            struct bits s = {src + start, 8U * sizes[j], 0};

            for (k = j; k < n; k += 8U) {
                unsigned int v;

                if (symbol(&s, &r->lit[t & 7U], &v))
                    return -1;
                lits[k] = (unsigned char)v;
            }
            if (ends_here(&s))
                return -1;
            start += sizes[j];
        }
        b = (struct bits){src + start, 8U * (len - start), 0};
    }

    /* "Decoding" */
    for (;;) {
        unsigned int tk, ll, mlf, c, x, off, ml, k;

        /* "Tokens" */
        if (symbol(&b, &r->tok, &tk))
            return -1;
        if (tk == TOKENS && (read_n(&b, 12U, &tk) || tk >= TOKENS))
            return -1;
        ll = tk & 15U;
        mlf = (tk >> 4) & 31U;
        c = tk >> 9;
        if (read_n(&b, class_bits[c], &x))
            return -1;
        off = c == 0 ? last : x * class_scale[c];
        if (ll == 15U) {
            if (value(&b, &r->ll, &x))
                return -1;
            ll = 15U + x;
        }
        if (ll > n - used || ll > page - size)
            return -1;
        for (k = 0; k < ll; k++)
            out[size++] = lits[used++];
        if (size == page) {
            /* the last sequence */
            if (mlf != 0 || c != 0)
                return -1;
            break;
        }
        ml = mlf + 4U;
        if (mlf == 31U) {
            if (value(&b, &r->ml, &x))
                return -1;
            ml = 35U + x;
        }
        last = off;
        if (off == 0 || off > size || ml > page - size)
            return -1;
        /* one byte at a time, so that a match can repeat what it writes */
        for (k = 0; k < ml; k++, size++)
            out[size] = out[size - off];
    }
    /* "at the stop: used == n", B ends here; that no bit after B was used, every read checked */
    return used == n && ends_here(&b) == 0 ? 0 : -1;
}
