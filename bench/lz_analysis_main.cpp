// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Where does the ratio of zstd come from? Takes the matches that lz4hc finds on every page and
// estimates what they would cost with entropy coding, see docs/explored-designs.md.
//
// The sequences (literal length, match length, offset) and the literal bytes get zstd-like symbols:
// small values directly, larger ones as a power-of-two bucket plus extra bits, and offsets that repeat
// one of the last three offsets as repeat codes like zstd's. The cost of a symbol is -log2 of its
// frequency over the whole corpus: a static model, with the tables fixed in the decoder, because a
// 4 KiB page has no room for its own tables. That is an estimate, a real entropy coder needs a bit more,
// and the model is trained on the pages it is measured on, so it is on the optimistic side.

#include "harness.h"
#include "kernel_codecs/zram_codec.h"
#include "lz_analysis.h"
#include "page_stats.h"
#include "zsmalloc_cost.h"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using quetschn::parse_lz4;
using quetschn::parsed_page;

// symbol and extra bits: values below 16 directly, above as 16 + bucket with bucket extra bits
struct symbol {
    unsigned code;
    unsigned extra_bits;
};

symbol length_symbol(std::uint32_t v) {
    if (v < 16) {
        return {v, 0};
    }
    auto const bucket = static_cast<unsigned>(std::bit_width(v) - 1); // 4 or more
    return {16 + bucket - 4, bucket};
}

// offsets: 0 to 2 repeat one of the last three offsets, 3 + bucket otherwise, like zstd
constexpr unsigned repeat_codes = 3;

struct model {
    std::array<double, 64> lit_len{};
    std::array<double, 64> match_len{};
    std::array<double, 64> offset{};
    std::array<double, 256> literal{};
};

// Counts (count = true) or costs in bits (count = false) of one page. with_repeat: offsets that
// repeat one of the last three get a repeat code.
double walk(parsed_page const& pg, model& m, bool count, bool with_repeat, bool code_literals) {
    auto bits = 0.0;
    auto add = [&](std::array<double, 64>& table, symbol s) {
        if (count) {
            table[s.code] += 1;
        } else {
            bits += -std::log2(table[s.code]) + s.extra_bits;
        }
    };
    auto rep = std::array<std::uint32_t, 3>{1, 4, 8}; // zstd's initial repeat offsets
    for (auto const& s : pg.sequences) {
        add(m.lit_len, length_symbol(s.literals));
        if (s.match == 0) {
            continue;
        }
        add(m.match_len, length_symbol(s.match - 4));
        auto code = symbol{};
        auto r = with_repeat ? 0U : repeat_codes;
        while (r < repeat_codes && rep[r] != s.offset) {
            ++r;
        }
        if (r < repeat_codes) {
            code = {r, 0};
            // move to front
            for (; r > 0; --r) {
                rep[r] = rep[r - 1];
            }
            rep[0] = s.offset;
        } else {
            auto const bucket = static_cast<unsigned>(std::bit_width(s.offset) - 1);
            code = {repeat_codes + bucket, bucket};
            rep = {s.offset, rep[0], rep[1]};
        }
        add(m.offset, code);
    }
    for (auto b : pg.literals) {
        if (count) {
            m.literal[b] += 1;
        } else {
            bits += code_literals ? -std::log2(m.literal[b]) : 8.0;
        }
    }
    return bits;
}

void normalize(auto& table) {
    auto total = 0.0;
    for (auto v : table) {
        total += v;
    }
    for (auto& v : table) {
        v = total == 0 ? 1.0 : std::max(v, 0.5) / total; // unseen symbols still get a cost
    }
}

