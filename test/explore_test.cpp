// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "bdelta.h"
#include "bytelz.h"
#include "seqlz.h"
#include "shuffle.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

namespace {

using page = std::array<std::uint64_t, 512>;

std::vector<unsigned char> compress(page const& p) {
    auto out = std::vector<unsigned char>(BDELTA_MAX_COMPRESSED);
    auto const len = bdelta_compress(p.data(), out.data(), static_cast<unsigned int>(out.size()));
    REQUIRE(len > 0);
    out.resize(len);
    return out;
}

page decompress(std::vector<unsigned char> const& c) {
    auto out = page{};
    REQUIRE(bdelta_decompress(c.data(), static_cast<unsigned int>(c.size()), out.data()) == 0);
    return out;
}

unsigned int mode(std::vector<unsigned char> const& c, std::size_t block) {
    return (c[block / 2] >> ((block % 2) * 4)) & 15U;
}

// page with block 0 set to the given 8 words, the rest zero
page block0(std::array<std::uint64_t, 8> const& words) {
    auto p = page{};
    std::copy(words.begin(), words.end(), p.begin());
    return p;
}

} // namespace

TEST_CASE("shuffle: byte j of word i goes to plane j, and back") {
    auto rng = std::mt19937_64(3);
    for (unsigned int size : {64U, 128U, 4096U}) {
        CAPTURE(size);
        auto src = std::vector<unsigned char>(size);
        for (auto& b : src) {
            b = static_cast<unsigned char>(rng());
        }
        auto shuffled = std::vector<unsigned char>(size);
        shuffle8(src.data(), shuffled.data(), size);
        auto const n = size / 8;
        auto ok = true;
        for (unsigned int i = 0; i < n; ++i) {
            for (unsigned int j = 0; j < 8; ++j) {
                ok = ok && shuffled[j * n + i] == src[8 * i + j];
            }
        }
        CHECK(ok);
        auto back = std::vector<unsigned char>(size);
        unshuffle8(shuffled.data(), back.data(), size);
        CHECK(back == src);
    }
}

TEST_CASE("bdelta: each mode where it is the smallest, with its size") {
    constexpr std::uint64_t ptr = 0x00007f1234560000ULL;
    struct expected {
        std::array<std::uint64_t, 8> words;
        unsigned int mode;
        unsigned int size;
    };
    auto const cases = std::array<expected, 8>{
        expected{{0, 0, 0, 0, 0, 0, 0, 0}, 0, 0},
        expected{{ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr}, 1, 8},
        // pointers within 127 bytes of the first, mixed with small integers
        expected{{ptr, ptr + 8, 3, ptr - 16, ptr + 120, 0, 0xffffffffffffffffULL, ptr}, 2, 17},
        expected{{ptr, ptr + 4000, 3, ptr - 16, ptr + 120, 0, 1000, ptr}, 3, 25},
        expected{{ptr, ptr + 400000, 3, ptr - 16, ptr + 120, 0, 100000, ptr}, 4, 41},
        // 32-bit values: 0x10000 + small, and small; as 64-bit words the pairs do not fit 1-byte deltas
        expected{
            {0x0001000500010000ULL, 0x0000000700010010ULL, 0, 0x0001007f00000001ULL, 0, 0, 0, 0x00010001fffffffeULL}, 5, 22},
        expected{
            {0x0001050000010000ULL, 0x0000000700010010ULL, 0, 0x0001007f00000001ULL, 0, 0, 0, 0x00010001fffffffeULL}, 6, 38},
        expected{{0x0123456789abcdefULL, 0xfedcba9876543210ULL, 1, 2, 3, 4, 5, 6}, 7, 64},
    };
    for (auto const& e : cases) {
        CAPTURE(e.mode);
        auto const p = block0(e.words);
        auto const c = compress(p);
        CHECK(mode(c, 0) == e.mode);
        for (std::size_t b = 1; b < 64; ++b) {
            CHECK(mode(c, b) == 0);
        }
        CHECK(c.size() == BDELTA_HEADER + e.size);
        CHECK(decompress(c) == p);
    }
}

TEST_CASE("bdelta: roundtrip of structured and random pages") {
    auto rng = std::mt19937_64(5);
    for (int round = 0; round < 400; ++round) {
        CAPTURE(round);
        auto p = page{};
        auto const kind = round % 5;
        auto const base = rng();
        for (std::size_t i = 0; i < p.size(); ++i) {
            auto const r = rng();
            switch (kind) {
            case 0:
                p[i] = r;
                break;
            case 1: // pointers near a base, small integers, zeros
                p[i] = r % 3 == 0 ? 0 : r % 3 == 1 ? (r >> 50) : base + ((r >> 8) % 70000) - 35000;
                break;
            case 2: // 32-bit arrays
                p[i] = ((r & 0xffff) << 32) | ((r >> 32) & 0x1ffff);
                break;
            case 3: // extremes: sign boundaries of every delta width
                p[i] = base + static_cast<std::uint64_t>(std::array<std::int64_t, 8>{
                                  -128, 127, -32768, 32767, -2147483648LL, 2147483647LL, 128, -129}[r % 8]);
                break;
            default: // runs
                p[i] = i % 16 < 8 ? base : 0;
                break;
            }
        }
        CHECK(decompress(compress(p)) == p);
    }
}

