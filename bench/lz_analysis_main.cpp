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

// A token byte whose 256 values are split into classes. A class has fields for the literal length
// and for the match length - 4, each with or without a 7-bit varint extension for values that do not
// fit, and one kind of offset: the last one, the one before, 1 byte (1 to 256), near (k bits in the
// token and 1 byte, 1 to 256 << k) or 2 bytes. It takes 1 << (ll_bits + ml_bits + k) token values.
// Each sequence is sent in the cheapest class that can hold it. The last sequence has only literals.
enum class off_kind { last, before, byte, near, word };

struct token_class {
    off_kind kind;
    unsigned ll_bits;
    bool ll_ext;
    unsigned ml_bits;
    bool ml_ext;
    unsigned near_bits = 0;

    [[nodiscard]] unsigned tokens() const {
        return 1U << (ll_bits + ml_bits + near_bits);
    }
};

struct layout_stats {
    double sequences = 0;
    double ll_extensions = 0;
    double ml_extensions = 0;
};

// bytes of a length in a field of bits, with or without extension; 0 if it does not fit
unsigned field_bytes(std::uint32_t v, unsigned bits, bool ext, bool& fits) {
    auto const cap = (1U << bits) - 1U;
    fits = true;
    if (!ext) {
        fits = v <= cap;
        return 0;
    }
    return v >= cap ? varint_bytes(v - cap) : 0U;
}

double layout_bytes(parsed_page const& pg, std::vector<token_class> const& layout, layout_stats* stats) {
    auto bytes = 0.0;
    auto last = 1U, before = 4U;
    for (auto const& s : pg.sequences) {
        auto best = ~0U;
        auto best_kind = off_kind::word;
        auto best_ll_ext = false, best_ml_ext = false;
        for (auto const& c : layout) {
            auto fits = true, f = true;
            auto b = 1U + field_bytes(s.literals, c.ll_bits, c.ll_ext, f);
            fits = f;
            if (s.match != 0) {
                b += field_bytes(s.match - 4, c.ml_bits, c.ml_ext, f);
                fits = fits && f;
                switch (c.kind) {
                case off_kind::last:
                    fits = fits && s.offset == last;
                    break;
                case off_kind::before:
                    fits = fits && s.offset == before;
                    break;
                case off_kind::byte:
                    fits = fits && s.offset <= 256;
                    b += 1;
                    break;
                case off_kind::near:
                    fits = fits && s.offset <= (256U << c.near_bits);
                    b += 1;
                    break;
                case off_kind::word:
                    b += 2;
                    break;
                }
            }
            if (fits && b < best) {
                best = b;
                best_kind = c.kind;
                best_ll_ext = c.ll_ext && s.literals >= (1U << c.ll_bits) - 1U;
                best_ml_ext = s.match != 0 && c.ml_ext && s.match - 4 >= (1U << c.ml_bits) - 1U;
            }
        }
        if (best == ~0U) {
            return 1e9; // the layout cannot send this sequence
        }
        bytes += best;
        if (stats != nullptr) {
            stats->sequences += 1;
            stats->ll_extensions += best_ll_ext ? 1 : 0;
            stats->ml_extensions += best_ml_ext ? 1 : 0;
        }
        if (s.match != 0 && best_kind != off_kind::last) {
            before = last;
            last = s.offset;
        }
    }
    return bytes;
}

