// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * own.c: the facts of a page's own literal table, level 3, for chapters 4, 6, 7, 8, 11 and 13. It
 * includes seqlz_compress.c and seqlz_decompress.c to reach own_code(), own_lengths(), log2_256()
 * and read_own_table():
 *   own.len.S, own.code.S     the fixed code OWN that describes a table, symbol S
 *   own.q.XX.*                quad[] for the 8 bits XX, field by field
 *   own.text.*                chapter 4's text page at level 3: its header, its table's
 *                             description read symbol by symbol as docs/format.md reads it,
 *                             and the codes of its bytes
 *   own.enc.*                 the encoder on that page: counts, the entropy estimate, the lengths
 *                             before and after own_lengths() makes the code complete, the choice
 *   own.dec.*                 read_own_table() on that page, step by step, checked against its result
 */
#include <stdarg.h>

#include "facts.h"
#include "seqlz_compress.c"
#include "seqlz_compress_literals.c"
#include "seqlz_decompress.c"
#include "seqlz_decompress_literals.c"

static char key_buf[128];

static const char* key(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(key_buf, sizeof key_buf, fmt, ap);
    va_end(ap);
    return key_buf;
}

static void str(const char* k, const char* fmt, ...) {
    static char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fact_str(k, buf);
}

static void fail(const char* what) {
    fprintf(stderr, "facts/own.c: %s\n", what);
    exit(1);
}

/* the n low bits of v, the top one first */
static const char* bin(unsigned long long v, unsigned int n) {
    static char ring[8][65];
    static unsigned int at;
    char* s = ring[at++ % 8];
    unsigned int k;

    for (k = 0; k < n; k++)
        s[k] = (char)('0' + ((v >> (n - 1 - k)) & 1));
    s[n] = 0;
    return s;
}

/*
 * a byte value as the text shows it: the space, a letter or digit, else in hex, so that a table cell
 * never gets a backtick or a bar
 */