TEST_CASE("bdelta: wrong lengths and unknown modes are rejected") {
    auto p = page{};
    p[9] = 42;
    auto c = compress(p);
    auto out = page{};
    c.push_back(0);
    CHECK(bdelta_decompress(c.data(), static_cast<unsigned int>(c.size()), out.data()) == -1);
    c.pop_back();
    CHECK(bdelta_decompress(c.data(), static_cast<unsigned int>(c.size() - 1), out.data()) == -1);
    auto const short_input = std::vector<unsigned char>(c.begin(), c.begin() + BDELTA_HEADER - 1);
    CHECK(bdelta_decompress(short_input.data(), static_cast<unsigned int>(short_input.size()), out.data()) == -1);
    CHECK(bdelta_decompress(nullptr, 0, out.data()) == -1);
    // mode 8 has no payload, so the length still matches; it must be rejected anyway
    auto bad = std::vector<unsigned char>(BDELTA_HEADER);
    bad[5] = 0x80;
    CHECK(bdelta_decompress(bad.data(), static_cast<unsigned int>(bad.size()), out.data()) == -1);
    bad[5] = 0x08;
    CHECK(bdelta_decompress(bad.data(), static_cast<unsigned int>(bad.size()), out.data()) == -1);
}

TEST_CASE("bdelta: any input of valid length is safe, and what it decodes to roundtrips") {
    static constexpr std::array<unsigned int, 8> sizes{0, 8, 17, 25, 41, 22, 38, 64};
    auto rng = std::mt19937_64(9);
    for (int round = 0; round < 2000; ++round) {
        CAPTURE(round);
        auto header = std::vector<unsigned char>(BDELTA_HEADER);
        auto total = std::size_t{BDELTA_HEADER};
        auto const allowed = static_cast<unsigned int>(rng() % 255 + 1);
        for (std::size_t b = 0; b < 64; ++b) {
            auto m = static_cast<unsigned int>(rng() % 8);
            while ((allowed & (1U << m)) == 0) {
                m = (m + 1) % 8;
            }
            header[b / 2] = static_cast<unsigned char>(header[b / 2] | (m << ((b % 2) * 4)));
            total += sizes[m];
        }
        // exactly sized, so ASan sees every read outside it
        auto c = std::vector<unsigned char>(total);
        std::memcpy(c.data(), header.data(), header.size());
        for (auto i = header.size(); i < total; ++i) {
            c[i] = static_cast<unsigned char>(rng());
        }
        auto const p = decompress(c);
        CHECK(decompress(compress(p)) == p);
    }
}

namespace {

struct seqlz_page {
    std::vector<unsigned char> bytes;
    std::vector<seqlz_sequence> sequences;
    std::vector<unsigned char> literals;
};

// A page built from random sequences, so the sequences that describe it are known. kind picks what
// dominates: 0 short offsets 1 to 7, 1 repeat offsets, 2 long runs, 3 anything.
seqlz_page random_seqlz_page(std::mt19937_64& rng, int kind) {
    auto p = seqlz_page{};
    auto last = std::array<unsigned, 3>{1, 4, 8};
    while (true) {
        auto const room = 4096 - p.bytes.size();
        auto lit = static_cast<unsigned>(rng() % (kind == 2 ? 3 : 20));
        if (rng() % 16 == 0) {
            lit += static_cast<unsigned>(rng() % 300);
        }
        if (p.bytes.empty() && lit == 0) {
            lit = 1;
        }
        if (lit + 4 > room || rng() % 200 == 0) {
            // the last sequence: literals up to the end of the page
            auto s = seqlz_sequence{static_cast<unsigned short>(room), 0, 0};
            for (std::size_t i = 0; i < room; ++i) {
                auto const b = static_cast<unsigned char>(rng());
                p.bytes.push_back(b);
                p.literals.push_back(b);
            }
            p.sequences.push_back(s);
            return p;
        }
        for (unsigned i = 0; i < lit; ++i) {
            auto const b = static_cast<unsigned char>(rng() % 4 == 0 ? 0 : rng());
            p.bytes.push_back(b);
            p.literals.push_back(b);
        }
        auto const have = static_cast<unsigned>(p.bytes.size());
        auto off = 0U;
        switch (kind) {
        case 0:
            off = 1 + static_cast<unsigned>(rng() % 7);
            break;
        case 1:
            off = last[rng() % 3];
            break;
        default:
            off = 1 + static_cast<unsigned>(rng() % have);
            break;
        }
        off = std::min(off, have);
        auto const max_len = static_cast<unsigned>(4096 - p.bytes.size());
        auto len = 4 + static_cast<unsigned>(rng() % (kind == 2 ? 3000 : 40));
        len = std::min(len, max_len);
        if (len < 4) {
            continue;
        }
        for (unsigned i = 0; i < len; ++i) {
            p.bytes.push_back(p.bytes[p.bytes.size() - off]);
        }
        last = {off, last[0], last[1]};
        p.sequences.push_back(seqlz_sequence{
            static_cast<unsigned short>(lit), static_cast<unsigned short>(len), static_cast<unsigned short>(off)});
        if (p.bytes.size() == 4096) {
            // ended with a match: an empty last sequence
            p.sequences.push_back(seqlz_sequence{0, 0, 0});
            return p;
        }
    }
}

std::unique_ptr<seqlz_tables, void (*)(seqlz_tables*)> default_tables(seqlz_lengths const& lengths = seqlz_default_lz4) {
    auto* t = static_cast<seqlz_tables*>(::operator new(seqlz_tables_size()));
    REQUIRE(seqlz_tables_init(t, &lengths) == 0);
    return {t, [](seqlz_tables* p) {
                ::operator delete(p);
            }};
}

} // namespace

