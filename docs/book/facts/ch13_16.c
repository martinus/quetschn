// SPDX-License-Identifier: MIT OR GPL-2.0-only
/*
 * ch13_16.c: the facts of chapters 13 (the literal streams), 14 (seqlz_decode()) and 16 (one page
 * through every function). It includes the codec's two .c files to reach their static functions, and
 * two macros of seqlz_decompress.c's sequence loop are redefined to record each sequence of one
 * decode, see ch14_seq() and ch14_slow(): which path it took is what the real loop did.
 */
// facts: also 16k
#include <stdarg.h>

#include "facts.h"

#include "seqlz_compress.c"

static int ch14_on;
static unsigned int ch14_seq(unsigned int d,
                             unsigned int nl,
                             unsigned int tok,
                             unsigned int off,
                             unsigned int skip,
                             int past_d,
                             int past_lit,
                             int past_in);
static void ch14_slow(void);

/*
 * TOK_ML() is used once in seqlz_decompress.c, right before the fast path's condition: the
 * sequence's numbers are there. SEQLZ_LL_CAP is used once there, the first line of the slow path.
 */
#undef TOK_ML
#define TOK_ML(e)                                      \
    (ch14_on ? ch14_seq((unsigned int)(d - (u8*)dst),  \
                        nl,                            \
                        (e),                           \
                        off,                           \
                        skip,                          \
                        d > d_fast,                    \
                        (unsigned long)lit > lit_fast, \
                        (unsigned long)br.p > in_fast) \
             : 0U,                                     \
     ((e) >> TOK_ML_AT) & 63U)
#undef SEQLZ_LL_CAP
#define SEQLZ_LL_CAP (ch14_on ? ch14_slow() : (void)0, (1U << SEQLZ_LL_BITS) - 1U)

#include "seqlz_decompress.c"

