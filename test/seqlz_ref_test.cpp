// SPDX-License-Identifier: MIT OR GPL-2.0-only
// seqlz.h is C, written for the kernel
extern "C" {
#include "seqlz.h"
}
#include "seqlz_ref.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

namespace {

seqlz_ref_tables format_tables() {
    return {
        seqlz_default_lengths.token, seqlz_default_lengths.ll, seqlz_default_lengths.ml, seqlz_lit_sets, seqlz_own_lengths};
}

// The bits of each stream's size, from byte 2 of a page with coded literals.
unsigned int size_width(std::vector<unsigned char> const& c) {
    return SEQLZ_SIZE_BITS_MIN + ((c[2] >> SEQLZ_LIT_WIDTH_AT) & 7U);
}

std::unique_ptr<seqlz_ref> make_ref() {
    auto r = std::make_unique<seqlz_ref>();
    auto const t = format_tables();
    REQUIRE(seqlz_ref_init(r.get(), &t, QUETSCHN_PAGE_BITS) == 0);
    return r;
}

// the tables of seqlz_compress() and seqlz_decode(), the ones compiled in
void fast_tables() {
    REQUIRE(seqlz_tables_init(&seqlz_default_lengths) == 0);
}

// Pages of several kinds, so that every class of offset, long lengths, escapes and coded literals occur.
std::vector<unsigned char> make_page(std::mt19937_64& rng, int kind) {
    static char const* const words[] = {"the ", "page ", "swap ", "memory ", "of ", "zram ", "kernel ", "and "};
    auto p = std::vector<unsigned char>(SEQLZ_PAGE);
    switch (kind % 7) {
    case 0: // random bytes, raw literals and few matches
        for (auto& b : p) {
            b = static_cast<unsigned char>(rng());
        }
        break;
    case 1: // text, literals worth coding
        for (std::size_t k = 0; k < p.size();) {
            for (char const* w = words[rng() % 8]; *w && k < p.size(); ++w) {
                p[k++] = static_cast<unsigned char>(*w);
            }
        }
        break;
    case 2: // pointers: multiples of 8 as offsets
        for (std::size_t k = 0; k + 8 <= p.size(); k += 8) {
            auto const v = 0x7f0000001000ULL + (rng() % 64) * 16;
            std::memcpy(&p[k], &v, 8);
        }
        break;
    case 3: // a short pattern with noise: small offsets and long matches
        for (std::size_t k = 0; k < p.size(); ++k) {
            p[k] = static_cast<unsigned char>(rng() % 50 == 0 ? rng() : k % 4);
        }
        break;
    case 4: // copies of earlier pieces at any distance, long and short
        for (std::size_t k = 0; k < p.size();) {
            if (k > 16 && rng() % 2) {
                auto const off = 1 + rng() % k;
                auto const n = std::min<std::size_t>(p.size() - k, 4 + rng() % (rng() % 4 ? 20 : 600));
                for (std::size_t i = 0; i < n; ++i, ++k) {
                    p[k] = p[k - off];
                }
            } else {
                p[k++] = static_cast<unsigned char>(rng() % 16);
            }
        }
        break;
    case 5: { // letters in their frequencies in English and few repeats: literals that any tables trained
              // on real pages code well, not only the ones of a particular training
        static char const letters[] = "eeeeeeeeeeeettttttttaaaaaaaoooooooiiiiiiinnnnnnnsssssshhhhhhrrrrrrdddd"
                                      "llllcccuuummmwwffggyyppbbvk";
        for (auto& b : p) {
            b = static_cast<unsigned char>(letters[rng() % (sizeof letters - 1)]);
        }
        break;
    }
    default: // mostly zeros, as in many swapped pages
        for (int i = 0; i < 40; ++i) {
            p[rng() % p.size()] = static_cast<unsigned char>(rng());
        }
    }
    return p;
}

std::vector<unsigned char> compress(std::vector<unsigned char> const& page, unsigned level) {
    auto st = std::make_unique<seqlz_state>();
    auto c = std::vector<unsigned char>(2 * SEQLZ_PAGE);
    auto const len = seqlz_compress(st.get(), page.data(), c.data(), static_cast<unsigned>(c.size()), level);
    REQUIRE(len > 0);
    c.resize(len);
    return c;
}

// Both decoders on the same input: the same verdict, and for a valid page the same bytes. Returns the
// page, empty for an invalid input.
std::vector<unsigned char> check_same(seqlz_ref const* r, std::vector<unsigned char> const& c) {
    auto fast = std::vector<unsigned char>(SEQLZ_PAGE, 0xa5);
    auto slow = std::vector<unsigned char>(SEQLZ_PAGE, 0x5a);
    auto scratch = std::vector<unsigned char>(SEQLZ_SCRATCH);
    // exactly sized copies, so that ASan sees a read past the end
    auto const in = std::make_unique<unsigned char[]>(c.size() + (c.empty() ? 1 : 0));
    std::copy(c.begin(), c.end(), in.get());
    auto const r_fast = seqlz_decode(in.get(), static_cast<unsigned>(c.size()), fast.data(), scratch.data());
    auto const r_ref = seqlz_ref_decode(r, in.get(), c.size(), slow.data());
    REQUIRE((r_ref == 0 || r_ref == -1));
    REQUIRE((r_fast == 0) == (r_ref == 0));
    if (r_ref != 0) {
        return {};
    }
    REQUIRE(fast == slow);
    return slow;
}

// Bits most significant first, as docs/format.md writes the bitstream, for pages made by hand.
struct bit_writer {
    std::vector<unsigned char> bytes;
    unsigned int n = 0;

