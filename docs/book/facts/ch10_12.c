// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * ch10_12.c: the facts of chapters 10 to 12, the encoder, the coded literals and the decoder's tools.
 * It includes seqlz_compress.c and seqlz_decompress.c to reach their static functions, so the worked
 * examples come from the real helpers:
 *   ch10.ab.*, ch10.ptr.*   the page of ab and the page of pointers put by put and flush by flush,
 *                           checked against what seqlz_compress() writes
 *   ch10.esc.*              the escape, and token 10 escaped
 *   ch11.*                  chapter 11's page of letters through code_literals(), step by step
 *   ch12.*                  the sizes and offsets of the decoder's tables, repeat_pattern(), and the
 *                           bit reader on made-up bytes and on the page of ab
 */
// facts: also 16k
#include <stdarg.h>

#include "facts.h"
#include "seqlz_compress.c"
#include "seqlz_decompress.c"

#define CAP (2 * SEQLZ_PAGE)

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

/* a few strings that live until the next calls: for the arguments of one str() */
static char* scratch(void) {
    static char ring[8][1024];
    static unsigned int at;

    return ring[at++ % 8];
}

/* the n low bits of v, the top one first */
static const char* bin(u64 v, unsigned int n) {
    char* s = scratch();
    unsigned int k;

    for (k = 0; k < n; k++)
        s[k] = (char)('0' + ((v >> (n - 1 - k)) & 1));
    s[n] = 0;
    return s;
}

/* n bytes as "02 00 61 62" */
static const char* hexb(const u8* p, unsigned int n) {
    char* s = scratch();
    unsigned int k;

    *s = 0;
    for (k = 0; k < n; k++)
        sprintf(s + 3 * k - (k ? 1 : 0), "%s%02x", k ? " " : "", p[k]);
    return s;
}

static void fail(const char* what) {
    fprintf(stderr, "facts/ch10_12.c: %s\n", what);
    exit(1);
}

/* ---- chapter 10: the encoder, one put and one flush at a time ---- */

static const struct seqlz_tables* T;
static u8 dst[CAP];

struct trace {
    const char* name;
    struct seqlz_encoder e;
    u8* low;                    /* the lowest byte a flush wrote */
    unsigned int seqs, flushes; /* how many of them get facts */
    unsigned int n_flush;
};

static void flush_traced(struct trace* r) {
    struct seqlz_encoder* e = &r->e;
    unsigned int cnt0 = e->cnt, n = r->n_flush + 1, k;
    u64 word = e->acc << ((64U - e->cnt) & 63U);
    u8* p0 = e->p;

    enc_flush(e);
    if (p0 - 8 < r->low)
        r->low = p0 - 8;
    r->n_flush = n;
    if (n > r->flushes)
        return;
    fact_num(key("ch10.%s.f%u.cnt0", r->name, n), cnt0);
    fact_num(key("ch10.%s.f%u.cnt1", r->name, n), e->cnt);
    fact_num(key("ch10.%s.f%u.shift", r->name, n), (64U - cnt0) & 63U);
    str(key("ch10.%s.f%u.word", r->name, n), "0x%016llx", (unsigned long long)word);
    str(key("ch10.%s.f%u.stored", r->name, n), "%s", hexb(p0 - 8, 8));
    fact_num(key("ch10.%s.f%u.moved", r->name, n), p0 - e->p);
    fact_num(key("ch10.%s.f%u.p", r->name, n), dst + CAP - e->p);
    for (k = 1; k <= 10; k++) {
        const u8* b = dst + CAP - k;

        str(key("ch10.%s.f%u.e%u", r->name, n, k), "%s", b < r->low ? "?" : hexb(b, 1));
    }
}

/* the bits of a length value for ch10.<name>.s<i>.mlv */
static void len_value_facts(const char* prefix, const struct value_table* vt, unsigned int v) {
    char pre[64];
    unsigned int extra, s = seqlz_len_symbol(v, &extra);
    u32 entry = vt->enc[s];
    unsigned int len = entry >> 16 & 15U, code = entry & 0xffffU, x = v & ((1U << extra) - 1U);
    u64 put = (u64)code << extra | x;

    snprintf(pre, sizeof pre, "%s", prefix);
    fact_num(key("%s.v", pre), v);
    fact_num(key("%s.sym", pre), s);
    fact_num(key("%s.extra", pre), extra);
    str(key("%s.entry", pre), "0x%x", entry);
    str(key("%s.codehex", pre), "0x%x", code);
    str(key("%s.code", pre), "%s", bin(code, len));
    fact_num(key("%s.len", pre), len);
    fact_num(key("%s.x", pre), x);
    str(key("%s.xbits", pre), "%s", bin(x, extra));
    str(key("%s.shifted", pre), "0x%llx", (unsigned long long)code << extra);
    str(key("%s.put", pre), "0x%llx", (unsigned long long)put);
    fact_num(key("%s.n", pre), len + extra);
    str(key("%s.bits", pre), "%s %s", bin(code, len), bin(x, extra));
}

