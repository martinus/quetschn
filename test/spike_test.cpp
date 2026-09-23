// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "wk64.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

using page = std::array<std::uint64_t, WK64_WORDS>;

using decoder = int (*)(void const*, unsigned int, void*);

struct named_decoder {
    char const* name;
    decoder fn;
};

constexpr auto decoders = std::array<named_decoder, 4>{
    named_decoder{"switch", wk64_decompress_switch},
    named_decoder{"branchless", wk64_decompress_branchless},
    named_decoder{"zeroskip", wk64_decompress_zeroskip},
    named_decoder{"slots", wk64_decompress_slots},
};

std::vector<unsigned char> compress(page const& p) {
    auto out = std::vector<unsigned char>(WK64_MAX_COMPRESSED);
    auto const len = wk64_compress(p.data(), out.data(), static_cast<unsigned int>(out.size()));
    REQUIRE(len > 0);
    out.resize(len);
    return out;
}

unsigned int tag(std::vector<unsigned char> const& c, std::size_t i) {
    return (c[i / 4] >> ((i % 4) * 2)) & 3U;
}

void check_roundtrip(page const& p) {
    auto const c = compress(p);
    for (auto const& d : decoders) {
        CAPTURE(std::string(d.name));
        auto out = page{};
        REQUIRE(d.fn(c.data(), static_cast<unsigned int>(c.size()), out.data()) == 0);
        CHECK(out == p);
    }
}

} // namespace

TEST_CASE("spike: sizes follow the tags") {
    SUBCASE("a zero page is only the tags") {
        auto const c = compress(page{});
        CHECK(c.size() == WK64_TAG_BYTES);
        check_roundtrip(page{});
    }
    SUBCASE("words that do not repeat are stored whole") {
        auto p = page{};
        for (std::size_t i = 0; i < p.size(); ++i) {
            p[i] = 0x0123456789abcdefULL * (i + 1) + (static_cast<std::uint64_t>(i) << 40);
        }
        auto const c = compress(p);
        // the high halves are all different, so almost every word is a miss; the exact count does not
        // matter, the formula does
        auto n = std::array<std::size_t, 4>{};
        for (std::size_t i = 0; i < p.size(); ++i) {
            ++n[tag(c, i)];
        }
        CHECK(n[3] > 400);
        CHECK(c.size() == WK64_TAG_BYTES + (n[1] + n[2] + 1) / 2 + 4 * n[2] + 8 * n[3]);
        check_roundtrip(p);
    }
    SUBCASE("zero, miss, exact, partial") {
        auto p = page{};
        p[1] = 0x00007f1234560010ULL; // miss: the table is all zero
        p[2] = 0x00007f1234560010ULL; // exact
        p[3] = 0x00007f1234560020ULL; // partial: same high half
        p[4] = 42;                    // partial against the zeros in its slot
        auto const c = compress(p);
        CHECK(tag(c, 0) == 0);
        CHECK(tag(c, 1) == 3);
        CHECK(tag(c, 2) == 1);
        CHECK(tag(c, 3) == 2);
        CHECK(tag(c, 4) == 2);
        CHECK(tag(c, 5) == 0);
        // tags, 3 indices in 2 bytes, 2 low halves, 1 full word
        CHECK(c.size() == WK64_TAG_BYTES + 2 + 2 * 4 + 8);
        check_roundtrip(p);
    }
}

TEST_CASE("spike: a group of 4 zero words resets slot 0 of the table") {
    // 0x0000000d_00000005 hashes to slot 0, like every small integer. Words 4 to 7 are zero and put 0
    // back into slot 0. Word 8 is then PARTIAL against that 0, and a decoder that skips the table
    // update for the zero group decodes 0x0000000d_00000007 instead.
    auto p = page{};
    p[1] = 0x0000000d00000005ULL;
    p[8] = 7;
    auto const c = compress(p);
    CHECK(tag(c, 1) == 3);
    CHECK(c[1] == 0); // words 4 to 7: one zero tag byte
    CHECK(tag(c, 8) == 2);
    check_roundtrip(p);
}

TEST_CASE("spike: a single zero word resets slot 0 of the table too") {
    // like above, but the zero word shares its tag byte with non-zero words, so no fast path applies
    auto p = page{};
    p[1] = 0x0000000d00000005ULL;
    p[2] = 0;
    p[3] = 7;
    auto const c = compress(p);
    CHECK(tag(c, 1) == 3);
    CHECK(tag(c, 2) == 0);
    CHECK(tag(c, 3) == 2);
    check_roundtrip(p);
}

