// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "bdelta.h"
#include "shuffle.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
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
