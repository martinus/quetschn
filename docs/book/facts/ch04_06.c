// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * ch04_06.c: the facts of chapter 4, the format, and chapter 6, seqlz.h. ch04.* are the made-up text
 * page with coded literals, the walk through the token table's canonical codes, the fields of the two
 * example pages as the spec reads them, offset classes and the SHA-256 of the 4 KiB tables. ch06.* are
 * the header's constants and the worked examples of its three functions. Built for 16 KiB pages too,
 * as k16.*.
 */
// facts: also 16k
#include "facts.h"

/* ---- SHA-256, for the tables' hashes that docs/format.md lists ---- */

static unsigned int ror(unsigned int x, unsigned int n) {
    return x >> n | x << (32 - n);
}

static void sha256(const unsigned char* p, unsigned long n, char* hex) {
    static const unsigned int k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    unsigned int h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned long total = (n + 9 + 63) / 64 * 64, i;
    unsigned int w[64], v[8], j;

    for (i = 0; i < total; i += 64) {
        for (j = 0; j < 64; j++) {
            unsigned long at = i + j;
            unsigned int byte = at < n            ? p[at]
                                : at == n         ? 0x80
                                : at >= total - 8 ? (unsigned int)(8 * n >> 8 * (total - 1 - at)) & 255
                                                  : 0;

            if (j % 4 == 0)
                w[j / 4] = 0;
            w[j / 4] |= byte << (24 - 8 * (j % 4));
        }
        for (j = 16; j < 64; j++)
            w[j] = w[j - 16] + (ror(w[j - 15], 7) ^ ror(w[j - 15], 18) ^ w[j - 15] >> 3) + w[j - 7] +
                   (ror(w[j - 2], 17) ^ ror(w[j - 2], 19) ^ w[j - 2] >> 10);
        memcpy(v, h, sizeof v);
        for (j = 0; j < 64; j++) {
            unsigned int t1 =
                v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[j] + w[j];
            unsigned int t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));

            memmove(v + 1, v, 7 * sizeof v[0]);
            v[4] += t1;
            v[0] = t1 + t2;
        }
        for (j = 0; j < 8; j++)
            h[j] += v[j];
    }
    for (j = 0; j < 8; j++)
        sprintf(hex + 8 * j, "%08x", h[j]);
}

/* ---- helpers ---- */

static char key_buf[128];

/* the key "prefix.suffix" */
static const char* K(const char* prefix, const char* suffix) {
    snprintf(key_buf, sizeof key_buf, "%s.%s", prefix, suffix);
    return key_buf;
}

/* v as n bits, the highest first */
static const char* bin(unsigned int v, unsigned int n) {
    static char buf[4][33];
    static unsigned int which;
    char* b = buf[which++ % 4];
    unsigned int k;

    for (k = 0; k < n; k++)
        b[k] = (char)('0' + ((v >> (n - 1 - k)) & 1));
    b[n] = 0;
    return b;
}

static void fact_list(const char* key, const unsigned int* v, unsigned int n) {
    unsigned int k;

    fact_key(key);
    putchar('[');
    for (k = 0; k < n; k++)
        printf("%s%u", k ? "," : "", v[k]);
    putchar(']');
}

/* the canonical codes of the three sequence tables, as format.md derives them from the lengths */
static char tok_code[SEQLZ_TOKEN_SYMBOLS + 1][17], ll_code[SEQLZ_LEN_SYMBOLS][17], ml_code[SEQLZ_LEN_SYMBOLS][17];

static void codes(void) {
    fact_canonical(seqlz_default_own.token, SEQLZ_TOKEN_SYMBOLS + 1, tok_code);
    fact_canonical(seqlz_default_own.ll, SEQLZ_LEN_SYMBOLS, ll_code);
    fact_canonical(seqlz_default_own.ml, SEQLZ_LEN_SYMBOLS, ml_code);
}

/* ---- chapter 4 ---- */

