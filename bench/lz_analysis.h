// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace quetschn {

// One lz4 sequence: literals, then a match, which the last sequence of a block does not have.
struct sequence {
    std::uint32_t literals = 0; // number of literal bytes before the match
    std::uint32_t match = 0;    // match length, 0 for the last sequence of a block
    std::uint32_t offset = 0;
};

struct parsed_page {
    std::uint32_t lz4_size = 0;
    std::vector<sequence> sequences;
    std::vector<std::uint8_t> literals; // of all sequences, in order
};

// Splits an lz4 block into its sequences. Throws std::runtime_error for a truncated block.
[[nodiscard]] parsed_page parse_lz4(std::uint8_t const* p, std::size_t n);

// The bytes the sequences stand for. Throws std::runtime_error for an offset before the start.
[[nodiscard]] std::vector<std::uint8_t> reconstruct(parsed_page const& p);

} // namespace quetschn