TEST_CASE("seqlz: pages from random sequences come back, with every kind of copy") {
    auto const t = default_tables();
    auto rng = std::mt19937_64(31);
    for (int round = 0; round < 800; ++round) {
        CAPTURE(round);
        auto const p = random_seqlz_page(rng, round % 4);
        REQUIRE(p.bytes.size() == 4096);
        auto c = std::vector<unsigned char>(3 * 4096);
        auto const len = seqlz_encode(t.get(),
                                      p.sequences.data(),
                                      static_cast<unsigned>(p.sequences.size()),
                                      p.literals.data(),
                                      static_cast<unsigned>(p.literals.size()),
                                      c.data(),
                                      static_cast<unsigned>(c.size()));
        REQUIRE(len > 0);
        // exactly sized, so ASan sees any read past the end
        c.resize(len);
        auto out = std::vector<unsigned char>(4096);
        REQUIRE(seqlz_decode(t.get(), c.data(), len, out.data()) == 0);
        CHECK(out == p.bytes);
        // and a byte less is not a valid page
        CHECK(seqlz_decode(t.get(), c.data(), len - 1, out.data()) == -1);
    }
}

TEST_CASE("seqlz: the token holds ll and ml - 4 up to their caps and the offset class") {
    auto constexpr ml_shift = SEQLZ_LL_BITS, cls_shift = SEQLZ_LL_BITS + SEQLZ_ML_BITS;
    CHECK(seqlz_token(0, 4, 0) == 0);
    CHECK(seqlz_token(2, 7, 1) == 2 + (3U << ml_shift) + (1U << cls_shift));
    CHECK(seqlz_token(SEQLZ_LL_CAP - 1, SEQLZ_ML_CAP + 3, 2) ==
          SEQLZ_LL_CAP - 1 + ((SEQLZ_ML_CAP - 1) << ml_shift) + (2U << cls_shift));
    CHECK(seqlz_token(SEQLZ_LL_CAP, SEQLZ_ML_CAP + 4, 0) == SEQLZ_LL_CAP + (SEQLZ_ML_CAP << ml_shift));
    CHECK(seqlz_token(4000, 3000, 2) == SEQLZ_LL_CAP + (SEQLZ_ML_CAP << ml_shift) + (2U << cls_shift));
    CHECK(seqlz_token(1, 0, 0) == 1);
    CHECK(seqlz_token(20, 0, 0) == SEQLZ_LL_CAP);
    auto bits = 0U;
    CHECK(seqlz_off_class(9, 9, &bits) == 0);
    CHECK(bits == 0);
    CHECK(seqlz_off_class(1, 9, &bits) == 1);
    CHECK(bits == 8);
    CHECK(seqlz_off_class(255, 9, &bits) == 1);
    CHECK(seqlz_off_class(256, 9, &bits) == 2);
    CHECK(bits == 12);
    CHECK(seqlz_off_class(4095, 9, &bits) == 2);
}