static void text_facts(void) {
    static u8 page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE];
    const char* P = "ch04.text";
    unsigned int raw, len, h, n, t, set, w, j, sum = 0, s[SEQLZ_LIT_STREAMS], best = 0, cost[SEQLZ_LIT_SETS] = {0};
    char buf[64], *b;

    words_page(page);
    raw = fact_compress(page, dst, 0);
    len = fact_compress(page, dst, 1);
    h = get_unaligned_le16(dst);
    n = h & (SEQLZ_LIT_CODED - 1U);
    t = dst[2];
    set = t & SEQLZ_LIT_CODE_MASK;
    w = SEQLZ_SIZE_BITS_MIN + ((t >> SEQLZ_LIT_WIDTH_AT) & 7);
    if (!(h & SEQLZ_LIT_CODED))
        return; /* chapter 4 quotes it for 4 KiB pages, where it is coded */
    /* s[j] = (S >> (j * w)) & ((1 << w) - 1), S the w bytes from byte 3 on, little endian */
    for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
        unsigned int k;

        for (s[j] = 0, k = 0; k < w; k++)
            s[j] |= (unsigned int)((dst[3 + (j * w + k) / 8] >> ((j * w + k) % 8)) & 1) << k;
        sum += s[j];
    }
    /* the literal table that codes them in the fewest bits */
    {
        static u8 lits[SEQLZ_PAGE];
        static struct seqlz_state st;
        static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
        unsigned int ns = seqlz_find(&st, page, seq, 2), i, at = 0, m = 0, k;

        for (i = 0; i < ns; i++) {
            memcpy(lits + m, page + at, seq[i].literals);
            m += seq[i].literals;
            at += seq[i].literals + seq[i].match;
        }
        for (k = 0; k < SEQLZ_LIT_SETS; k++) {
            for (i = 0; i < m; i++)
                cost[k] += seqlz_lit_sets[k][lits[i]];
            if (cost[k] < cost[best])
                best = k;
        }
    }
    fact_num(K(P, "raw"), raw);
    fact_num(K(P, "len"), len);
    fact_hex(K(P, "head"), dst, 3 + w);
    fact_hex(K(P, "hdr"), dst, 2);
    fact_hex(K(P, "t_hex"), dst + 2, 1);
    fact_hex(K(P, "sizes_hex"), dst + 3, w);
    snprintf(buf, sizeof buf, "0x%04x", h);
    fact_str(K(P, "h"), buf);
    snprintf(buf, sizeof buf, "0x%04x", n);
    fact_str(K(P, "n_hex"), buf);
    fact_num(K(P, "n"), n);
    snprintf(buf, sizeof buf, n % 8 ? "%u or %u" : "%u", n / 8, n / 8 + 1);
    fact_str(K(P, "per_stream"), buf);
    snprintf(buf, sizeof buf, "0x%02x", t);
    fact_str(K(P, "t"), buf);
    fact_str(K(P, "t_bin"), bin(t, 8));
    for (j = 0; j < 8; j++) {
        snprintf(buf, sizeof buf, "t_bit.%u", j);
        fact_num(K(P, buf), (t >> j) & 1);
    }
    fact_num(K(P, "set"), set);
    fact_num(K(P, "best"), best);
    fact_num(K(P, "w"), w);
    fact_num(K(P, "w5"), w - 5);
    b = buf + sprintf(buf, "0x");
    for (j = w; j--;)
        b += sprintf(b, "%02x", dst[3 + j]);
    fact_str(K(P, "S"), buf);
    snprintf(buf, sizeof buf, "0x%x", s[0]);
    fact_str(K(P, "s0_hex"), buf);
    fact_list(K(P, "s"), s, SEQLZ_LIT_STREAMS);
    fact_num(K(P, "sum"), sum);
    fact_num(K(P, "avg"), (sum + 4) / 8);
    fact_num(K(P, "streams_at"), 3 + w);
    fact_num(K(P, "b_at"), 3 + w + sum);
    fact_num(K(P, "b_bytes"), len - (3 + w + sum));
    snprintf(buf, sizeof buf, "%.1f", 8.0 * sum / n);
    fact_str(K(P, "bits_per_lit"), buf);
}

