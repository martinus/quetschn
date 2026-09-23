// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "kernel_codecs/zram_codec.h"
#include "lz_analysis.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

std::vector<std::uint8_t> lz4hc(std::vector<std::uint8_t> const& src) {
    auto p = quetschn_params{};
    p.level = 9;
    p.page_size = 4096;
    REQUIRE(quetschn_codec_lz4hc.setup_params(&p) == 0);
    auto s = quetschn_stream{};
    REQUIRE(quetschn_codec_lz4hc.create(&p, &s) == 0);
    auto dst = std::vector<std::uint8_t>(2 * src.size() + 64);
    auto len = static_cast<unsigned int>(dst.size());
    REQUIRE(quetschn_codec_lz4hc.compress(&p, &s, src.data(), static_cast<unsigned int>(src.size()), dst.data(), &len) == 0);
    quetschn_codec_lz4hc.destroy(&s);
    quetschn_codec_lz4hc.release_params(&p);
    dst.resize(len);
    return dst;
}

} // namespace

TEST_CASE("lz analysis: every page comes back from the parsed lz4 sequences") {
    auto rng = std::mt19937_64(21);
    for (int round = 0; round < 50; ++round) {
        CAPTURE(round);
        auto page = std::vector<std::uint8_t>(4096);
        // runs, repeats at small and large distances, long literal runs, random: all length encodings
        for (std::size_t i = 0; i < page.size(); ++i) {
            auto const r = rng();
            switch (round % 5) {
            case 0:
                page[i] = static_cast<std::uint8_t>(r);
                break;
            case 1:
                page[i] = static_cast<std::uint8_t>(i % 7 == 0 ? r : 0);
                break;
            case 2:
                page[i] = i >= 1000 ? page[i - 1000] : static_cast<std::uint8_t>(r);
                break;
            case 3:
                page[i] = static_cast<std::uint8_t>(i < 300 ? r : 'x');
                break;
            default:
                page[i] = static_cast<std::uint8_t>((r % 4 == 0) ? r : i / 64);
                break;
            }
        }
        auto const c = lz4hc(page);
        auto const parsed = quetschn::parse_lz4(c.data(), c.size());
        CHECK(parsed.lz4_size == c.size());
        CHECK(quetschn::reconstruct(parsed) == page);
    }
}

TEST_CASE("lz analysis: truncated blocks and bad offsets are errors") {
    auto page = std::vector<std::uint8_t>(4096);
    for (std::size_t i = 0; i < page.size(); ++i) {
        page[i] = static_cast<std::uint8_t>(i < 400 ? i * 7 : 0);
    }
    auto c = lz4hc(page);
    // Every cut either parses, as a shorter block, or throws. It never reads past the cut, which ASan
    // checks: the copy has exactly the cut's size.
    auto errors = 0;
    for (std::size_t n = 1; n < c.size(); ++n) {
        auto const cut = std::vector<std::uint8_t>(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(n));
        try {
            (void)quetschn::parse_lz4(cut.data(), cut.size());
        } catch (std::runtime_error const&) {
            ++errors;
        }
    }
    CHECK(errors > 0);
    auto parsed = quetschn::parse_lz4(c.data(), c.size());
    REQUIRE(parsed.sequences.size() > 1);
    parsed.sequences[1].offset = 60000;
    CHECK_THROWS_AS((void)quetschn::reconstruct(parsed), std::runtime_error);
}