/*
 * The page through a copy of encode_emit()'s bits, the facts of the first r->seqs sequences and
 * r->flushes flushes; fails unless the page comes out as seqlz_compress() writes it.
 */
static void encode_traced(struct trace* r, void (*make)(u8*)) {
    static u8 page[SEQLZ_PAGE], ref[CAP];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    struct seqlz_encoder* e = &r->e;
    const u8* in = page;
    unsigned int n, i, len, ref_len;

    make(page);
    n = seqlz_find(&st, page, seq);
    memset(dst, 0, sizeof dst);
    encoder_init(e, T, dst, CAP, page + SEQLZ_PAGE);
    r->low = dst + CAP;
    r->n_flush = 0;
    for (i = 0; i < n; i++) {
        unsigned int ll = seq[i].literals, ml = i + 1 == n ? 0 : seq[i].match, off = seq[i].offset;
        unsigned int raw, cls, tok, tlen, cnt0 = e->cnt;
        u32 code;
        u64 v;

        memcpy(e->lit, in, ll);
        e->lit += ll;
        in += ll + ml;
        if (ml == 0)
            break;
        cls = seqlz_off_class(off, e->last, &raw);
        tok = seqlz_token(ll, ml, cls);
        code = token_code(T, tok, &tlen);
        v = (u64)code << raw | ((off >> SEQLZ_OFF_SHIFT(cls)) & (0U - (cls != 0)));
        enc_put(e, v, tlen + raw);
        if (i < r->seqs) {
            char pre[64];

            snprintf(pre, sizeof pre, "ch10.%s.s%u", r->name, i + 1);
            fact_num(key("%s.ll", pre), ll);
            fact_num(key("%s.ml", pre), ml);
            fact_num(key("%s.ml4", pre), ml - 4);
            fact_num(key("%s.off", pre), off);
            fact_num(key("%s.cls", pre), cls);
            fact_num(key("%s.raw", pre), raw);
            fact_num(key("%s.tok", pre), tok);
            fact_num(key("%s.tlen", pre), tlen);
            str(key("%s.code", pre), "%s", bin(code, tlen));
            str(key("%s.codehex", pre), "0x%x", code);
            str(key("%s.offbits", pre), "%s", bin(off >> SEQLZ_OFF_SHIFT(cls), raw));
            str(key("%s.put", pre), "0x%llx", (unsigned long long)v);
            fact_num(key("%s.putn", pre), tlen + raw);
            str(key("%s.putbits", pre), "%s", bin(v, tlen + raw));
            str(key("%s.acc", pre), "0x%llx", (unsigned long long)e->acc);
            fact_num(key("%s.cnt", pre), e->cnt);
            /* the bits that count, those from before this put first */
            str(key("%s.accbits", pre), "%s%s%s", bin(e->acc >> (tlen + raw), cnt0), cnt0 ? " " : "", bin(e->acc, tlen + raw));
            if (ml - 4 >= SEQLZ_ML_CAP)
                len_value_facts(key("%s.mlv", pre), &T->ml, ml - 4 - SEQLZ_ML_CAP);
        }
        if (ll >= SEQLZ_LL_CAP) {
            put_len_value(e, &T->ll, ll - SEQLZ_LL_CAP);
            flush_traced(r);
        }
        if (ml - 4 >= SEQLZ_ML_CAP) {
            put_len_value(e, &T->ml, ml - 4 - SEQLZ_ML_CAP);
            if (i < r->seqs) {
                str(key("ch10.%s.s%u.mlv.acc", r->name, i + 1), "0x%llx", (unsigned long long)e->acc);
                fact_num(key("ch10.%s.s%u.mlv.cnt", r->name, i + 1), e->cnt);
            }
        }
        flush_traced(r);
        e->last = off;
    }
    str(key("ch10.%s.last8", r->name), "%s", hexb(dst + CAP - 8, 8));
    str(key("ch10.%s.le64", r->name), "0x%016llx", (unsigned long long)get_unaligned_le64(dst + CAP - 8));
    len = encoder_finish(e, dst, CAP);
    ref_len = fact_compress(page, ref, 0);
    if (len != ref_len || memcmp(dst, ref, len))
        fail("the copy of encode_emit() does not write what seqlz_compress() writes");
    str(key("ch10.%s.u16", r->name), "%s", hexb(dst, 2));
    str(key("ch10.%s.head8", r->name), "%s", hexb(e->lit, len - (e->lit - dst) < 8 ? len - (e->lit - dst) : 8));
}

/* the smallest dst the page fits into with raw literals */
static unsigned int min_dst(void (*make)(u8*)) {
    static u8 page[SEQLZ_PAGE], out[CAP];
    static struct seqlz_state st;
    unsigned int cap;

    make(page);
    for (cap = SEQLZ_HEADER + ENC_ROOM; cap <= CAP; cap++)
        if (seqlz_compress(T, &st, page, out, cap, false))
            return cap;
    fail("a page does not fit two pages");
    return 0;
}