static const char* byte_name(unsigned int b) {
    static char ring[8][32];
    static unsigned int at;
    char* s = ring[at++ % 8];

    if (b == ' ')
        snprintf(s, 32, "the space");
    else if ((b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z'))
        snprintf(s, 32, "`%c`", b);
    else
        snprintf(s, 32, "`0x%02x`", b);
    return s;
}

/* log2(x) for x >= 1, bit by bit by squaring: no libm */
static double log2_of(double x) {
    double r = 0, bit = 1;
    unsigned int k;

    while (x >= 2) {
        x /= 2;
        r += 1;
    }
    for (k = 0; k < 40; k++) {
        x *= x;
        bit /= 2;
        if (x >= 2) {
            x /= 2;
            r += bit;
        }
    }
    return r;
}

/* the fixed code OWN */
static void code_facts(const struct seqlz_tables* t) {
    unsigned int s;

    fact_num("own.symbols", SEQLZ_OWN_SYMBOLS);
    fact_num("own.run_at", SEQLZ_OWN_RUN);
    fact_num("own.lit_own", SEQLZ_LIT_OWN);
    fact_num("own.code_bits", SEQLZ_OWN_CODE_BITS);
    for (s = 0; s < SEQLZ_OWN_SYMBOLS; s++) {
        unsigned int e = t->own.enc[s], l = e >> 8;

        if (l != seqlz_own_lengths[s])
            fail("own.enc[] does not have the lengths of seqlz_own_lengths");
        fact_num(key("own.len.%u", s), l);
        fact_str(key("own.code.%u", s), bin(e & 0xffU, l));
    }
    fact_num("own.size.decode", sizeof t->own.decode);
    fact_num("own.size.enc", sizeof t->own.enc);
    fact_num("own.size.quad", sizeof t->own.quad);
    fact_num("own.size.all", sizeof t->own);
    fact_num("own.quad_none", OWN_QUAD_NONE);
    fact_num("own.quad_run_max", OWN_QUAD_RUN_MAX);
}

/* quad[k], field by field */
static void quad_facts(const struct seqlz_tables* t, unsigned int k) {
    u32 e = t->own.quad[k];

    fact_str(key("own.q.%02x.bits", k), bin(k, 8));
    str(key("own.q.%02x.entry", k), "0x%08x", e);
    fact_num(key("own.q.%02x.alone", k), e >> OWN_QUAD_ALONE_AT);
    fact_num(key("own.q.%02x.l1", k), e & 15U);
    fact_num(key("own.q.%02x.l2", k), e >> 4 & 15U);
    fact_num(key("own.q.%02x.end1", k), e >> OWN_QUAD_END1_AT & 15U);
    fact_num(key("own.q.%02x.end2", k), e >> OWN_QUAD_END2_AT & 15U);
    fact_num(key("own.q.%02x.run1", k), e >> OWN_QUAD_RUN1_AT & OWN_QUAD_RUN_MAX);
    fact_num(key("own.q.%02x.run2", k), e >> OWN_QUAD_RUN2_AT & OWN_QUAD_RUN_MAX);
    fact_num(key("own.q.%02x.sym", k), t->own.decode[k] & 0xffU);
    fact_num(key("own.q.%02x.symlen", k), t->own.decode[k] >> 8);
    for (unsigned int pos = 1; pos < 8; pos++)
        fact_str(key("own.q.%02x.w%u", k, pos), bin(k << pos & 0xffU, 8));
}

/* bit i of p, most significant bit of each byte first */
static unsigned int bit_at(const u8* p, unsigned int i) {
    return p[i / 8] >> (7 - i % 8) & 1U;
}

/*
 * The description at p as docs/format.md reads it, one symbol at a time with the canonical code of
 * seqlz_own_lengths: what each symbol says, and the lengths of the byte values. Returns the bits used.
 */
static unsigned int read_description(const u8* p, u8* len, unsigned int* rows) {
    unsigned int first[17] = {0}, count[17] = {0}, at = 0, b = 0, space = 0, n = 0, s, l;
    unsigned int sym_of[17][SEQLZ_OWN_SYMBOLS];

    for (s = 0; s < SEQLZ_OWN_SYMBOLS; s++)
        sym_of[seqlz_own_lengths[s]][count[seqlz_own_lengths[s]]++] = s;
    for (l = 1; l <= 16; l++)
        first[l] = (first[l - 1] + count[l - 1]) << 1;
    memset(len, 0, 256);
    while (space < 1024U) {
        unsigned int c = 0, from = at, x = 0, k;
        char what[160];

        for (l = 1;; l++) {
            c = c << 1 | bit_at(p, at++);
            if (l > 8)
                fail("not a code of OWN");
            if (c - first[l] < count[l])
                break;
        }
        s = sym_of[l][c - first[l]];
        if (s < SEQLZ_OWN_RUN + 1U) {
            len[b] = (u8)(s + 1U);
            space += 1024U >> (s + 1U);
            snprintf(what, sizeof what, "%s gets a code of %u bits", byte_name(b), s + 1U);
            b++;
        } else {
            unsigned int kk = s - SEQLZ_OWN_RUN;

            for (k = 0; k + 1 < kk; k++)
                x = x << 1 | bit_at(p, at++);
            x += 1U << (kk - 1U);
            if (x == 1)
                snprintf(what, sizeof what, "a run of 1: %s has no code", byte_name(b));
            else
                snprintf(what, sizeof what, "a run of %u: %s to %s have no code", x, byte_name(b), byte_name(b + x - 1));
            b += x;
        }
        {
            char bits[32];

            for (k = 0; k < at - from; k++)
                bits[k] = (char)('0' + bit_at(p, from + k));
            bits[k] = 0;
            if (s < SEQLZ_OWN_RUN + 1U)
                fact_str(key("own.text.d.%u.bits", n), bits);
            else
                str(key("own.text.d.%u.bits", n), "%.*s %s", l, bits, bits + l);
        }
        fact_num(key("own.text.d.%u.sym", n), s);
        fact_str(key("own.text.d.%u.what", n), what);
        fact_num(key("own.text.d.%u.space", n), space);
        n++;
    }
    if (space != 1024U)
        fail("the description overfills the code space");
    *rows = n;
    return at;
}

/* chapter 4's text page at level 3, and what the encoder did for it */
static const u8* text_facts(const struct seqlz_tables* t, unsigned int* d_out) {
    static u8 page[SEQLZ_BLOCK_SIZE], dst[2 * SEQLZ_BLOCK_SIZE], len[256];
    static struct seqlz_state st;
    struct own_work* w = own_work(&st);
    unsigned int n, h, b2, wdt, d, rows, used, k, nb = 0, sum = 0, s[SEQLZ_LIT_STREAMS];

    words_page(page);
    n = seqlz_compress(&st, page, dst, sizeof dst, SEQLZ_LEVEL_OWN);
    h = get_unaligned_le16(dst);
    b2 = dst[2];
    if (!n || !(h & SEQLZ_LIT_CODED) || (b2 & SEQLZ_LIT_CODE_MASK) != SEQLZ_LIT_OWN)
        fail("the text page has no table of its own at level 3");
    wdt = SEQLZ_SIZE_BITS_MIN + (b2 >> SEQLZ_LIT_WIDTH_AT & 7U);
    for (k = 0; k < SEQLZ_LIT_STREAMS; k++) {
        unsigned int j;

        for (s[k] = 0, j = 0; j < wdt; j++)
            s[k] |= (unsigned int)(dst[3 + (k * wdt + j) / 8] >> ((k * wdt + j) % 8) & 1U) << j;
        sum += s[k];
    }
    used = read_description(dst + SEQLZ_LIT_HEADER(wdt), len, &rows);
    d = (used + 7) / 8;
    if (SEQLZ_LIT_HEADER(wdt) + d + sum > n)
        fail("the streams do not fit the page");
    for (k = 0; k < 256; k++)
        nb += len[k] != 0;
    fact_num("own.text.len", n);
    fact_num("own.text.n", h & (SEQLZ_LIT_CODED - 1U));
    str("own.text.h", "0x%04x", h);
    str("own.text.t", "0x%02x", b2);
    fact_str("own.text.t_bin", bin(b2, 8));
    fact_num("own.text.lcode", b2 & SEQLZ_LIT_CODE_MASK);
    fact_num("own.text.w", wdt);
    fact_num("own.text.w5", wdt - SEQLZ_SIZE_BITS_MIN);
    fact_num("own.text.desc_at", SEQLZ_LIT_HEADER(wdt));
    fact_num("own.text.dbytes", d);
    fact_hex("own.text.desc", dst + SEQLZ_LIT_HEADER(wdt), d);
    fact_num("own.text.desc_bits", used);
    fact_num("own.text.desc_pad", 8 * d - used);
    fact_num("own.text.rows", rows);
    fact_num("own.text.last_row", rows - 1);
    fact_num("own.text.with_code", nb);
    fact_num("own.text.streams_at", SEQLZ_LIT_HEADER(wdt) + d);
    fact_num("own.text.sum", sum);
    fact_num("own.text.b_at", SEQLZ_LIT_HEADER(wdt) + d + sum);
    fact_num("own.text.b_bytes", n - SEQLZ_LIT_HEADER(wdt) - d - sum);
    for (k = 0; k < 256; k++)
        if (len[k]) {
            if (len[k] != w->len[k])
                fail("the description does not say the encoder's lengths");
            fact_num(key("own.text.clen.%02x", k), len[k]);
            fact_str(key("own.text.code.%02x", k), bin(w->enc[k] >> 8, len[k]));
        }
    *d_out = d;
    return dst + SEQLZ_LIT_HEADER(wdt);
}

/* own_code() and own_lengths() on the text page, step by step */
static void encoder_facts(const struct seqlz_tables* t) {
    static u8 page[SEQLZ_BLOCK_SIZE], dst[2 * SEQLZ_BLOCK_SIZE];
    static struct seqlz_state st;
    static const u8 shown[] = {' ', 'a', 'd', 'e', 'l', 'n', 'u'};
    struct own_work* w = own_work(&st);
    u64 even[SEQLZ_LIT_STREAMS] = {0}, odd[SEQLZ_LIT_STREAMS] = {0};
    unsigned int n_lit, bits = ~0U, set = 0, j, k, coded = 0, sizes[SEQLZ_LIT_STREAMS], width, header, d, est;
    unsigned int h = 0, n = 0, space0 = 0, own_coded, own_width, init_len[256] = {0}, own_bits = 0;
    const u8* lits;

    words_page(page);
    if (!seqlz_compress(&st, page, dst, sizeof dst, 1))
        fail("the text page does not compress");
    n_lit = get_unaligned_le16(dst);
    lits = dst + SEQLZ_HEADER;
    /* seqlz_code_literals()' first steps, with the counts for the page's own table */
    memset(w->freq, 0, sizeof w->freq);
    lit_cost(lits, n_lit, even, odd, w->freq);
    for (k = 0; k < SEQLZ_LIT_SETS; k++) {
        unsigned int b = 0;

        for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
            b += lit_bits(even, odd, j, k);
        if (b < bits) {
            bits = b;
            set = k;
        }
    }
    for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
        sizes[j] = (lit_bits(even, odd, j, set) + 7U) / 8U;
        coded += sizes[j];
    }
    width = size_width(sizes);
    header = SEQLZ_LIT_HEADER(width);
    fact_num("own.enc.n_lit", n_lit);
    fact_num("own.enc.set", set);
    fact_num("own.enc.set_bits", bits);
    fact_num("own.enc.set_coded", coded);
    fact_num("own.enc.set_width", width);
    fact_num("own.enc.set_header", header);
    fact_num("own.enc.limit", header + coded);
    fact_num("own.enc.freq0.20", w->freq[0][' ']);
    fact_num("own.enc.freq1.20", w->freq[1][' ']);
    fact_num("own.enc.min_lits", OWN_LITERALS_MIN);
    fact_num("own.enc.max_lits", OWN_LITERALS_MAX);
    fact_num("own.enc.saves_min", OWN_SAVES_MIN);
    fact_num("own.enc.work", OWN_WORK);
    fact_num("own.enc.work_size", sizeof(struct own_work));
    fact_num("own.enc.desc_room", OWN_DESC_ROOM);
    fact_num("own.enc.work_at", (unsigned int)((u8*)w - (u8*)&st));
    /* the present bits of bytes 0x60 to 0x63, the backtick and a to c, as own_code() makes them */
    {
        const u64 low = 0x7fff7fff7fff7fffULL;
        u64 c = own_lanes(&w->freq[0][0x60]) + own_lanes(&w->freq[1][0x60]), m;

        str("own.enc.pr.c", "0x%016llx", (unsigned long long)c);
        m = (c & low) + low;
        str("own.enc.pr.plus", "0x%016llx", (unsigned long long)m);
        m = ((c | m) & ~low) >> 15;
        str("own.enc.pr.tops", "0x%016llx", (unsigned long long)m);
        str("own.enc.pr.mul", "0x%016llx", (unsigned long long)(m * 0x0001000200040008ULL));
        fact_str("own.enc.pr.nibble", bin(m * 0x0001000200040008ULL >> 48 & 15U, 4));
    }
    /* own_code() itself */
    d = own_code(w, n_lit, header + coded);
    if (!d)
        fail("own_code() gives the text page no table");
    for (k = 0; k < 256; k++)
        if (w->freq[0][k]) {
            h += w->freq[0][k] * log2_256(w->freq[0][k]);
            n++;
            /* the literals' bits with the code, which own_code() checks */
            own_bits += w->freq[0][k] * (w->len[k] & 15U);
        }
    h = n_lit * log2_256(n_lit) - h;
    est = h / 2048U + n * 115U / 256U;
    fact_num("own.enc.n", n);
    fact_num("own.enc.h", h);
    str("own.enc.h_bits", "%.2f", h / 256.0 / n_lit);
    fact_num("own.enc.h_bytes", h / 2048U);
    fact_num("own.enc.desc_est", n * 115U / 256U);
    fact_num("own.enc.est", est);
    fact_num("own.enc.est_lhs", est + SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN) + OWN_SAVES_MIN);
    fact_num("own.enc.d", d);
    fact_num("own.enc.bits", own_bits);
    fact_num("own.enc.bits_bytes", own_bits / 8U);
    str("own.enc.bits_per_lit", "%.2f", (double)own_bits / n_lit);
    str("own.enc.set_bits_per_lit", "%.2f", (double)bits / n_lit);
    fact_num("own.enc.first_lhs", SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MIN) + d + own_bits / 8U + OWN_SAVES_MIN);
    own_coded = own_sizes(w, lits, n_lit);
    own_width = size_width(w->sizes);
    fact_num("own.enc.own_coded", own_coded);
    fact_num("own.enc.own_width", own_width);
    fact_num("own.enc.own_header", SEQLZ_LIT_HEADER(own_width));
    fact_num("own.enc.second_lhs", SEQLZ_LIT_HEADER(own_width) + d + own_coded + OWN_SAVES_MIN);
    fact_num("own.enc.total", SEQLZ_LIT_HEADER(own_width) + d + own_coded);
    fact_num("own.enc.fixed_total", header + coded);
    /*
     * own_lengths()' first loop again, for the bytes the text shows: the shortest l with
     * freq * 2^l >= n_lit, and the gain, checked against what own_lengths() stored
     */
    {
        const u64 inv = (1ULL << 40) / n_lit;

        str("own.enc.inv", "%llu", (unsigned long long)inv);
        for (k = 0; k < n; k++) {
            unsigned int b = w->bytes[k], f = w->freq[0][b], l = (unsigned int)(__builtin_clz(f) - __builtin_clz(n_lit));
            u64 q;

            l += (f << l) < n_lit;
            l += !l;
            q = ((u64)f << l) * inv >> 36;
            q = l > SEQLZ_LIT_BITS || q < 16U ? 16U : q > 31U ? 31U : q;
            if (q - 16U != w->gain[k])
                fail("the replay of own_lengths() gives another gain");
            init_len[b] = min(l, SEQLZ_LIT_BITS);
            space0 += 1024U >> init_len[b];
        }
    }
    for (k = 0; k < sizeof shown; k++) {
        unsigned int b = shown[k], i;

        for (i = 0; i < n && w->bytes[i] != b; i++)
            ;
        if (i == n)
            fail("a byte the text shows is not a literal");
        fact_num(key("own.enc.b.%02x.freq", b), w->freq[0][b]);
        fact_num(key("own.enc.b.%02x.l0", b), init_len[b]);
        fact_num(key("own.enc.b.%02x.f2l", b), w->freq[0][b] << init_len[b]);
        fact_num(key("own.enc.b.%02x.f2l1", b), w->freq[0][b] << (init_len[b] - 1U));
        fact_num(key("own.enc.b.%02x.gain", b), w->gain[i]);
        fact_num(key("own.enc.b.%02x.q", b), w->gain[i] + 16U);
        str(key("own.enc.b.%02x.ratio", b), "%.3f", (double)(w->freq[0][b] << init_len[b]) / n_lit);
        fact_num(key("own.enc.b.%02x.len", b), w->len[b]);
        fact_num(key("own.enc.b.%02x.log", b), log2_256(w->freq[0][b]));
        str(key("own.enc.b.%02x.log_exact", b), "%.1f", 256.0 * log2_of(w->freq[0][b]));
    }
    fact_num("own.enc.nu_diff", w->freq[0]['n'] - w->freq[0]['u']);
    fact_num("own.enc.clz_space", __builtin_clz(w->freq[0][' ']));
    fact_num("own.enc.clz_n", __builtin_clz(n_lit));
    fact_num("own.enc.space0", space0);
    fact_num("own.enc.room0", 1024U - space0);
    {
        unsigned int changed = 0;

        for (k = 0; k < 256; k++)
            if (init_len[k] && init_len[k] != w->len[k]) {
                if (init_len[k] != w->len[k] + 1U)
                    fail("own_lengths() changed a length by more than 1");
                changed++;
            }
        fact_num("own.enc.changed", changed);
    }
    /* log2_256() of the space's count, the table index */
    {
        unsigned int f = w->freq[0][' '], e = 31U - (unsigned int)__builtin_clz(f);

        fact_num("own.enc.log.e", e);
        fact_num("own.enc.log.idx", (f << (31U - e)) >> 26 & 31U);
        fact_num("own.enc.log.frac", log2_frac[(f << (31U - e)) >> 26 & 31U]);
        str("own.enc.log.shifted", "0x%08x", f << (31U - e));
    }
}