/* the walk through the token table's codes: first[l] and the symbols with codes of l bits */
static void walk_facts(void) {
    unsigned int count[17] = {0}, first[17] = {0}, syms[16], c = 0, s, l, m, coded = 0;
    char key[64];

    for (s = 0; s <= SEQLZ_TOKEN_SYMBOLS; s++)
        count[seqlz_default_own.token[s]]++;
    for (s = 0; s < SEQLZ_TOKEN_SYMBOLS; s++)
        coded += seqlz_default_own.token[s] != 0;
    count[0] = 0;
    for (l = 1; l <= 11; l++) {
        c = (c + count[l - 1]) << 1;
        first[l] = c;
    }
    for (l = 4; l <= 6; l++) {
        snprintf(key, sizeof key, "ch04.first.%u", l);
        fact_num(key, first[l]);
        snprintf(key, sizeof key, "ch04.first_bin.%u", l);
        fact_str(key, bin(first[l], l));
        for (s = m = 0; s <= SEQLZ_TOKEN_SYMBOLS && m < 16; s++)
            if (seqlz_default_own.token[s] == l)
                syms[m++] = s;
        snprintf(key, sizeof key, "ch04.syms.%u", l);
        fact_list(key, syms, m);
    }
    for (l = 4; l <= 4; l++)
        for (s = m = 0; s <= SEQLZ_TOKEN_SYMBOLS; s++)
            if (seqlz_default_own.token[s] == l) {
                snprintf(key, sizeof key, "ch04.short.%u.ll", m);
                fact_num(key, s & 15);
                snprintf(key, sizeof key, "ch04.short.%u.ml", m);
                fact_num(key, ((s >> 4) & 31) + 4);
                snprintf(key, sizeof key, "ch04.short.%u.cls", m++);
                fact_num(key, s >> 9);
            }
    fact_num("ch04.tok_coded", coded);
    fact_num("ch04.esc_bits", seqlz_default_own.token[SEQLZ_ESCAPE] + SEQLZ_ESCAPE_BITS);
    fact_num("ch04.lens_unit", 1U << SEQLZ_TOKEN_BITS);
}

/* the bits of a length value, as value(table) reads them */
static void value_facts(const char* P, const char* name, char (*code)[17], unsigned int v, char* all) {
    unsigned int extra, sym = seqlz_len_symbol(v, &extra);
    char key[64];

    snprintf(key, sizeof key, "%ssym", name);
    fact_num(K(P, key), sym);
    snprintf(key, sizeof key, "%scode", name);
    fact_str(K(P, key), code[sym]);
    strcat(all, code[sym]);
    snprintf(key, sizeof key, "%sb", name);
    fact_num(K(P, key), extra);
    if (extra) {
        snprintf(key, sizeof key, "%sbits", name);
        fact_str(K(P, key), bin(v - (1U << extra), extra));
        strcat(all, bin(v - (1U << extra), extra));
        snprintf(key, sizeof key, "%sread", name);
        fact_num(K(P, key), v - (1U << extra));
    }
    snprintf(key, sizeof key, "%sval", name);
    fact_num(K(P, key), v);
}

/*
 * The sequences of an example page, field by field as the spec reads them: ch04.<page>.s.<i>.tok, .code,
 * .llf, .mlf, .cls, .offbits, .offread, .off, the length values' .mlsym, .mlcode, .mlb, .mlbits,
 * .mlread, .mlval (and .ll... the same), .ll, .ml, and .bits, all bits of the sequence. The bits are checked against the
 * page's bitstream.
 */
