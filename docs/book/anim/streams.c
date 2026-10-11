// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * streams.c: the data of the animation "streams" in chapter 13. Compresses chapter 16's page with
 * coded literals and prints, as JSON, its literals in page order with the code length each has in the
 * page's literal table, the stream sizes of the header, and what decode_literals() writes after the
 * literals in its last step. Fails if anything disagrees with seqlz_decode().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz_internal.h"

static void le(u8* p, unsigned long long v, unsigned int n) {
    while (n--) {
        *p++ = (u8)v;
        v >>= 8;
    }
}

/* chapter 16's page, a copy of slab_page() in stepper/trace.c */
static void slab_page(u8* p) {
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

    memset(p, 0, SEQLZ_PAGE);
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

static int fail(const char* what) {
    fprintf(stderr, "streams.c: %s\n", what);
    return 1;
}

int main(void) {
    static u8 page[SEQLZ_PAGE], raw[2 * SEQLZ_PAGE], comp[2 * SEQLZ_PAGE], out[SEQLZ_PAGE];
    static u8 scratch[SEQLZ_SCRATCH];
    static struct seqlz_state st;
    struct seqlz_tables* t = malloc(seqlz_tables_size());
    unsigned int n, n_raw, n_lit, set, width, i, k, bits[SEQLZ_LIT_STREAMS] = {0};
    unsigned int sz[SEQLZ_LIT_STREAMS];
    unsigned long long h = 0;

    if (!t || seqlz_tables_init(t, &seqlz_default_own))
        return fail("tables");
    slab_page(page);
    n_raw = seqlz_compress(t, &st, page, raw, sizeof(raw), 0);
    n = seqlz_compress(t, &st, page, comp, sizeof(comp), 1);
    if (!n || !n_raw)
        return fail("compress");
    n_lit = get_unaligned_le16(comp);
    if (!(n_lit & SEQLZ_LIT_CODED))
        return fail("the page's literals are not coded");
    n_lit &= SEQLZ_LIT_CODED - 1U;
    if (n_lit != get_unaligned_le16(raw))
        return fail("raw and coded pages differ in literals");
    /* byte 2: the table in bits 0 to 2, width - 5 in bits 3 to 5 */
    set = comp[2] & (SEQLZ_LIT_SETS - 1U);
    width = SEQLZ_SIZE_BITS_MIN + ((comp[2] >> SEQLZ_LIT_WIDTH_AT) & 7U);
    for (i = 0; i < width; i++)
        h |= (unsigned long long)comp[SEQLZ_LIT_HEADER(0) + i] << (8 * i);
    for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
        sz[i] = (unsigned int)(h >> (i * width)) & ((1U << width) - 1U);
    memset(scratch, 0xaa, sizeof(scratch));
    if (seqlz_decode(t, comp, n, out, scratch) || memcmp(out, page, SEQLZ_PAGE))
        return fail("the page does not decode to itself");
    /* the decoder's literals are the encoder's raw literals */
    if (memcmp(scratch, raw + SEQLZ_HEADER, n_lit))
        return fail("decoded literals differ from the raw ones");
    /* literal k in stream k % 8: each stream's code lengths fill exactly its size */
    for (k = 0; k < n_lit; k++)
        bits[k % SEQLZ_LIT_STREAMS] += t->lit[set].enc[scratch[k]] & 63U;
    for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
        if ((bits[i] + 7U) / 8U != sz[i])
            return fail("a stream's codes do not fill its size");
    /* the decode table gives the same length as the encoder's entry */
    for (k = 0; k < 256; k++) {
        u32 e = t->lit[set].enc[k];
        u32 code = e >> 8, len = e & 63U;
        u16 d = t->lit[set].decode[code << (SEQLZ_LIT_BITS - len)];

        if ((d & 63U) != len || d >> 8 != k)
            return fail("decode and enc disagree");
    }
    printf("{\"set\":%u,\"width\":%u,\"comp\":%u,\"raw\":%u,\"rounds\":%u,\"lit\":[", set, width, n, n_raw, SEQLZ_LIT_ROUNDS);
    for (k = 0; k < n_lit; k++)
        printf("%s%u", k ? "," : "", scratch[k]);
    printf("],\"len\":[");
    for (k = 0; k < n_lit; k++)
        printf("%s%u", k ? "," : "", t->lit[set].enc[scratch[k]] & 63U);
    printf("],\"sz\":[");
    for (i = 0; i < SEQLZ_LIT_STREAMS; i++)
        printf("%s%u", i ? "," : "", sz[i]);
    /* the last step decodes into out[k + i] for all 8 streams, k = n_lit - r */
    printf("],\"spare\":[");
    for (k = n_lit; k < n_lit - n_lit % 40U % 8U + 8U; k++)
        printf("%s%u", k > n_lit ? "," : "", scratch[k]);
    printf("]}\n");
    free(t);
    return 0;
}
