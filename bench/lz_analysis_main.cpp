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
#include "seqlz.h"
#include "zsmalloc_cost.h"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
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

// A byte oriented format without entropy coding: per sequence one token byte with ll_bits for the
// literal length, ml_bits for the match length - 4, and the rest for how the offset is sent. Lengths
// at or above what fits in the token follow as a 7-bit varint. With 2 offset bits (at most 2): the last offset,
// the one before, 1 byte or 2 bytes; with 1: the last offset or 2 bytes; with 0: always 2 bytes.
struct byte_format {
    unsigned ll_bits;
    unsigned ml_bits;
    char const* name;
};

unsigned varint_bytes(std::uint32_t v) {
    return v < 128 ? 1U : v < 16384 ? 2U : 3U;
}

// The bytes of one page in the format, without the literals; with tokens, the token bytes are counted
// (tokens[byte] += 1) and left out, for an entropy coded token byte.
double byte_format_bytes(parsed_page const& pg, byte_format f, std::array<double, 256>* tokens) {
    auto const off_bits = 8U - f.ll_bits - f.ml_bits;
    auto const ll_cap = (1U << f.ll_bits) - 1U, ml_cap = (1U << f.ml_bits) - 1U;
    auto bytes = 0.0;
    auto rep0 = 1U, rep1 = 4U;
    for (auto const& s : pg.sequences) {
        auto const ll = std::min(s.literals, ll_cap);
        auto ml = 0U, mode = 0U;
        bytes += s.literals >= ll_cap ? varint_bytes(s.literals - ll_cap) : 0U;
        if (s.match != 0) {
            ml = std::min(s.match - 4, ml_cap);
            bytes += s.match - 4 >= ml_cap ? varint_bytes(s.match - 4 - ml_cap) : 0U;
            if (off_bits >= 1 && s.offset == rep0) {
                mode = 0;
            } else if (off_bits == 2 && s.offset == rep1) {
                mode = 1;
            } else if (off_bits == 2 && s.offset <= 256) {
                mode = 2;
                bytes += 1;
            } else {
                mode = off_bits == 2 ? 3U : 1U;
                bytes += 2;
            }
            if (s.offset != rep0) {
                rep1 = rep0;
                rep0 = s.offset;
            }
        }
        auto const token = (ll | ml << f.ll_bits | mode << (f.ll_bits + f.ml_bits)) & 255U;
        if (tokens != nullptr) {
            (*tokens)[token] += 1;
        } else {
            bytes += 1;
        }
    }
    return bytes;
}

// A joint token for seqlz: min(ll, ll_cap), min(ml - 4, ml_cap) and a class of the offset in one
// Huffman symbol, so the decoder needs one table lookup per sequence and not two. Class 0 is the last
// offset; class c >= 1 holds the offsets whose bit width - 1 is in [lo_c, hi_c], sent raw after the
// token: hi_c bits if lo_c == hi_c (the top bit is known), else hi_c + 1 bits. Lengths at or above the
// cap follow as seqlz's length values (a bucket symbol and extra bits) from their own model.
struct joint_format {
    unsigned ll_cap;
    unsigned ml_cap;
    std::vector<std::pair<unsigned, unsigned>> classes; // [lo, hi] of the bucket for class 1, 2, ...
    char const* name;
    bool separate = false; // the class as its own symbol after the token, as seqlz does
};

struct joint_model {
    std::vector<double> token;
    std::array<double, 64> ll{};
    std::array<double, 64> ml{};
    std::array<double, 64> off{}; // the class, if separate
};