static void page_walk(const char* name, void (*make)(u8*)) {
    static u8 page[SEQLZ_PAGE], dst[2 * SEQLZ_PAGE];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    static char all[8 * 2 * SEQLZ_PAGE], want[8 * 2 * SEQLZ_PAGE];
    unsigned int len, n, i, last = 1, n_lit, nb, k;
    char P[64], key[64];

    make(page);
    len = fact_compress(page, dst, 0);
    n_lit = get_unaligned_le16(dst);
    n = seqlz_find(&st, page, seq, 2);
    all[0] = 0;
    snprintf(P, sizeof P, "ch04.%s", name);
    fact_hex(K(P, "hdr"), dst, SEQLZ_HEADER);
    fact_hex(K(P, "lithex"), dst + SEQLZ_HEADER, n_lit);
    if (n_lit < sizeof key) {
        memcpy(key, dst + SEQLZ_HEADER, n_lit);
        key[n_lit] = 0;
        fact_str(K(P, "litstr"), key);
    }
    if (len <= 64) {
        /* the page for printf, as "\x02\x00" */
        static char esc[4 * 64 + 1];

        for (k = 0; k < len; k++)
            sprintf(esc + 4 * k, "\\x%02x", dst[k]);
        fact_str(K(P, "printf"), esc);
    }
    for (i = 0; i + 1 < n; i++) {
        unsigned int raw, cls = seqlz_off_class(seq[i].offset, last, &raw);
        unsigned int tok = seqlz_token(seq[i].literals, seq[i].match, cls);
        char Q[96];
        size_t before = strlen(all);

        snprintf(Q, sizeof Q, "%s.s.%u", P, i);
        fact_num(K(Q, "tok"), tok);
        /* .code is the token's code, or the escape's code and the token in 12 bits */
        fact_num(K(Q, "escaped"), !seqlz_default_own.token[tok]);
        if (seqlz_default_own.token[tok]) {
            fact_str(K(Q, "code"), tok_code[tok]);
            strcat(all, tok_code[tok]);
        } else {
            fact_str(K(Q, "esccode"), tok_code[SEQLZ_ESCAPE]);
            fact_str(K(Q, "tokbits"), bin(tok, SEQLZ_ESCAPE_BITS));
            snprintf(key, sizeof key, "%s%s", tok_code[SEQLZ_ESCAPE], bin(tok, SEQLZ_ESCAPE_BITS));
            fact_str(K(Q, "code"), key);
            strcat(all, key);
        }
        fact_num(K(Q, "llf"), tok & 15);
        fact_num(K(Q, "mlf"), (tok >> 4) & 31);
        fact_num(K(Q, "cls"), cls);
        fact_num(K(Q, "raw"), raw);
        if (raw) {
            unsigned int sent = seq[i].offset >> SEQLZ_OFF_SHIFT(cls);

            fact_str(K(Q, "offbits"), bin(sent, raw));
            strcat(all, bin(sent, raw));
            fact_num(K(Q, "offread"), sent);
        }
        fact_num(K(Q, "off"), seq[i].offset);
        if (seq[i].literals >= SEQLZ_LL_CAP)
            value_facts(Q, "ll", ll_code, seq[i].literals - SEQLZ_LL_CAP, all);
        if (seq[i].match - 4U >= SEQLZ_ML_CAP)
            value_facts(Q, "ml", ml_code, seq[i].match - 4U - SEQLZ_ML_CAP, all);
        fact_num(K(Q, "ll"), seq[i].literals);
        fact_num(K(Q, "ml"), seq[i].match);
        fact_num(K(Q, "bits"), (long long)(strlen(all) - before));
        last = seq[i].offset;
    }
    nb = len - SEQLZ_HEADER - n_lit;
    fact_num(K(P, "bbits"), 8 * nb);
    for (k = 0; k < 8 * nb; k++)
        want[k] = (char)('0' + ((dst[SEQLZ_HEADER + n_lit + k / 8] >> (7 - k % 8)) & 1));
    want[8 * nb] = 0;
    k = (unsigned int)strlen(all);
    if (strncmp(all, want, k) || k + 8 <= 8 * nb || strchr(want + k, '1')) {
        fprintf(stderr, "ch04_06.c: the fields of %s are not its bitstream\n", name);
        exit(1);
    }
    fact_str(K(P, "padbits"), want + k);
}

/* the pointer page of chapter 9: which tokens its sequences have */
static void pointer_facts(void) {
    static u8 page[SEQLZ_PAGE];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    unsigned int n, i, last = 1, raw, t49 = 0, t34 = 0;

    pointer_page(page);
    n = seqlz_find(&st, page, seq, 2);
    for (i = 0; i + 1 < n; i++) {
        unsigned int tok = seqlz_token(seq[i].literals, seq[i].match, seqlz_off_class(seq[i].offset, last, &raw));

        t49 += tok == 49;
        t34 += tok == 34;
        last = seq[i].offset;
    }
    fact_num("ch04.ptr.tokens", n - 1);
    fact_num("ch04.ptr.t49", t49);
    fact_num("ch04.ptr.t34", t34);
}