    void put(unsigned int v, unsigned int bits) {
        for (unsigned int k = bits; k-- > 0;) {
            if (n % 8 == 0) {
                bytes.push_back(0);
            }
            bytes.back() = static_cast<unsigned char>(bytes.back() | ((v >> k) & 1U) << (7 - n % 8));
            ++n;
        }
    }

    // a symbol's canonical code, from the codes the reference decoder built
    void symbol(seqlz_ref_code const& c, unsigned int sym) {
        for (unsigned int l = 1; l < 16; ++l) {
            for (unsigned int i = 0; i < c.count[l]; ++i) {
                if (c.sym[c.start[l] + i] == sym) {
                    put(c.first[l] + i, l);
                    return;
                }
            }
        }
        FAIL("no code for symbol ", sym);
    }

    // a length value: below 16 its own symbol, else the position of its top bit and the bits below
    void value(seqlz_ref_code const& c, unsigned int v) {
        if (v < 16) {
            symbol(c, v);
            return;
        }
        auto const b = static_cast<unsigned int>(std::bit_width(v)) - 1U;
        symbol(c, 12 + b);
        put(v - (1U << b), b);
    }

    // a token, escaped when it has no code
    void token(seqlz_ref_code const& c, unsigned int tok) {
        if (seqlz_default_lengths.token[tok] != 0) {
            symbol(c, tok);
        } else {
            symbol(c, SEQLZ_ESCAPE);
            put(tok, 12);
        }
    }
};

// literals raw literals, k * 7 for literal k, and no bits: valid only with PAGE of them.
std::vector<unsigned char> literals_only(unsigned int literals) {
    auto c_page = std::vector<unsigned char>{static_cast<unsigned char>(literals), static_cast<unsigned char>(literals >> 8)};
    for (unsigned int k = 0; k < literals; ++k) {
        c_page.push_back(static_cast<unsigned char>(k * 7));
    }
    return c_page;
}

// A page of PAGE raw literals with a token that takes them all, with class c and the offset bits of
// class 1 if c is 1: the last sequence as it was before it lost its token. Invalid.
std::vector<unsigned char> one_sequence(seqlz_ref const& r, unsigned int c) {
    auto w = bit_writer{};
    w.token(r.tok, 15 + 512 * c);
    if (c == 1) {
        w.put(1, 4);
    }
    w.value(r.ll, SEQLZ_PAGE - 15U); // ll = 15 + value(LL) = PAGE
    auto c_page = literals_only(SEQLZ_PAGE);
    c_page.insert(c_page.end(), w.bytes.begin(), w.bytes.end());
    return c_page;
}

// A page of one literal, 7, and a match of the rest of the page from 1 back, the repeat offset a page
// starts with. *padding gets the bits left in the last byte.
std::vector<unsigned char> one_match(seqlz_ref const& r, unsigned int* padding) {
    auto w = bit_writer{};
    w.token(r.tok, 1 + 16 * 31);
    w.value(r.ml, SEQLZ_PAGE - 1U - 35U); // ml = 35 + value(ML) = PAGE - 1
    auto c_page = std::vector<unsigned char>{1, 0, 7};
    c_page.insert(c_page.end(), w.bytes.begin(), w.bytes.end());
    *padding = static_cast<unsigned int>(8 * w.bytes.size()) - w.n;
    return c_page;
}

} // namespace