/*
 * read_own_table() on the text page's description, step by step: the same steps again, recorded, and the
 * decode table they give must be the one read_own_table() builds
 */
static void decoder_facts(const struct seqlz_tables* t, const u8* desc, unsigned int d) {
    static u8 page[SEQLZ_BLOCK_SIZE], bucket[SEQLZ_SCRATCH], dt_page[SEQLZ_BLOCK_SIZE];
    const unsigned int full = 1U << SEQLZ_LIT_BITS;
    unsigned int at = 0, b = 0, space = 0, step = 0, k, i, pos = 0, count[OWN_QUAD_NONE + 1] = {0};
    u8 order[OWN_QUAD_NONE + 1][256];
    u16* lt = (u16*)dt_page;
    const u8* end;

    while (space < full) {
        unsigned int top = 0, e, l1, l2, next, two, used;

        for (k = 0; k < 8; k++)
            top = top << 1 | (pos + k < 8 * d ? bit_at(desc, pos + k) : 0U);
        e = t->own.quad[top];
        str(key("own.dec.%u.top", step), "%s", bin(top, 8));
        str(key("own.dec.%u.e", step), "0x%08x", e);
        if (e >> OWN_QUAD_ALONE_AT) {
            unsigned int dd = t->own.decode[top], nb = (dd & 0xffU) - SEQLZ_OWN_RUN, x = 0;

            for (k = 0; k + 1 < nb; k++)
                x = x << 1 | bit_at(desc, pos + (dd >> 8) + k);
            b += (1U << (nb - 1U)) + x;
            used = (dd >> 8) + nb - 1U;
            fact_str(key("own.dec.%u.kind", step), "alone");
            fact_num(key("own.dec.%u.run", step), (1U << (nb - 1U)) + x);
        } else {
            l1 = e & 15U;
            b += e >> OWN_QUAD_RUN1_AT & OWN_QUAD_RUN_MAX;
            next = space + (full >> l1);
            two = next < full;
            l2 = two ? e >> 4 & 15U : OWN_QUAD_NONE;
            used = e >> (two ? OWN_QUAD_END2_AT : OWN_QUAD_END1_AT) & 15U;
            order[l1][count[l1]++] = (u8)b;
            fact_str(key("own.dec.%u.kind", step), l2 == OWN_QUAD_NONE ? "one" : "two");
            fact_num(key("own.dec.%u.l1", step), l1);
            fact_num(key("own.dec.%u.b1", step), b);
            fact_num(key("own.dec.%u.run1", step), e >> OWN_QUAD_RUN1_AT & OWN_QUAD_RUN_MAX);
            b += 1U + (two ? e >> OWN_QUAD_RUN2_AT & OWN_QUAD_RUN_MAX : 0U);
            if (l2 != OWN_QUAD_NONE) {
                order[l2][count[l2]++] = (u8)b;
                fact_num(key("own.dec.%u.l2", step), l2);
                fact_num(key("own.dec.%u.b2", step), b);
                fact_num(key("own.dec.%u.run2", step), e >> OWN_QUAD_RUN2_AT & OWN_QUAD_RUN_MAX);
            }
            space = next + (full >> l2);
            b += l2 != OWN_QUAD_NONE;
        }
        fact_num(key("own.dec.%u.used", step), used);
        fact_num(key("own.dec.%u.space", step), space);
        pos += used;
        step++;
    }
    fact_num("own.dec.steps", step);
    fact_num("own.dec.bits", pos);
    /* the real one */
    end = read_own_table(&t->own, desc, desc + d + 64, (u16*)page, bucket);
    if (end != desc + d)
        fail("read_own_table() ends elsewhere");
    /* the canonical table from the steps' buckets, as read_own_table() writes it */
    for (i = 1, at = 0; i <= SEQLZ_LIT_BITS; i++)
        for (k = 0; k < count[i]; k++) {
            unsigned int r;

            fact_num(key("own.dt.%02x.first", order[i][k]), at);
            for (r = 0; r < full >> i; r++)
                lt[at++] = (u16)(i | order[i][k] << 8);
            fact_num(key("own.dt.%02x.last", order[i][k]), at - 1);
            str(key("own.dt.%02x.entry", order[i][k]), "0x%04x", i | order[i][k] << 8);
        }
    if (at != full || memcmp(lt, page, sizeof(u16) * full))
        fail("the replay of read_own_table() gives another decode table");
    fact_num("own.dt.bytes", sizeof(u16) * full);
    fact_num("own.dt.bucket_bytes", (OWN_QUAD_NONE + 1U) * 256U);
    fact_num("own.dec.step_max", OWN_STEP_MAX);
}

