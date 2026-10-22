// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * The made-up pages the book's figures are built on, one copy for all of them: the C programs of
 * anim/, stepper/ and facts/ include it, and build.py puts docs/book on their include path. The
 * chapters quote numbers of these pages through facts/pages.c, so a change here changes every
 * figure and those numbers at once.
 */
#ifndef BOOK_PAGES_H
#define BOOK_PAGES_H

#include <stdio.h>
#include <string.h>

#include "seqlz_internal.h"

/* n bytes of v, little endian */
static inline void le(u8* p, unsigned long long v, unsigned int n) {
    while (n--) {
        *p++ = (u8)v;
        v >>= 8;
    }
}

/* chapter 16's page: a slab of 48 task records, a text after them, zeros up to the end */
static inline void slab_page(u8* p) {
    static const char* const names[] = {"kworker/0:1", "bash", "systemd", "sshd", "kworker/1:2", "cron"};
    static const char* const text = "zram stores each page in memory, compressed on its own. A page\n"
                                    "that comes back is decompressed while the program waits for it,\n"
                                    "so reading must be quick. Writing happens when memory runs low.\n"
                                    "Most pages hold pointers, small numbers and short strings, with\n"
                                    "many zero bytes between them. The compressor finds the bytes\n"
                                    "that repeat and writes a copy instead; the other bytes stay as\n"
                                    "literals. Text like this has few repeats but uses few letters,\n"
                                    "and a Huffman code gives the frequent ones short codes: e, t,\n"
                                    "a, o and the space take four or five bits, rare ones up to ten.\n"
                                    "Every table is fixed, so a page carries none of its own.\n";
    unsigned int i;

    memset(p, 0, SEQLZ_BLOCK_SIZE);
    memcpy(p, "TASKSLAB", 8);
    le(p + 8, 48, 4);
    le(p + 12, 32, 4);
    for (i = 0; i < 48; i++) {
        u8* r = p + 64 + 32 * i;

        le(r, 0xffff888104a3c040ULL + 32 * ((i * 5 + 1) % 48), 8);
        le(r + 8, 1000 + 3 * i, 4);
        le(r + 12, i % 3, 2);
        le(r + 14, 120, 2);
        strcpy((char*)r + 16, names[i % 6]);
    }
    memcpy(p + 64 + 32 * 48, text, strlen(text));
}

/* chapter 9's page: 512 pointers of 8 bytes */
static inline void pointer_page(u8* p) {
    unsigned int i;

    for (i = 0; i < 512; i++)
        le(p + 8 * i, 0x7f1234560010ULL + 16ULL * i, 8);
}

/* chapter 4's page: ab 2048 times */
static inline void ab_page(u8* p) {
    unsigned int i;

    for (i = 0; i < SEQLZ_BLOCK_SIZE; i++)
        p[i] = i & 1 ? 'b' : 'a';
}

/* chapter 2's page: two records of a cat and a dog, then zeros */
static inline void cat_page(u8* p) {
    memset(p, 0, SEQLZ_BLOCK_SIZE);
    memcpy(p, "cat=1,dog=2;cat=3,dog=4;", 24);
}

/* chapter 4's second page: three matches of three kinds of offset, then z to the end */
static inline void second_page(u8* p) {
    memcpy(p, "abcdefghijklllllxycdefghi", 25);
    memset(p + 25, 'z', SEQLZ_BLOCK_SIZE - 25);
}

/*
 * chapter 11's page: 200 letters of "etaoinshrdlu " drawn by the C library's classic linear
 * congruential generator from seed 1, then zeros
 */
static inline void letters_page(u8* p) {
    unsigned int k, seed = 1;

    memset(p, 0, SEQLZ_BLOCK_SIZE);
    for (k = 0; k < 200; k++) {
        seed = seed * 1103515245U + 12345U;
        p[k] = (u8) "etaoinshrdlu "[(seed >> 16) % 13];
    }
}

/*
 * chapter 4's text page: "words" of 2 to 9 letters of etaoinshrdlu, each followed by a space, drawn
 * by the C library's classic linear congruential generator from seed 1, as letters_page()
 */
static inline void words_page(u8* p) {
    unsigned int at = 0, seed = 1, k, n;

    while (at < SEQLZ_BLOCK_SIZE) {
        seed = seed * 1103515245U + 12345U;
        n = 2 + (seed >> 16) % 8;
        for (k = 0; k < n && at < SEQLZ_BLOCK_SIZE; k++) {
            seed = seed * 1103515245U + 12345U;
            p[at++] = (u8) "etaoinshrdlu"[(seed >> 16) % 12];
        }
        if (at < SEQLZ_BLOCK_SIZE)
            p[at++] = ' ';
    }
}

/* "name":[n bytes as numbers], for the JSON the programs print */
static inline void array(const char* name, const u8* p, unsigned int n) {
    unsigned int i;

    printf("\"%s\":[", name);
    for (i = 0; i < n; i++)
        printf("%s%u", i ? "," : "", p[i]);
    printf("]");
}

#endif
