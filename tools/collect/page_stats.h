// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace quetschn {

struct page_stats {
    std::size_t zero_bytes = 0;
    // zram stores a page whose machine words are all equal without any codec, see page_same_filled()
    // in drivers/block/zram/zram_drv.c. All-zero pages are the most common case of this.
    bool same_filled = false;
    std::uint64_t fill_value = 0; // the repeated word in native byte order, only valid if same_filled
    double entropy_bits = 0.0;    // Shannon entropy of the byte histogram, 0 to 8 bits per byte
};

// page.size() must be a multiple of 8
[[nodiscard]] page_stats analyze_page(std::span<std::byte const> page);

} // namespace quetschn