/* the counts OWN is built from, the line "own" of bench/seqlz_counts_4k.txt, and the pages they are from */
static void counts_facts(void) {
    const char* at = strstr(__FILE__, "docs/book/facts/");
    char path[1024], line[4096];
    unsigned long long c[SEQLZ_OWN_SYMBOLS], all = 0, long_lens = 0;
    unsigned int pages = 0, k, n;
    FILE* f;

    if (!at)
        fail("not in docs/book/facts/");
    snprintf(path, sizeof path, "%.*sbench/seqlz_counts_4k.txt", (int)(at - __FILE__), __FILE__);
    f = fopen(path, "r");
    if (!f)
        fail("cannot read bench/seqlz_counts_4k.txt");
    while (fgets(line, sizeof line, f)) {
        const char* p = strstr(line, " pages that level 3");

        /* "# own: the symbols of the descriptions of the 210160 pages that level 3 gives ..." */
        if (!strncmp(line, "# own:", 6) && p) {
            while (p > line && p[-1] >= '0' && p[-1] <= '9')
                p--;
            pages = (unsigned int)atoi(p);
        }
        if (!strncmp(line, "own ", 4)) {
            char* q = line + 4;

            n = (unsigned int)strtoul(q, &q, 10);
            if (n != SEQLZ_OWN_SYMBOLS)
                fail("the line own has another number of symbols");
            for (k = 0; k < n; k++) {
                c[k] = strtoull(q, &q, 10);
                all += c[k];
                if (k >= 6 && k <= 9)
                    long_lens += c[k];
            }
        }
    }
    fclose(f);
    /* the specification's length, to the nearest 10 lines, for chapter 4 */
    snprintf(path, sizeof path, "%.*sdocs/format.md", (int)(at - __FILE__), __FILE__);
    f = fopen(path, "r");
    if (!f)
        fail("cannot read docs/format.md");
    for (n = 0; fgets(line, sizeof line, f);)
        n++;
    fclose(f);
    fact_num("ch04.format_lines", (n + 5) / 10 * 10);
    if (!pages || !all)
        fail("no counts of own in bench/seqlz_counts_4k.txt");
    fact_num("own.counts.pages", pages);
    str("own.counts.share_7_10", "%.0f", 100.0 * long_lens / all);
}

int main(void) {
    const struct seqlz_tables* t = fact_tables();
    unsigned int d;
    const u8* desc;

    code_facts(t);
    counts_facts();
    quad_facts(t, 0xd1);
    quad_facts(t, 0xfc);
    quad_facts(t, 0x05);
    desc = text_facts(t, &d);
    encoder_facts(t);
    decoder_facts(t, desc, d);
    facts_end();
    return 0;
}
