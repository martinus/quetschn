// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Trains the static Huffman tables of seqlz (explore/seqlz.h) on a corpus: the matches of lz4 or
// lz4hc on every page, split into seqlz's symbols, counted, and turned into code lengths of at most
// SEQLZ_MAX_BITS bits. Writes a C initializer for explore/seqlz_default_tables.c, or with --blob the
// 2099 bytes that zram's dictionary parameter can carry. With --lit-sets the literal tables of
// explore/seqlz_lit_sets.c instead.

#include "harness.h"
#include "kernel_codecs/zram_codec.h"
#include "lz_analysis.h"
#include "page_stats.h"
#include "seqlz.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

#ifndef LIT_FLOOR
#    define LIT_FLOOR 0.0
#endif

// The optimal code lengths of at most max_bits bits, by package-merge (Larmore and Hirschberg): the
// lists of the levels from max_bits up to 1 each hold the symbols and the pairs of the level below,
// sorted by weight; a symbol's length is how often it is among the 2n - 2 lightest items of the top
// list, counted through the pairs. With 1536 tokens and 11 bits, halving all counts until a Huffman
// code fits cost 18% more token bits than no limit at all.
std::vector<unsigned char> code_lengths(std::vector<double> const& counts, unsigned max_bits) {
    auto const n = counts.size();
    struct item {
        double weight;
        std::size_t left, right; // items of the level below; left == right: a symbol, left is its index
        bool leaf;
    };
    auto symbols = std::vector<std::size_t>(n);
    for (std::size_t i = 0; i < n; ++i) {
        symbols[i] = i;
    }
    std::stable_sort(symbols.begin(), symbols.end(), [&](auto a, auto b) {
        return counts[a] < counts[b];
    });
    auto levels = std::vector<std::vector<item>>();
    auto leaves = std::vector<item>();
    for (auto sym : symbols) {
        leaves.push_back({counts[sym], sym, sym, true});
    }
    levels.push_back(leaves);
    for (unsigned level = 1; level < max_bits; ++level) {
        auto const& below = levels.back();
        auto pairs = std::vector<item>();
        for (std::size_t i = 0; i + 1 < below.size(); i += 2) {
            pairs.push_back({below[i].weight + below[i + 1].weight, i, i + 1, false});
        }
        auto merged = std::vector<item>();
        std::merge(leaves.begin(),
                   leaves.end(),
                   pairs.begin(),
                   pairs.end(),
                   std::back_inserter(merged),
                   [](auto const& a, auto const& b) {
                       return a.weight < b.weight;
                   });
        levels.push_back(std::move(merged));
    }
    auto lengths = std::vector<unsigned char>(n);
    // count the symbols under the lightest 2n - 2 items of the top level
    auto count = [&](auto& self, std::size_t level, std::size_t index) -> void {
        auto const& it = levels[level][index];
        if (it.leaf) {
            ++lengths[it.left];
            return;
        }
        self(self, level - 1, it.left);
        self(self, level - 1, it.right);
    };
    for (std::size_t i = 0; i < 2 * n - 2; ++i) {
        count(count, levels.size() - 1, i);
    }
    return lengths;
}

// The token lengths with an escape: only the k most frequent tokens get a code, the others the escape's
// code and SEQLZ_ESCAPE_BITS bits. k as it gives the fewest bits, with an escape of at most 8 bits
// (seqlz_tables_init() checks that).
std::vector<unsigned char> token_lengths(std::vector<double> const& counts) {
    auto order = std::vector<std::size_t>(counts.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
        return counts[a] > counts[b];
    });
    auto best = std::vector<unsigned char>();
    auto best_bits = 0.0;
    // at most 1 << SEQLZ_TOKEN_BITS codes fit, the escape is one of them
    auto const most = std::min(counts.size(), (std::size_t{1} << SEQLZ_TOKEN_BITS) - 1);
    for (auto k = std::size_t{64}; k <= most; k = k + 64 <= most || k == most ? k + 64 : most) {
        auto kept = std::vector<double>();
        auto escaped = 1.0;
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (i < k) {
                kept.push_back(counts[order[i]]);
            } else {
                escaped += counts[order[i]];
            }
        }
        kept.push_back(escaped);
        auto const l = code_lengths(kept, SEQLZ_TOKEN_BITS);
        if (l.back() > SEQLZ_MAX_ESCAPE_LEN) {
            continue;
        }
        auto bits = (escaped - 1.0) * (l.back() + SEQLZ_ESCAPE_BITS);
        auto lengths = std::vector<unsigned char>(counts.size() + 1, 0);
        for (std::size_t i = 0; i < k; ++i) {
            bits += counts[order[i]] * l[i];
            lengths[order[i]] = l[i];
        }
        lengths.back() = l.back();
        if (best.empty() || bits < best_bits) {
            best = lengths;
            best_bits = bits;
        }
    }
    return best;
}