TEST_CASE("spike: roundtrip of pointer-like and random pages") {
    auto rng = std::mt19937_64(7);
    for (int round = 0; round < 200; ++round) {
        CAPTURE(round);
        auto p = page{};
        auto const kind = round % 4;
        for (auto& w : p) {
            auto const r = rng();
            switch (kind) {
            case 0: // random
                w = r;
                break;
            case 1: // pointers into a few regions, small integers, zeros
                w = (r % 3 == 0) ? 0 : (r % 3 == 1) ? (r >> 58) : (0x00007f0000000000ULL | ((r % 4) << 32) | (r & 0xfff8));
                break;
            case 2: // a few distinct words
                w = 0x1111111111111111ULL * (r % 5);
                break;
            default: // everything in between
                w = (r & 1) ? r : (r & 0xffff);
                break;
            }
        }
        check_roundtrip(p);
    }
}

TEST_CASE("spike: the encoder refuses a buffer that is too small") {
    auto p = page{};
    p[0] = 1;
    auto out = std::vector<unsigned char>(WK64_MAX_COMPRESSED);
    auto const len = wk64_compress(p.data(), out.data(), static_cast<unsigned int>(out.size()));
    REQUIRE(len > 0);
    CHECK(wk64_compress(p.data(), out.data(), len - 1) == 0);
    CHECK(wk64_compress(p.data(), out.data(), len) == len);
}

TEST_CASE("spike: a wrong length is rejected") {
    auto p = page{};
    p[3] = 0xdeadbeefULL;
    auto c = compress(p);
    c.resize(c.size() + 1);
    for (auto const& d : decoders) {
        CAPTURE(std::string(d.name));
        auto out = page{};
        CHECK(d.fn(c.data(), static_cast<unsigned int>(c.size()), out.data()) == -1);
        CHECK(d.fn(c.data(), static_cast<unsigned int>(c.size() - 2), out.data()) == -1);
        // shorter than the tags, in a buffer of exactly that size, so ASan sees any read past it
        auto const short_input = std::vector<unsigned char>(c.begin(), c.begin() + WK64_TAG_BYTES - 1);
        CHECK(d.fn(short_input.data(), static_cast<unsigned int>(short_input.size()), out.data()) == -1);
        CHECK(d.fn(nullptr, 0, out.data()) == -1);
    }
}

TEST_CASE("spike: any input is safe, and both decoders agree on it") {
    // Random tags, and random section contents of exactly the length the tags ask for. The buffer is
    // allocated with exactly that size, so ASan catches every read outside it.
    auto rng = std::mt19937_64(11);
    for (int round = 0; round < 2000; ++round) {
        CAPTURE(round);
        auto tags = std::vector<unsigned char>(WK64_TAG_BYTES);
        // bias toward some tag kinds per round, so that empty sections come up too
        auto const allowed = static_cast<unsigned int>(rng() % 15 + 1);
        auto n = std::array<std::size_t, 4>{};
        for (std::size_t i = 0; i < WK64_WORDS; ++i) {
            auto t = static_cast<unsigned int>(rng() % 4);
            while ((allowed & (1U << t)) == 0) {
                t = (t + 1) % 4;
            }
            tags[i / 4] = static_cast<unsigned char>(tags[i / 4] | (t << ((i % 4) * 2)));
            ++n[t];
        }
        auto const size = WK64_TAG_BYTES + (n[1] + n[2] + 1) / 2 + 4 * n[2] + 8 * n[3];
        auto c = std::vector<unsigned char>(size);
        std::memcpy(c.data(), tags.data(), tags.size());
        for (auto i = tags.size(); i < size; ++i) {
            c[i] = static_cast<unsigned char>(rng());
        }
        auto first = page{};
        REQUIRE(wk64_decompress_switch(c.data(), static_cast<unsigned int>(size), first.data()) == 0);
        for (auto const& d : decoders) {
            CAPTURE(std::string(d.name));
            auto out = page{};
            REQUIRE(d.fn(c.data(), static_cast<unsigned int>(size), out.data()) == 0);
            // slots takes the slot to update from the stream; on streams the encoder cannot produce
            // that can decode differently, which is fine as long as it is memory safe
            if (d.fn != wk64_decompress_slots) {
                CHECK(out == first);
            }
            // and a byte less is never accepted
            CHECK(d.fn(c.data(), static_cast<unsigned int>(size - 1), out.data()) == -1);
        }
    }
}
