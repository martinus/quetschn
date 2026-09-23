// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "page_stats.h"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace quetschn {

page_stats analyze_page(std::span<std::byte const> page) {
    if (page.empty() || page.size() % sizeof(std::uint64_t) != 0) {
        throw std::invalid_argument("analyze_page: size must be a non-zero multiple of 8");
    }
    auto stats = page_stats{};

    auto hist = std::array<std::size_t, 256>{};
    for (auto b : page) {
        ++hist[static_cast<unsigned char>(b)];
    }
    stats.zero_bytes = hist[0];
    auto const n = static_cast<double>(page.size());
    for (auto count : hist) {
        if (count != 0) {
            auto const p = static_cast<double>(count) / n;
            stats.entropy_bits -= p * std::log2(p);
        }
    }

    std::uint64_t first = 0;
    std::memcpy(&first, page.data(), sizeof(first));
    stats.same_filled = true;
    for (std::size_t pos = sizeof(first); pos < page.size(); pos += sizeof(first)) {
        std::uint64_t word = 0;
        std::memcpy(&word, page.data() + pos, sizeof(word));
        if (word != first) {
            stats.same_filled = false;
            break;
        }
    }
    if (stats.same_filled) {
        stats.fill_value = first;
    }
    return stats;
}

} // namespace quetschn