#if QUETSCHN_PAGE_BITS == 12
TEST_CASE("seqlz_ref: the example of docs/format.md is ab 2048 times") {
    auto const r = make_ref();
    auto const c = std::array<unsigned char, 9>{0x02, 0x00, 0x61, 0x62, 0x23, 0xf2, 0xff, 0xfb, 0x60};
    auto out = std::vector<unsigned char>(SEQLZ_PAGE);
    REQUIRE(seqlz_ref_decode(r.get(), c.data(), c.size(), out.data()) == 0);
    for (std::size_t k = 0; k < out.size(); ++k) {
        REQUIRE(out[k] == (k % 2 ? 'b' : 'a'));
    }
    // without the last byte the match is cut, and the last sequence has no token: token 0 after the
    // match, as the page had before, is invalid
    auto const shorter = std::array<unsigned char, 8>{0x02, 0x00, 0x61, 0x62, 0x23, 0xf2, 0xff, 0xfb};
    CHECK(seqlz_ref_decode(r.get(), shorter.data(), shorter.size(), out.data()) == -1);
    // token 0 has no code: the escape and the 12 bits of 0
    auto const with_token = std::array<unsigned char, 11>{0x02, 0x00, 0x61, 0x62, 0x23, 0xf2, 0xff, 0xfb, 0x64, 0x00, 0x00};
    CHECK(seqlz_ref_decode(r.get(), with_token.data(), with_token.size(), out.data()) == -1);
}
#endif

TEST_CASE("seqlz_ref: the compressor's pages decode the same with both decoders, raw and coded literals") {
    fast_tables();
    auto const r = make_ref();
    auto rng = std::mt19937_64(41);
    auto coded = 0;
    for (int round = 0; round < 300; ++round) {
        CAPTURE(round);
        auto const page = make_page(rng, round);
        for (unsigned level = 1; level <= SEQLZ_LEVELS; ++level) {
            auto const c = compress(page, level);
            coded += (c[1] & 0x80) != 0;
            REQUIRE(check_same(r.get(), c) == page);
        }
    }
    // the pages have to exercise both layouts
    CHECK(coded > 30);
}

// c with bit q of byte i flipped. A copy and a write of a byte the code computes made gcc warn of a write
// past the copy's end, with -O3, on arm64 and x86-64.
std::vector<unsigned char> with_bit_flipped(std::vector<unsigned char> c, std::size_t i, unsigned q) {
    for (auto& b : c) {
        if (static_cast<std::size_t>(&b - c.data()) == i) {
            b = static_cast<unsigned char>(b ^ (1U << q));
        }
    }
    return c;
}