std::string layout_name(std::vector<token_class> const& layout) {
    auto name = std::string();
    for (auto const& c : layout) {
        static char const* const kinds[] = {"last", "before", "byte", "near", "word"};
        name += std::string(name.empty() ? "" : ", ") + kinds[static_cast<int>(c.kind)] +
                (c.kind == off_kind::near ? std::to_string(c.near_bits) : "") + " " + std::to_string(c.ll_bits) +
                (c.ll_ext ? "e" : "") + "/" + std::to_string(c.ml_bits) + (c.ml_ext ? "e" : "");
    }
    return name;
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
        // token classes: bytelz's layout, then a search from it that changes one class at a time
        auto layout_cost = [&](std::vector<token_class> const& layout, layout_stats* st) {
            auto tokens = 0U;
            for (auto const& tc : layout) {
                tokens += tc.tokens();
            }
            if (tokens > 256) {
                return 1e18;
            }
            auto sum_cost = 0.0;
            for (auto const& p : pages) {
                sum_cost += cost(static_cast<double>(p.literals.size()) + layout_bytes(p, layout, st));
            }
            return sum_cost;
        };
        auto report = [&](std::vector<token_class> const& layout) {
            auto st = layout_stats{};
            auto const layout_sum = layout_cost(layout, &st);
            std::printf("%5.2f%%  ll ext %4.1f%%  ml ext %4.1f%%  %s\n",
                        100.0 * layout_sum / total,
                        100.0 * st.ll_extensions / st.sequences,
                        100.0 * st.ml_extensions / st.sequences,
                        layout_name(layout).c_str());
        };
        std::printf("\ntoken classes (offset kind ll/ml bits, e: with extension), Σ zsmalloc cost\n");
        using k = off_kind;
        auto layout = std::vector<token_class>{{k::last, 3, true, 3, true},
                                               {k::before, 3, true, 3, true},
                                               {k::byte, 3, true, 3, true},
                                               {k::word, 3, true, 3, true}};
        report(layout);
        auto const start = layout;
        // Add a near class, then improve one field at a time while it gets better. Better: smaller,
        // with each extension counted as weight bytes more, because each is a branch in the decoder
        // that mispredicts.
        for (auto const weight : {0.0, 0.5, 1.0, 2.0}) {
            layout = start;
            layout.push_back({k::near, 0, false, 0, false, 3});
            auto score = [&](std::vector<token_class> const& l) {
                auto st = layout_stats{};
                auto const x = layout_cost(l, &st);
                return x + weight * (st.ll_extensions + st.ml_extensions);
            };
            auto best = score(layout);
            for (bool better = true; better;) {
                better = false;
                for (std::size_t i = 0; i < layout.size(); ++i) {
                    for (int change = 0; change < 12; ++change) {
                        auto l = layout;
                        auto& cl = l[i];
                        switch (change) {
                        case 0:
                            cl.ll_bits += 1;
                            break;
                        case 1:
                            cl.ll_bits -= cl.ll_bits > 0 ? 1U : 0U;
                            break;
                        case 2:
                            cl.ml_bits += 1;
                            break;
                        case 3:
                            cl.ml_bits -= cl.ml_bits > 0 ? 1U : 0U;
                            break;
                        case 4:
                            cl.ll_ext = !cl.ll_ext;
                            break;
                        case 5:
                            cl.ml_ext = !cl.ml_ext;
                            break;
                        case 6:
                            cl.near_bits += cl.kind == k::near ? 1U : 0U;
                            break;
                        case 7:
                            cl.near_bits -= cl.kind == k::near && cl.near_bits > 0 ? 1U : 0U;
                            break;
                        case 8:
                            cl.ll_bits += 1;
                            cl.ml_bits -= cl.ml_bits > 0 ? 1U : 0U;
                            break;
                        case 9:
                            cl.ml_bits += 1;
                            cl.ll_bits -= cl.ll_bits > 0 ? 1U : 0U;
                            break;
                        case 10:
                            cl.ml_bits += 1;
                            cl.near_bits -= cl.kind == k::near && cl.near_bits > 0 ? 1U : 0U;
                            break;
                        default:
                            cl.ll_bits += 1;
                            cl.near_bits -= cl.kind == k::near && cl.near_bits > 0 ? 1U : 0U;
                            break;
                        }
                        auto const x = score(l);
                        if (x < best - 0.5) {
                            best = x;
                            layout = l;
                            better = true;
                        }
                    }
                }
            }
            std::printf("weight %.1f: ", weight);
            report(layout);
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
