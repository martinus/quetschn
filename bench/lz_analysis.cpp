// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "lz_analysis.h"

#include <stdexcept>

namespace quetschn {

// lz4 block format: token (4 bits literal length, 4 bits match length - 4), length bytes of 255 plus a
// rest, the literals, 2 bytes offset, more match length bytes. The last sequence has no match.
parsed_page parse_lz4(std::uint8_t const* p, std::size_t n) {
    auto out = parsed_page{};
    out.lz4_size = static_cast<std::uint32_t>(n);
    std::size_t i = 0;
    auto more = [&](std::uint32_t& len) {
        std::uint8_t b = 0;
        do {
            if (i >= n) {
                throw std::runtime_error("truncated lz4 block");
            }
            b = p[i++];
            len += b;
        } while (b == 255);
    };
    while (i < n) {
        auto const token = p[i++];
        auto s = sequence{};
        s.literals = token >> 4U;
        if (s.literals == 15) {
            more(s.literals);
        }
        if (i + s.literals > n) {
            throw std::runtime_error("truncated lz4 literals");
        }
        out.literals.insert(out.literals.end(), p + i, p + i + s.literals);
        i += s.literals;
        if (i == n) {
            out.sequences.push_back(s);
            break;
        }
        if (i + 2 > n) {
            throw std::runtime_error("truncated lz4 offset");
        }
        s.offset = static_cast<std::uint32_t>(p[i] | (p[i + 1] << 8U));
        i += 2;
        s.match = (token & 15U) + 4;
        if ((token & 15U) == 15) {
            more(s.match);
        }
        out.sequences.push_back(s);
    }
    return out;
}

std::vector<std::uint8_t> reconstruct(parsed_page const& p) {
    auto out = std::vector<std::uint8_t>();
    std::size_t lit = 0;
    for (auto const& s : p.sequences) {
        if (lit + s.literals > p.literals.size()) {
            throw std::runtime_error("reconstruct: more literals than parsed");
        }
        out.insert(out.end(),
                   p.literals.begin() + static_cast<std::ptrdiff_t>(lit),
                   p.literals.begin() + static_cast<std::ptrdiff_t>(lit + s.literals));
        lit += s.literals;
        if (s.match == 0) {
            continue;
        }
        if (s.offset == 0 || s.offset > out.size()) {
            throw std::runtime_error("reconstruct: offset before the start");
        }
        // byte by byte, matches may overlap their own output
        auto from = out.size() - s.offset;
        for (std::uint32_t k = 0; k < s.match; ++k) {
            out.push_back(out[from + k]);
        }
    }
    return out;
}

} // namespace quetschn