// Pages whose literals level 3 codes as deltas: fields of 4 bytes whose 3 high bytes every second field repeats from
// the one before (SEQLZ_DELTA_4 or SEQLZ_DELTA_LAST); records of 16 bytes, each the one before with a bit flipped
// here and there, which leaves literals next to matches 16 bytes back (SEQLZ_DELTA_LAST); or runs of a 3-byte
// pattern, matches of offset 3, each followed by 8 bytes a little above the byte 6 before them (SEQLZ_DELTA_LAST
// with the last offset 3, which counts as 6).
std::vector<unsigned char> delta_page(std::mt19937_64& rng, int kind) {
    auto p = std::vector<unsigned char>(SEQLZ_PAGE);
    if (kind % 3 == 2) {
        for (std::size_t k = 0; k < p.size();) {
            auto const pattern = std::array<unsigned char, 3>{
                static_cast<unsigned char>(rng()), static_cast<unsigned char>(rng()), static_cast<unsigned char>(rng())};
            for (int i = 0; i < 12 && k < p.size(); ++i, ++k) {
                p[k] = pattern[static_cast<std::size_t>(i % 3)];
            }
            for (int i = 0; i < 8 && k < p.size(); ++i, ++k) {
                p[k] = static_cast<unsigned char>(p[k - 6] + 1 + rng() % 3);
            }
        }
        return p;
    }
    if (kind % 3 == 0) {
        auto high = std::array<unsigned char, 3>{};
        for (std::size_t k = 0; k < p.size(); k += 4) {
            if (k % 8 == 0) {
                for (auto& b : high) {
                    b = static_cast<unsigned char>(rng());
                }
            }
            p[k] = static_cast<unsigned char>(rng());
            std::copy(high.begin(), high.end(), p.begin() + static_cast<std::ptrdiff_t>(k) + 1);
        }
        return p;
    }
    for (std::size_t k = 0; k < p.size(); ++k) {
        p[k] = k < 16 ? static_cast<unsigned char>(rng()) : p[k - 16];
        if (k >= 16 && rng() % 12 == 0) {
            p[k] = static_cast<unsigned char>(p[k] ^ 1U << rng() % 8);
        }
    }
    return p;
}

TEST_CASE("seqlz_ref: literals as deltas decode the same with both decoders, and damaged deltas alike") {
    // The delta byte and byte 2 with each bit flipped: other deltas, invalid ones, a page without deltas whose
    // delta byte is then read as the first byte of the stream sizes.
    fast_tables();
    auto const r = make_ref();
    auto rng = std::mt19937_64(53);
    auto deltas = std::array<int, 4>{};
    auto last3 = 0;
    for (int round = 0; round < 40; ++round) {
        CAPTURE(round);
        auto const page = delta_page(rng, round);
        auto const c = compress(page, 3);
        REQUIRE(check_same(r.get(), c) == page);
        auto const at = SEQLZ_LIT_HEADER(0);
        auto const has_delta = (c[1] & 0x80) != 0 && (c[2] >> SEQLZ_LIT_DELTA_AT) != 0;
        ++deltas[has_delta ? c[at] & 3U : 0U];
        last3 += round % 3 == 2 && has_delta && c[at] == SEQLZ_DELTA_LAST;
        if (!has_delta) {
            continue;
        }
        for (auto q = 0U; q < 8U; ++q) {
            check_same(r.get(), with_bit_flipped(c, at, q));
            check_same(r.get(), with_bit_flipped(c, 2, q));
        }
    }
    // the pages get deltas, SEQLZ_DELTA_4 and SEQLZ_DELTA_LAST, the latter also after an offset of 3
    CHECK(deltas[SEQLZ_DELTA_4] > 5);
    CHECK(deltas[SEQLZ_DELTA_LAST] > 5);
    CHECK(last3 > 5);
}

TEST_CASE("seqlz_ref: pages' own literal tables with one bit flipped are valid or invalid for both decoders alike") {
    // Every bit of the table and the first bytes after it, one at a time: lengths that overfill the code
    // space or leave it short, runs past byte value 255, bits after the table's end, two runs in a row.
    fast_tables();
    auto const r = make_ref();
    auto rng = std::mt19937_64(47);
    auto pages = 0;
    for (int round = 0; pages < 30 && round < 2000; ++round) {
        // zeros after the first 4 KiB: a page of 16 KiB with more literals has no room for the work of
        // its own table
        auto page = make_page(rng, round);
        std::fill(page.begin() + 4096, page.end(), 0);
        auto const c = compress(page, 3);
        if (!(c[1] & 0x80) || (c[2] & SEQLZ_LIT_CODE_MASK) != SEQLZ_LIT_OWN) {
            continue;
        }
        ++pages;
        auto const at = SEQLZ_LIT_HEADER(size_width(c)) + (c[2] >> SEQLZ_LIT_DELTA_AT);
        for (auto q = 8 * at; q < 8 * std::min<std::size_t>(c.size(), at + 160); ++q) {
            auto bad = c;
            bad[q / 8] = static_cast<unsigned char>(bad[q / 8] ^ (0x80U >> (q % 8)));
            check_same(r.get(), bad);
        }
    }
    CHECK(pages == 30);
}