TEST_CASE("seqlz: a sequence with long lengths and a large offset needs more bits than one refill") {
    // 1100 literals (value 1085, 10 extra bits), a match of 2100 (value 2081, 11 extra bits) at offset
    // 1050 (12 raw bits), then 896 literals: token, two codes and 33 more bits in one sequence
    auto const t = default_tables();
    auto rng = std::mt19937_64(41);
    auto p = seqlz_page{};
    for (int i = 0; i < 1100; ++i) {
        p.bytes.push_back(static_cast<unsigned char>(rng()));
    }
    p.literals = p.bytes;
    for (int i = 0; i < 2100; ++i) {
        p.bytes.push_back(p.bytes[p.bytes.size() - 1050]);
    }
    for (int i = 0; i < 896; ++i) {
        auto const b = static_cast<unsigned char>(rng());
        p.bytes.push_back(b);
        p.literals.push_back(b);
    }
    p.sequences = {{1100, 2100, 1050}, {896, 0, 0}};
    auto c = std::vector<unsigned char>(3 * 4096);
    auto const len = seqlz_encode(t.get(), p.sequences.data(), 2, p.literals.data(), 1996, c.data(), 3 * 4096);
    REQUIRE(len > 0);
    c.resize(len);
    auto out = std::vector<unsigned char>(4096);
    REQUIRE(seqlz_decode(t.get(), c.data(), len, out.data()) == 0);
    CHECK(out == p.bytes);
}

TEST_CASE("seqlz: tables that are no prefix code are rejected") {
    auto t = default_tables();
    auto l = seqlz_default_lz4;
    CHECK(seqlz_tables_init(t.get(), &l) == 0);
    l.ll[0] = 10; // longer than SEQLZ_MAX_BITS
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    l = seqlz_default_lz4;
    l.ml[0] = 1; // together with the others more codes than fit: over-subscribed
    l.ml[1] = 1;
    l.ml[2] = 1;
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    l = seqlz_default_lz4;
    std::memset(l.ml, 0, sizeof(l.ml)); // no code at all
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    // A complete code: all tokens with 11 bits, then the first ones 10 bits, then 9, until they fill
    // the 11-bit table exactly.
    l = seqlz_default_lz4;
    auto complete = [&] {
        std::memset(l.token, 11, sizeof(l.token));
        auto units = SEQLZ_TOKEN_SYMBOLS;
        for (unsigned bits = 10; units < 2048; --bits) {
            for (unsigned i = 0; i < SEQLZ_TOKEN_SYMBOLS && units < 2048; ++i) {
                units += 1U << (10 - bits);
                l.token[i] = static_cast<unsigned char>(bits);
            }
        }
    };
    auto with_bits = [&](unsigned char bits, unsigned nth) {
        auto* p = std::find(l.token, l.token + SEQLZ_TOKEN_SYMBOLS, bits);
        for (unsigned k = 0; k < nth; ++k) {
            p = std::find(p + 1, l.token + SEQLZ_TOKEN_SYMBOLS, bits);
        }
        REQUIRE(p != l.token + SEQLZ_TOKEN_SYMBOLS);
        return static_cast<std::size_t>(p - l.token);
    };
    complete();
    CHECK(seqlz_tables_init(t.get(), &l) == 0);
    l.token[with_bits(10, 0)] = 11;
    CHECK(seqlz_tables_init(t.get(), &l) == -1); // a gap of one 11-bit code
    complete();
    l.token[with_bits(11, 0)] = 12;
    CHECK(seqlz_tables_init(t.get(), &l) == -1); // longer than 11 bits
    // A complete code, and then one of 12 bits more: the Kraft sum over 11 bits is still complete,
    // only the length check stops the 12-bit code.
    complete();
    auto const a = with_bits(10, 0), b = with_bits(10, 1);
    l.token[a] = 0;
    l.token[b] = 9;
    CHECK(seqlz_tables_init(t.get(), &l) == 0);
    l.token[a] = 12;
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    // a gap: every bit pattern must start a code, the decoder does not check
    l = seqlz_default_lz4;
    auto const shortest = std::min_element(l.ll, l.ll + SEQLZ_LEN_SYMBOLS);
    *shortest = static_cast<unsigned char>(*shortest + 1);
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
}

