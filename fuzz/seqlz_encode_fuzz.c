// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * Any list of sequences through seqlz_encode(), as they come, also ones that are not a page: matches
 * shorter than 4, offset 0 or past the position, lengths that do not add up. seqlz_encode() must not
 * write past the two pages of dst, and a page it accepts must decode to the page its sequences make.
 * The other targets only reach seqlz_encode() with the matcher's sequences, which are always valid.
 *
 * Byte 0: bit 0 lets the last sequence's literals fill the page, bits 1 and 2 pick the level, 1 plus
 * their value mod 3, bit 3 gives seqlz_encode() the page, where the sequences make one, for level 3's
 * deltas, and bit 4 makes the literals grow by one every 4, which deltas code well. Then 4 bytes per
 * sequence: the literals, the match, and two bytes for the offset, each taken as it is.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz.h"
#include "unwritten.h"

#define DST_CAP (2U * SEQLZ_BLOCK_SIZE)

static int tables_built;
/* the work memory of level 3 */
static struct seqlz_state state;
static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
static unsigned char literals[SEQLZ_BLOCK_SIZE], page[SEQLZ_BLOCK_SIZE];
/* each an allocation of its own exact size, for ASan */
static unsigned char *dst, *out, *scratch;

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

/* the page the n sequences make from literals, if they make one */
static int make_page(unsigned int n) {
    unsigned int k, i, pos = 0, used = 0;

    for (k = 0; k < n; k++) {
        if (pos + seq[k].literals > SEQLZ_BLOCK_SIZE)
            return 0;
        for (i = 0; i < seq[k].literals; i++)
            page[pos++] = literals[used++];
        if (k + 1 == n)
            break;
        if (seq[k].match < 4 || seq[k].offset == 0 || seq[k].offset > pos || pos + seq[k].match > SEQLZ_BLOCK_SIZE)
            return 0;
        for (i = 0; i < seq[k].match; i++, pos++)
            page[pos] = page[pos - seq[k].offset];
    }
    return pos == SEQLZ_BLOCK_SIZE;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    unsigned int n = 0, k, n_lit = 0, sum = 0, len;
    int made;

    if (size < 5)
        return 0;
    if (!tables_built) {
        tables_built = 1;
        dst = malloc(DST_CAP);
        out = malloc(SEQLZ_BLOCK_SIZE);
        scratch = malloc(SEQLZ_SCRATCH);
        if (!dst || !out || !scratch || seqlz_tables_init(&seqlz_default_lengths) || !seqlz_all_symbols())
            abort();
    }
    for (k = 1; k + 4 <= size && n < SEQLZ_MAX_SEQUENCES; k += 4, n++) {
        seq[n].literals = data[k];
        seq[n].match = data[k + 1];
        seq[n].offset =
            (unsigned short)(((unsigned int)data[k + 2] | (unsigned int)data[k + 3] << 8) % (SEQLZ_BLOCK_SIZE + 4U));
        sum += seq[n].literals + seq[n].match;
    }
    sum -= seq[n - 1].match;
    if ((data[0] & 1) && sum - seq[n - 1].literals <= SEQLZ_BLOCK_SIZE) {
        sum -= seq[n - 1].literals;
        seq[n - 1].literals = (unsigned short)(SEQLZ_BLOCK_SIZE - sum);
        sum = SEQLZ_BLOCK_SIZE;
    }
    for (k = 0; k < n; k++)
        n_lit += seq[k].literals;
    if (n_lit > SEQLZ_BLOCK_SIZE)
        n_lit = SEQLZ_BLOCK_SIZE;
    for (k = 0; k < n_lit; k++)
        literals[k] = (unsigned char)(data[0] & 16 ? k / 4 : k * 0x9e3779b1U >> 24);
    made = make_page(n);

    unwritten(dst, DST_CAP);
    unwritten(scratch, SEQLZ_SCRATCH);
    len = seqlz_encode(
        &state, seq, n, literals, n_lit, made && (data[0] & 8) ? page : NULL, dst, DST_CAP, 1U + (data[0] >> 1 & 3U) % 3U);
    if (len == 0)
        return 0;
    if (len > DST_CAP || sum != SEQLZ_BLOCK_SIZE)
        abort();
    /* accepted, so the sequences make a page: compare */
    if (!made)
        abort();
    memset(out, 0xa5, SEQLZ_BLOCK_SIZE);
    unwritten(out, SEQLZ_BLOCK_SIZE);
    if (seqlz_decode(dst, len, out, scratch) != 0 || memcmp(out, page, SEQLZ_BLOCK_SIZE) != 0)
        abort();
    return 0;
}