TEST_CASE("seqlz_ref: damaged pages are valid or invalid for both decoders alike") {
    fast_tables();
    auto const r = make_ref();
    auto rng = std::mt19937_64(43);
    for (int round = 0; round < 1000; ++round) {
        CAPTURE(round);
        auto const page = compress(make_page(rng, round), 1U + static_cast<unsigned>(round) % 3U);
        // each damage on its own copy of the page
        auto c = page;
        for (int f = 0; f < 1 + round % 3; ++f) { // bits flipped
            c[rng() % c.size()] ^= static_cast<unsigned char>(1U << (rng() % 8));
        }
        check_same(r.get(), c);
        c = page; // cut short
        c.resize(rng() % c.size());
        check_same(r.get(), c);
        c = page; // bytes appended, which a valid page ignores
        for (int k = 0; k < 1 + round % 9; ++k) {
            c.push_back(static_cast<unsigned char>(rng()));
        }
        check_same(r.get(), c);
        c = page; // a byte replaced, often in the headers
        c[rng() % std::min<std::size_t>(c.size(), 1U + static_cast<unsigned>(round) % 24U)] =
            static_cast<unsigned char>(rng());
        check_same(r.get(), c);
    }
}

TEST_CASE("seqlz_ref: tables that are no complete prefix code, and other page sizes, are refused") {
    auto r = std::make_unique<seqlz_ref>();
    auto t = format_tables();
    CHECK(seqlz_ref_init(r.get(), &t, QUETSCHN_PAGE_BITS) == 0);
    CHECK(seqlz_ref_init(r.get(), &t, 13) == -1);
    // one code a bit longer leaves a gap
    auto tok = std::vector<unsigned char>(seqlz_default_lengths.token, seqlz_default_lengths.token + SEQLZ_TOKEN_SYMBOLS + 1);
    for (auto& l : tok) {
        if (l != 0) {
            ++l;
            break;
        }
    }
    t.tok = tok.data();
    CHECK(seqlz_ref_init(r.get(), &t, QUETSCHN_PAGE_BITS) == -1);
}

TEST_CASE("seqlz_ref: a page of literals only has no bits, and as many literals as the page") {
    fast_tables();
    auto const r = make_ref();
    auto out = std::vector<unsigned char>(SEQLZ_PAGE);
    auto const good = literals_only(SEQLZ_PAGE);
    REQUIRE(seqlz_ref_decode(r.get(), good.data(), good.size(), out.data()) == 0);
    CHECK(out[7] == 49);
    check_same(r.get(), good);
    for (auto const& bad : {literals_only(SEQLZ_PAGE - 1), one_sequence(*r, 0), one_sequence(*r, 1)}) {
        CHECK(seqlz_ref_decode(r.get(), bad.data(), bad.size(), out.data()) == -1);
        check_same(r.get(), bad);
    }
}