/* a number, and as .tex with a thin space between groups of 3 digits from 10 000 on: 31\,744 */
static void tex_num(const char* k, unsigned int v) {
    char name[64];

    snprintf(name, sizeof name, "%s", k);
    fact_num(name, v);
    if (v >= 10000)
        str(key("%s.tex", name), "%u\\,%03u", v / 1000, v % 1000);
    else
        str(key("%s.tex", name), "%u", v);
}

static void chapter10(void) {
    struct trace ab = {.name = "ab", .seqs = 1, .flushes = 1};
    struct trace ptr = {.name = "ptr", .seqs = 4, .flushes = 4};
    unsigned int tok, len, codes = 0, most = 0, extra;
    u32 ee = T->token.enc[SEQLZ_ESCAPE], c;

    encode_traced(&ab, ab_page);
    encode_traced(&ptr, pointer_page);
    fact_num("ch10.ab.min_dst", min_dst(ab_page));
    fact_num("ch10.ptr.min_dst", min_dst(pointer_page));
    fact_num("ch10.slab.min_dst", min_dst(slab_page));

    /* the escape, and token 10 escaped */
    str("ch10.esc.entry", "0x%x", ee);
    fact_num("ch10.esc.len", ee >> TOK_ENC_LEN_AT);
    str("ch10.esc.codehex", "0x%x", ee & TOK_ENC_CODE_MASK);
    c = token_code(T, 10, &len);
    fact_num("ch10.esc10.enc", T->token.enc[10]);
    fact_num("ch10.esc10.len", len);
    str("ch10.esc10.ret", "0x%x", c);
    str("ch10.esc10.bits", "%s %s", bin(c >> SEQLZ_ESCAPE_BITS, len - SEQLZ_ESCAPE_BITS), bin(c, SEQLZ_ESCAPE_BITS));

    /* tokens with a code of their own; the most bits a token and its offset take */
    for (tok = 0; tok < SEQLZ_TOKEN_SYMBOLS; tok++) {
        unsigned int raw = SEQLZ_RAW_BITS(tok >> (SEQLZ_LL_BITS + SEQLZ_ML_BITS));

        codes += T->token.enc[tok] != 0;
        token_code(T, tok, &len);
        if (len + raw > most)
            most = len + raw;
    }
    fact_num("ch10.tok_codes", codes);
    fact_num("ch10.max_tok_off", most);

    /* the bound on the bitstream: 31 bits per 4 bytes of the page */
    tex_num("ch10.bound.seqs", SEQLZ_PAGE / 4);
    tex_num("ch10.bound.bits", SEQLZ_PAGE / 4 * 31);
    tex_num("ch10.bound.bytes", SEQLZ_PAGE / 4 * 31 / 8);
    tex_num("ch10.bound.page", SEQLZ_PAGE);
    tex_num("ch10.bound.total", SEQLZ_HEADER + SEQLZ_PAGE + SEQLZ_PAGE / 4 * 31 / 8);
    tex_num("ch10.bound.two", 2 * SEQLZ_PAGE);
    tex_num("ch10.bound.left", 2 * SEQLZ_PAGE - (SEQLZ_HEADER + SEQLZ_PAGE + SEQLZ_PAGE / 4 * 31 / 8));
    fact_num("ch10.len_sym_max", SEQLZ_LEN_SYMBOLS - 1);
    fact_num("ch10.len_extra_max", SEQLZ_LEN_SYMBOLS - 1 - SEQLZ_LEN_LOG_BASE);
    fact_num("ch10.len_bits_max", SEQLZ_MAX_BITS + SEQLZ_LEN_SYMBOLS - 1 - SEQLZ_LEN_LOG_BASE);
    fact_num("ch10.enc_room", ENC_ROOM);
    fact_num("ch10.dst_min", SEQLZ_HEADER + ENC_ROOM);
    /* a match of 3 makes ml - 4 wrap */
    fact_num("ch10.wrap.ml4", 3U - 4U);
    seqlz_len_symbol(3U - 4U - SEQLZ_ML_CAP, &extra);
    fact_num("ch10.wrap.extra", extra);
}

/* ---- chapter 11: code_literals() on the page of letters ---- */

/* letters_page() of pages.h with n letters */
static unsigned int letters_n;

static void letters_page_n(u8* p) {
    unsigned int k, seed = 1;

    memset(p, 0, SEQLZ_PAGE);
    for (k = 0; k < letters_n; k++) {
        seed = seed * 1103515245U + 12345U;
        p[k] = (u8) "etaoinshrdlu "[(seed >> 16) % 13];
    }
}

static int coded_with(unsigned int n) {
    static u8 page[SEQLZ_PAGE], out[CAP];

    letters_n = n;
    letters_page_n(page);
    return fact_compress(page, out, 1) < fact_compress(page, out, 0);
}