/* the offset classes of a few offsets, as seqlz_off_class() picks them */
static void offset_facts(void) {
    static const unsigned int ex[][2] = {{24, 24}, {24, 16}, {2, 1}, {8, 1}, {100, 1}, {256, 1}, {300, 1}, {2048, 1}};
    unsigned int k, raw, cls;
    char P[64];

    for (k = 0; k < sizeof ex / sizeof ex[0]; k++) {
        cls = seqlz_off_class(ex[k][0], ex[k][1], &raw);
        snprintf(P, sizeof P, "ch04.oc.%u_%u", ex[k][0], ex[k][1]);
        fact_num(K(P, "cls"), cls);
        fact_num(K(P, "raw"), raw);
        fact_num(K(P, "read"), ex[k][0] >> SEQLZ_OFF_SHIFT(cls));
    }
    for (k = 0; k < SEQLZ_OFF_CLASSES; k++) {
        snprintf(P, sizeof P, "ch04.raw.%u", k);
        fact_num(P, SEQLZ_RAW_BITS(k));
    }
}

static void sha_facts(void) {
    static unsigned char lit[SEQLZ_LIT_SETS * 256];
    char hex[65];
    unsigned int k;

    sha256(seqlz_default_own.token, sizeof seqlz_default_own.token, hex);
    fact_str("ch04.sha.TOK", hex);
    sha256(seqlz_default_own.ll, sizeof seqlz_default_own.ll, hex);
    fact_str("ch04.sha.LL", hex);
    sha256(seqlz_default_own.ml, sizeof seqlz_default_own.ml, hex);
    fact_str("ch04.sha.ML", hex);
    for (k = 0; k < SEQLZ_LIT_SETS; k++)
        memcpy(lit + 256 * k, seqlz_lit_sets[k], 256);
    sha256(lit, sizeof lit, hex);
    fact_str("ch04.sha.LIT", hex);
    sha256(seqlz_own_lengths, sizeof seqlz_own_lengths, hex);
    fact_str("ch04.sha.OWN", hex);
}

/* the longest code of each kind of table */
static void longest_facts(void) {
    unsigned int s, k, tok = 0, ll = 0, ml = 0, lit = 0;

    for (s = 0; s <= SEQLZ_TOKEN_SYMBOLS; s++)
        tok = seqlz_default_own.token[s] > tok ? seqlz_default_own.token[s] : tok;
    for (s = 0; s < SEQLZ_LEN_SYMBOLS; s++) {
        ll = seqlz_default_own.ll[s] > ll ? seqlz_default_own.ll[s] : ll;
        ml = seqlz_default_own.ml[s] > ml ? seqlz_default_own.ml[s] : ml;
    }
    for (k = 0; k < SEQLZ_LIT_SETS; k++)
        for (s = 0; s < 256; s++)
            lit = seqlz_lit_sets[k][s] > lit ? seqlz_lit_sets[k][s] : lit;
    fact_num("ch04.longest.tok", tok);
    fact_num("ch04.longest.ll", ll);
    fact_num("ch04.longest.ml", ml);
    fact_num("ch04.longest.lit", lit);
}

/* ---- chapter 6 ---- */