/* a key made with printf's format */
static inline const char* key(const char* fmt, ...) {
    static char k[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(k, sizeof k, fmt, ap);
    va_end(ap);
    return k;
}

static inline void kstr(const char* k, const char* fmt, ...) {
    char v[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    fact_str(k, v);
}

static inline void die(const char* what) {
    fprintf(stderr, "ch13_16: %s\n", what);
    exit(1);
}

/* the n low bits of v as 0 and 1 */
static inline const char* binary(unsigned long long v, unsigned int n) {
    static char buf[4][72];
    static unsigned int k;
    char* b = buf[k++ % 4];
    unsigned int i;

    for (i = 0; i < n; i++)
        b[i] = (char)('0' + (v >> (n - 1 - i) & 1U));
    b[n] = 0;
    return b;
}

/* n numbers as "1, 2 and 3" (and_ = 1) or "1, 2, 3" */
static inline void klist(const char* k, const unsigned int* v, unsigned int n, int and_) {
    char buf[512];
    unsigned int i, w = 0;

    buf[0] = 0;
    for (i = 0; i < n; i++)
        w += (unsigned int)snprintf(buf + w, sizeof buf - w, "%s%u", !i ? "" : and_ && i == n - 1 ? " and " : ", ", v[i]);
    fact_str(k, buf);
}

/* the literal table chapter 16's page is coded with, checked where the encoder picks it */
#define CH16_SET 3U

/* ---- chapter 14's constants, also for 16 KiB pages */

static void ch14_constants(void) {
    const struct seqlz_tables* t = fact_tables();
    unsigned int s, coded = 0, off, len, max_byte = 0, min_step = 8, min_step_off = 0, max_value = 0;

    fact_num("ch13.k.rounds", SEQLZ_LIT_ROUNDS);
    fact_num("ch13.k.streams", SEQLZ_LIT_STREAMS);
    fact_num("ch13.k.lit_bits", SEQLZ_LIT_BITS);
    fact_num("ch14.need", SEQLZ_TOKEN_BITS + QUETSCHN_PAGE_BITS);
    fact_num("ch14.class3_bits", SEQLZ_RAW_BITS(3));
    for (s = 0, off = 0; s < SEQLZ_OFF_CLASSES; s++)
        if (SEQLZ_RAW_BITS(s) > off)
            off = SEQLZ_RAW_BITS(s);
    fact_num("ch14.max_off_bits", off);
    fact_num("ch14.fast_seq_bits", SEQLZ_TOKEN_BITS + off);
    fact_num("ch14.fast_two_bits", 2 * (SEQLZ_TOKEN_BITS + off));
    fact_num("ch14.fast_left", 56 - 2 * (SEQLZ_TOKEN_BITS + off));
    fact_num("ch14.fast_after_one", 56 - (SEQLZ_TOKEN_BITS + off));
    fact_num("ch14.room.page", FAST_BLOCK_ROOM);
    fact_num("ch14.room.lit", FAST_LIT_ROOM);
    fact_num("ch14.room.input", FAST_IN_ROOM);
    fact_num("ch14.fast_nl", SEQLZ_LL_CAP - 1);
    fact_num("ch14.fast_len", SEQLZ_ML_CAP - 1 + 4);
    fact_num("ch14.pf.token", sizeof(t->token.decode) / L1_CACHE_BYTES + 1);
    fact_num("ch14.pf.len", sizeof(t->ll.decode) / L1_CACHE_BYTES + 1);
    fact_num("ch14.token_bytes", sizeof(t->token.decode));
    for (s = 0; s < SEQLZ_TOKEN_SYMBOLS; s++)
        coded += t->token.enc[s] != 0;
    fact_num("ch14.tokens_coded", coded);
    fact_num("ch14.no_token", (1U << SEQLZ_ESCAPE_BITS) - SEQLZ_TOKEN_SYMBOLS);

    /* the furthest byte copy_match_fast() writes, for offsets below 8 and the fast path's lengths */
    for (off = 1; off < 8; off++) {
        if (repeat_step[off] < min_step) {
            min_step = repeat_step[off];
            min_step_off = off;
        }
        for (len = 4; len <= SEQLZ_ML_CAP - 1 + 4; len++) {
            unsigned int fill;

            for (fill = 0x5a; fill <= 0xa5; fill += 0x4b) {
                u8 buf[128];
                unsigned int k;

                for (k = 0; k < 64; k++)
                    buf[k] = (u8)(k * 37 + 11);
                memset(buf + 64, (int)fill, 64);
                copy_match_fast(buf + 64, off, len);
                for (k = 127; k >= 64; k--)
                    if (buf[k] != fill)
                        break;
                if (k - 64 > max_byte)
                    max_byte = k - 64;
            }
        }
    }
    fact_num("ch14.cmf.max_byte", max_byte);
    fact_num("ch14.cmf.room", max_byte + 1);
    fact_num("ch14.cmf.max_write", SEQLZ_LL_CAP - 1 + max_byte + 1);
    fact_num("ch14.cmf.min_step", min_step);
    fact_num("ch14.cmf.min_step_off", min_step_off);
    fact_num("ch14.cmf.five_stores", 4 * min_step + 8);
    fact_num("ch14.cmf.step1", repeat_step[1]);
    fact_num("ch14.cmf.step2", repeat_step[2]);
    fact_num("ch14.cmf.step4", repeat_step[4]);
    fact_num("ch14.cmf.five_stores_8", 4 * 8 + 8);

    /* the largest length value the ml table can give */
    for (s = 0; s < (1U << SEQLZ_MAX_BITS); s++) {
        u32 e = t->ml.decode[s];
        unsigned int v = ((e >> 8) & 0xffffU) + (1U << ((e >> 4) & 15U)) - 1U;

        if (v > max_value)
            max_value = v;
    }
    fact_num("ch14.max_value", max_value);
    fact_num("ch14.max_len", max_value + SEQLZ_ML_CAP + 4);
    fact_num("ch14.max_rounds", SEQLZ_BLOCK_SIZE / 4);
}

static struct ch14_rec {
    unsigned int d, nl, len, off, skip, value_ll, value_ml, past_d, past_lit, past_in, slow;
} ch14_recs[SEQLZ_MAX_SEQUENCES];
static unsigned int ch14_n;

static unsigned int ch14_seq(unsigned int d,
                             unsigned int nl,
                             unsigned int tok,
                             unsigned int off,
                             unsigned int skip,
                             int past_d,
                             int past_lit,
                             int past_in) {
    struct ch14_rec* r = &ch14_recs[ch14_n++];

    r->d = d;
    r->nl = nl;
    r->len = ((tok) >> TOK_ML_AT) & 63U;
    r->off = off;
    r->skip = skip;
    r->value_ll = nl == 15;
    r->value_ml = r->len == SEQLZ_ML_CAP + 4;
    r->past_d = past_d;
    r->past_lit = past_lit;
    r->past_in = past_in;
    r->slow = 0;
    return 0;
}

static void ch14_slow(void) {
    ch14_recs[ch14_n - 1].slow = 1;
}

#if QUETSCHN_PAGE_BITS == 12

/* ---- chapter 13: the literal streams of chapter 11's page */

static void ch13(void) {
    static u8 page[SEQLZ_BLOCK_SIZE], src[2 * SEQLZ_BLOCK_SIZE], out[SEQLZ_BLOCK_SIZE], scratch[SEQLZ_SCRATCH],
        mine[SEQLZ_SCRATCH];
    const struct seqlz_tables* t = fact_tables();
    unsigned int n = fact_compress((letters_page(page), page), src, 1), n_lit, width, sz[8], i, j, k, rest, steps, r, ks[64],
                 nk = 0, starts[8], round0_bits = 0, last7 = 0;
    const u8 *start[8], *ip[8], *q, *end = src + n;
    u64 b[8], total, b_codes = 0;
    const u16* lt;
    u8 h[SEQLZ_SIZE_BITS_MAX + 4U] = {0};

    n_lit = get_unaligned_le16(src);
    if (!(n_lit & SEQLZ_LIT_CODED))
        die("letters: not coded");
    n_lit &= SEQLZ_LIT_CODED - 1U;
    width = SEQLZ_SIZE_BITS_MIN + ((src[2] >> SEQLZ_LIT_WIDTH_AT) & 7U);
    q = src + SEQLZ_LIT_HEADER(width);
    lt = t->lit[src[2] & 7U].decode;
    fact_num("ch13.set", src[2] & 7U);
    fact_num("ch13.width", width);
    fact_num("ch13.mask", (1U << width) - 1U);
    fact_hex("ch13.size_hex", src + SEQLZ_LIT_HEADER(0), width);
    total = lit_stream_sizes(src + SEQLZ_LIT_HEADER(0), width, q, sz, start);
    memcpy(h, src + SEQLZ_LIT_HEADER(0), width);
    for (k = 0; k < 8; k++) {
        unsigned int at = k * width / 8U, sh = k * width % 8U, le = get_unaligned_le32(h + at);

        fact_num(key("ch13.sz.%u.bit", k), k * width);
        fact_num(key("ch13.sz.%u.byte", k), at);
        fact_num(key("ch13.sz.%u.shift", k), sh);
        kstr(key("ch13.sz.%u.le", k), "0x%08x", le);
        kstr(key("ch13.sz.%u.le_short", k), "0x%x", le);
        fact_num(key("ch13.sz.%u.size", k), sz[k]);
        /* the zeros at the top of a load are h's when it reaches past the size bytes */
        fact_num(key("ch13.sz.%u.past", k), at + 4 > width);
        if ((le >> sh & ((1U << width) - 1U)) != sz[k])
            die("ch13: a size");
        starts[k] = (unsigned int)(start[k] - src);
    }
    kstr("ch13.sz.0.low", "0x%02x", h[0]);
    klist("ch13.starts", starts, 8, 1);
    fact_num("ch13.total", total);
    fact_num("ch13.max_load_byte", 7 * SEQLZ_SIZE_BITS_MAX / 8);
    fact_num("ch13.max_load_bit", 7 * SEQLZ_SIZE_BITS_MAX);

    /* decode_literals(), its steps as they are, with the record of stream 0's first two refills */
    for (i = 0; i < 8; i++) {
        ip[i] = start[i];
        b[i] = 1;
    }
    for (k = 0; k + 40 <= n_lit; k += 40) {
        ks[nk++] = k;
        for (i = 0; i < 8; i++) {
            const u8* before = ip[i];
            u64 old = b[i];

            b[i] = lit_refill(&ip[i], b[i], end);
            if (i == 7)
                last7 = (unsigned int)(ip[7] - src);
            if (i == 0 && k == 0) {
                fact_num("ch13.s0.start", (unsigned int)(start[0] - src));
                fact_hex("ch13.s0.bytes", start[0], 11);
                fact_hex("ch13.s0.first8", start[0], 8);
                fact_num("ch13.s0.odd", start[0][7] & 1);
                kstr("ch13.s0.bits0", "0x%016llx", (unsigned long long)b[0]);
            }
            if (i == 0 && k == 40) {
                unsigned int used = (unsigned int)__builtin_ctzll(old);
                u64 load = get_unaligned_be64(before + (used >> 3));

                if (old != b_codes)
                    die("ch13: stream 0 between refills");
                fact_num("ch13.r.used", used);
                fact_num("ch13.r.bytes", used >> 3);
                fact_num("ch13.r.at", (unsigned int)(before + (used >> 3) - src));
                fact_num("ch13.r.shift", used & 7U);
                fact_hex("ch13.r.load_hex", before + (used >> 3), 8);
                kstr("ch13.r.load", "0x%016llx", (unsigned long long)load);
                kstr("ch13.r.or1", "0x%016llx", (unsigned long long)(load | 1U));
                kstr("ch13.r.bits", "0x%016llx", (unsigned long long)b[0]);
                fact_num("ch13.r.valid_before", 63 - used);
                fact_num("ch13.r.valid_after", 63 - (used & 7U));
                fact_num("ch13.r.new_bits", (63 - (used & 7U)) - (63 - used));
                fact_str("ch13.r.dropped", binary(before[used >> 3] >> (8 - (used & 7U)), used & 7U));
                /* the figure: 9 units a bit from x = 100 */
                fact_num("ch13.fig.w0", 63 * 9);
                fact_num("ch13.fig.c0", 100 + 63 * 9 / 2);
                fact_num("ch13.fig.w1", (63 - used) * 9);
                fact_num("ch13.fig.c1", 100 + (63 - used) * 9 / 2);
                fact_num("ch13.fig.s1", 100 + (63 - used) * 9);
                fact_num("ch13.fig.z1", 100 + (63 - used) * 9 + 9);
                fact_num("ch13.fig.wz1", used * 9);
                fact_num("ch13.fig.cz1", 100 + (63 - used) * 9 + 9 + used * 9 / 2);
                fact_num("ch13.fig.l1", 100 + (63 - used) * 9 + 4);
                fact_num("ch13.fig.w2", (63 - (used & 7U)) * 9);
                fact_num("ch13.fig.s2", 100 + (63 - (used & 7U)) * 9);
                fact_num("ch13.fig.z2", 100 + (63 - (used & 7U)) * 9 + 9);
                fact_num("ch13.fig.wz2", (used & 7U) * 9);
                fact_num("ch13.fig.l2", 100 + (63 - (used & 7U)) * 9 + 4);
            }
        }
        for (j = 0; j < 5; j++)
            for (i = 0; i < 8; i++) {
                unsigned int at = k + 8 * j + i;

                if (i == 0 && k == 0) {
                    unsigned int e = lt[b[0] >> 54];

                    fact_str(key("ch13.c.%u.top", j), binary(b[0] >> 54, 10));
                    kstr(key("ch13.c.%u.entry", j), "0x%04x", e);
                    fact_num(key("ch13.c.%u.len", j), e & 63U);
                    kstr(key("ch13.c.%u.lit", j), "%c", (char)(e >> 8));
                    round0_bits += e & 63U;
                    if (j == 4) {
                        /* the last of the 5 codes: its bits in the byte before the refill's first */
                        unsigned int first = round0_bits - (e & 63U), cut = round0_bits / 8 * 8;

                        fact_num("ch13.c.last_split", first < cut);
                        fact_str("ch13.c.last_prev", binary(b[0] >> (64 - (cut - first)), cut - first));
                        fact_num("ch13.c.last_prev_byte", (unsigned int)(start[0] - src) + cut / 8 - 1);
                    }
                }
                b[i] = lit_decode(&mine[at], b[i], lt, 63U);
                if (i == 0 && k == 0 && j == 4) {
                    b_codes = b[0];
                    fact_num("ch13.c.bits", round0_bits);
                    kstr("ch13.c.after", "0x%016llx", (unsigned long long)b[0]);
                }
            }
    }
    klist("ch13.rounds", ks, nk, 0);
    fact_num("ch13.k_last_round", ks[nk - 1]);
    fact_num("ch13.k_last_end", ks[nk - 1] + 40);
    fact_num("ch13.k_end", k);
    rest = n_lit - k;
    steps = rest / 8;
    r = rest % 8;
    fact_num("ch13.rest", rest);
    fact_num("ch13.steps", steps);
    fact_num("ch13.rest_r", r);
    for (i = 0; i < 8; i++) {
        b[i] = lit_refill(&ip[i], b[i], end);
        if (i == 7)
            last7 = (unsigned int)(ip[7] - src);
    }
    for (j = 0; j < steps; j++)
        for (i = 0; i < 8; i++)
            b[i] = lit_decode(&mine[k + 8 * j + i], b[i], lt, 63U);
    k += 8 * steps;
    for (i = 0; i < 8; i++)
        b[i] = lit_decode(&mine[k + i], b[i], lt, (0U - (r > i)) & 63U);
    for (i = 0; i < 8; i++)
        if (!lit_stream_ends(start[i], sz[i], ip[i], b[i]))
            die("ch13: a stream does not end");

    /* the same as the real decoder, also the bytes behind the literals */
    memset(scratch, 0, sizeof scratch);
    if (seqlz_decode(src, n, out, scratch) || memcmp(scratch, mine, k + 8) || memcmp(out, page, SEQLZ_BLOCK_SIZE))
        die("ch13: the replica differs from seqlz_decode()");

    fact_num("ch13.n", n);
    fact_num("ch13.bitstream", n - (unsigned int)(q + total - src));
    fact_num("ch13.s7.start", starts[7]);
    fact_num("ch13.s7.size", sz[7]);
    fact_num("ch13.s7.last_refill", last7);
    fact_num("ch13.s7.before_end", n - last7);
    fact_num("ch13.s7.tail", n - last7 < 8);
    fact_num("ch13.last_lit", k);
    fact_num("ch13.last_lit_byte", page[k]);
    fact_num("ch13.extra_first", k + r);
    fact_num("ch13.extra_last", k + 7);
    {
        /* which streams decoded which byte behind the literals, "streams 1 to 6 got `20`, a space, ..." */
        char buf[256];
        unsigned int w = 0, a = r;

        buf[0] = 0;
        while (a < 8) {
            unsigned int z = a;
            u8 c = mine[k + a];

            while (z + 1 < 8 && mine[k + z + 1] == c)
                z++;
            if (z > a)
                w += (unsigned int)snprintf(buf + w, sizeof buf - w, "%sstreams %u to %u got `%02x`", w ? ", " : "", a, z, c);
            else
                w += (unsigned int)snprintf(buf + w, sizeof buf - w, "%sstream %u got `%02x`", w ? ", " : "", a, c);
            if (c == ' ')
                w += (unsigned int)snprintf(buf + w, sizeof buf - w, ", a space");
            else if (c > ' ' && c < 127)
                w += (unsigned int)snprintf(buf + w, sizeof buf - w, ", a `%c`", c);
            a = z + 1;
        }
        fact_str("ch13.extra_desc", buf);
    }

    /* stream 0's end, and three damaged pages */
    {
        unsigned int used = (unsigned int)(8 * (ip[0] - start[0])) + (unsigned int)__builtin_ctzll(b[0]);
        unsigned int last = starts[0] + sz[0] - 1, left = 8 * sz[0] - used, d;
        static u8 bad[2 * SEQLZ_BLOCK_SIZE];

        fact_num("ch13.e.size", sz[0]);
        fact_num("ch13.e.size_bits", 8 * sz[0]);
        fact_num("ch13.e.ip", (unsigned int)(ip[0] - start[0]));
        fact_num("ch13.e.ctz", (unsigned int)__builtin_ctzll(b[0]));
        fact_num("ch13.e.used", used);
        fact_num("ch13.e.left", left);
        fact_num("ch13.e.last", last);
        kstr("ch13.e.last_val", "0x%02x", src[last]);
        kstr("ch13.e.mask", "0x%02x", (1U << left) - 1U);
        fact_num("ch13.e.masked", src[last] & ((1U << left) - 1U));
        kstr("ch13.e.bad_val", "0x%02x", src[last] | 1U);
        fact_num("ch13.e.bad_masked", (src[last] | 1U) & ((1U << left) - 1U));
        if (left == 0 || src[last] & 1U)
            die("ch13: the damaged byte is no change");
        for (d = 0; d < 3; d++) {
            memcpy(bad, src, n);
            if (d == 0) {
                bad[last] |= 1U;
            } else {
                /* stream 0's size one less or one more, in its width bits from byte 3 */
                unsigned int nsz = d == 1 ? sz[0] - 1 : sz[0] + 1;
                u32 v = get_unaligned_le32(bad + 3);
                unsigned int b;

                v = (v & ~((1U << width) - 1U)) | nsz;
                for (b = 0; b < 4; b++)
                    bad[3 + b] = (u8)(v >> 8 * b);
                fact_num(key("ch13.e.size%u", d), nsz);
                fact_num(key("ch13.e.size%u_bits", d), 8 * nsz);
                fact_num(key("ch13.e.left%u", d), (long long)(8 * nsz) - used);
            }
            if (seqlz_decode(bad, n, out, scratch) != -EINVAL)
                die("ch13: a damaged page is accepted");
        }
    }

    /* the literal tables' place in struct seqlz_tables */
    {
        unsigned int at = (unsigned int)((const u8*)t->lit[0].decode - (const u8*)t), mod = at % 64;

        fact_num("ch13.lt.mod8", at % 8);
        fact_num("ch13.lt.lines", sizeof(t->lit[0].decode) / 64 + (mod != 0));
        fact_num("ch13.lt.prefetches", sizeof(t->lit[0].decode) / L1_CACHE_BYTES + 1);
        fact_num("ch13.lt.loop_prefetches", sizeof(t->lit[0].decode) / L1_CACHE_BYTES);
        fact_num("ch13.lt.last_entry", (sizeof(t->lit[0].decode) - mod) / 2);
    }
    fact_num("ch13.max_write", SEQLZ_BLOCK_SIZE + SEQLZ_LIT_STREAMS - 1);
}

/* ---- chapter 14: tokens, the page of ab, the trace of chapter 16's page */

/* one of the three tokens of the offset table */
static void ch14_token(const char* name, unsigned int s, unsigned int off) {
    const struct seqlz_tables* t = fact_tables();
    unsigned int cls = s / 512, raw_bits = SEQLZ_RAW_BITS(cls), te = t->token.enc[s], n = te >> TOK_ENC_LEN_AT;
    unsigned int ob = cls ? off >> SEQLZ_OFF_SHIFT(cls) : 0, raw, back;
    u32 e;
    u64 bits;

    if (!te)
        die("ch14: a token of the table has no code");
    e = t->token.decode[(te & TOK_ENC_CODE_MASK) << (SEQLZ_TOKEN_BITS - n)];
    if (e != token_entry(s, n))
        die("ch14: a token's entry");
    /* the code, the offset bits, then 0101... */
    bits = (u64)(te & TOK_ENC_CODE_MASK) << (64 - n);
    if (raw_bits)
        bits |= (u64)ob << (64 - n - raw_bits);
    bits |= 0x5555555555555555ULL >> (n + raw_bits);
    back = TOK_BACK(e);
    raw = (unsigned int)((bits << TOK_N(e)) >> back) << TOK_SHIFT(e);
    if (cls && raw != off)
        die("ch14: a token's offset");
    fact_num(key("ch14.tk.%s.s", name), s);
    fact_num(key("ch14.tk.%s.ll", name), s % 16);
    fact_num(key("ch14.tk.%s.ml", name), s / 16 % 32 + 4);
    fact_num(key("ch14.tk.%s.cls", name), cls);
    fact_num(key("ch14.tk.%s.off", name), off);
    fact_str(key("ch14.tk.%s.code", name), binary(te & TOK_ENC_CODE_MASK, n));
    fact_num(key("ch14.tk.%s.n", name), n);
    fact_str(key("ch14.tk.%s.ob", name), binary(ob, raw_bits));
    fact_num(key("ch14.tk.%s.raw_bits", name), raw_bits);
    fact_num(key("ch14.tk.%s.ob_val", name), ob);
    kstr(key("ch14.tk.%s.entry", name), "0x%08x", e);
    fact_num(key("ch14.tk.%s.N", name), TOK_N(e));
    fact_num(key("ch14.tk.%s.BACK", name), TOK_BACK(e));
    fact_num(key("ch14.tk.%s.SHIFT", name), TOK_SHIFT(e));
    fact_num(key("ch14.tk.%s.DROP", name), TOK_DROP(e));
    kstr(key("ch14.tk.%s.bits", name), "0x%016llx", (unsigned long long)bits);
    kstr(key("ch14.tk.%s.shl", name), "0x%016llx", (unsigned long long)(bits << TOK_N(e)));
    kstr(key("ch14.tk.%s.shr", name), "0x%llx", (unsigned long long)((bits << TOK_N(e)) >> back));
    fact_num(key("ch14.tk.%s.raw", name), raw);
    kstr(key("ch14.tk.%s.raw32", name), "0x%08x", (unsigned int)((bits << TOK_N(e)) >> back));
    fact_num(key("ch14.tk.%s.index", name), (unsigned int)(bits >> (64 - SEQLZ_TOKEN_BITS)));
    fact_num(key("ch14.tk.%s.fill", name), 1U << (SEQLZ_TOKEN_BITS - n));
    fact_num(key("ch14.tk.%s.idx_lo", name), (te & TOK_ENC_CODE_MASK) << (SEQLZ_TOKEN_BITS - n));
    fact_num(key("ch14.tk.%s.idx_hi", name), (((te & TOK_ENC_CODE_MASK) + 1) << (SEQLZ_TOKEN_BITS - n)) - 1);
}

static void ch14(void) {
    static u8 page[SEQLZ_BLOCK_SIZE], src[2 * SEQLZ_BLOCK_SIZE], out[SEQLZ_BLOCK_SIZE], scratch[SEQLZ_SCRATCH];
    const struct seqlz_tables* t = fact_tables();
    unsigned int n, s, i, fast = 0, slow = 0;
    u32 e;

    /* token 513, escaped */
    if (t->token.enc[513])
        die("ch14: token 513 has a code");
    e = token_entry(513, 0);
    kstr("ch14.t513.entry", "0x%08x", e);
    fact_num("ch14.t513.N", TOK_N(e));
    fact_num("ch14.t513.BACK", TOK_BACK(e));
    fact_num("ch14.t513.DROP", TOK_DROP(e));
    fact_num("ch14.t513.ll", TOK_LL(e));
    fact_num("ch14.t513.ml", ((e) >> TOK_ML_AT) & 63U);
    fact_num("ch14.t513.cls", 513 / 512);
    fact_num("ch14.t513.raw_bits", SEQLZ_RAW_BITS(1));
    fact_str("ch14.t513.bin", binary(513, SEQLZ_ESCAPE_BITS));

    ch14_token("a", 576, 2);
    ch14_token("b", 2097, 24);
    ch14_token("c", 49, 0);

    /* the page of ab, the decoder's steps done by hand with its own helpers */
    {
        struct seqlz_bit_reader br;
        unsigned int idx, tok_s, raw, v, vn, vx, vbase, len;
        u32 ve;

        ab_page(page);
        n = fact_compress(page, src, 0);
        br = (struct seqlz_bit_reader){.p = src + 2 + 2, .end = src + n};
        if (get_unaligned_le16(src) != 2)
            die("ch14: ab has not 2 literals");
        fact_num("ch14.ab.dfast", SEQLZ_BLOCK_SIZE - FAST_BLOCK_ROOM);
        fact_num("ch14.ab.lit_fast_below", FAST_LIT_ROOM - n);
        seqlz_br_refill(&br);
        kstr("ch14.ab.bits", "0x%016llx", (unsigned long long)br.bits);
        fact_num("ch14.ab.count", br.count);
        fact_num("ch14.ab.loads", (unsigned int)(br.p - (src + 4)));
        idx = (unsigned int)(br.bits >> (64 - SEQLZ_TOKEN_BITS));
        e = t->token.decode[idx];
        fact_str("ch14.ab.idx_bin", binary(idx, SEQLZ_TOKEN_BITS));
        fact_num("ch14.ab.idx", idx);
        kstr("ch14.ab.entry", "0x%08x", e);
        fact_num("ch14.ab.escaped", e >> TOK_ESCAPE_AT);
        if (e >> TOK_ESCAPE_AT) {
            /* the escape, as lines 605 to 617 take it: drop its code, refill, the token in 12 bits */
            fact_num("ch14.ab.esc_n", TOK_DROP(e));
            seqlz_br_drop(&br, TOK_DROP(e));
            fact_num("ch14.ab.esc_count", br.count);
            seqlz_br_refill(&br);
            fact_num("ch14.ab.esc_count_refill", br.count);
            tok_s = (unsigned int)(br.bits >> (64U - SEQLZ_ESCAPE_BITS));
            fact_str("ch14.ab.esc_bits", binary(tok_s, SEQLZ_ESCAPE_BITS));
            seqlz_br_drop(&br, SEQLZ_ESCAPE_BITS);
            fact_num("ch14.ab.esc_count2", br.count);
            e = token_entry(tok_s, 0);
            kstr("ch14.ab.entry2", "0x%08x", e);
        } else {
            for (tok_s = 0; tok_s < SEQLZ_TOKEN_SYMBOLS; tok_s++)
                if (t->token.enc[tok_s] && token_entry(tok_s, t->token.enc[tok_s] >> TOK_ENC_LEN_AT) == e)
                    break;
        }
        fact_num("ch14.ab.tok", tok_s);
        fact_num("ch14.ab.N", TOK_N(e));
        fact_num("ch14.ab.ll", TOK_LL(e));
        fact_num("ch14.ab.ml", ((e) >> TOK_ML_AT) & 63U);
        fact_num("ch14.ab.cls", tok_s / 512);
        fact_num("ch14.ab.BACK", TOK_BACK(e));
        fact_num("ch14.ab.DROP", TOK_DROP(e));
        fact_num("ch14.ab.value_bit", e >> TOK_VALUE_AT & 1U);
        raw = (unsigned int)((br.bits << TOK_N(e)) >> TOK_BACK(e)) << TOK_SHIFT(e);
        fact_num("ch14.ab.raw_n", 64 - TOK_BACK(e));
        fact_str("ch14.ab.raw_bin", binary(raw >> TOK_SHIFT(e), 64 - TOK_BACK(e)));
        fact_num("ch14.ab.raw_sent", raw >> TOK_SHIFT(e));
        fact_num("ch14.ab.SHIFT", TOK_SHIFT(e));
        fact_num("ch14.ab.raw", raw);
        seqlz_br_drop(&br, TOK_DROP(e));
        fact_num("ch14.ab.count2", br.count);
        fact_num("ch14.ab.readable", n - 2);
        seqlz_br_refill(&br);
        ve = t->ml.decode[br.bits >> (64U - SEQLZ_MAX_BITS)];
        vn = ve & 15U;
        vx = (ve >> 4) & 15U;
        vbase = (ve >> 8) & 0xffffU;
        fact_str("ch14.ab.vcode", binary(br.bits >> (64 - vn), vn));
        v = seqlz_br_value(&br, &t->ml);
        fact_num("ch14.ab.vsym", seqlz_len_symbol(v, &i));
        fact_num("ch14.ab.vx", vx);
        fact_num("ch14.ab.vextra", v - vbase);
        fact_num("ch14.ab.value", v);
        fact_num("ch14.ab.count3", br.count);
        len = (((e) >> TOK_ML_AT) & 63U) + v;
        fact_num("ch14.ab.len", len);
        fact_num("ch14.ab.room", SEQLZ_BLOCK_SIZE - TOK_LL(e));
        /* copy_match() with off < 8: stores while 8 bytes fit, then byte by byte */
        fact_num("ch14.ab.store_end", (SEQLZ_BLOCK_SIZE - TOK_LL(e)) / 8 * 8 - 1);
        fact_num("ch14.ab.bytewise", len - (SEQLZ_BLOCK_SIZE - TOK_LL(e)) / 8 * 8);
        if (len != SEQLZ_BLOCK_SIZE - TOK_LL(e) || tok_s == SEQLZ_TOKEN_SYMBOLS || br.count >= 8)
            die("ch14: the page of ab");
    }

    /* chapter 16's page, decoded with the sequence loop recording */
    slab_page(page);
    n = fact_compress(page, src, 1);
    ch14_n = 0;
    ch14_on = 1;
    s = (unsigned int)seqlz_decode(src, n, out, scratch);
    ch14_on = 0;
    if (s || memcmp(out, page, SEQLZ_BLOCK_SIZE))
        die("ch14: chapter 16's page");
    for (i = 0; i < ch14_n; i++) {
        const struct ch14_rec* r = &ch14_recs[i];
        /* the fast path's condition, as a check that the hooks saw the paths the loop took */
        unsigned int cond = !(r->value_ll || r->value_ml) && !r->past_d && !r->past_lit && !r->past_in;

        if (cond == r->slow)
            die("ch14: the recorded path is not the fast path's condition");
        fast += !r->slow;
        slow += r->slow;
    }
    fact_num("ch14.seqs", ch14_n);
    fact_num("ch14.fast", fast);
    fact_num("ch14.slow", slow);
    {
        unsigned int lines = 0, with_value = 0, ll_value = 0, ml_value = 0, nov[8], n_nov = 0, past = 1;

        for (i = 0; i < ch14_n; i++) {
            const struct ch14_rec* r = &ch14_recs[i];
            char line[160];
            const char* why;

            if (r->slow) {
                with_value += r->value_ll || r->value_ml;
                ll_value += r->value_ll;
                ml_value += r->value_ml;
                if (!r->value_ll && !r->value_ml) {
                    if (n_nov < 8)
                        nov[n_nov++] = i;
                    past &= r->past_in;
                }
            }
            if (i == ch14_n - 1)
                why = "the last match, to the end of the page";
            else if (r->slow && r->value_ll && r->value_ml)
                why = "a literal and a match length value follow";
            else if (r->slow && r->value_ll)
                why = "ll 15: a literal length value follows";
            else if (r->slow && r->value_ml)
                why = "a match length value follows";
            else if (r->slow && r->past_in)
                why = "br.p is past in_fast: fewer than 8 bytes of input left";
            else if (r->slow && r->past_d)
                why = "d is past d_fast: fewer than 64 bytes of the page left";
            else if (r->slow)
                why = "lit is past lit_fast";
            else
                why = r->skip ? "skip=1: no refill" : "skip=0: refill";
            snprintf(line,
                     sizeof line,
                     "%s d=%-4u nl=%-2u len=%-2u off=%-5u %s",
                     r->slow ? "SLOW" : "FAST",
                     r->d,
                     r->nl,
                     r->len,
                     r->off,
                     why);
            if (i < 8 || i + 5 >= ch14_n)
                fact_str(key("ch14.trace.%u", lines++), line);
        }
        {
            static struct seqlz_state st;
            static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
            unsigned int ns = seqlz_find(&st, page, seq, 2), left = seq[ns - 1].literals;
            char end[32];

            if (seq[ns - 1].match || ns != ch14_n + 1)
                die("ch14: the last sequence");
            snprintf(end, sizeof end, "END  d=%u", SEQLZ_BLOCK_SIZE - left);
            kstr(key("ch14.trace.%u", lines++),
                 "%-35s%u literals left for %u bytes: the last sequence, no token",
                 end,
                 left,
                 left);
        }
        fact_num("ch16.slow_value", with_value);
        fact_num("ch16.slow_ll", ll_value);
        fact_num("ch16.slow_ml", ml_value);
        fact_num("ch16.slow_novalue_n", n_nov);
        fact_num("ch16.slow_novalue_past_in", past);
        klist("ch16.slow_novalue", nov, n_nov, 1);
    }
}

/* ---- chapter 16: the page through every function */

/* the zsmalloc model of bench/zsmalloc_cost.cpp, for 4 KiB pages: the class an object of len bytes is in */
struct zs_class {
    unsigned int size, pages, objs;
};

static struct zs_class zs_kept[256];
static unsigned int zs_of[256], zs_n, zs_huge;

static void zs_init(void) {
    unsigned int num = (4096 - 32 + 15) / 16 + 1, i;

    for (i = num; i-- > 0;) {
        unsigned int size = 32 + i * 16 < 4096 ? 32 + i * 16 : 4096, pages = 1, objs, p, waste = ~0U;

        if (size & (size - 1))
            for (p = 1; p <= 8; p++)
                if (p * 4096 % size < waste) {
                    waste = p * 4096 % size;
                    pages = p;
                }
        objs = pages * 4096 / size;
        if (pages != 1 && objs != 1 && !zs_huge)
            zs_huge = size - 7;
        if (zs_n && zs_kept[zs_n - 1].pages == pages && zs_kept[zs_n - 1].objs == objs) {
            zs_of[i] = zs_n - 1;
            continue;
        }
        zs_kept[zs_n++] = (struct zs_class){size, pages, objs};
        zs_of[i] = zs_n - 1;
    }
}

static unsigned int zs_index(unsigned int len) {
    unsigned int size = (len >= zs_huge ? 4096 : len) + 8;

    return size > 32 ? (size - 32 + 15) / 16 : 0;
}

static const struct zs_class* zs_class(unsigned int len) {
    return &zs_kept[zs_of[zs_index(len)]];
}

static double zs_cost(unsigned int len) {
    return (double)(zs_class(len)->pages * 4096) / zs_class(len)->objs;
}

/* the bits of a length value */
static unsigned int value_bits(const struct value_table* vt, unsigned int v) {
    unsigned int x, s = seqlz_len_symbol(v, &x);

    return (vt->enc[s] >> 16 & 15) + x;
}

struct ch16_seq {
    unsigned int pos, ll, ml, off, cls, tok, clen, escaped, raw, vbits, bits;
};

/* the sequences of a page and their bits, as chapter 16's program computes them */
static unsigned int ch16_seqs(const u8* page, struct ch16_seq* out) {
    static struct seqlz_state st;
    static struct seqlz_sequence seq[SEQLZ_MAX_SEQUENCES];
    const struct seqlz_tables* t = fact_tables();
    unsigned int n = seqlz_find(&st, page, seq, 2), i, pos = 0, last = 1;

    for (i = 0; i < n; i++) {
        struct ch16_seq* q = &out[i];
        unsigned int te;

        memset(q, 0, sizeof *q);
        q->pos = pos;
        q->ll = seq[i].literals;
        q->ml = seq[i].match;
        q->off = seq[i].offset;
        if (!q->ml)
            break;
        q->cls = seqlz_off_class(q->off, last, &q->raw);
        q->tok = seqlz_token(q->ll, q->ml, q->cls);
        te = t->token.enc[q->tok];
        q->escaped = !te;
        q->clen = te ? te >> TOK_ENC_LEN_AT : (t->token.enc[SEQLZ_ESCAPE] >> TOK_ENC_LEN_AT) + SEQLZ_ESCAPE_BITS;
        if (q->ll >= SEQLZ_LL_CAP)
            q->vbits += value_bits(&t->ll, q->ll - SEQLZ_LL_CAP);
        if (q->ml - 4 >= SEQLZ_ML_CAP)
            q->vbits += value_bits(&t->ml, q->ml - 4 - SEQLZ_ML_CAP);
        q->bits = q->clen + q->raw + q->vbits;
        pos += q->ll + q->ml;
        last = q->off;
    }
    return n;
}

/* the bits and bytes of the 8 streams of n literals with literal table k */
static unsigned int stream_bits(const u8* lits, unsigned int n, unsigned int k, unsigned int* bytes, unsigned int* sizes) {
    const struct seqlz_tables* t = fact_tables();
    unsigned int j, m, bits = 0;

    *bytes = 0;
    for (j = 0; j < 8; j++) {
        unsigned int sb = 0;

        for (m = j; m < n; m += 8)
            sb += t->lit[k].enc[lits[m]] & 255U;
        bits += sb;
        *bytes += (sb + 7) / 8;
        if (sizes)
            sizes[j] = (sb + 7) / 8;
    }
    return bits;
}

static void ch16(void) {
    static u8 page[SEQLZ_BLOCK_SIZE], raw[2 * SEQLZ_BLOCK_SIZE], coded[2 * SEQLZ_BLOCK_SIZE];
    static struct ch16_seq q[SEQLZ_MAX_SEQUENCES];
    static const unsigned int show[] = {0, 1, 2, 5, 6, 7, 8, 11, 12, 13, 24, 98, 99, 177, 178, 179};
    const struct seqlz_tables* t = fact_tables();
    unsigned int n, i, k, n_raw, n_coded, n_lit, bits = 0, tok_bits = 0, off_bits = 0, v_bits = 0, matched = 0;
    unsigned int per_class[SEQLZ_OFF_CLASSES] = {0}, sizes[8], starts[8], best = ~0U, set = 0, bytes, w;
    unsigned int part_seqs[4] = {0}, part_lits[4] = {0}, part_bits[4] = {0}, text_end, c4_offs[16], n_c4 = 0;
    unsigned int c5_ptr = 0, c5_mode = 0, c5_mode_n = 0, lines = 0;
    unsigned long long lit_part_bits[4] = {0};
    static const unsigned int part_start[5] = {0, 64, 64 + 32 * 48, 0, SEQLZ_BLOCK_SIZE};
    unsigned int pstart[5];

    slab_page(page);
    n = ch16_seqs(page, q);
    memcpy(pstart, part_start, sizeof pstart);
    for (text_end = SEQLZ_BLOCK_SIZE; text_end > 0 && !page[text_end - 1]; text_end--)
        ;
    pstart[3] = text_end;
    for (i = pstart[2]; i < text_end; i++)
        lines += page[i] == '\n';
    fact_num("ch16.page.records_end", pstart[2]);
    fact_num("ch16.page.records_last", pstart[2] - 1);
    fact_num("ch16.page.text_end", text_end);
    fact_num("ch16.page.text_last", text_end - 1);
    fact_num("ch16.page.text_len", text_end - pstart[2]);
    fact_num("ch16.page.text_lines", lines);
    fact_num("ch16.page.zeros", SEQLZ_BLOCK_SIZE - text_end);
    /* the figure of the page, 680 units for 4096 bytes from x = 20 */
    kstr("ch16.fig.p.64", "%.1f", 20 + 64 * 680.0 / 4096);
    kstr("ch16.fig.p.w0", "%.1f", 64 * 680.0 / 4096);
    kstr("ch16.fig.p.w1", "%.1f", (pstart[2] - 64) * 680.0 / 4096);
    kstr("ch16.fig.p.x2", "%.1f", 20 + pstart[2] * 680.0 / 4096);
    kstr("ch16.fig.p.w2", "%.1f", (text_end - pstart[2]) * 680.0 / 4096);
    kstr("ch16.fig.p.x3", "%.1f", 20 + text_end * 680.0 / 4096);
    kstr("ch16.fig.p.w3", "%.1f", (SEQLZ_BLOCK_SIZE - text_end) * 680.0 / 4096);
    kstr("ch16.fig.p.c1", "%.0f", 20 + (64 + pstart[2]) / 2 * 680.0 / 4096);
    kstr("ch16.fig.p.c2", "%.1f", 20 + (pstart[2] + text_end) / 2.0 * 680.0 / 4096);
    kstr("ch16.fig.p.c3", "%.1f", 20 + (text_end + SEQLZ_BLOCK_SIZE) / 2.0 * 680.0 / 4096);

    for (i = 0, n_lit = 0; i < n; i++) {
        unsigned int p;

        n_lit += q[i].ll;
        for (p = 0; p < 3 && (q[i].pos >= SEQLZ_BLOCK_SIZE ? SEQLZ_BLOCK_SIZE - 1 : q[i].pos) >= pstart[p + 1]; p++)
            ;
        part_seqs[p]++;
        part_lits[p] += q[i].ll;
        part_bits[p] += q[i].bits;
        for (k = 0; k < q[i].ll; k++) {
            unsigned int at = q[i].pos + k, lp;

            for (lp = 0; lp < 3 && at >= pstart[lp + 1]; lp++)
                ;
            lit_part_bits[lp] += t->lit[CH16_SET].enc[page[at]] & 255U;
        }
        if (!q[i].ml)
            continue;
        bits += q[i].bits;
        tok_bits += q[i].clen;
        off_bits += q[i].raw;
        v_bits += q[i].vbits;
        matched += q[i].ml;
        per_class[q[i].cls]++;
        if (q[i].cls == 4 && q[i].pos >= 64 && q[i].pos < pstart[2]) {
            for (k = 0; k < n_c4 && c4_offs[k] != q[i].off; k++)
                ;
            if (k == n_c4 && n_c4 < 16)
                c4_offs[n_c4++] = q[i].off;
        }
        if (q[i].cls == 5) {
            unsigned int at = q[i].pos + q[i].ll, m = 0;

            c5_ptr += at >= 64 && at < pstart[2] && (at - 64) % 32 == 1 && q[i].ml == 7;
            for (k = 0; k < n; k++)
                m += q[k].ml && q[k].cls == 5 && q[k].off == q[i].off;
            if (m > c5_mode_n) {
                c5_mode_n = m;
                c5_mode = q[i].off;
            }
        }
    }
    for (i = 0; i < n_c4; i++)
        for (k = i + 1; k < n_c4; k++)
            if (c4_offs[k] < c4_offs[i]) {
                unsigned int x = c4_offs[i];

                c4_offs[i] = c4_offs[k];
                c4_offs[k] = x;
            }
    fact_num("ch16.seqs", n);
    fact_num("ch16.matches", n - 1);
    fact_num("ch16.matched", matched);
    fact_num("ch16.lits", n_lit);
    fact_num("ch16.bits", bits);
    fact_num("ch16.bytes", (bits + 7) / 8);
    fact_num("ch16.full_bytes", bits / 8);
    fact_num("ch16.pad_bits", bits % 8);
    fact_num("ch16.pad", (8 - bits % 8) % 8);
    fact_num("ch16.tok_bits", tok_bits);
    fact_num("ch16.off_bits", off_bits);
    fact_num("ch16.v_bits", v_bits);
    for (i = 0; i < SEQLZ_OFF_CLASSES; i++)
        fact_num(key("ch16.class.%u", i), per_class[i]);
    /* the class 4 offsets of the records' sequences */
    klist("ch16.c4_offs", c4_offs, n_c4, 1);
    fact_num("ch16.c5_mode", c5_mode);
    fact_num("ch16.c5_mode_n", c5_mode_n);
    fact_num("ch16.c5_mode_records", c5_mode / 32);
    fact_num("ch16.c5_ptr", c5_ptr);
    for (i = 0; i < 4; i++) {
        fact_num(key("ch16.part.%u.seqs", i), part_seqs[i]);
        fact_num(key("ch16.part.%u.lits", i), part_lits[i]);
        fact_num(key("ch16.part.%u.bits", i), part_bits[i]);
    }
    kstr("ch16.part.records_per_seq", "%.1f", (double)part_bits[1] / part_seqs[1]);
    fact_num("ch16.part.records_pct", 100 * part_bits[1] / bits);
    kstr("ch16.lit0.text", "%.2f", (double)lit_part_bits[2] / part_lits[2]);
    kstr("ch16.lit0.records", "%.2f", (double)lit_part_bits[1] / part_lits[1]);
    for (i = 128, k = 1; i < 256; i++)
        k &= (t->lit[CH16_SET].enc[i] & 255U) == 10;
    fact_num("ch16.lit0.high_all10", k);
    for (k = 0; k < sizeof show / sizeof show[0]; k++) {
        const struct ch16_seq* s = &q[show[k]];

        i = show[k];
        fact_num(key("ch16.s.%u.pos", i), s->pos);
        fact_num(key("ch16.s.%u.ll", i), s->ll);
        fact_num(key("ch16.s.%u.ml", i), s->ml);
        fact_num(key("ch16.s.%u.off", i), s->off);
        fact_num(key("ch16.s.%u.cls", i), s->cls);
        fact_num(key("ch16.s.%u.tok", i), s->tok);
        fact_num(key("ch16.s.%u.clen", i), s->clen);
        fact_num(key("ch16.s.%u.esc", i), s->escaped);
        fact_num(key("ch16.s.%u.raw", i), s->raw);
        fact_num(key("ch16.s.%u.vbits", i), s->vbits);
        fact_num(key("ch16.s.%u.bits", i), s->bits);
        fact_num(key("ch16.s.%u.ob", i), s->cls ? s->off >> SEQLZ_OFF_SHIFT(s->cls) : 0);
        fact_num(key("ch16.s.%u.end", i), s->pos + s->ll + s->ml);
        fact_str(key("ch16.s.%u.ob_bin", i), binary(s->cls ? s->off >> SEQLZ_OFF_SHIFT(s->cls) : 0, s->raw));
    }
    fact_num("ch16.s.98_99_bits", q[98].bits + q[99].bits);
    fact_num("ch16.s.prev_off_7", q[6].off);
    fact_num("ch16.s.177.cand", q[177].pos + q[177].ll + q[177].ml - 2);
    {
        /* sequence 178's length value */
        unsigned int v = q[178].ml - 4 - SEQLZ_ML_CAP, x, sym = seqlz_len_symbol(v, &x);

        fact_num("ch16.v178.value", v);
        fact_num("ch16.v178.len", 32 - (unsigned int)__builtin_clz(v));
        fact_num("ch16.v178.top", x);
        fact_num("ch16.v178.sym", sym);
        fact_num("ch16.v178.code_len", t->ml.enc[sym] >> 16 & 15);
        fact_num("ch16.v178.extra", v - (1U << x));
        fact_num("ch16.v178.top_val", 1U << x);
    }
    /* sequence 0's bits, and how encode_emit() writes them */
    {
        unsigned int esc = t->token.enc[SEQLZ_ESCAPE], v = q[0].ml - 4 - SEQLZ_ML_CAP, x, sym = seqlz_len_symbol(v, &x);
        unsigned int word = (esc & TOK_ENC_CODE_MASK) << SEQLZ_ESCAPE_BITS | q[0].tok;

        kstr("ch16.e0.word", "0x%04x", word);
        fact_num("ch16.e0.word_bits", (esc >> TOK_ENC_LEN_AT) + SEQLZ_ESCAPE_BITS);
        fact_num("ch16.e0.ml4", q[0].ml - 4);
        fact_num("ch16.e0.value", v);
        fact_num("ch16.e0.sym", sym);
        fact_str("ch16.e0.acc",
                 key("%s %s %s",
                     binary(esc & TOK_ENC_CODE_MASK, esc >> TOK_ENC_LEN_AT),
                     binary(q[0].tok, SEQLZ_ESCAPE_BITS),
                     binary(t->ml.enc[sym] & 0xffff, t->ml.enc[sym] >> 16 & 15)));
        fact_num("ch16.e0.full", q[0].bits / 8);
        fact_num("ch16.e0.left", q[0].bits % 8);
        kstr("ch16.e0.top", "0x%02x", word >> ((esc >> TOK_ENC_LEN_AT) + SEQLZ_ESCAPE_BITS - 8) & 0xff);
        kstr("ch16.e0.next", "0x%02x", word & 0xff);
        fact_num("ch16.e0.lit_end", SEQLZ_HEADER + q[0].ll);
        fact_num("ch16.e0.room", 2 * SEQLZ_BLOCK_SIZE - SEQLZ_HEADER);
        fact_num("ch16.enc_min", SEQLZ_HEADER + ENC_ROOM);
        fact_num("ch16.hash_entries", 1U << SEQLZ_HASH_BITS);
        fact_num("ch16.hash_bytes", sizeof(((struct seqlz_state*)0)->table));
        fact_num("ch16.byte12", page[12]);
        kstr("ch16.byte12_hex", "0x%02x", page[12]);
        kstr("ch16.byte64_hex", "0x%02x", page[64]);
        fact_num("ch16.e0.store", q[0].pos + q[0].ll + q[0].ml - 2);
        fact_num("ch16.e0.zeros_to", q[0].pos + q[0].ll + q[0].ml - 1);
    }

    /* raw literals */
    n_raw = fact_compress(page, raw, 0);
    fact_num("ch16.raw.len", n_raw);
    kstr("ch16.raw.pct", "%.1f", 100.0 * n_raw / SEQLZ_BLOCK_SIZE);
    fact_num("ch16.raw.lit_end", SEQLZ_HEADER + n_lit);
    fact_hex("ch16.raw.u16", raw, 2);
    fact_num("ch16.raw.gap", 2 * SEQLZ_BLOCK_SIZE - (bits + 7) / 8 - (SEQLZ_HEADER + n_lit));
    fact_num("ch16.raw.copies", (bits + 7) / 8 / 8);
    fact_num("ch16.raw.singles", (bits + 7) / 8 % 8);
    fact_hex("ch16.raw.b4", raw + SEQLZ_HEADER + n_lit, 4);
    fact_num("ch16.raw.buf_start", 2 * SEQLZ_BLOCK_SIZE - (bits + 7) / 8);
    if (n_raw != SEQLZ_HEADER + n_lit + (bits + 7) / 8)
        die("ch16: raw length");

    /* the literal tables as code_literals() weighs them */
    for (k = 0; k < SEQLZ_LIT_SETS; k++) {
        unsigned int b = stream_bits(raw + SEQLZ_HEADER, n_lit, k, &bytes, NULL);

        if (b < best) {
            best = b;
            set = k;
        }
    }
    stream_bits(raw + SEQLZ_HEADER, n_lit, set, &bytes, sizes);
    fact_num("ch16.lt.set", set);
    if (set != CH16_SET)
        die("ch16: the page takes another literal table than CH16_SET");
    fact_num("ch16.lt.bits", best);
    fact_num("ch16.lt.bytes", bytes);
    kstr("ch16.lt.per_lit", "%.2f", (double)best / n_lit);
    klist("ch16.lt.sizes", sizes, 8, 1);
    klist("ch16.lt.sizes_sp", sizes, 8, 0);
    kstr("ch16.lt.sizes_plain",
         "%u %u %u %u %u %u %u %u",
         sizes[0],
         sizes[1],
         sizes[2],
         sizes[3],
         sizes[4],
         sizes[5],
         sizes[6],
         sizes[7]);
    fact_num("ch16.lt.chunk", SEQLZ_LIT_STREAMS * LIT_COST_RUN);
    fact_num("ch16.lt.chunks", n_lit / (SEQLZ_LIT_STREAMS * LIT_COST_RUN));
    fact_num("ch16.lt.chunk_rest", n_lit % (SEQLZ_LIT_STREAMS * LIT_COST_RUN));
    for (k = 0, w = 1U << (SEQLZ_SIZE_BITS_MIN - 1); k < 8; k++)
        w |= sizes[k];
    fact_num("ch16.lt.or", w);
    fact_str("ch16.lt.or_bin", binary(w, 32 - (unsigned int)__builtin_clz(w)));
    fact_num("ch16.lt.or_min", 1U << (SEQLZ_SIZE_BITS_MIN - 1));
    w = 32 - (unsigned int)__builtin_clz(w);
    fact_num("ch16.lt.width", w);
    fact_num("ch16.lt.header", SEQLZ_LIT_HEADER(w));
    fact_num("ch16.lt.rule_l", bytes + SEQLZ_LIT_CODED_MIN);
    fact_num("ch16.lt.n16", n_lit / 16);
    fact_num("ch16.lt.rule_r", n_lit - n_lit / 16);
    fact_num("ch16.lt.more", n_lit / 8 + 1);
    fact_num("ch16.lt.less", n_lit / 8);
    fact_num("ch16.lt.n_more", n_lit % 8);
    for (k = 0, starts[0] = SEQLZ_LIT_HEADER(w); k < 8; k++)
        if (k)
            starts[k] = starts[k - 1] + sizes[k - 1];
    klist("ch16.lt.starts", starts, 8, 0);
    for (k = 0; k < 8; k++)
        fact_num(key("ch16.lt.start.%u", k), starts[k]);
    fact_num("ch16.lt.end", starts[0] + bytes);

    /* coded literals */
    n_coded = fact_compress(page, coded, 1);
    fact_num("ch16.coded.len", n_coded);
    kstr("ch16.coded.pct", "%.1f", 100.0 * n_coded / SEQLZ_BLOCK_SIZE);
    kstr("ch16.coded.u16", "0x%04x", get_unaligned_le16(coded));
    kstr("ch16.coded.nlit_hex", "0x%04x", get_unaligned_le16(coded) & (SEQLZ_LIT_CODED - 1U));
    fact_hex("ch16.coded.u16_bytes", coded, 2);
    kstr("ch16.coded.byte2", "0x%02x", coded[2]);
    fact_num("ch16.coded.wfield", w - SEQLZ_SIZE_BITS_MIN);
    fact_hex("ch16.coded.sizes_hex", coded + SEQLZ_LIT_HEADER(0), w);
    fact_num("ch16.coded.saved", n_raw - n_coded);
    if (coded[2] != (set | (w - SEQLZ_SIZE_BITS_MIN) << SEQLZ_LIT_WIDTH_AT) || n_coded != starts[0] + bytes + (bits + 7) / 8 ||
        memcmp(coded + starts[0] + bytes, raw + SEQLZ_HEADER + n_lit, 4))
        die("ch16: the coded page is not as the chapter says");
    /* the figure of the compressed page, 680 units for its bytes from x = 20 */
    for (k = 0; k <= 9; k++) {
        unsigned int at = k == 0 ? 0 : k == 9 ? starts[0] + bytes : starts[k - 1];
        unsigned int to = k == 9 ? n_coded : k == 8 ? starts[0] + bytes : starts[k];

        if (k == 0)
            to = starts[0];
        kstr(key("ch16.fig.c.x%u", k), "%.1f", 20 + at * 680.0 / n_coded);
        kstr(key("ch16.fig.c.w%u", k), "%.1f", (to - at) * 680.0 / n_coded);
        kstr(key("ch16.fig.c.m%u", k), "%.1f", 20 + (at + to) / 2.0 * 680.0 / n_coded);
    }
    kstr("ch16.fig.c.lits_mid", "%.0f", 20 + (starts[0] + starts[0] + bytes) / 2.0 * 680.0 / n_coded);

    /* the decoder's view */
    {
        unsigned int rounds = n_lit / 40, rest = n_lit - 40 * rounds, idx;
        struct seqlz_bit_reader br = {.p = coded + starts[0] + bytes, .end = coded + n_coded};
        u32 e;

        fact_num("ch16.d.rounds", rounds);
        fact_num("ch16.d.round_lits", 40 * rounds);
        fact_num("ch16.d.rest", rest);
        fact_num("ch16.d.steps", rest / 8);
        fact_num("ch16.d.r", rest % 8);
        fact_num("ch16.d.r_last", rest % 8 - 1);
        fact_num("ch16.d.extra_lo", n_lit);
        fact_num("ch16.d.extra_hi", n_lit + 8 - rest % 8 - 1);
        fact_num("ch16.d.first_quiet", rest % 8);
        seqlz_br_refill(&br);
        fact_num("ch16.d.p_moved", (unsigned int)(br.p - (coded + starts[0] + bytes)));
        fact_num("ch16.d.count", br.count);
        idx = (unsigned int)(br.bits >> (64 - SEQLZ_TOKEN_BITS));
        e = t->token.decode[idx];
        fact_str("ch16.d.idx_bin", binary(idx, SEQLZ_TOKEN_BITS));
        fact_num("ch16.d.idx", idx);
        kstr("ch16.d.entry", "0x%08x", e);
        fact_num("ch16.d.esc_len", TOK_DROP(e));
        fact_num("ch16.d.esc_fill", 1U << (SEQLZ_TOKEN_BITS - TOK_DROP(e)));
        if (!(e >> TOK_ESCAPE_AT))
            die("ch16: the first token is no escape");
        seqlz_br_drop(&br, TOK_DROP(e));
        seqlz_br_refill(&br);
        idx = (unsigned int)(br.bits >> (64U - SEQLZ_ESCAPE_BITS));
        fact_str("ch16.d.esc_bin", binary(idx, SEQLZ_ESCAPE_BITS));
        fact_num("ch16.d.esc_idx", idx);
        e = token_entry(idx, 0);
        kstr("ch16.d.esc_entry", "0x%08x", e);
        fact_num("ch16.d.esc_ll", TOK_LL(e));
        fact_num("ch16.d.esc_ml", e >> TOK_ML_AT & 63U);
        fact_num("ch16.d.esc_back", TOK_BACK(e));
        fact_num("ch16.d.esc_drop", TOK_DROP(e));
        fact_num("ch16.d.m0_stores", (q[0].ml + 7) / 8);
        fact_num("ch16.d.m0_written", (q[0].ml + 7) / 8 * 8);
        fact_num("ch16.d.t1_fill", 1U << (SEQLZ_TOKEN_BITS - q[1].clen));
        fact_str("ch16.d.t1_code", binary(t->token.enc[q[1].tok] & TOK_ENC_CODE_MASK, q[1].clen));
        fact_num("ch16.d.t1_back", 64 - q[1].raw);
    }

    /* zsmalloc */
    zs_init();
    {
        const struct zs_class *c = zs_class(n_coded), *cr = zs_class(n_raw);
        unsigned int idx = zs_index(n_coded), hi = n_coded, lo = n_coded, merged = 0;

        fact_num("ch16.zs.size", n_coded + 8);
        kstr("ch16.zs.frac", "%.2f", (n_coded + 8 - 32) / 16.0);
        fact_num("ch16.zs.idx", idx);
        fact_num("ch16.zs.nominal", 32 + idx * 16);
        fact_num("ch16.zs.class", c->size);
        fact_num("ch16.zs.pages", c->pages);
        fact_num("ch16.zs.objs", c->objs);
        kstr("ch16.zs.div", "%.2f", (double)c->pages * 4096 / c->size);
        kstr("ch16.zs.cost", "%.1f", zs_cost(n_coded));
        for (k = 0; k < (4096 - 32 + 15) / 16 + 1; k++)
            if (zs_of[k] == zs_of[idx] && 32 + k * 16 != c->size) {
                merged = 32 + k * 16;
            }
        fact_num("ch16.zs.merged", merged);
        while (zs_class(hi + 1) == c)
            hi++;
        while (zs_class(lo - 1) == c)
            lo--;
        fact_num("ch16.zs.grow", hi - n_coded);
        fact_num("ch16.zs.hi", hi);
        fact_num("ch16.zs.shrink", n_coded - (lo - 1));
        fact_num("ch16.zs.lo", lo - 1);
        fact_num("ch16.zs.lower", zs_class(lo - 1)->size);
        fact_num("ch16.zs.raw_size", n_raw + 8);
        fact_num("ch16.zs.raw_class", cr->size);
        fact_num("ch16.zs.raw_pages", cr->pages);
        fact_num("ch16.zs.raw_pages_bytes", cr->pages * 4096);
        fact_num("ch16.zs.raw_objs", cr->objs);
        kstr("ch16.zs.raw_cost", "%.1f", zs_cost(n_raw));
        kstr("ch16.zs.saved", "%.1f", zs_cost(n_raw) - zs_cost(n_coded));
    }

    /* the page without its text */
    {
        static struct ch16_seq q2[SEQLZ_MAX_SEQUENCES];
        unsigned int n2, lits = 0, b2 = 0, best2 = ~0U, set2 = 0, bytes2 = 0, len2, len2c;

        memset(page + pstart[2], 0, SEQLZ_BLOCK_SIZE - pstart[2]);
        n2 = ch16_seqs(page, q2);
        for (i = 0; i < n2; i++) {
            lits += q2[i].ll;
            b2 += q2[i].bits;
        }
        len2 = fact_compress(page, raw, 0);
        for (k = 0; k < SEQLZ_LIT_SETS; k++) {
            unsigned int by, b = stream_bits(raw + SEQLZ_HEADER, lits, k, &by, NULL);

            if (b < best2) {
                best2 = b;
                set2 = k;
                bytes2 = by;
            }
        }
        len2c = fact_compress(page, coded, 1);
        fact_num("ch16.nt.lits", lits);
        fact_num("ch16.nt.bytes", (b2 + 7) / 8);
        fact_num("ch16.nt.len", len2);
        fact_num("ch16.nt.set", set2);
        fact_num("ch16.nt.coded", bytes2);
        fact_num("ch16.nt.rule_l", bytes2 + SEQLZ_LIT_CODED_MIN);
        fact_num("ch16.nt.n16", lits / 16);
        fact_num("ch16.nt.rule_r", lits - lits / 16);
        fact_num("ch16.nt.kept_raw", len2c == len2);
    }
}

#endif

int main(void) {
    ch14_constants();
#if QUETSCHN_PAGE_BITS == 12
    ch13();
    ch14();
    ch16();
#endif
    facts_end();
    return 0;
}