static void chapter11(void) {
    static u8 page[SEQLZ_PAGE], raw[CAP], cod[CAP], check[SEQLZ_PAGE];
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    u64 even[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = {{0}}, odd[SEQLZ_LIT_STREAMS][LIT_COST_WORDS] = {{0}};
    static const u8 four[4] = {0x65, 0x20, 0x65, 0x00};
    const u64 lanes = 0x00ff00ff00ff00ffULL;
    unsigned int total[SEQLZ_LIT_SETS], order[SEQLZ_LIT_SETS], sizes[SEQLZ_LIT_STREAMS], q[SEQLZ_LIT_STREAMS + 1];
    unsigned int n, n_lit, raw_len, cod_len, letters, i, j, k, set, coded = 0, all = 0, width, cnt, sum, bits;
    const u32* enc;
    u64 x = 0, acc;
    char* s;

    fact_num("ch11.lit_stream_max", LIT_STREAM_MAX);
    fact_num("ch11.coded_min", SEQLZ_LIT_CODED_MIN);
    fact_num("ch11.coded_min_h", SEQLZ_HEADER + SEQLZ_LIT_CODED_MIN);
    fact_num("ch11.header_max", SEQLZ_LIT_HEADER(SEQLZ_SIZE_BITS_MAX));
    fact_num("ch11.save_min", SEQLZ_LIT_CODED_MIN - 1 - SEQLZ_SIZE_BITS_MAX);
    fact_num("ch11.hash_entries", 1U << SEQLZ_HASH_BITS);
    fact_num("ch11.hash_bytes", sizeof_field(struct seqlz_state, table));
    fact_num("ch11.stream_lits", SEQLZ_PAGE / SEQLZ_LIT_STREAMS);
    fact_num("ch11.stream_bits", SEQLZ_PAGE / SEQLZ_LIT_STREAMS * SEQLZ_LIT_BITS);
    /* the rest is the page of letters, which chapter 11 shows with 4 KiB pages */
    if (SEQLZ_PAGE != 4096)
        return;
    letters_page(page);
    letters_n = 200;
    letters_page_n(check);
    if (memcmp(page, check, SEQLZ_PAGE))
        fail("letters_page_n() is not letters_page()");
    for (letters = 0; page[letters]; letters++)
        ;
    fact_num("ch11.letters", letters);
    fact_num("ch11.zeros", SEQLZ_PAGE - letters);
    str("ch11.first24", "%.24s", (const char*)page);
    n = seqlz_find(&st, page, seq);
    if (n != 2)
        fail("the page of letters has other sequences now: chapter 11 says 2");
    fact_num("ch11.ml", seq[0].match);
    fact_num("ch11.off", seq[0].offset);
    raw_len = fact_compress(page, raw, 0);
    cod_len = fact_compress(page, cod, 1);
    n_lit = get_unaligned_le16(raw);
    fact_num("ch11.n", n_lit);
    fact_num("ch11.n16", n_lit / 16);
    str("ch11.raw.u16", "%s", hexb(raw, 2));
    fact_num("ch11.raw.bs", SEQLZ_HEADER + n_lit);
    fact_num("ch11.raw.last", raw_len - 1);
    fact_num("ch11.cod.last", cod_len - 1);
    fact_num("ch11.bs", raw_len - SEQLZ_HEADER - n_lit);
    str("ch11.u16hex", "0x%04x", get_unaligned_le16(cod));
    str("ch11.u16", "%s", hexb(cod, 2));
    str("ch11.byte2", "%s", hexb(cod + 2, 1));
    /* the coded page as a dump, 16 bytes to a line, the offset in hex first */
    for (i = 0; i * 16 < cod_len; i++)
        str(key("ch11.dump.%u", i), "%03x  %s", 16 * i, hexb(cod + 16 * i, cod_len - 16 * i < 16 ? cod_len - 16 * i : 16));
    fact_num("ch11.dump_lines", i);

    /* the 4 literals of the worked example: lit_cost and the sums in x */
    for (i = 0; i < 3; i++) {
        u8 b = four[i == 2 ? 3 : i];

        s = scratch();
        *s = 0;
        for (k = 0; k < SEQLZ_LIT_SETS; k++)
            sprintf(s + strlen(s), " %2u", (unsigned int)(T->lit_cost[b][0] >> (8 * k) & 255U));
        str(key("ch11.cost.%02x", b),
            "byte %02x: lengths%s   lit_cost 0x%016llx",
            b,
            s,
            (unsigned long long)T->lit_cost[b][0]);
    }
    for (i = 0; i < 4; i++) {
        x += T->lit_cost[four[i]][0];
        str(key("ch11.x.%u", i + 1), "after %02x:  0x%016llx", four[i], (unsigned long long)x);
    }
    str("ch11.x", "0x%016llx", (unsigned long long)x);
    for (k = 0; k < SEQLZ_LIT_SETS; k++) {
        str(key("ch11.sum.%u.hex", k), "%02x", (unsigned int)(x >> (8 * k) & 255U));
        fact_num(key("ch11.sum.%u.dec", k), x >> (8 * k) & 255U);
    }
    str("ch11.even", "0x%016llx", (unsigned long long)(x & lanes));
    str("ch11.odd", "0x%016llx", (unsigned long long)(x >> 8 & lanes));
    str("ch11.odd32", "0x%08llx", (unsigned long long)((x >> 8 & lanes) >> 32));

    /* the pricing of the page, the table with the fewest bits, the sizes */
    lit_cost(T, page, n_lit, even, odd);
    for (k = 0; k < SEQLZ_LIT_SETS; k++) {
        total[k] = 0;
        for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
            total[k] += lit_bits(even, odd, j, k);
        order[k] = k;
    }
    for (i = 0; i < SEQLZ_LIT_SETS; i++)
        for (k = i + 1; k < SEQLZ_LIT_SETS; k++)
            if (total[order[k]] < total[order[i]]) {
                unsigned int o = order[i];

                order[i] = order[k];
                order[k] = o;
            }
    for (i = 0; i < 3; i++) {
        s = scratch();
        *s = 0;
        for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
            sprintf(s + strlen(s), "%s%u", j ? " " : "", lit_bits(even, odd, j, order[i]));
        fact_num(key("ch11.rank.%u.table", i), order[i]);
        str(key("ch11.rank.%u.bits", i), "%s", s);
        fact_num(key("ch11.rank.%u.total", i), total[order[i]]);
    }
    fact_num("ch11.others.min", total[order[3]]);
    fact_num("ch11.others.max", total[order[SEQLZ_LIT_SETS - 1]]);
    set = order[0];
    if ((cod[2] & 7U) != set)
        fail("code_literals() chose another table than the pricing");
    fact_num("ch11.set", set);
    q[0] = 0;
    for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
        sizes[j] = (lit_bits(even, odd, j, set) + 7U) / 8U;
        coded += sizes[j];
        all |= sizes[j];
    }
    bits = lit_bits(even, odd, 0, set);
    fact_num("ch11.s0.bits", bits);
    fact_num("ch11.s0.bits7", bits + 7);
    fact_num("ch11.s0.bytes", sizes[0]);
    fact_num("ch11.s0.whole", bits / 8);
    fact_num("ch11.s0.rem", bits % 8);
    fact_num("ch11.s0.zeros", 8 - bits % 8);
    s = scratch();
    *s = 0;
    for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
        sprintf(s + strlen(s), "%s%u", j == 0 ? "" : j + 1 == SEQLZ_LIT_STREAMS ? " and " : ", ", sizes[j]);
    str("ch11.sizes", "%s", s);
    s = scratch();
    *s = 0;
    for (j = 0; j < SEQLZ_LIT_STREAMS; j++)
        sprintf(s + strlen(s), "%s%u", j ? ", " : "", sizes[j]);
    str("ch11.sizes_comma", "%s", s);
    fact_num("ch11.coded", coded);
    for (j = 1; j < SEQLZ_LIT_STREAMS; j++)
        if (sizes[j] > sizes[0])
            fail("chapter 11 says stream 0 is the largest");
    str("ch11.max.bin", "0b%s", bin(sizes[0], 32 - (unsigned int)__builtin_clz(sizes[0])));
    all |= 1U << (SEQLZ_SIZE_BITS_MIN - 1U);
    fact_num("ch11.max.all", all);
    fact_num("ch11.max.clz", __builtin_clz(all));
    width = 32U - (unsigned int)__builtin_clz(all);
    fact_num("ch11.width", width);
    fact_num("ch11.header", SEQLZ_LIT_HEADER(width));
    for (j = 0; j <= SEQLZ_LIT_STREAMS; j++) {
        q[j] = j ? q[j - 1] + sizes[j - 1] : SEQLZ_LIT_HEADER(width);
        fact_num(key("ch11.q.%u", j), q[j]);
    }
    str("ch11.q8hex", "0x%x", q[SEQLZ_LIT_STREAMS]);
    fact_num("ch11.s0.lastbyte", q[1] - 1);
    str("ch11.s0.lasthex", "0x%x", q[1] - 1);
    str("ch11.s0.last", "%s", hexb(cod + q[1] - 1, 1));
    str("ch11.s0.lastbits", "%s", bin(cod[q[1] - 1] >> (8 - bits % 8), bits % 8));
    str("ch11.s0.head4", "%s", hexb(cod + q[0], 4));
    str("ch11.s0.head4bin",
        "%s %s %s %s",
        bin(cod[q[0]], 8),
        bin(cod[q[0] + 1], 8),
        bin(cod[q[0] + 2], 8),
        bin(cod[q[0] + 3], 8));
    if (q[SEQLZ_LIT_STREAMS] + raw_len - SEQLZ_HEADER - n_lit != cod_len)
        fail("the streams do not end where the bitstream starts");
    str("ch11.sizes.hex", "%s", hexb(cod + 3, width));
    str("ch11.sizes.le", "0x%llx", (unsigned long long)(get_unaligned_le64(cod) >> 24 & ((1ULL << (8 * width)) - 1)));
    fact_num("ch11.rule.lhs", coded + SEQLZ_LIT_CODED_MIN);
    fact_num("ch11.rule.rhs", n_lit - n_lit / 16);
    fact_num("ch11.saves", raw_len - cod_len);
    fact_num("ch11.saves_pct", (200 * (raw_len - cod_len) + raw_len) / (2 * raw_len));

    /* stream 0 in the chosen table: lit_put() for its first 4 literals, then lit_flush() */
    enc = T->lit[set].enc;
    acc = 0;
    sum = 0;
    for (i = 0; i < 4; i++) {
        u8 b = page[8 * i];

        acc = lit_put(acc, &sum, enc, b);
        str(key("ch11.put.%u.ch", i), "%c", b);
        str(key("ch11.put.%u.enc", i), "0x%x", enc[b]);
        str(key("ch11.put.%u.code", i), "%s", bin(enc[b] >> 8, enc[b] & 255U));
        fact_num(key("ch11.put.%u.len", i), enc[b] & 255U);
        str(key("ch11.put.%u.acc", i), "0x%llx", (unsigned long long)acc);
        str(key("ch11.put.%u.sum", i), "0x%x", sum);
        fact_num(key("ch11.put.%u.low", i), sum & 255U);
    }
    s = scratch();
    *s = 0;
    for (i = 0; i < 5; i++) {
        u8 b = page[8 * i];

        sprintf(s + strlen(s), "%s`%s` (`%c`)", i ? ", " : "", bin(enc[b] >> 8, enc[b] & 255U), b);
    }
    str("ch11.s0.codes5", "%s", s);
    s = scratch();
    *s = 0;
    for (i = 0; i < 4; i++)
        sprintf(s + strlen(s), "%s%s", i ? " " : "", bin(enc[page[8 * i]] >> 8, enc[page[8 * i]] & 255U));
    str("ch11.acc.bits", "%s", s);
    cnt = 0;
    {
        static u8 buf[16];
        unsigned int nb = cnt + (sum & 255U);
        u64 sh = acc << ((64U - nb) & 63U);
        u8* p = lit_flush(buf, buf + sizeof buf, acc, &cnt, &sum);

        fact_num("ch11.flush.n", nb);
        fact_num("ch11.flush.shift", (64U - nb) & 63U);
        str("ch11.flush.sh", "0x%016llx", (unsigned long long)sh);
        str("ch11.flush.w", "0x%016llx", (unsigned long long)__builtin_bswap64(sh));
        str("ch11.flush.stored", "%s", hexb(buf, 8));
        fact_num("ch11.flush.moved", p - buf);
        fact_num("ch11.flush.cnt", cnt);
        str("ch11.flush.part", "%s", hexb(buf + (p - buf), 1));
        str("ch11.flush.partbits", "%s", bin(buf[p - buf] >> (8 - cnt), cnt));
        str("ch11.flush.final", "%s", hexb(buf, (unsigned int)(p - buf)));
    }

    /* lit_sizes(), line by line */
    acc = 0;
    cnt = 0;
    for (j = 0; j < SEQLZ_LIT_STREAMS; j++) {
        char* line = scratch();
        char* head = scratch();

        if (j)
            sprintf(head, "j=%u  acc |= %u << %u", j, sizes[j], cnt);
        else
            sprintf(head, "j=0  acc = %u", sizes[j]);
        acc |= (u64)sizes[j] << cnt;
        cnt += width;
        sprintf(line, "%-29scnt %2u", head, cnt);
        for (; cnt >= 8U; cnt -= 8U) {
            sprintf(line + strlen(line), "  -> write 0x%02x, cnt %u", (unsigned int)(acc & 255U), cnt - 8);
            acc >>= 8;
        }
        str(key("ch11.sz.%u", j), "%s", line);
    }

    /* the tail of lit_streams() for half = 0 and half = 4 */
    for (k = 0; k + 28U < n_lit; k += 32)
        ;
    fact_num("ch11.tail0", k);
    i = 0;
    for (; k < n_lit; k++) {
        char* line = scratch();

        sprintf(line, "k = %u  case %u  stream %u", k, k % 8, k % 8);
        if ((k & 3U) == 3U) {
            sprintf(line + strlen(line), ", then k += 4 -> %u, k++ -> %u", k + 4, k + 5);
            k += 4;
        }
        str(key("ch11.tr.%u", i++), "%s", line);
    }
    str(key("ch11.tr.%u", i++), "k = %u  stop", k);
    fact_num("ch11.tr.lines", i);
    for (k = 4, i = 4; k + 28U < n_lit; k += 32)
        i = k;
    fact_num("ch11.block4", i);
    fact_num("ch11.tail4", k);
    for (; k < n_lit; k++)
        if ((k & 3U) == 3U) {
            fact_num("ch11.tr4.last", k);
            fact_num("ch11.tr4.jump", k + 4);
            break;
        }
    slab_page(page);
    fact_compress(page, raw, 0);
    fact_num("ch11.slab.s0", (get_unaligned_le16(raw) + SEQLZ_LIT_STREAMS - 1U) / SEQLZ_LIT_STREAMS);
    fact_num("ch11.l190", coded_with(190));
    fact_num("ch11.l195", coded_with(195));
}

