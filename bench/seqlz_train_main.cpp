// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Trains the static Huffman tables of seqlz (explore/seqlz.h) on a corpus: the matches of seqlz's own
// matcher on every page, split into seqlz's symbols, counted, and turned into code lengths of at most
// SEQLZ_MAX_BITS bits. Writes a C initializer for explore/seqlz_default_tables_4k.inc, with --lit-sets
// the literal tables of explore/seqlz_lit_sets.c instead.

#include "harness.h"
#include "page_stats.h"
#include "seqlz.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <span>
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

// how lit_sets() searches: the starts, each a different seed, and the rounds at most. k-means stops
// where no page moves, after 9 to 17 rounds on the training corpora; which start wins moves the
// result on other pages by up to 5%, so it takes many (docs/explored-designs.md)
struct lit_search {
    unsigned seeds = 32;
    unsigned first_seed = 1;
    unsigned rounds = 60;
};

// SEQLZ_LIT_SETS literal tables for pages with coded literals, one chosen per page: k-means over the
// pages' literal histograms. Each page goes to the table that codes its literals in the fewest bits,
// each table is the code of its pages' literals. A page whose literals no table codes in 1/16 fewer
// bytes, as seqlz_encode_coded() wants, stays raw and counts for no table: otherwise the pages with
// the flattest literals got tables that never paid. The most used table first.
std::vector<std::vector<unsigned char>> lit_sets(std::vector<std::array<double, 256>> const& pages, lit_search const& opt) {
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
    // the start with the fewest bits on these pages wins
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
    for (std::uint64_t seed = opt.first_seed; seed < opt.first_seed + opt.seeds; ++seed) {
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
        for (unsigned round = 0; round < opt.rounds; ++round) {
            auto const before = s_assign;
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
                // the encoder's rule
                auto const n = weights[i];
                auto const coded = std::ceil(best_bits / 8.0) + SEQLZ_LIT_CODED_MIN;
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
            if (round > 0 && s_assign == before) {
                std::fprintf(stderr,
                             "seed %llu: no page moved after round %u, %.0f bits\n",
                             static_cast<unsigned long long>(seed),
                             round,
                             total);
                break;
            }
        }
        std::fprintf(stderr, "seed %llu: %.0f bits\n", static_cast<unsigned long long>(seed), total);
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
                 "usage: quetschn-seqlz-train --corpus <base> [--lit-sets [--seeds n] [--first-seed n] [--rounds n]]\n"
                 "\n"
                 "Counts seqlz's symbols over the matches of its own matcher on every page. Prints the code\n"
                 "lengths as a C initializer.\n"
                 "--lit-sets prints the literal tables of explore/seqlz_lit_sets.c instead, from the pages with\n"
                 "more than 64 literals: k-means from --seeds starts (32), seeds --first-seed (1) on, each at\n"
                 "most --rounds rounds (60).\n");
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    auto want_lit_sets = false;
    auto search = lit_search{};
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--corpus" && has_value) {
            base = argv[++i];
        } else if (arg == "--lit-sets") {
            want_lit_sets = true;
        } else if (arg == "--seeds" && has_value) {
            search.seeds = std::max(1U, static_cast<unsigned>(std::stoul(argv[++i])));
        } else if (arg == "--first-seed" && has_value) {
            search.first_seed = static_cast<unsigned>(std::stoul(argv[++i]));
        } else if (arg == "--rounds" && has_value) {
            search.rounds = static_cast<unsigned>(std::stoul(argv[++i]));
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
        if (c.page_size != SEQLZ_PAGE) {
            throw std::invalid_argument("the corpus has pages of " + std::to_string(c.page_size) + " bytes, this build " +
                                        std::to_string(SEQLZ_PAGE));
        }
        // start at 1: a symbol that never occurs here must still get a code
        auto token = std::vector<double>(SEQLZ_TOKEN_SYMBOLS, 1.0);
        auto ll = std::vector<double>(SEQLZ_LEN_SYMBOLS, 1.0);
        auto ml = std::vector<double>(SEQLZ_LEN_SYMBOLS, 1.0);
        auto state = std::make_unique<seqlz_state>();
        auto buffer = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
        auto pages = std::size_t{0};
        auto page_lits = std::vector<std::array<double, 256>>(); // per page with more than 64 literals
        for (std::size_t i = 0; i < c.size(); ++i) {
            auto const src = c.page(i);
            if (quetschn::analyze_page(src).same_filled) {
                continue;
            }
            ++pages;
            auto const sequences = std::span(buffer.data(), seqlz_find(state.get(), src.data(), buffer.data()));
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
                if (s.match - 4U >= SEQLZ_ML_CAP) {
                    ml[seqlz_len_symbol(s.match - 4U - SEQLZ_ML_CAP, &extra)] += 1;
                }
                last = s.offset;
            }
        }

        if (want_lit_sets) {
            auto const sets = lit_sets(page_lits, search);
            std::printf("/* %u tables of at most %u bits, trained on the %zu pages with more than 64 literals of %zu "
                        "pages of %s, seqlz */\n{\n",
                        SEQLZ_LIT_SETS,
                        SEQLZ_LIT_BITS,
                        page_lits.size(),
                        pages,
                        base.c_str());
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

        auto print = [](char const* name, unsigned char const* l, unsigned n) {
            std::printf("    .%s = {", name);
            for (unsigned i = 0; i < n; ++i) {
                std::printf("%s%u", i == 0 ? "" : ", ", l[i]);
            }
            std::printf("},\n");
        };
        std::printf("/* trained on %zu pages of %s, seqlz */\n{\n", pages, base.c_str());
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