TEST_CASE("seqlz_ref: a page ends where its bits end, the rest of the last byte 0") {
    fast_tables();
    auto const r = make_ref();
    auto out = std::vector<unsigned char>(SEQLZ_PAGE);
    auto padding = 0U;
    auto const good = one_match(*r, &padding);
    REQUIRE(seqlz_ref_decode(r.get(), good.data(), good.size(), out.data()) == 0);
    CHECK(out.back() == 7);
    auto longer = good;
    longer.push_back(0);
    CHECK(seqlz_ref_decode(r.get(), longer.data(), longer.size(), out.data()) == -1);
    check_same(r.get(), longer);
    // with 16 KiB pages this page's bits fill its last byte, there is no bit after them to set
    if (padding > 0) {
        auto set_bit = good;
        set_bit.back() = static_cast<unsigned char>(set_bit.back() | 1U);
        CHECK(seqlz_ref_decode(r.get(), set_bit.data(), set_bit.size(), out.data()) == -1);
        check_same(r.get(), set_bit);
    }

    // the compressor's pages, raw and coded, with one byte more, of either value
    auto rng = std::mt19937_64(126);
    for (int round = 0; round < 20; ++round) {
        auto const c = compress(make_page(rng, round % 6), 1U + static_cast<unsigned>(round) % 3U);
        REQUIRE(!check_same(r.get(), c).empty());
        for (unsigned char const extra : {static_cast<unsigned char>(0x00), static_cast<unsigned char>(0xff)}) {
            auto bad = c;
            bad.push_back(extra);
            CHECK(seqlz_ref_decode(r.get(), bad.data(), bad.size(), out.data()) == -1);
            check_same(r.get(), bad);
        }
        // the lowest bit of the last byte set: where that is a bit after the last code, the page is
        // invalid, and both decoders must say the same
        auto flipped = c;
        flipped.back() = static_cast<unsigned char>(flipped.back() ^ 1U);
        check_same(r.get(), flipped);
    }

    // a literal stream one byte longer than its codes: stream 0's size one more, a 0 byte after it
    auto tried = 0;
    for (int round = 0; tried < 10; ++round) {
        CAPTURE(round);
        auto const c = compress(make_page(rng, 5), 2);
        auto const w = size_width(c);
        auto S = std::vector<unsigned int>(8);
        for (unsigned int j = 0; j < 8; ++j) {
            for (unsigned int i = 0; i < w; ++i) {
                auto const q = j * w + i;
                S[j] |= ((c[3 + q / 8] >> (q % 8)) & 1U) << i;
            }
        }
        if (S[0] + 1 >= 1U << w) {
            continue;
        }
        ++tried;
        ++S[0];
        auto bad = c;
        for (unsigned int q = 0; q < 8 * w; ++q) {
            auto const bitv = (S[q / w] >> (q % w)) & 1U;
            bad[3 + q / 8] = static_cast<unsigned char>((bad[3 + q / 8] & ~(1U << (q % 8))) | bitv << (q % 8));
        }
        bad.insert(bad.begin() + 3 + w + S[0] - 1, 0);
        CHECK(seqlz_ref_decode(r.get(), bad.data(), bad.size(), out.data()) == -1);
        check_same(r.get(), bad);
        // the lowest bit of stream 0's last byte set: where that is a bit after its last code, the
        // page is invalid, and both decoders must say the same
        auto flipped = c;
        flipped[3 + w + S[0] - 2] = static_cast<unsigned char>(flipped[3 + w + S[0] - 2] ^ 1U);
        check_same(r.get(), flipped);
    }
}

// A page of n literals literal_of(k), coded with literal table 0, or with code and the page's own table
// desc, then a match from 1 back to the end of the page; valid if the last literal repeats as the match
// needs, as for literals all the same.
template <typename F>
std::vector<unsigned char> coded_literals(
    seqlz_ref const& r, unsigned int n, F literal_of, seqlz_ref_code const& code, std::vector<unsigned char> const& desc) {
    auto streams = std::vector<bit_writer>(8);
    for (unsigned int k = 0; k < n; ++k) {
        streams[k % 8].symbol(code, literal_of(k));
    }
    auto w = 5U;
    for (auto const& s : streams) {
        w = std::max(w, static_cast<unsigned int>(std::bit_width(s.bytes.size())));
    }
    auto c = std::vector<unsigned char>{
        static_cast<unsigned char>(n),
        static_cast<unsigned char>(0x80 | n >> 8),
        static_cast<unsigned char>((w - SEQLZ_SIZE_BITS_MIN) << SEQLZ_LIT_WIDTH_AT | (desc.empty() ? 0U : SEQLZ_LIT_OWN))};
    auto sizes = bit_writer{};
    for (int j = 7; j >= 0; --j) {
        sizes.put(static_cast<unsigned int>(streams[static_cast<std::size_t>(j)].bytes.size()), w);
    }
    // the sizes are one little endian number, s[0] in the lowest bits: the bytes of the big endian
    // writer the other way round
    c.insert(c.end(), sizes.bytes.rbegin(), sizes.bytes.rend());
    c.insert(c.end(), desc.begin(), desc.end());
    for (auto const& s : streams) {
        c.insert(c.end(), s.bytes.begin(), s.bytes.end());
    }
    auto seq = bit_writer{};
    // ll and ml - 4 at their caps, class 0 with the offset 1 a page starts with
    seq.token(r.tok, 15 + 16 * 31);
    seq.value(r.ll, n - 15);
    seq.value(r.ml, SEQLZ_PAGE - n - 35);
    c.insert(c.end(), seq.bytes.begin(), seq.bytes.end());
    return c;
}