/* ---- chapter 12: the decoder's tools ---- */

/* the lines of src/seqlz_decompress.c, found from where this file is: docs/book/facts/ */
static void count_lines(void) {
    char path[4096];
    const char* at = strstr(__FILE__, "docs/book/facts/");
    FILE* f;
    unsigned int n = 0;
    int c;

    if (!at)
        fail("not in docs/book/facts/");
    snprintf(path, sizeof path, "%.*ssrc/seqlz_decompress.c", (int)(at - __FILE__), __FILE__);
    f = fopen(path, "r");
    if (!f)
        fail("cannot read src/seqlz_decompress.c");
    while ((c = getc(f)) != EOF)
        n += c == '\n';
    fclose(f);
    fact_num("ch12.lines", n);
}

/* the suffix of an ordinal number: st, nd, rd or th */
static const char* ordinal(unsigned long n) {
    if (n % 100 / 10 == 1)
        return "th";
    return n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
}

/* a decode table: its entries, their size, bytes, rounds of prefetch_lines() and cache lines */
static void table_facts(const char* name, unsigned long bytes, unsigned long esize, unsigned long off) {
    fact_num(key("ch12.%s.entries", name), bytes / esize);
    fact_num(key("ch12.%s.esize", name), esize);
    fact_num(key("ch12.%s.bytes", name), bytes);
    fact_num(key("ch12.%s.rounds", name), bytes / PREFETCH_STEP);
    fact_num(key("ch12.%s.cachelines", name), bytes / 64);
    fact_num(key("ch12.%s.off", name), off);
    fact_num(key("ch12.%s.mod", name), off % 64);
    /* where the struct starts a page: the entries in the line after the loop's, if any */
    if (off % 64) {
        fact_num(key("ch12.%s.first_extra", name), (bytes - off % 64) / esize);
        fact_num(key("ch12.%s.last_entry", name), bytes / esize - 1);
        fact_num(key("ch12.%s.extra_line", name), bytes / 64 + 1);
        str(key("ch12.%s.extra_ord", name), "%lu%s", bytes / 64 + 1, ordinal(bytes / 64 + 1));
    }
}