void usage() {
    std::fprintf(stderr, "usage: quetschn-lz-analysis --corpus <base> [--level <lz4hc level, default 9>]\n");
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    int level = 9;
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        if (arg == "--corpus" && i + 1 < argc) {
            base = argv[++i];
        } else if (arg == "--level" && i + 1 < argc) {
            auto const v = std::string_view(argv[++i]);
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), level);
            if (ec != std::errc{} || ptr != v.data() + v.size()) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (base.empty()) {
        usage();
        return 2;
    }
    try {
        auto const c = quetschn::load_corpus(base);
        auto const zs = quetschn::zsmalloc_model(quetschn::zsmalloc_config{.page_size = c.page_size});

        auto params = quetschn_params{};
        params.level = level;
        params.page_size = static_cast<unsigned int>(c.page_size);
        if (quetschn_codec_lz4hc.setup_params(&params) != 0) {
            throw std::invalid_argument("lz4hc rejects level " + std::to_string(level));
        }
        auto stream = quetschn_stream{};
        if (quetschn_codec_lz4hc.create(&params, &stream) != 0) {
            throw std::runtime_error("lz4hc: create failed");
        }
        auto dst = std::vector<std::uint8_t>(2 * c.page_size);
        auto pages = std::vector<parsed_page>();
        for (std::size_t i = 0; i < c.size(); ++i) {
            auto const src = c.page(i);
            if (quetschn::analyze_page(src).same_filled) {
                continue;
            }
            auto len = static_cast<unsigned int>(dst.size());
            if (quetschn_codec_lz4hc.compress(
                    &params, &stream, src.data(), static_cast<unsigned int>(src.size()), dst.data(), &len) != 0) {
                throw std::runtime_error("lz4hc: compress failed");
            }
            auto parsed = parse_lz4(dst.data(), len);
            // the estimate is only as good as the parse, so every page must come back from it
            auto const back = quetschn::reconstruct(parsed);
            if (back.size() != src.size() || std::memcmp(back.data(), src.data(), src.size()) != 0) {
                throw std::runtime_error("page " + std::to_string(i) + " does not come back from its lz4 sequences");
            }
            pages.push_back(std::move(parsed));
        }
        quetschn_codec_lz4hc.destroy(&stream);
        quetschn_codec_lz4hc.release_params(&params);

        // cost of a page of the given size, zram stores it raw at huge_class_size and above
        auto cost = [&](double bytes) {
            auto const n = static_cast<unsigned int>(std::ceil(bytes));
            return zs.cost(n);
        };
        auto const total = static_cast<double>(pages.size() * c.page_size);
        auto print = [&](char const* what, double sum) {
            std::printf("%-58s %5.1f%%\n", what, 100.0 * sum / total);
        };

        auto sum = 0.0;
        auto literal_bytes = 0.0;
        auto sequences = 0.0;
        for (auto const& p : pages) {
            sum += zs.cost(p.lz4_size);
            literal_bytes += static_cast<double>(p.literals.size());
            sequences += static_cast<double>(p.sequences.size());
        }
        std::printf("corpus %s, %zu pages, lz4hc level %d\n", base.c_str(), pages.size(), level);
        std::printf("%.1f literal bytes and %.1f sequences per page\n\n",
                    literal_bytes / static_cast<double>(pages.size()),
                    sequences / static_cast<double>(pages.size()));
        std::printf("%-58s %6s\n", "Σ zsmalloc cost of the same lz4hc matches, coded as", "");
        print("lz4 format (what lz4hc writes)", sum);

        // 2 bytes per page for a header, e.g. the number of sequences
        constexpr double header_bits = 16;
        for (auto const [repeat, literals, what] : {std::tuple{false, false, "entropy coded sequences, raw literals"},
                                                    std::tuple{true, false, "... with repeat offsets"},
                                                    std::tuple{true, true, "... and entropy coded literals"}}) {
            auto m = model{};
            for (auto const& p : pages) {
                (void)walk(p, m, true, repeat, literals);
            }
            normalize(m.lit_len);
            normalize(m.match_len);
            normalize(m.offset);
            normalize(m.literal);
            auto s = 0.0;
            for (auto const& p : pages) {
                s += cost((walk(p, m, false, repeat, literals) + header_bits) / 8.0);
            }
            print(what, s);
        }
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