/* the constants of seqlz.h for the page size it is built for */
static void header_facts(void) {
    const char* P = "ch06";
    unsigned int extra;
    char buf[64];

    fact_num(K(P, "PAGE"), SEQLZ_PAGE);
    fact_num(K(P, "MAX_ESCAPE_LEN"), SEQLZ_MAX_ESCAPE_LEN);
    fact_num(K(P, "LEN_SYMBOLS"), SEQLZ_LEN_SYMBOLS);
    snprintf(buf, sizeof buf, "0x%X", SEQLZ_RAW_NIBBLES);
    fact_str(K(P, "RAW_NIBBLES"), buf);
    fact_num(K(P, "RAW_NIBBLES_dec"), SEQLZ_RAW_NIBBLES);
    snprintf(buf,
             sizeof buf,
             "%u, %u, %u, %u, %u, %u",
             SEQLZ_RAW_BITS(0),
             SEQLZ_RAW_BITS(1),
             SEQLZ_RAW_BITS(2),
             SEQLZ_RAW_BITS(3),
             SEQLZ_RAW_BITS(4),
             SEQLZ_RAW_BITS(5));
    fact_str(K(P, "RAW_BITS"), buf);
    fact_num(K(P, "RAW_BITS3"), SEQLZ_RAW_BITS(3));
    fact_num(K(P, "RAW_BITS5"), SEQLZ_RAW_BITS(5));
    fact_num(K(P, "CLASS5_BITS"), SEQLZ_CLASS5_BITS);
    fact_num(K(P, "class5_end"), 8U << SEQLZ_CLASS5_BITS);
    fact_num(K(P, "SCRATCH"), SEQLZ_SCRATCH);
    fact_num(K(P, "HASH_BITS"), SEQLZ_HASH_BITS);
    fact_num(K(P, "HASH_ENTRIES"), 1U << SEQLZ_HASH_BITS);
    fact_num(K(P, "MAX_SEQUENCES"), SEQLZ_MAX_SEQUENCES);
    fact_num(K(P, "lengths"), sizeof(struct seqlz_lengths));
    fact_num(K(P, "state"), sizeof(struct seqlz_state));
    /* the largest length values: ll before a match of 4, a match after 1 literal */
    fact_num(K(P, "max_ll_value"), SEQLZ_PAGE - 4U - SEQLZ_LL_CAP);
    fact_num(K(P, "max_ml_value"), SEQLZ_PAGE - 1U - 4U - SEQLZ_ML_CAP);
    fact_num(K(P, "max_ll_sym"), seqlz_len_symbol(SEQLZ_PAGE - 4U - SEQLZ_LL_CAP, &extra));
    fact_num(K(P, "max_ml_sym"), seqlz_len_symbol(SEQLZ_PAGE - 1U - 4U - SEQLZ_ML_CAP, &extra));
    fact_num(K(P, "last_sym"), SEQLZ_LEN_SYMBOLS - 1U);
}