// bits of one page, counting (count = true) or costing; ll_ext, ml_ext count the sequences with one
double joint_walk(parsed_page const& pg, joint_format const& f, joint_model& m, bool count, double* ll_ext, double* ml_ext) {
    auto bits = 0.0;
    auto last = 1U;
    auto const n_ll = f.ll_cap + 1, n_ml = f.ml_cap + 1;
    auto add = [&](std::array<double, 64>& table, std::uint32_t v) {
        auto const s = length_symbol(v);
        if (count) {
            table[s.code] += 1;
        } else {
            bits += -std::log2(table[s.code]) + s.extra_bits;
        }
    };
    for (auto const& s : pg.sequences) {
        auto const ll = std::min(s.literals, f.ll_cap);
        auto ml = 0U, cls = 0U;
        if (s.literals >= f.ll_cap) {
            add(m.ll, s.literals - f.ll_cap);
            *ll_ext += count ? 0 : 1;
        }
        if (s.match != 0) {
            ml = std::min(s.match - 4, f.ml_cap);
            if (s.match - 4 >= f.ml_cap) {
                add(m.ml, s.match - 4 - f.ml_cap);
                *ml_ext += count ? 0 : 1;
            }
            if (s.offset != last) {
                auto const b = static_cast<unsigned>(std::bit_width(s.offset) - 1);
                cls = 1;
                while (cls <= f.classes.size() && b > f.classes[cls - 1].second) {
                    ++cls;
                }
                auto const [lo, hi] = f.classes[cls - 1];
                bits += count ? 0.0 : (lo == hi ? hi : hi + 1);
            }
            last = s.offset;
        }
        auto const sym = ll + n_ll * (ml + n_ml * (f.separate ? 0U : cls));
        if (count) {
            m.token[sym] += 1;
        } else {
            bits += -std::log2(m.token[sym]);
        }
        if (f.separate && s.match != 0) {
            if (count) {
                m.off[cls] += 1;
            } else {
                bits += -std::log2(m.off[cls]);
            }
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
    std::fprintf(stderr,
                 "usage: quetschn-lz-analysis --corpus <base> [--codec lz4|lz4hc|seqlz] [--level <n>]\n"
                 "\n"
                 "The matches come from --codec, default lz4hc, at zram's default level unless --level; seqlz is\n"
                 "seqlz-fast's own matcher.\n");
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    int level = QUETSCHN_LEVEL_DEFAULT;
    auto const* codec = &quetschn_codec_lz4hc;
    auto own_matcher = false;
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        if (arg == "--corpus" && i + 1 < argc) {
            base = argv[++i];
        } else if (arg == "--codec" && i + 1 < argc) {
            auto const name = std::string_view(argv[++i]);
            if (name == "lz4") {
                codec = &quetschn_codec_lz4;
            } else if (name == "seqlz") {
                own_matcher = true;
            } else if (name != "lz4hc") {
                usage();
                return 2;
            }
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
        if (codec->setup_params(&params) != 0) {
            throw std::invalid_argument(std::string(codec->name) + " rejects level " + std::to_string(level));
        }
        auto stream = quetschn_stream{};
        if (codec->create(&params, &stream) != 0) {
            throw std::runtime_error(std::string(codec->name) + ": create failed");
        }
        auto dst = std::vector<std::uint8_t>(2 * c.page_size);
        auto pages = std::vector<parsed_page>();
        auto state = std::make_unique<seqlz_state>();
        auto seqs = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
        for (std::size_t i = 0; i < c.size(); ++i) {
            auto const src = c.page(i);
            if (quetschn::analyze_page(src).same_filled) {
                continue;
            }
            auto parsed = parsed_page{};
            if (own_matcher) {
                auto const n = seqlz_find(state.get(), src.data(), seqs.data());
                auto in = std::size_t{0};
                for (unsigned k = 0; k < n; ++k) {
                    parsed.sequences.push_back({seqs[k].literals, seqs[k].match, seqs[k].offset});
                    for (unsigned j = 0; j < seqs[k].literals; ++j) {
                        parsed.literals.push_back(static_cast<std::uint8_t>(src[in + j]));
                    }
                    in += seqs[k].literals + seqs[k].match;
                }
            } else {
                auto len = static_cast<unsigned int>(dst.size());
                if (codec->compress(&params, &stream, src.data(), static_cast<unsigned int>(src.size()), dst.data(), &len) !=
                    0) {
                    throw std::runtime_error(std::string(codec->name) + ": compress failed");
                }
                parsed = parse_lz4(dst.data(), len);
            }
            // the estimate is only as good as the parse, so every page must come back from it
            auto const back = quetschn::reconstruct(parsed);
            if (back.size() != src.size() || std::memcmp(back.data(), src.data(), src.size()) != 0) {
                throw std::runtime_error("page " + std::to_string(i) + " does not come back from its sequences");
            }
            pages.push_back(std::move(parsed));
        }
        codec->destroy(&stream);
        codec->release_params(&params);

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
            sum += own_matcher ? 0.0 : zs.cost(p.lz4_size);
            literal_bytes += static_cast<double>(p.literals.size());
            sequences += static_cast<double>(p.sequences.size());
        }
        if (own_matcher) {
            std::printf("corpus %s, %zu pages, seqlz-fast's matcher\n", base.c_str(), pages.size());
        } else {
            std::printf("corpus %s, %zu pages, %s level %d\n", base.c_str(), pages.size(), codec->name, params.level);
        }
        std::printf("%.1f literal bytes and %.1f sequences per page\n\n",
                    literal_bytes / static_cast<double>(pages.size()),
                    sequences / static_cast<double>(pages.size()));
        std::printf("%-58s %6s\n", "Σ zsmalloc cost of the same matches, coded as", "");
        if (!own_matcher) {
            print("lz4 format (what the codec writes)", sum);
        }

        // 2 bytes per page for a header, e.g. the number of sequences
        constexpr double header_bits = 16;
        for (auto const& [repeat, literals, what] : {std::tuple{false, false, "entropy coded sequences, raw literals"},
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

        // 4 bytes of header, as seqlz: the number of sequences and of literal bytes
        std::printf("\n%-58s %6s\n", "byte oriented, token bits for ll / ml - 4 / offset", "");
        for (auto const f : {byte_format{4, 4, "4 / 4 / 0 (lz4 with varints)"},
                             byte_format{4, 3, "4 / 3 / 1 (last offset or 2 bytes)"},
                             byte_format{3, 4, "3 / 4 / 1"},
                             byte_format{3, 3, "3 / 3 / 2 (last, the one before, 1 or 2 bytes)"},
                             byte_format{2, 4, "2 / 4 / 2"}}) {
            auto s = 0.0;
            for (auto const& p : pages) {
                s += cost(4.0 + static_cast<double>(p.literals.size()) + byte_format_bytes(p, f, nullptr));
            }
            print(f.name, s);
        }
        {
            // seqlz itself and joint tokens, with the same static model (optimistic, trained on the
            // pages it is measured on); header 4 bytes
            auto buckets = [](unsigned from, unsigned to) {
                auto v = std::vector<std::pair<unsigned, unsigned>>();
                for (auto b = from; b <= to; ++b) {
                    v.emplace_back(b, b);
                }
                return v;
            };
            using cls = std::vector<std::pair<unsigned, unsigned>>;
            std::printf("\n%-58s %6s %7s %7s\n", "joint token: ll cap / ml cap / offset classes", "", "ll ext", "ml ext");
            for (auto const& f :
                 {joint_format{15, 31, buckets(0, 11), "seqlz: 15 / 31, last + 12 buckets as own symbol", true},
                  joint_format{7, 7, buckets(0, 11), "7 / 7 / last + 12 buckets"},
                  joint_format{
                      7, 7, cls{{0, 3}, {4, 5}, {6, 7}, {8, 8}, {9, 9}, {10, 10}, {11, 11}}, "7 / 7 / last + 7 classes"},
                  joint_format{7, 15, cls{{0, 4}, {5, 7}, {8, 9}, {10, 11}}, "7 / 15 / last + 4 classes"},
                  joint_format{
                      3, 15, cls{{0, 3}, {4, 5}, {6, 7}, {8, 8}, {9, 9}, {10, 10}, {11, 11}}, "3 / 15 / last + 7 classes"},
                  joint_format{
                      7, 15, cls{{0, 3}, {4, 5}, {6, 7}, {8, 8}, {9, 9}, {10, 10}, {11, 11}}, "7 / 15 / last + 7 classes"},
                  joint_format{15, 15, cls{{0, 4}, {5, 7}, {8, 9}, {10, 11}}, "15 / 15 / last + 4 classes"},
                  joint_format{7, 31, cls{{0, 4}, {5, 7}, {8, 9}, {10, 11}}, "7 / 31 / last + 4 classes"},
                  joint_format{15, 31, cls{{0, 7}, {8, 11}}, "15 / 31 / last + 2 classes"},
                  joint_format{7, 31, cls{{0, 7}, {8, 11}}, "7 / 31 / last + 2 classes"},
                  joint_format{15, 31, cls{{0, 11}}, "15 / 31 / last + 1 class of 12 bits"}}) {
                auto m = joint_model{};
                auto const n_ll = f.ll_cap + 1, n_ml = f.ml_cap + 1;
                m.token.assign(n_ll * n_ml * (f.separate ? 1 : f.classes.size() + 1), 0.0);
                auto dummy = 0.0;
                for (auto const& p : pages) {
                    (void)joint_walk(p, f, m, true, &dummy, &dummy);
                }
                normalize(m.token);
                normalize(m.ll);
                normalize(m.ml);
                normalize(m.off);
                auto s = 0.0, lle = 0.0, mle = 0.0, n_seq = 0.0;
                for (auto const& p : pages) {
                    s += cost(static_cast<double>(p.literals.size()) + (joint_walk(p, f, m, false, &lle, &mle) + 32.0) / 8.0);
                    n_seq += static_cast<double>(p.sequences.size());
                }
                std::printf("%-58s %5.1f%% %6.1f%% %6.1f%%  %zu symbols\n",
                            f.name,
                            100.0 * s / total,
                            100.0 * lle / n_seq,
                            100.0 * mle / n_seq,
                            m.token.size());
            }
        }

        for (auto const f : {byte_format{4, 4, "4 / 4 / 0, token byte entropy coded"},
                             byte_format{3, 3, "3 / 3 / 2, token byte entropy coded"}}) {
            auto tokens = std::array<double, 256>{};
            for (auto const& p : pages) {
                (void)byte_format_bytes(p, f, &tokens);
            }
            normalize(tokens);
            auto s = 0.0;
            for (auto const& p : pages) {
                auto counts = std::array<double, 256>{};
                auto bytes = byte_format_bytes(p, f, &counts);
                auto bits = 0.0;
                for (unsigned t = 0; t < 256; ++t) {
                    bits += counts[t] * -std::log2(tokens[t]);
                }
                s += cost(4.0 + static_cast<double>(p.literals.size()) + bytes + bits / 8.0);
            }
            print(f.name, s);
        }
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