static void br_facts(const char* pre, const struct seqlz_bit_reader* r, const u8* base) {
    str(key("%s.bits", pre), "0x%016llx", (unsigned long long)r->bits);
    fact_num(key("%s.count", pre), r->count);
    fact_num(key("%s.p", pre), r->p - base);
}

/* a refill of the fast half, with what it loads */
static void refill_facts(unsigned int step, struct seqlz_bit_reader* r, const u8* base) {
    char pre[32];

    snprintf(pre, sizeof pre, "ch12.br.%u", step);
    if (r->end - r->p < 8)
        fail("the made-up stream is too short for the fast half");
    str(key("%s.v", pre), "0x%016llx", (unsigned long long)get_unaligned_be64(r->p));
    str(key("%s.vs", pre), "0x%llx", (unsigned long long)(get_unaligned_be64(r->p) >> r->count));
    fact_num(key("%s.c0", pre), r->count);
    fact_num(key("%s.p0", pre), r->p - base);
    fact_num(key("%s.num", pre), 63 - r->count);
    seqlz_br_refill(r);
    br_facts(pre, r, base);
}

static void chapter12(void) {
    static const u8 made_up[24] = {0xdc, 0x65, 0xef, 0xdb, 0x8a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11, 0x22, 0x33};
    static u8 page[SEQLZ_PAGE], out[CAP], buf[32];
    struct seqlz_bit_reader r = {made_up, made_up + sizeof made_up, 0, 0};
    unsigned int off, len, n_lit, n, x, v;
    u32 e;
    u64 b;

    count_lines();
    table_facts("tok", sizeof_field(struct token_table, decode), sizeof(u32), offsetof(struct seqlz_tables, token.decode));
    table_facts("ll", sizeof_field(struct value_table, decode), sizeof(u32), offsetof(struct seqlz_tables, ll.decode));
    table_facts("ml", sizeof_field(struct value_table, decode), sizeof(u32), offsetof(struct seqlz_tables, ml.decode));
    table_facts("lit", sizeof_field(struct lit_table, decode), sizeof(u16), offsetof(struct seqlz_tables, lit[0].decode));
    fact_num("ch12.lit1.off", offsetof(struct seqlz_tables, lit[1].decode));
    fact_num("ch12.lines_raw", (2 * sizeof_field(struct value_table, decode) + sizeof_field(struct token_table, decode)) / 64);
    fact_num("ch12.lines_coded",
             (2 * sizeof_field(struct value_table, decode) + sizeof_field(struct token_table, decode) +
              sizeof_field(struct lit_table, decode)) /
                 64);
    fact_num("ch12.kib_coded",
             (2 * sizeof_field(struct value_table, decode) + sizeof_field(struct token_table, decode) +
              sizeof_field(struct lit_table, decode)) /
                 1024);

    /* repeat_pattern() for the offsets 1 to 7: the pattern abc..., other bytes after it */
    for (off = 1; off < 8; off++) {
        memset(buf, 'X', sizeof buf);
        memcpy(buf + 8, "abcdefg", off);
        str(key("ch12.rep.%u.pat", off), "%.*s", (int)off, "abcdefg");
        str(key("ch12.rep.%u.mask", off), "0x%llx", ~0ULL >> (64U - 8U * off));
        fact_num(key("ch12.rep.%u.shift", off), 64U - 8U * off);
        str(key("ch12.rep.%u.prod", off), "0x%016llx", (unsigned long long)repeat_pattern(buf + 8 + off, off));
        put_unaligned_le64(repeat_pattern(buf + 8 + off, off), buf + 16);
        str(key("ch12.rep.%u.bytes", off), "%.8s", (const char*)buf + 16);
        fact_num(key("ch12.rep.%u.step", off), repeat_step[off]);
        memset(buf, 0, sizeof buf);
        buf[8] = 1;
        str(key("ch12.rep.%u.mul", off), "0x%016llx", (unsigned long long)repeat_pattern(buf + 8 + off, off));
    }

    /* the bit reader on made-up bytes: refill, drop 15, refill, drop 23, refill */
    refill_facts(1, &r, made_up);
    seqlz_br_drop(&r, 15);
    br_facts("ch12.br.drop1", &r, made_up);
    str("ch12.br.drop1.low", "0x%llx", (unsigned long long)(r.bits & ((1ULL << (64 - r.count)) - 1)));
    refill_facts(2, &r, made_up);
    seqlz_br_drop(&r, 23);
    br_facts("ch12.br.drop2", &r, made_up);
    refill_facts(3, &r, made_up);

    fact_num("ch12.base_max", 1U << (SEQLZ_LEN_SYMBOLS - 1 - SEQLZ_LEN_LOG_BASE));
    /* the page of ab, as chapter 12 shows it with 4 KiB pages: the byte half, the token, the ml value */
    if (SEQLZ_PAGE != 4096)
        return;
    ab_page(page);
    len = fact_compress(page, out, 0);
    n_lit = get_unaligned_le16(out);
    r = (struct seqlz_bit_reader){out + SEQLZ_HEADER + n_lit, out + len, 0, 0};
    seqlz_br_refill(&r);
    str("ch12.ab.bits", "0x%016llx", (unsigned long long)r.bits);
    fact_num("ch12.ab.count", r.count);
    fact_num("ch12.ab.at_end", r.p == r.end);
    e = T->token.decode[r.bits >> (64U - SEQLZ_TOKEN_BITS)];
    if (e >> TOK_ESCAPE_AT)
        fail("the token of the page of ab is escaped: chapter 12 drops its code and offset at once");
    n = TOK_DROP(e);
    fact_num("ch12.ab.tokdrop", n);
    seqlz_br_drop(&r, n);
    str("ch12.ab.bits2", "0x%016llx", (unsigned long long)r.bits);
    fact_num("ch12.ab.count2", r.count);
    e = T->ml.decode[r.bits >> (64U - SEQLZ_MAX_BITS)];
    n = e & 15U;
    x = (e >> 4) & 15U;
    fact_num("ch12.ab.idx", r.bits >> (64U - SEQLZ_MAX_BITS));
    str("ch12.ab.idxhex", "0x%02llx", (unsigned long long)(r.bits >> (64U - SEQLZ_MAX_BITS)));
    str("ch12.ab.entry", "0x%08x", e);
    str("ch12.ab.low", "0x%02x", e & 255U);
    fact_num("ch12.ab.n", n);
    fact_num("ch12.ab.x", x);
    str("ch12.ab.code", "%s", bin(r.bits >> (64U - n), n));
    fact_num("ch12.ab.sym", SEQLZ_LEN_LOG_BASE + x);
    str("ch12.ab.basehex", "0x%x", (e >> 8) & 0xffffU);
    fact_num("ch12.ab.base", (e >> 8) & 0xffffU);
    b = r.bits << n;
    str("ch12.ab.shl", "0x%016llx", (unsigned long long)b);
    str("ch12.ab.shr1", "0x%016llx", (unsigned long long)(b >> 1));
    fact_num("ch12.ab.shr2", 63U - x);
    str("ch12.ab.xhex", "0x%llx", (unsigned long long)((b >> 1) >> (63U - x)));
    fact_num("ch12.ab.xval", (b >> 1) >> (63U - x));
    str("ch12.ab.xbits", "%s", bin((b >> 1) >> (63U - x), x));
    v = seqlz_br_value(&r, &T->ml);
    if (v != ((e >> 8) & 0xffffU) + ((b >> 1) >> (63U - x)))
        fail("seqlz_br_value() is not the sum of its parts");
    fact_num("ch12.ab.value", v);
    fact_num("ch12.ab.dropped", n + x);
    fact_num("ch12.ab.count3", r.count);
    fact_num("ch12.ml_tok", SEQLZ_ML_CAP + 4);
    fact_num("ch12.ab.match", v + SEQLZ_ML_CAP + 4);
    e = T->ml.decode[0];
    str("ch12.ml0.entry", "0x%08x", e);
    fact_num("ch12.ml0.n", e & 15U);
    fact_num("ch12.ml0.x", (e >> 4) & 15U);
    fact_num("ch12.ml0.base", (e >> 8) & 0xffffU);
    str("ch12.ml0.code", "%s", bin(0, e & 15U));
}

int main(void) {
    T = fact_tables();
    chapter10();
    chapter11();
    chapter12();
    facts_end();
    return 0;
}
