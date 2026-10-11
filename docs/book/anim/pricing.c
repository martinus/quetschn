// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * pricing.c: the data of the animation "pricing" in chapter 11. Compresses chapter 16's page with raw
 * literals, prices its literals with the encoder's own lit_cost(), and prints as JSON: the literals,
 * the code lengths in all 8 tables of each byte value they use (from t->lit_cost), each stream's bits
 * in each table (lit_bits()), and the table seqlz_compress() picked for the page.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "seqlz_compress.c"

static void le(u8 *p, unsigned long long v, unsigned int n)
{
	while (n--) {
		*p++ = (u8)v;
		v >>= 8;
	}
}

/* chapter 16's page, a copy of slab_page() in stepper/trace.c */
static void slab_page(u8 *p)
{
	static const char *const names[] = { "kworker/0:1", "bash", "systemd",
					     "sshd", "kworker/1:2", "cron" };
	static const char *const text =
		"zram stores each page in memory, compressed on its own. A page\n"
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
		u8 *r = p + 64 + 32 * i;

		le(r, 0xffff888104a3c040ULL + 32 * ((i * 5 + 1) % 48), 8);
		le(r + 8, 1000 + 3 * i, 4);
		le(r + 12, i % 3, 2);
		le(r + 14, 120, 2);
		strcpy((char *)r + 16, names[i % 6]);
	}
	memcpy(p + 64 + 32 * 48, text, strlen(text));
}

int main(void)
{
	static u8 page[SEQLZ_PAGE], raw[2 * SEQLZ_PAGE], comp[2 * SEQLZ_PAGE];
	static struct seqlz_state st;
	struct seqlz_tables *t = malloc(seqlz_tables_size());
	u64 even[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { 0 };
	u64 odd[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = { 0 };
	unsigned int n_lit, j, k, used[256] = { 0 }, first = 1;
	const u8 *lit;

	if (!t || seqlz_tables_init(t, &seqlz_default_own))
		return 1;
	slab_page(page);
	if (!seqlz_compress(t, &st, page, raw, sizeof(raw), 0) ||
	    !seqlz_compress(t, &st, page, comp, sizeof(comp), 1))
		return 1;
	n_lit = get_unaligned_le16(raw);
	lit = raw + SEQLZ_HEADER;
	lit_cost(t, lit, n_lit, even, odd);
	/* each table's lane must be the sum of the code lengths of the stream's literals */
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
		for (k = 0; k < SEQLZ_LIT_SETS; k++) {
			unsigned int i, b = 0;

			for (i = j; i < n_lit; i += SEQLZ_LIT_STREAMS)
				b += t->lit[k].enc[lit[i]] & 63U;
			if (b != lit_bits(even, odd, j, k)) {
				fprintf(stderr, "pricing.c: lit_cost() disagrees with the tables\n");
				return 1;
			}
		}
	/* byte k of lit_cost[b] is b's code length in table k */
	for (j = 0; j < 256; j++)
		for (k = 0; k < SEQLZ_LIT_SETS; k++)
			if ((t->lit_cost[j][0] >> (8 * k) & 255U) != (t->lit[k].enc[j] & 63U))
				return 1;
	printf("{\"set\":%u,\"run\":%u,\"lit\":[", comp[2] & (SEQLZ_LIT_SETS - 1U), LIT_COST_RUN);
	for (k = 0; k < n_lit; k++) {
		printf("%s%u", k ? "," : "", lit[k]);
		used[lit[k]] = 1;
	}
	printf("],\"cost\":{");
	for (k = 0; k < 256; k++) {
		if (!used[k])
			continue;
		printf("%s\"%u\":[", first ? "" : ",", k);
		first = 0;
		for (j = 0; j < SEQLZ_LIT_SETS; j++)
			printf("%s%u", j ? "," : "", (unsigned int)(t->lit_cost[k][0] >> (8 * j) & 255U));
		printf("]");
	}
	printf("},\"bits\":[");
	for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
		printf("%s[", j ? "," : "");
		for (k = 0; k < SEQLZ_LIT_SETS; k++)
			printf("%s%u", k ? "," : "", lit_bits(even, odd, j, k));
		printf("]");
	}
	printf("]}\n");
	free(t);
	return 0;
}