TEST_CASE("seqlz: any input is safe for the decoder") {
    auto const t = default_tables();
    auto rng = std::mt19937_64(37);
    auto out = std::vector<unsigned char>(4096);
    for (int round = 0; round < 3000; ++round) {
        CAPTURE(round);
        std::vector<unsigned char> c;
        if (round % 2 == 0) {
            // random bytes with a plausible header
            c.resize(8 + rng() % 3000);
            for (auto& b : c) {
                b = static_cast<unsigned char>(rng());
            }
            auto const n_lit = static_cast<unsigned>(rng() % (c.size() - 8));
            c[0] = static_cast<unsigned char>(1 + rng() % 200);
            c[1] = 0;
            c[2] = static_cast<unsigned char>(n_lit);
            c[3] = static_cast<unsigned char>(n_lit >> 8);
            auto const rest = static_cast<unsigned>(c.size() - 8 - n_lit);
            auto const a = rest == 0 ? 0U : static_cast<unsigned>(rng() % rest);
            c[4] = static_cast<unsigned char>(a);
            c[5] = static_cast<unsigned char>(a >> 8);
            c[6] = static_cast<unsigned char>((rest - a) / 2);
            c[7] = static_cast<unsigned char>(((rest - a) / 2) >> 8);
        } else {
            // a valid page with some bits flipped
            auto const p = random_seqlz_page(rng, round % 4);
            c.resize(3 * 4096);
            auto const len = seqlz_encode(t.get(),
                                          p.sequences.data(),
                                          static_cast<unsigned>(p.sequences.size()),
                                          p.literals.data(),
                                          static_cast<unsigned>(p.literals.size()),
                                          c.data(),
                                          static_cast<unsigned>(c.size()));
            REQUIRE(len > 0);
            c.resize(len);
            for (int f = 0; f < 3; ++f) {
                c[rng() % c.size()] ^= static_cast<unsigned char>(1U << (rng() % 8));
            }
        }
        auto const ret = seqlz_decode(t.get(), c.data(), static_cast<unsigned>(c.size()), out.data());
        CHECK((ret == 0 || ret == -1));
    }
}

namespace {

// the literals of a page for its sequences
std::vector<unsigned char> literals_of(std::vector<unsigned char> const& bytes, seqlz_sequence const* seq, unsigned n) {
    auto out = std::vector<unsigned char>();
    std::size_t pos = 0;
    for (unsigned i = 0; i < n; ++i) {
        out.insert(out.end(),
                   bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                   bytes.begin() + static_cast<std::ptrdiff_t>(pos + seq[i].literals));
        pos += seq[i].literals + seq[i].match;
    }
    return out;
}

} // namespace

TEST_CASE("seqlz: the compressor's pages come back, with a state shared over many pages") {
    // One state for all pages, as zram has one per CPU: nothing of one page may change the next.
    auto const t = default_tables(seqlz_default_own);
    auto st = std::make_unique<seqlz_state>();
    auto rng = std::mt19937_64(43);
    auto c = std::vector<unsigned char>(2 * 4096);
    auto out = std::vector<unsigned char>(4096);
    for (int round = 0; round < 800; ++round) {
        CAPTURE(round);
        auto const p = random_seqlz_page(rng, round % 4);
        auto const len = seqlz_compress(t.get(), st.get(), p.bytes.data(), c.data(), static_cast<unsigned>(c.size()));
        REQUIRE(len > 0);
        auto exact = std::vector<unsigned char>(c.begin(), c.begin() + len);
        REQUIRE(seqlz_decode(t.get(), exact.data(), len, out.data()) == 0);
        CHECK(out == p.bytes);
    }
}

TEST_CASE("seqlz: the compressor writes what seqlz_encode writes for seqlz_find's sequences") {
    auto const t = default_tables(seqlz_default_own);
    // two states with the same history, one for each side
    auto a = std::make_unique<seqlz_state>();
    auto b = std::make_unique<seqlz_state>();
    auto rng = std::mt19937_64(47);
    auto seq = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
    for (int round = 0; round < 300; ++round) {
        CAPTURE(round);
        auto const p = random_seqlz_page(rng, round % 4);
        auto const n = seqlz_find(a.get(), p.bytes.data(), seq.data());
        auto const lits = literals_of(p.bytes, seq.data(), n);
        auto expected = std::vector<unsigned char>(2 * 4096);
        auto const elen =
            seqlz_encode(t.get(), seq.data(), n, lits.data(), static_cast<unsigned>(lits.size()), expected.data(), 2 * 4096);
        auto got = std::vector<unsigned char>(2 * 4096);
        auto const glen = seqlz_compress(t.get(), b.get(), p.bytes.data(), got.data(), 2 * 4096);
        REQUIRE(elen > 0);
        REQUIRE(glen == elen);
        CHECK(std::equal(got.begin(), got.begin() + glen, expected.begin()));
    }
}

TEST_CASE("seqlz: the matcher finds a repeat with its whole length") {
    // Random bytes, and bytes 1 to 1 + len again at 62, with other bytes around both copies. Within the
    // first 64 bytes, where the matcher looks at every position.
    auto state = std::make_unique<seqlz_state>();
    auto seq = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
    auto rng = std::mt19937_64(67);
    for (unsigned len = 4; len <= 56; ++len) {
        CAPTURE(len);
        auto bytes = std::vector<unsigned char>(4096);
        for (auto& b : bytes) {
            b = static_cast<unsigned char>(rng());
        }
        std::copy_n(bytes.begin() + 1, len, bytes.begin() + 62);
        bytes[61] = static_cast<unsigned char>(bytes[0] + 1);
        bytes[62 + len] = static_cast<unsigned char>(bytes[1 + len] + 1);
        auto const n = seqlz_find(state.get(), bytes.data(), seq.data());
        REQUIRE(n == 2);
        CHECK(seq[0].literals == 62);
        CHECK(seq[0].match == len);
        CHECK(seq[0].offset == 61);
    }
}

