// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "seqlz.h"
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
    return {seqlz_default_own.token, seqlz_default_own.ll, seqlz_default_own.ml, seqlz_lit_sets};
}

std::unique_ptr<seqlz_ref> make_ref() {
    auto r = std::make_unique<seqlz_ref>();
    auto const t = format_tables();
    REQUIRE(seqlz_ref_init(r.get(), &t, QUETSCHN_PAGE_BITS) == 0);
    return r;
}

std::unique_ptr<seqlz_tables, void (*)(seqlz_tables*)> fast_tables() {
    auto* t = static_cast<seqlz_tables*>(::operator new(seqlz_tables_size()));
    REQUIRE(seqlz_tables_init(t, &seqlz_default_own) == 0);
    return {t, [](seqlz_tables* p) {
                ::operator delete(p);
            }};
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
    case 5: // bytes of skewed frequencies and few repeats: literals that code well
        for (auto& b : p) {
            b = static_cast<unsigned char>(
                32U + static_cast<unsigned>(std::min(std::countr_zero(rng() | 1ULL << 40), 30)) * 3U + rng() % 3);
        }
        break;
    default: // mostly zeros, as in many swapped pages
        for (int i = 0; i < 40; ++i) {
            p[rng() % p.size()] = static_cast<unsigned char>(rng());
        }
    }
    return p;
}

std::vector<unsigned char> compress(seqlz_tables const* t, std::vector<unsigned char> const& page, int lit) {
    auto st = std::make_unique<seqlz_state>();
    auto c = std::vector<unsigned char>(2 * SEQLZ_PAGE);
    auto const len = seqlz_compress(t, st.get(), page.data(), c.data(), static_cast<unsigned>(c.size()), lit);
    REQUIRE(len > 0);
    c.resize(len);
    return c;
}

// Both decoders on the same input: the same verdict, and for a valid page the same bytes. Returns the
// page, empty for an invalid input.
std::vector<unsigned char> check_same(seqlz_tables const* t, seqlz_ref const* r, std::vector<unsigned char> const& c) {
    auto fast = std::vector<unsigned char>(SEQLZ_PAGE, 0xa5);
    auto slow = std::vector<unsigned char>(SEQLZ_PAGE, 0x5a);
    auto scratch = std::vector<unsigned char>(SEQLZ_SCRATCH);
    // exactly sized copies, so that ASan sees a read past the end
    auto const in = std::make_unique<unsigned char[]>(c.size() + (c.empty() ? 1 : 0));
    std::copy(c.begin(), c.end(), in.get());
    auto const r_fast = seqlz_decode(t, in.get(), static_cast<unsigned>(c.size()), fast.data(), scratch.data());
    auto const r_ref = seqlz_ref_decode(r, in.get(), c.size(), slow.data());
    REQUIRE((r_ref == 0 || r_ref == -1));
    REQUIRE((r_fast == 0) == (r_ref == 0));
    if (r_ref != 0) {
        return {};
    }
    REQUIRE(fast == slow);
    return slow;
}

// Bits most significant first, as FORMAT.md writes the bitstream, for pages made by hand.
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

    // a token, escaped when it has no code
    void token(seqlz_ref_code const& c, unsigned int tok) {
        if (seqlz_default_own.token[tok] != 0) {
            symbol(c, tok);
        } else {
            symbol(c, SEQLZ_ESCAPE);
            put(tok, 12);
        }
    }
};

// A page of PAGE raw literals and one sequence that takes them all: its token with class c, and
// the offset bits of class 1 if c is 1. Valid only for c == 0.
std::vector<unsigned char> one_sequence(seqlz_ref const& r, unsigned int literals, unsigned int c) {
    auto w = bit_writer{};
    w.token(r.tok, 15 + 512 * c);
    if (c == 1) {
        w.put(1, 4);
    }
    // ll = 15 + value(LL) = PAGE: the value has its top bit as the symbol, the rest as extra bits
    auto const v = SEQLZ_PAGE - 15U;
    auto const b = static_cast<unsigned int>(std::bit_width(v)) - 1U;
    w.symbol(r.ll, 12 + b);
    w.put(v - (1U << b), b);
    auto c_page = std::vector<unsigned char>{static_cast<unsigned char>(literals), static_cast<unsigned char>(literals >> 8)};
    for (unsigned int k = 0; k < literals; ++k) {
        c_page.push_back(static_cast<unsigned char>(k * 7));
    }
    c_page.insert(c_page.end(), w.bytes.begin(), w.bytes.end());
    return c_page;
}

} // namespace

#if QUETSCHN_PAGE_BITS == 12
TEST_CASE("seqlz_ref: the example of FORMAT.md is ab 2048 times") {
    auto const r = make_ref();
    auto const c = std::array<unsigned char, 9>{0x02, 0x00, 0x61, 0x62, 0xe2, 0x65, 0xdf, 0xdb, 0x98};
    auto out = std::vector<unsigned char>(SEQLZ_PAGE);
    REQUIRE(seqlz_ref_decode(r.get(), c.data(), c.size(), out.data()) == 0);
    for (std::size_t k = 0; k < out.size(); ++k) {
        REQUIRE(out[k] == (k % 2 ? 'b' : 'a'));
    }
    // without the last token's last bit the page cannot end
    auto const shorter = std::array<unsigned char, 8>{0x02, 0x00, 0x61, 0x62, 0xe2, 0x65, 0xdf, 0xdb};
    CHECK(seqlz_ref_decode(r.get(), shorter.data(), shorter.size(), out.data()) == -1);
}
#endif

