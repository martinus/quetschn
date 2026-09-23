// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "bdelta.h"
#include "seqlz.h"
#include "shuffle.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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

std::unique_ptr<seqlz_tables, void (*)(seqlz_tables*)> default_tables() {
    auto* t = static_cast<seqlz_tables*>(::operator new(seqlz_tables_size()));
    REQUIRE(seqlz_tables_init(t, &seqlz_default_lz4) == 0);
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

TEST_CASE("seqlz: the token holds ll up to 15 and ml - 4 up to 31, the last sequence has ml - 4 = 0") {
    CHECK(seqlz_token(0, 4) == 0);
    CHECK(seqlz_token(3, 9) == 3 + 16 * 5);
    CHECK(seqlz_token(14, 34) == 14 + 16 * 30);
    CHECK(seqlz_token(15, 35) == 15 + 16 * 31);
    CHECK(seqlz_token(4000, 3000) == 15 + 16 * 31);
    CHECK(seqlz_token(7, 0) == 7);
    CHECK(seqlz_token(20, 0) == 15);
}

TEST_CASE("seqlz: a sequence with long lengths and a large offset needs more bits than one refill") {
    // 1100 literals (value 1085, 10 extra bits), a match of 2100 (value 2081, 11 extra bits) at offset
    // 1050 (10 extra bits), then 896 literals: token, three codes and 31 extra bits in one sequence
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
    l.off[0] = 1; // together with the others more codes than fit: over-subscribed
    l.off[1] = 1;
    l.off[2] = 1;
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    l = seqlz_default_lz4;
    std::memset(l.ml, 0, sizeof(l.ml)); // no code at all
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    // 512 tokens of 9 bits each fill the table exactly, a complete code
    l = seqlz_default_lz4;
    std::memset(l.token, 9, sizeof(l.token));
    CHECK(seqlz_tables_init(t.get(), &l) == 0);
    // tokens may have 11 bits, two more than the others. Tokens 0 to 2 with 10, 11 and 11 bits need
    // the room of one 9-bit code, so two others get 8 bits: complete again.
    auto complete = [&] {
        std::memset(l.token, 9, sizeof(l.token));
        l.token[0] = 10;
        l.token[1] = 11;
        l.token[2] = 11;
        l.token[4] = 8;
        l.token[5] = 8;
    };
    complete();
    CHECK(seqlz_tables_init(t.get(), &l) == 0);
    l.token[5] = 9;
    CHECK(seqlz_tables_init(t.get(), &l) == -1); // a gap of one 9-bit code
    complete();
    l.token[2] = 12; // two 12-bit codes instead of an 11 and a 9-bit one, a 9-bit one becomes 8
    l.token[6] = 12;
    l.token[7] = 8;
    CHECK(seqlz_tables_init(t.get(), &l) == -1); // complete, but longer than 11 bits
    // A complete 11-bit code, plus one code of 12 bits: the Kraft sum over 11 bits is complete, only
    // the length check stops the 12-bit code.
    complete();
    l.token[10] = 0;
    l.token[11] = 8;
    CHECK(seqlz_tables_init(t.get(), &l) == 0);
    l.token[10] = 12;
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    // a gap: every bit pattern must start a code, the decoder does not check
    l = seqlz_default_lz4;
    l.off[0] = static_cast<unsigned char>(l.off[0] + 1);
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

std::unique_ptr<seqlz_tables, void (*)(seqlz_tables*)> own_tables() {
    auto* t = static_cast<seqlz_tables*>(::operator new(seqlz_tables_size()));
    REQUIRE(seqlz_tables_init(t, &seqlz_default_own) == 0);
    return {t, [](seqlz_tables* p) {
                ::operator delete(p);
            }};
}

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
    // One state for all pages: its hash table keeps entries of earlier pages, which point anywhere into
    // the page, also behind the current position; they must only ever cost a failed comparison.
    auto const t = own_tables();
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
    auto const t = own_tables();
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

TEST_CASE("seqlz: a bitstream that does not fit makes a raw page, a too small buffer an error") {
    auto const t = own_tables();
    auto st = std::make_unique<seqlz_state>();
    auto rng = std::mt19937_64(53);
    auto const p = random_seqlz_page(rng, 0); // many short matches, a long bitstream
    // the back half of dst has room for only a few bytes of bitstream
    auto small = std::vector<unsigned char>(SEQLZ_HEADER + 4096 + 64 + 40);
    auto const len = seqlz_compress(t.get(), st.get(), p.bytes.data(), small.data(), static_cast<unsigned>(small.size()));
    REQUIRE(len > 4096);
    CHECK(len < 4096 + 16);
    small.resize(len);
    auto out = std::vector<unsigned char>(4096);
    REQUIRE(seqlz_decode(t.get(), small.data(), len, out.data()) == 0);
    CHECK(out == p.bytes);
    auto tiny = std::vector<unsigned char>(SEQLZ_HEADER + 4096 + 63);
    CHECK(seqlz_compress(t.get(), st.get(), p.bytes.data(), tiny.data(), static_cast<unsigned>(tiny.size())) == 0);
}

TEST_CASE("seqlz: the compressor needs a code for every symbol") {
    // a complete code where one offset symbol has none: fine for the decoder, not for the encoder
    // two offset codes of 3 bits and twelve of 4: 2/8 + 12/16 = 1, and symbol 14 has none
    auto l = seqlz_default_own;
    unsigned char const off[SEQLZ_OFF_SYMBOLS] = {3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0};
    std::memcpy(l.off, off, sizeof(off));
    auto* t = static_cast<seqlz_tables*>(::operator new(seqlz_tables_size()));
    auto const init = seqlz_tables_init(t, &l);
    if (init == 0) {
        auto st = std::make_unique<seqlz_state>();
        auto bytes = std::vector<unsigned char>(4096, 7);
        bytes[100] = 1;
        auto c = std::vector<unsigned char>(2 * 4096);
        CHECK(seqlz_compress(t, st.get(), bytes.data(), c.data(), 2 * 4096) == 0);
    }
    ::operator delete(t);
    CHECK(init == 0); // the lengths above are still a complete code, otherwise this test tests nothing
}