template <typename F>
std::vector<unsigned char> coded_literals(seqlz_ref const& r, unsigned int n, F literal_of) {
    return coded_literals(r, n, literal_of, r.lit[0], {});
}

// n literals b: stream j has the codes of the literals j, j + 8, ..., all of the same length, so as n
// grows each stream's bits end at every place of a byte.
std::vector<unsigned char> coded_run(seqlz_ref const& r, unsigned int n, unsigned int b) {
    return coded_literals(r, n, [b](unsigned int) {
        return b;
    });
}

// What own_table() does to the table it writes
enum class own_damage { none, overfull, above_255, padding_bit };

// A page's own table in which only the bytes x < y have a code, of 1 bit each: the runs before them, as
// docs/format.md has them, and their lengths. *padding gets the bits after its last symbol.
std::vector<unsigned char>
own_table(seqlz_ref const& r, unsigned int x, unsigned int y, own_damage damage, unsigned int* padding) {
    auto w = bit_writer{};
    auto run = [&](unsigned int n) {
        if (n > 0) {
            auto const k = static_cast<unsigned int>(std::bit_width(n));
            w.symbol(r.own, SEQLZ_OWN_RUN + k);
            w.put(n - (1U << (k - 1)), k - 1);
        }
    };
    run(x);
    // with overfull a code of 2 bits for x, then one of 1 bit for y and one more for y + 1
    w.symbol(r.own, damage == own_damage::overfull ? 1 : 0);
    run(damage == own_damage::above_255 ? 255 - x : y - x - 1);
    w.symbol(r.own, 0);
    if (damage == own_damage::overfull) {
        w.symbol(r.own, 0);
    }
    *padding = 8 * static_cast<unsigned int>(w.bytes.size()) - w.n;
    if (damage == own_damage::padding_bit) {
        w.bytes.back() |= 1U;
    }
    return w.bytes;
}

TEST_CASE("seqlz_ref: pages with a literal table of their own, and tables invalid by one rule") {
    // Runs before the bytes that the fast decoder takes two at a time with the lengths, 3 and 1, or on
    // their own, 97 and 254; each damage makes the page invalid, for both decoders.
    fast_tables();
    auto const r = make_ref();
    auto out = std::vector<unsigned char>(SEQLZ_PAGE);
    for (auto const& [x, y] : {std::pair{3U, 5U}, std::pair{97U, 98U}, std::pair{0U, 255U}}) {
        CAPTURE(x);
        // x gets code 0, y code 1
        auto code = seqlz_ref_code{};
        code.count[1] = 2;
        code.sym[0] = static_cast<unsigned short>(x);
        code.sym[1] = static_cast<unsigned short>(y);
        for (unsigned int n = 16; n < 120; ++n) {
            CAPTURE(n);
            auto const literal = [&](unsigned int k) {
                return (k + 1) * 2654435761U >> 31 ? y : x;
            };
            auto padding = 0U;
            auto const good = coded_literals(*r, n, literal, code, own_table(*r, x, y, own_damage::none, &padding));
            auto const page = check_same(r.get(), good);
            REQUIRE(page.size() == SEQLZ_PAGE);
            for (unsigned int k = 0; k < SEQLZ_PAGE; ++k) {
                REQUIRE(page[k] == literal(std::min(k, n - 1)));
            }
            for (auto const d : {own_damage::overfull, own_damage::above_255, own_damage::padding_bit}) {
                if (d == own_damage::padding_bit && padding == 0) {
                    continue;
                }
                auto unused = 0U;
                auto const bad = coded_literals(*r, n, literal, code, own_table(*r, x, y, d, &unused));
                CHECK(seqlz_ref_decode(r.get(), bad.data(), bad.size(), out.data()) == -1);
                check_same(r.get(), bad);
            }
            // the page ends in the table
            auto const at = SEQLZ_LIT_HEADER(5U + ((good[2] >> 4) & 7U));
            auto const cut = std::vector<unsigned char>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(at) + 1);
            CHECK(seqlz_ref_decode(r.get(), cut.data(), cut.size(), out.data()) == -1);
            check_same(r.get(), cut);
        }
    }
}