TEST_CASE("seqlz_ref: the compressor's pages decode the same with both decoders, raw and coded literals") {
    auto const t = fast_tables();
    auto const r = make_ref();
    auto rng = std::mt19937_64(41);
    auto coded = 0;
    for (int round = 0; round < 300; ++round) {
        CAPTURE(round);
        auto const page = make_page(rng, round);
        for (int lit = 0; lit < 2; ++lit) {
            auto const c = compress(t.get(), page, lit);
            coded += (c[1] & 0x80) != 0;
            REQUIRE(check_same(t.get(), r.get(), c) == page);
        }
    }
    // the pages have to exercise both layouts
    CHECK(coded > 30);
}

TEST_CASE("seqlz_ref: damaged pages are valid or invalid for both decoders alike") {
    auto const t = fast_tables();
    auto const r = make_ref();
    auto rng = std::mt19937_64(43);
    for (int round = 0; round < 1000; ++round) {
        CAPTURE(round);
        auto const page = compress(t.get(), make_page(rng, round), round % 2);
        // each damage on its own copy of the page
        auto c = page;
        for (int f = 0; f < 1 + round % 3; ++f) { // bits flipped
            c[rng() % c.size()] ^= static_cast<unsigned char>(1U << (rng() % 8));
        }
        check_same(t.get(), r.get(), c);
        c = page; // cut short
        c.resize(rng() % c.size());
        check_same(t.get(), r.get(), c);
        c = page; // bytes appended, which a valid page ignores
        for (int k = 0; k < 1 + round % 9; ++k) {
            c.push_back(static_cast<unsigned char>(rng()));
        }
        check_same(t.get(), r.get(), c);
        c = page; // a byte replaced, often in the headers
        c[rng() % std::min<std::size_t>(c.size(), 1U + static_cast<unsigned>(round) % 24U)] =
            static_cast<unsigned char>(rng());
        check_same(t.get(), r.get(), c);
    }
}

TEST_CASE("seqlz_ref: tables that are no complete prefix code, and other page sizes, are refused") {
    auto r = std::make_unique<seqlz_ref>();
    auto t = format_tables();
    CHECK(seqlz_ref_init(r.get(), &t, QUETSCHN_PAGE_BITS) == 0);
    CHECK(seqlz_ref_init(r.get(), &t, 13) == -1);
    // one code a bit longer leaves a gap
    auto tok = std::vector<unsigned char>(seqlz_default_own.token, seqlz_default_own.token + SEQLZ_TOKEN_SYMBOLS + 1);
    for (auto& l : tok) {
        if (l != 0) {
            ++l;
            break;
        }
    }
    t.tok = tok.data();
    CHECK(seqlz_ref_init(r.get(), &t, QUETSCHN_PAGE_BITS) == -1);
}

TEST_CASE("seqlz_ref: the last sequence needs class 0, and as many literals as the header has") {
    auto const t = fast_tables();
    auto const r = make_ref();
    auto out = std::vector<unsigned char>(SEQLZ_PAGE);
    auto const good = one_sequence(*r, SEQLZ_PAGE, 0);
    REQUIRE(seqlz_ref_decode(r.get(), good.data(), good.size(), out.data()) == 0);
    CHECK(out[7] == 49);
    check_same(t.get(), r.get(), good);
    for (auto const& bad : {one_sequence(*r, SEQLZ_PAGE, 1), one_sequence(*r, SEQLZ_PAGE - 1, 0)}) {
        CHECK(seqlz_ref_decode(r.get(), bad.data(), bad.size(), out.data()) == -1);
        check_same(t.get(), r.get(), bad);
    }
}

TEST_CASE("seqlz_ref: coded literals need byte 2's top bits zero and every code inside its stream") {
    auto const t = fast_tables();
    auto const r = make_ref();
    auto rng = std::mt19937_64(47);
    auto out = std::vector<unsigned char>(SEQLZ_PAGE);
    auto tried = 0;
    for (int round = 0; tried < 20; ++round) {
        CAPTURE(round);
        auto const c = compress(t.get(), make_page(rng, 5), 1);
        REQUIRE((c[1] & 0x80) != 0);
        for (unsigned int top : {0x40U, 0x80U}) {
            auto bad = c;
            bad[2] = static_cast<unsigned char>(bad[2] | top);
            CHECK(seqlz_ref_decode(r.get(), bad.data(), bad.size(), out.data()) == -1);
            check_same(t.get(), r.get(), bad);
        }
        // one byte of stream 0 given to stream 1: the same bytes, but stream 0's last code is outside
        auto const w = 5U + ((c[2] >> 3) & 7U);
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
        check_same(t.get(), r.get(), bad);
    }
}