// SEQLZ_LIT_SETS literal tables for pages with coded literals, one chosen per page: k-means over the
// pages' literal histograms. Each page goes to the table that codes its literals in the fewest bits,
// each table is the code of its pages' literals. A page whose literals no table codes in 1/16 fewer
// bytes, as seqlz_encode_coded() wants, stays raw and counts for no table: otherwise the pages with
// the flattest literals got tables that never paid. The most used table first.
std::vector<std::vector<unsigned char>> lit_sets(std::vector<std::array<double, 256>> const& pages) {
    auto const k_sets = std::size_t{SEQLZ_LIT_SETS};
    if (pages.size() < k_sets) {
        throw std::runtime_error("too few pages with literals for the literal tables");
    }
    auto code = [](std::array<double, 256> const& h) {
        // every byte gets a code, the encoder needs one: + 1, and FLOOR of all literals
        auto c = std::vector<double>(256);
        auto n = 0.0;
        for (auto v : h) {
            n += v;
        }
        for (std::size_t s = 0; s < 256; ++s) {
            c[s] = h[s] + 1.0 + n * LIT_FLOOR;
        }
        return code_lengths(c, SEQLZ_LIT_BITS);
    };
    auto bits = [](std::array<double, 256> const& h, std::vector<unsigned char> const& l) {
        auto b = 0.0;
        for (std::size_t s = 0; s < 256; ++s) {
            b += h[s] * l[s];
        }
        return b;
    };
    // start: pages drawn by their number of literals, each smoothed with a tenth of the mean histogram;
    // 4 starts with fixed seeds, the one with the fewest bits on these pages wins
    auto weights = std::vector<double>();
    auto mean = std::array<double, 256>{};
    for (auto const& h : pages) {
        auto n = 0.0;
        for (std::size_t s = 0; s < 256; ++s) {
            n += h[s];
            mean[s] += h[s] / static_cast<double>(pages.size());
        }
        weights.push_back(n);
    }
    auto sets = std::vector<std::vector<unsigned char>>();
    auto assign = std::vector<std::size_t>(pages.size());
    auto best_total = 0.0;
    for (std::uint64_t seed = 1; seed <= 4; ++seed) {
        auto rng = std::mt19937_64(seed);
        auto draw = std::discrete_distribution<std::size_t>(weights.begin(), weights.end());
        auto s_sets = std::vector<std::vector<unsigned char>>();
        for (std::size_t k = 0; k < k_sets; ++k) {
            auto h = pages[draw(rng)];
            for (std::size_t s = 0; s < 256; ++s) {
                h[s] += mean[s] / 10.0;
            }
            s_sets.push_back(code(h));
        }
        auto s_assign = std::vector<std::size_t>(pages.size());
        auto total = 0.0;
        for (int round = 0; round < 15; ++round) {
            auto sums = std::vector<std::array<double, 256>>(k_sets, std::array<double, 256>{});
            auto used = std::vector<std::size_t>(k_sets);
            total = 0.0;
            for (std::size_t i = 0; i < pages.size(); ++i) {
                auto best = std::size_t{0};
                auto best_bits = bits(pages[i], s_sets[0]);
                for (std::size_t k = 1; k < k_sets; ++k) {
                    auto const b = bits(pages[i], s_sets[k]);
                    if (b < best_bits) {
                        best = k;
                        best_bits = b;
                    }
                }
                // the encoder's rule, the bytes of the coded literals and the header
                auto const n = weights[i];
                auto const coded = std::ceil(best_bits / 8.0) + SEQLZ_LIT_HEADER;
                s_assign[i] = best;
                if (coded >= n - std::floor(n / 16.0)) {
                    s_assign[i] = k_sets;
                    total += 8.0 * n;
                    continue;
                }
                total += 8.0 * coded;
                ++used[best];
                for (std::size_t s = 0; s < 256; ++s) {
                    sums[best][s] += pages[i][s];
                }
            }
            for (std::size_t k = 0; k < k_sets; ++k) {
                if (used[k] != 0) {
                    s_sets[k] = code(sums[k]);
                    continue;
                }
                // a table no page wants starts again from the page whose literals cost most per byte
                auto worst = std::size_t{0};
                auto worst_rate = -1.0;
                for (std::size_t i = 0; i < pages.size(); ++i) {
                    auto b = bits(pages[i], s_sets[0]);
                    for (std::size_t j = 1; j < k_sets; ++j) {
                        b = std::min(b, bits(pages[i], s_sets[j]));
                    }
                    if (b / weights[i] > worst_rate && s_assign[i] < k_sets) {
                        worst = i;
                        worst_rate = b / weights[i];
                    }
                }
                auto h = pages[worst];
                for (std::size_t sym = 0; sym < 256; ++sym) {
                    h[sym] += mean[sym] / 10.0;
                }
                s_sets[k] = code(h);
            }
        }
        if (sets.empty() || total < best_total) {
            sets = s_sets;
            assign = s_assign;
            best_total = total;
        }
    }
    auto used = std::vector<std::size_t>(k_sets + 1);
    for (auto a : assign) {
        ++used[a];
    }
    auto order = std::vector<std::size_t>(k_sets);
    for (std::size_t k = 0; k < k_sets; ++k) {
        order[k] = k;
    }
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
        return used[a] > used[b];
    });
    auto sorted = std::vector<std::vector<unsigned char>>();
    for (auto k : order) {
        sorted.push_back(sets[k]);
    }
    return sorted;
}

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-seqlz-train --corpus <base> [--codec lz4|lz4hc|seqlz] [--level <n>] [--blob <file>]\n"
                 "                           [--lit-sets]\n"
                 "\n"
                 "Counts seqlz's symbols over the matches of --codec (default lz4; seqlz is its own matcher) on\n"
                 "every page. Prints the\n"
                 "code lengths as a C initializer, or writes them to --blob for zram's dictionary parameter.\n"
                 "--lit-sets prints the literal tables of explore/seqlz_lit_sets.c instead, from the pages with\n"
                 "more than 64 literals.\n");
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    auto blob = std::string();
    auto const* codec = &quetschn_codec_lz4;
    auto own_matcher = false;
    auto want_lit_sets = false;
    int level = QUETSCHN_LEVEL_DEFAULT;
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--corpus" && has_value) {
            base = argv[++i];
        } else if (arg == "--lit-sets") {
            want_lit_sets = true;
        } else if (arg == "--blob" && has_value) {
            blob = argv[++i];
        } else if (arg == "--codec" && has_value) {
            auto const name = std::string_view(argv[++i]);
            if (name == "seqlz") {
                own_matcher = true;
            } else if (name == "lz4hc") {
                codec = &quetschn_codec_lz4hc;
            } else if (name != "lz4") {
                usage();
                return 2;
            }
        } else if (arg == "--level" && has_value) {
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

        // start at 1: a symbol that never occurs here must still get a code
        auto token = std::vector<double>(SEQLZ_TOKEN_SYMBOLS, 1.0);
        auto ll = std::vector<double>(SEQLZ_LEN_SYMBOLS, 1.0);
        auto ml = std::vector<double>(SEQLZ_LEN_SYMBOLS, 1.0);
        auto dst = std::vector<std::uint8_t>(2 * c.page_size);
        auto state = std::make_unique<seqlz_state>();
        auto seqs = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
        // what the tables are for, in the output
        auto const matcher =
            own_matcher ? std::string("seqlz") : std::string(codec->name) + " level " + std::to_string(params.level);
        auto sequences = std::vector<quetschn::sequence>(); // of one page, reused
        auto pages = std::size_t{0};
        auto page_lits = std::vector<std::array<double, 256>>(); // per page with more than 64 literals
        for (std::size_t i = 0; i < c.size(); ++i) {
            auto const src = c.page(i);
            if (quetschn::analyze_page(src).same_filled) {
                continue;
            }
            ++pages;
            if (own_matcher) {
                // seqlz's own matcher, as seqlz-fast uses it
                auto const n = seqlz_find(state.get(), src.data(), seqs.data());
                sequences.clear();
                for (unsigned k = 0; k < n; ++k) {
                    sequences.push_back({seqs[k].literals, seqs[k].match, seqs[k].offset});
                }
            } else {
                auto len = static_cast<unsigned int>(dst.size());
                if (codec->compress(&params, &stream, src.data(), static_cast<unsigned int>(src.size()), dst.data(), &len) !=
                    0) {
                    throw std::runtime_error(std::string(codec->name) + ": compress failed");
                }
                sequences = quetschn::parse_lz4(dst.data(), len).sequences;
            }
            // the literal bytes, for pages with coded literals
            {
                auto in = std::size_t{0};
                auto h = std::array<double, 256>{};
                auto n = std::size_t{0};
                for (auto const& s : sequences) {
                    for (std::size_t k = 0; k < s.literals; ++k) {
                        h[static_cast<unsigned char>(src[in + k])] += 1;
                    }
                    n += s.literals;
                    in += s.literals + s.match;
                }
                if (n > 64) {
                    page_lits.push_back(h);
                }
            }
            // the same symbols and the same repeat offset as seqlz_encode()
            auto last = 1U;
            auto extra = 0U;
            for (auto const& s : sequences) {
                auto const cls = s.match == 0 ? 0U : seqlz_off_class(s.offset, last, &extra);
                token[seqlz_token(s.literals, s.match, cls)] += 1;
                if (s.literals >= SEQLZ_LL_CAP) {
                    ll[seqlz_len_symbol(s.literals - SEQLZ_LL_CAP, &extra)] += 1;
                }
                if (s.match == 0) {
                    continue;
                }
                if (s.match - 4 >= SEQLZ_ML_CAP) {
                    ml[seqlz_len_symbol(s.match - 4 - SEQLZ_ML_CAP, &extra)] += 1;
                }
                last = s.offset;
            }
        }
        codec->destroy(&stream);
        codec->release_params(&params);

        if (want_lit_sets) {
            auto const sets = lit_sets(page_lits);
            std::printf("/* %u tables of at most %u bits, trained on the %zu pages with more than 64 literals of %zu "
                        "pages of %s, %s */\n{\n",
                        SEQLZ_LIT_SETS,
                        SEQLZ_LIT_BITS,
                        page_lits.size(),
                        pages,
                        base.c_str(),
                        matcher.c_str());
            for (auto const& l : sets) {
                std::printf("    {");
                for (std::size_t s = 0; s < l.size(); ++s) {
                    std::printf("%s%u", s == 0 ? "" : ", ", l[s]);
                }
                std::printf("},\n");
            }
            std::printf("}\n");
            return 0;
        }

        auto lengths = seqlz_lengths{};
        auto const l_token = token_lengths(token);
        auto const l_ll = code_lengths(ll, SEQLZ_MAX_BITS);
        auto const l_ml = code_lengths(ml, SEQLZ_MAX_BITS);
        std::copy(l_token.begin(), l_token.end(), lengths.token);
        std::copy(l_ll.begin(), l_ll.end(), lengths.ll);
        std::copy(l_ml.begin(), l_ml.end(), lengths.ml);

        if (!blob.empty()) {
            auto out = std::ofstream(blob, std::ios::binary);
            out.write(reinterpret_cast<char const*>(&lengths), sizeof(lengths));
            if (!out) {
                throw std::runtime_error("cannot write " + blob);
            }
            std::printf("%zu pages, %s, %zu bytes to %s\n", pages, matcher.c_str(), sizeof(lengths), blob.c_str());
            return 0;
        }
        auto print = [](char const* name, unsigned char const* l, unsigned n) {
            std::printf("    .%s = {", name);
            for (unsigned i = 0; i < n; ++i) {
                std::printf("%s%u", i == 0 ? "" : ", ", l[i]);
            }
            std::printf("},\n");
        };
        std::printf("/* trained on %zu pages of %s, %s */\n{\n", pages, base.c_str(), matcher.c_str());
        print("token", lengths.token, SEQLZ_TOKEN_SYMBOLS + 1);
        print("ll", lengths.ll, SEQLZ_LEN_SYMBOLS);
        print("ml", lengths.ml, SEQLZ_LEN_SYMBOLS);
        std::printf("}\n");
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
