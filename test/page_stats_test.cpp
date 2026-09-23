// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "page_stats.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

using quetschn::analyze_page;

namespace {

std::vector<std::byte> page_of_words(std::uint64_t word) {
    auto page = std::vector<std::byte>(4096);
    for (std::size_t pos = 0; pos < page.size(); pos += sizeof(word)) {
        std::memcpy(page.data() + pos, &word, sizeof(word));
    }
    return page;
}

} // namespace

TEST_CASE("page_stats: all-zero page") {
    auto const s = analyze_page(page_of_words(0));
    CHECK(s.zero_bytes == 4096);
    CHECK(s.same_filled);
    CHECK(s.fill_value == 0);
    CHECK(s.entropy_bits == doctest::Approx(0.0));
}

TEST_CASE("page_stats: a repeated word is same-filled, even with different bytes in the word") {
    // zram compares whole words, so 01 02 03 ... 08 repeated is same-filled. 8 byte values, each 1/8 of
    // the page, is exactly 3 bits of entropy.
    auto const s = analyze_page(page_of_words(0x0807060504030201));
    CHECK(s.same_filled);
    CHECK(s.fill_value == 0x0807060504030201);
    CHECK(s.zero_bytes == 0);
    CHECK(s.entropy_bits == doctest::Approx(3.0));
}

TEST_CASE("page_stats: one different word anywhere breaks same-filled") {
    for (std::size_t pos : {std::size_t{0}, std::size_t{2048}, std::size_t{4088}}) {
        CAPTURE(pos);
        auto page = page_of_words(0x4242424242424242);
        page[pos] = std::byte{0x43};
        auto const s = analyze_page(page);
        CHECK_FALSE(s.same_filled);
        CHECK(s.fill_value == 0);
    }
}

TEST_CASE("page_stats: every byte value equally often is 8 bits of entropy") {
    auto page = std::vector<std::byte>(4096);
    for (std::size_t i = 0; i < page.size(); ++i) {
        page[i] = static_cast<std::byte>(i % 256);
    }
    auto const s = analyze_page(page);
    CHECK(s.entropy_bits == doctest::Approx(8.0));
    CHECK(s.zero_bytes == 16);
    CHECK_FALSE(s.same_filled);
}

TEST_CASE("page_stats: sizes that are not whole words are rejected") {
    auto page = std::vector<std::byte>(4095);
    CHECK_THROWS_AS((void)analyze_page(page), std::invalid_argument);
    CHECK_THROWS_AS((void)analyze_page({}), std::invalid_argument);
}