TEST_CASE("seqlz_ref: literal streams that end at every place of a byte, their padding read from the page") {
    // The decoder once read a stream's last padding bit from its 64 bits, where the stream's last bit is
    // the 1 that marks how far it read when its last 8 bytes are in them; real pages hit that 3 times in
    // 60 000. It takes codes of 9 and of 10 bits: 57 bits after the last refill are 5 codes of 10 bits
    // and 7 bits used of the first byte, which codes of one even length never give.
    fast_tables();
    auto const r = make_ref();
    for (unsigned int seed = 0; seed < 4; ++seed) {
        CAPTURE(seed);
        for (unsigned int n = 300; n < 600; ++n) {
            CAPTURE(n);
            check_same(r.get(), coded_literals(*r, n, [seed](unsigned int k) {
                           return ((k + 1) * 2654435761U + seed * 40503U) >> 31 ? 0xffU : 0x71U;
                       }));
        }
    }
    for (unsigned int b : {0x65U, 0x71U, 0x00U, 0xffU}) {
        CAPTURE(b);
        for (unsigned int n = 16; n + 40 <= SEQLZ_PAGE && n < 1200; ++n) {
            CAPTURE(n);
            auto const page = check_same(r.get(), coded_run(*r, n, b));
            REQUIRE(page.size() == SEQLZ_PAGE);
            CHECK(std::all_of(page.begin(), page.end(), [&](unsigned char x) {
                return x == b;
            }));
        }
    }
}

TEST_CASE("seqlz_ref: coded literals need a delta byte of 1 to 3, a code up to 8 and every code inside its stream") {
    fast_tables();
    auto const r = make_ref();
    auto rng = std::mt19937_64(47);
    auto out = std::vector<unsigned char>(SEQLZ_PAGE);
    auto tried = 0;
    for (int round = 0; tried < 20; ++round) {
        CAPTURE(round);
        auto const c = compress(make_page(rng, 5), 2);
        REQUIRE((c[1] & 0x80) != 0);
        // deltas, with a delta byte of 0 or 4 after byte 2, or a code of the literals above the page's own
        // table, 9 to 15
        for (unsigned int dt : {0U, 4U}) {
            auto bad = c;
            bad[2] = static_cast<unsigned char>(bad[2] | 1U << SEQLZ_LIT_DELTA_AT);
            bad.insert(bad.begin() + 3, static_cast<unsigned char>(dt));
            CHECK(seqlz_ref_decode(r.get(), bad.data(), bad.size(), out.data()) == -1);
            check_same(r.get(), bad);
        }
        for (unsigned int b2 : {(c[2] & ~15U) | 9U, (c[2] & ~15U) | 15U}) {
            auto bad = c;
            bad[2] = static_cast<unsigned char>(b2);
            CHECK(seqlz_ref_decode(r.get(), bad.data(), bad.size(), out.data()) == -1);
            check_same(r.get(), bad);
        }
        // one byte of stream 0 given to stream 1: the same bytes, but stream 0's last code is outside
        auto const w = size_width(c);
        auto S = std::vector<unsigned int>(8);
        for (unsigned int j = 0; j < 8; ++j) {
            for (unsigned int i = 0; i < w; ++i) {
                auto const q = j * w + i;
                S[j] |= ((c[3 + q / 8] >> (q % 8)) & 1U) << i;
            }
        }
        if (S[0] == 0 || S[1] + 1 >= 1U << w) {
            continue;
        }
        ++tried;
        --S[0];
        ++S[1];
        auto bad = c;
        for (unsigned int q = 0; q < 8 * w; ++q) {
            auto const bitv = (S[q / w] >> (q % w)) & 1U;
            bad[3 + q / 8] = static_cast<unsigned char>((bad[3 + q / 8] & ~(1U << (q % 8))) | bitv << (q % 8));
        }
        CHECK(seqlz_ref_decode(r.get(), bad.data(), bad.size(), out.data()) == -1);
        check_same(r.get(), bad);
    }
}