static void ch06_facts(void) {
    static const unsigned int lens[] = {5, 16, 31, 100, 4059};
    static const unsigned int offs[] = {1, 2, 8, 15, 24, 255, 256, 264, 2040, 2048, 4095};
    static const unsigned int toks[][3] = {{2, 7, 4}, {1, 7, 0}, {2, 4094, 1}, {20, 100, 3}, {0, 4, 0}};
    unsigned int k, extra, raw, sym, cls, aligned;
    char P[64];

    header_facts();
    fact_num("ch06.MAX_BITS", SEQLZ_MAX_BITS);
    fact_num("ch06.TOKEN_BITS", SEQLZ_TOKEN_BITS);
    fact_num("ch06.OFF_CLASSES", SEQLZ_OFF_CLASSES);
    fact_num("ch06.ESCAPE", SEQLZ_ESCAPE);
    fact_num("ch06.ESCAPE_BITS", SEQLZ_ESCAPE_BITS);
    fact_num("ch06.LL_CAP", SEQLZ_LL_CAP);
    fact_num("ch06.ML_CAP", SEQLZ_ML_CAP);
    fact_num("ch06.LEN_DIRECT", SEQLZ_LEN_DIRECT);
    fact_num("ch06.LEN_LOG_BASE", SEQLZ_LEN_LOG_BASE);
    fact_num("ch06.LIT_BITS", SEQLZ_LIT_BITS);
    fact_num("ch06.LIT_SETS", SEQLZ_LIT_SETS);
    fact_num("ch06.LIT_STREAMS", SEQLZ_LIT_STREAMS);
    fact_num("ch06.LIT_ROUNDS", SEQLZ_LIT_ROUNDS);
    fact_num("ch06.LIT_HEADER_MIN", SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN));
    fact_num("ch06.LIT_HEADER_MAX", SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MAX));
    fact_num("ch06.LIT_CODED_MIN", SEQLZ_LIT_CODED_MIN);
    fact_num("ch06.LL_BITS", SEQLZ_LL_BITS);
    fact_num("ch06.ML_BITS", SEQLZ_ML_BITS);
    fact_num("ch06.HEADER", SEQLZ_HEADER);
    snprintf(P, sizeof P, "0x%X", SEQLZ_LIT_CODED);
    fact_str("ch06.LIT_CODED", P);
    snprintf(P,
             sizeof P,
             "%u, %u, %u, %u, %u, %u",
             SEQLZ_OFF_SHIFT(0),
             SEQLZ_OFF_SHIFT(1),
             SEQLZ_OFF_SHIFT(2),
             SEQLZ_OFF_SHIFT(3),
             SEQLZ_OFF_SHIFT(4),
             SEQLZ_OFF_SHIFT(5));
    fact_str("ch06.OFF_SHIFT", P);
    fact_num("ch06.SIZE_BITS_MIN", SEQLZ_SIZE_BITS_MIN);
    fact_num("ch06.SIZE_BITS_MAX", SEQLZ_SIZE_BITS_MAX);
    fact_num("ch06.LIT_WIDTH_AT", SEQLZ_LIT_WIDTH_AT);
    fact_num("ch06.LIT_XOR_AT", SEQLZ_LIT_XOR_AT);
    fact_num("ch06.XOR_4", SEQLZ_XOR_4);
    fact_num("ch06.XOR_8", SEQLZ_XOR_8);
    fact_num("ch06.XOR_LAST", SEQLZ_XOR_LAST);
    fact_num("ch06.sequence", sizeof(struct seqlz_sequence));
    fact_num("ch06.esc_len", seqlz_default_own.token[SEQLZ_ESCAPE]);
    fact_num("ch06.esc_pair", seqlz_default_own.token[SEQLZ_ESCAPE] + SEQLZ_ESCAPE_BITS + SEQLZ_RAW_BITS(3));
    for (k = 0; k < sizeof lens / sizeof lens[0]; k++) {
        sym = seqlz_len_symbol(lens[k], &extra);
        snprintf(P, sizeof P, "ch06.lensym.%u", lens[k]);
        fact_str(K(P, "bin"), bin(lens[k], 32 - (unsigned int)__builtin_clz(lens[k])));
        fact_num(K(P, "clz"), (unsigned int)__builtin_clz(lens[k]));
        fact_num(K(P, "sym"), sym);
        fact_num(K(P, "extra"), extra);
        fact_num(K(P, "low"), extra ? lens[k] - (1U << extra) : 0);
    }
    for (k = 0; k < sizeof offs / sizeof offs[0]; k++) {
        unsigned int o = offs[k];

        cls = seqlz_off_class(o, 0, &raw);
        snprintf(P, sizeof P, "ch06.off.%u", o);
        /* the terms of seqlz_off_class() for a new offset, which must add up to its class */
        aligned = ((o & 7U) == 0) & (o < 8U << SEQLZ_CLASS5_BITS);
        fact_num(K(P, "lt256"), o < 256U);
        fact_num(K(P, "eq2"), o == 2U);
        fact_num(K(P, "aligned"), aligned);
        if (3U - (o < 256U) - (o == 2U) + 2U * aligned != cls)
            exit(1);
        fact_num(K(P, "sum"), cls);
        fact_num(K(P, "cls"), cls);
        fact_num(K(P, "raw"), raw);
        fact_num(K(P, "sent"), o >> SEQLZ_OFF_SHIFT(cls));
    }
    for (k = 0; k < sizeof toks / sizeof toks[0]; k++) {
        unsigned int t = seqlz_token(toks[k][0], toks[k][1], toks[k][2]);

        snprintf(P, sizeof P, "ch06.tok.%u", k);
        fact_num(K(P, "a"), t & SEQLZ_LL_CAP);
        fact_num(K(P, "b"), (t >> SEQLZ_LL_BITS) & SEQLZ_ML_CAP);
        fact_num(K(P, "b16"), ((t >> SEQLZ_LL_BITS) & SEQLZ_ML_CAP) << SEQLZ_LL_BITS);
        fact_num(K(P, "c512"), t >> (SEQLZ_LL_BITS + SEQLZ_ML_BITS) << (SEQLZ_LL_BITS + SEQLZ_ML_BITS));
        fact_num(K(P, "tok"), t);
    }
}

int main(void) {
    codes();
    text_facts();
    walk_facts();
    page_walk("ab", ab_page);
    page_walk("second", second_page);
    pointer_facts();
    offset_facts();
    sha_facts();
    longest_facts();
    ch06_facts();
    facts_end();
    return 0;
}