TEST_CASE("seqlz: the most bits per page fit into two pages, less than two pages is an error") {
    // The most bits per page byte: matches of 4 bytes without literals, each with an offset that is not
    // one of the last three. 12 literals, then 1021 such matches cycling through 4 offsets.
    auto const t = default_tables();
    auto rng = std::mt19937_64(59);
    auto bytes = std::vector<unsigned char>();
    for (int i = 0; i < 12; ++i) {
        bytes.push_back(static_cast<unsigned char>(rng()));
    }
    auto seq = std::vector<seqlz_sequence>{{12, 4, 5}};
    unsigned short const offsets[4] = {5, 6, 7, 9};
    for (int i = 1; i < 1021; ++i) {
        seq.push_back({0, 4, offsets[i % 4]});
    }
    seq.push_back({0, 0, 0});
    for (auto const& q : seq) {
        for (unsigned k = 0; k < q.match; ++k) {
            bytes.push_back(bytes[bytes.size() - q.offset]);
        }
    }
    REQUIRE(bytes.size() == 4096);
    auto const literals = std::vector<unsigned char>(bytes.begin(), bytes.begin() + 12);
    // exactly two pages, so ASan sees any write past them
    auto c = std::vector<unsigned char>(2 * 4096);
    auto const len =
        seqlz_encode(t.get(), seq.data(), static_cast<unsigned>(seq.size()), literals.data(), 12, c.data(), 2 * 4096);
    REQUIRE(len > 0);
    c.resize(len);
    auto out = std::vector<unsigned char>(4096);
    REQUIRE(seqlz_decode(t.get(), c.data(), len, out.data()) == 0);
    CHECK(out == bytes);

    auto st = std::make_unique<seqlz_state>();
    auto small = std::vector<unsigned char>(2 * 4096 - 1);
    CHECK(seqlz_compress(t.get(), st.get(), bytes.data(), small.data(), static_cast<unsigned>(small.size())) == 0);
    CHECK(seqlz_encode(t.get(),
                       seq.data(),
                       static_cast<unsigned>(seq.size()),
                       literals.data(),
                       12,
                       small.data(),
                       static_cast<unsigned>(small.size())) == 0);
}

TEST_CASE("seqlz: the compressor needs a code for every symbol") {
    // a complete code where one match length symbol has none: fine for the decoder, not for the
    // encoder. Two codes of 2 bits, ten of 5 and twelve of 6: 2/4 + 10/32 + 12/64 = 1, symbol 24 none
    auto l = seqlz_default_own;
    unsigned char const ml[SEQLZ_LEN_SYMBOLS] = {2, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 0};
    std::memcpy(l.ml, ml, sizeof(ml));
    auto* t = static_cast<seqlz_tables*>(::operator new(seqlz_tables_size()));
    auto const init = seqlz_tables_init(t, &l);
    if (init == 0) {
        auto st = std::make_unique<seqlz_state>();
        auto bytes = std::vector<unsigned char>(4096, 7);
        bytes[100] = 1;
        auto c = std::vector<unsigned char>(2 * 4096);
        CHECK(seqlz_all_symbols(t) == 0);
        CHECK(seqlz_compress(t, st.get(), bytes.data(), c.data(), 2 * 4096) == 0);
    }
    ::operator delete(t);
    CHECK(init == 0); // the lengths above are still a complete code, otherwise this test tests nothing
}

namespace {

// seqlz written from its description in seqlz.h alone, to pin the format: encoder and decoder share
// code, so a change to that code would still roundtrip and only change the format.
std::vector<unsigned char> reference_encode(seqlz_lengths const& lengths,
                                            std::vector<seqlz_sequence> const& seq,
                                            std::vector<unsigned char> const& literals) {
    // canonical Huffman codes, the first code of each length after the codes of all shorter lengths,
    // written least significant bit first, so bit reversed
    auto codes = [](unsigned char const* len, unsigned n) {
        auto count = std::array<unsigned, 17>{};
        for (unsigned s = 0; s < n; ++s) {
            ++count[len[s]];
        }
        count[0] = 0;
        auto next = std::array<unsigned, 17>{};
        auto code = 0U;
        for (unsigned l = 1; l <= 16; ++l) {
            code = (code + count[l - 1]) << 1;
            next[l] = code;
        }
        auto out = std::vector<unsigned>(n);
        for (unsigned s = 0; s < n; ++s) {
            auto c = next[len[s]]++;
            auto r = 0U;
            for (unsigned i = 0; i < len[s]; ++i) {
                r |= ((c >> i) & 1U) << (len[s] - 1 - i);
            }
            out[s] = r;
        }
        return out;
    };
    auto const token = codes(lengths.token, SEQLZ_TOKEN_SYMBOLS);
    auto const ll = codes(lengths.ll, SEQLZ_LEN_SYMBOLS);
    auto const ml = codes(lengths.ml, SEQLZ_LEN_SYMBOLS);

    auto bits = std::vector<bool>();
    auto put = [&](unsigned v, unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            bits.push_back(((v >> i) & 1U) != 0);
        }
    };
    auto value = [&](std::vector<unsigned> const& code, unsigned char const* len, unsigned v) {
        if (v < 16) {
            put(code[v], len[v]);
            return;
        }
        auto const b = static_cast<unsigned>(std::bit_width(v) - 1);
        put(code[12 + b], len[12 + b]);
        put(v - (1U << b), b);
    };
    auto last_offset = 1U;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        auto const last = i + 1 == seq.size();
        auto const l = static_cast<unsigned>(seq[i].literals);
        auto const m = last ? 0U : static_cast<unsigned>(seq[i].match);
        auto const o = static_cast<unsigned>(seq[i].offset);
        // class 0: the last offset, 1: below 256 in 8 raw bits, 2: in 12
        auto const cls = last || o == last_offset ? 0U : o < 256 ? 1U : 2U;
        auto const tok = std::min(l, SEQLZ_LL_CAP) + ((m == 0 ? 0U : std::min(m - 4U, SEQLZ_ML_CAP)) << SEQLZ_LL_BITS) +
                         (cls << (SEQLZ_LL_BITS + SEQLZ_ML_BITS));
        put(token[tok], lengths.token[tok]);
        put(o, cls == 0 ? 0U : cls == 1 ? 8U : 12U);
        if (l >= SEQLZ_LL_CAP) {
            value(ll, lengths.ll, l - SEQLZ_LL_CAP);
        }
        if (last) {
            break;
        }
        if (m - 4 >= SEQLZ_ML_CAP) {
            value(ml, lengths.ml, m - 4 - SEQLZ_ML_CAP);
        }
        last_offset = o;
    }
    auto out = std::vector<unsigned char>{static_cast<unsigned char>(seq.size()),
                                          static_cast<unsigned char>(seq.size() >> 8),
                                          static_cast<unsigned char>(literals.size()),
                                          static_cast<unsigned char>(literals.size() >> 8)};
    out.insert(out.end(), literals.begin(), literals.end());
    for (std::size_t i = 0; i < bits.size(); i += 8) {
        auto byte = 0U;
        for (std::size_t k = 0; k < 8 && i + k < bits.size(); ++k) {
            byte |= (bits[i + k] ? 1U : 0U) << k;
        }
        out.push_back(static_cast<unsigned char>(byte));
    }
    return out;
}

} // namespace

TEST_CASE("seqlz: the encoder writes the format as seqlz.h describes it") {
    auto const t = default_tables();
    auto rng = std::mt19937_64(61);
    for (int round = 0; round < 400; ++round) {
        CAPTURE(round);
        auto const p = random_seqlz_page(rng, round % 4);
        auto got = std::vector<unsigned char>(2 * 4096);
        auto const len = seqlz_encode(t.get(),
                                      p.sequences.data(),
                                      static_cast<unsigned>(p.sequences.size()),
                                      p.literals.data(),
                                      static_cast<unsigned>(p.literals.size()),
                                      got.data(),
                                      2 * 4096);
        got.resize(len);
        CHECK(got == reference_encode(seqlz_default_lz4, p.sequences, p.literals));
    }
}

namespace {

// bytelz decoded from its description in bytelz.h alone, byte by byte, to pin the format: encoder and
// decoder share the matcher and the copies, a change to those would still roundtrip.
bool reference_bytelz_decode(std::vector<unsigned char> const& c, std::vector<unsigned char>& out) {
    out.clear();
    auto i = std::size_t{0};
    auto last = 1U, before = 4U;
    auto extension = [&](unsigned& v) {
        v = 0;
        for (unsigned shift = 0;; shift += 7) {
            if (i == c.size() || shift > 14) {
                return false;
            }
            auto const b = c[i++];
            v |= (b & 127U) << shift;
            if ((b & 128U) == 0) {
                return true;
            }
        }
    };
    while (true) {
        if (i == c.size()) {
            return false;
        }
        auto const tok = c[i++];
        auto ll = tok & 7U, v = 0U;
        if (ll == 7 && !extension(v)) {
            return false;
        }
        ll += v;
        for (unsigned k = 0; k < ll; ++k) {
            if (i == c.size()) {
                return false;
            }
            out.push_back(c[i++]);
        }
        if (out.size() >= 4096) {
            return out.size() == 4096 && i == c.size() && (tok >> 3) == 0;
        }
        auto off = 0U;
        switch (tok >> 6) {
        case 0:
            off = last;
            break;
        case 1:
            off = before;
            before = last;
            break;
        case 2:
            if (i == c.size()) {
                return false;
            }
            off = c[i++] + 1U;
            before = last;
            break;
        default:
            if (c.size() - i < 2) {
                return false;
            }
            off = static_cast<unsigned>(c[i]) | static_cast<unsigned>(c[i + 1]) << 8;
            i += 2;
            before = last;
        }
        last = off;
        auto ml = ((tok >> 3) & 7U) + 4U;
        if (ml == 11 && !extension(v)) {
            return false;
        }
        ml += ml == 11 ? v : 0U;
        if (off == 0 || off > out.size()) {
            return false;
        }
        for (unsigned k = 0; k < ml; ++k) {
            out.push_back(out[out.size() - off]);
        }
    }
}

std::vector<unsigned char> bytelz_compressed(bytelz_state& st, std::vector<unsigned char> const& bytes) {
    auto c = std::vector<unsigned char>(2 * 4096);
    auto const len = bytelz_compress(&st, bytes.data(), c.data(), static_cast<unsigned>(c.size()));
    REQUIRE(len > 0);
    c.resize(len);
    return c;
}

} // namespace

TEST_CASE("bytelz: pages come back, and decode as bytelz.h describes the format") {
    auto st = std::make_unique<bytelz_state>();
    auto rng = std::mt19937_64(71);
    auto out = std::vector<unsigned char>(4096);
    auto ref = std::vector<unsigned char>();
    for (int round = 0; round < 800; ++round) {
        CAPTURE(round);
        auto const p = random_seqlz_page(rng, round % 4);
        auto const c = bytelz_compressed(*st, p.bytes);
        REQUIRE(bytelz_decode(c.data(), static_cast<unsigned>(c.size()), out.data()) == 0);
        CHECK(out == p.bytes);
        REQUIRE(reference_bytelz_decode(c, ref));
        CHECK(ref == p.bytes);
    }
}

TEST_CASE("bytelz: a page without matches is a token, 2 bytes of extension and the page") {
    auto st = std::make_unique<bytelz_state>();
    auto rng = std::mt19937_64(83);
    auto bytes = std::vector<unsigned char>(4096);
    for (auto& b : bytes) {
        b = static_cast<unsigned char>(rng());
    }
    auto const c = bytelz_compressed(*st, bytes);
    REQUIRE(c.size() == 1 + 2 + 4096);
    CHECK(c[0] == 7); // ll 7 and an extension, nothing else
    CHECK(c[1] == (((4096 - 7) & 127) | 128));
    CHECK(c[2] == (4096 - 7) >> 7);
}

TEST_CASE("bytelz: less than two pages of room is an error, and so is a cut off page") {
    auto st = std::make_unique<bytelz_state>();
    auto rng = std::mt19937_64(73);
    auto const p = random_seqlz_page(rng, 1);
    auto c = std::vector<unsigned char>(2 * 4096);
    CHECK(bytelz_compress(st.get(), p.bytes.data(), c.data(), 2 * 4096 - 1) == 0);
    c = bytelz_compressed(*st, p.bytes);
    auto out = std::vector<unsigned char>(4096);
    for (auto len = std::size_t{0}; len < c.size(); ++len) {
        CAPTURE(len);
        CHECK(bytelz_decode(c.data(), static_cast<unsigned>(len), out.data()) == -1);
    }
    CHECK(bytelz_decode(c.data(), static_cast<unsigned>(c.size()), out.data()) == 0);
}

TEST_CASE("bytelz: any input is safe for the decoder") {
    auto st = std::make_unique<bytelz_state>();
    auto rng = std::mt19937_64(79);
    auto out = std::vector<unsigned char>(4096);
    auto ref = std::vector<unsigned char>();
    for (int round = 0; round < 3000; ++round) {
        CAPTURE(round);
        auto c = std::vector<unsigned char>();
        if (round % 2 == 0) {
            c.resize(1 + rng() % 4000);
            for (auto& b : c) {
                b = static_cast<unsigned char>(rng());
            }
        } else {
            // a valid page with some bits flipped
            c = bytelz_compressed(*st, random_seqlz_page(rng, round % 4).bytes);
            for (int f = 0; f < 3; ++f) {
                c[rng() % c.size()] ^= static_cast<unsigned char>(1U << (rng() % 8));
            }
        }
        auto const ret = bytelz_decode(c.data(), static_cast<unsigned>(c.size()), out.data());
        // the same verdict as the reference, and the same page where both accept it
        CHECK(ret == (reference_bytelz_decode(c, ref) ? 0 : -1));
        if (ret == 0) {
            CHECK(std::equal(ref.begin(), ref.end(), out.begin()));
        }
    }
}
