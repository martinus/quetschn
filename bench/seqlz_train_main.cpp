// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Trains the static Huffman tables of seqlz (src/seqlz.h) on a corpus: the matches of seqlz's own
// matcher on every page, split into seqlz's symbols, counted, and turned into code lengths of at most
// SEQLZ_MAX_BITS bits. Writes a C initializer for src/seqlz_default_tables_4k.inc or _16k.inc, by the
// page size of the build, with --lit-sets the literal tables of src/seqlz_lit_sets_4k.inc or _16k.inc.
// --counts also writes the counts the tables are built from, --from-counts builds them from such a file
// instead of a corpus: bench/seqlz_counts_4k.txt and _16k.txt are the ones of the tables in src/.

#include "harness.h"
#include "page_stats.h"
#include "seqlz_train.h"
// seqlz.h is C, written for the kernel
extern "C" {
#include "seqlz.h"
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
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
// the literal tables, the histogram each one is built from and its pages, the most used table first
struct lit_result {
    std::vector<std::vector<unsigned char>> sets;
    std::vector<std::array<double, 256>> sources;
    std::vector<double> pages;
};

lit_result lit_sets(std::vector<std::array<double, 256>> const& pages, lit_search const& opt) {
    auto const k_sets = std::size_t{SEQLZ_LIT_SETS};
    if (pages.size() < k_sets) {
        throw std::runtime_error("too few pages with literals for the literal tables");
    }
    auto code = [](std::array<double, 256> const& h) {
        return quetschn::seqlz_lit_lengths(h);
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
    auto sources = std::vector<std::array<double, 256>>();
    auto assign = std::vector<std::size_t>(pages.size());
    auto best_total = 0.0;
    for (std::uint64_t seed = opt.first_seed; seed < opt.first_seed + opt.seeds; ++seed) {
        auto rng = std::mt19937_64(seed);
        auto draw = std::discrete_distribution<std::size_t>(weights.begin(), weights.end());
        auto s_sets = std::vector<std::vector<unsigned char>>();
        // the histogram each table is built from
        auto s_sources = std::vector<std::array<double, 256>>();
        for (std::size_t k = 0; k < k_sets; ++k) {
            auto h = pages[draw(rng)];
            for (std::size_t s = 0; s < 256; ++s) {
                h[s] += mean[s] / 10.0;
            }
            s_sets.push_back(code(h));
            s_sources.push_back(h);
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
                    s_sources[k] = sums[k];
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
                s_sources[k] = h;
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
            sources = s_sources;
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
    auto sorted = lit_result{};
    for (auto k : order) {
        sorted.sets.push_back(sets[k]);
        sorted.sources.push_back(sources[k]);
        sorted.pages.push_back(static_cast<double>(used[k]));
    }
    return sorted;
}

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-seqlz-train (--corpus <base> [--counts <file>] | --from-counts <file>)\n"
                 "                            [--lit-sets [--seeds n] [--first-seed n] [--rounds n]]\n"
                 "\n"
                 "Counts seqlz's symbols over the matches of its own matcher on every page. Prints the code\n"
                 "lengths as a C initializer.\n"
                 "--lit-sets prints the literal tables of src/seqlz_lit_sets_4k.inc or _16k.inc instead, from\n"
                 "the pages with more than 64 literals: k-means from --seeds starts (32), seeds --first-seed (1)\n"
                 "on, each at most --rounds rounds (60).\n"
                 "--counts also writes the counts both kinds of tables are built from to <file>, e.g.\n"
                 "bench/seqlz_counts_4k.txt; --from-counts builds the tables from such a file.\n");
}

void print_lengths(quetschn::seqlz_built_tables const& t, std::string const& what) {
    auto print = [](char const* name, std::vector<unsigned char> const& l) {
        std::printf("    .%s = {", name);
        for (std::size_t i = 0; i < l.size(); ++i) {
            std::printf("%s%u", i == 0 ? "" : ", ", l[i]);
        }
        std::printf("},\n");
    };
    std::printf("/* %s, seqlz */\n{\n", what.c_str());
    print("token", t.token);
    print("ll", t.ll);
    print("ml", t.ml);
    std::printf("}\n");
}

void print_lit_sets(std::vector<std::vector<unsigned char>> const& sets, std::string const& what) {
    std::printf("/* %u tables of at most %u bits, %s, seqlz */\n{\n", SEQLZ_LIT_SETS, SEQLZ_LIT_BITS, what.c_str());
    for (auto const& l : sets) {
        std::printf("    {");
        for (std::size_t s = 0; s < l.size(); ++s) {
            std::printf("%s%u", s == 0 ? "" : ", ", l[s]);
        }
        std::printf("},\n");
    }
    std::printf("}\n");
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    auto counts_out = std::string();
    auto counts_in = std::string();
    auto want_lit_sets = false;
    auto search = lit_search{};
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--corpus" && has_value) {
            base = argv[++i];
        } else if (arg == "--counts" && has_value) {
            counts_out = argv[++i];
        } else if (arg == "--from-counts" && has_value) {
            counts_in = argv[++i];
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
    if (base.empty() == counts_in.empty() || (!counts_in.empty() && !counts_out.empty())) {
        usage();
        return 2;
    }
    try {
        if (!counts_in.empty()) {
            auto in = std::ifstream(counts_in);
            if (!in) {
                throw std::runtime_error("cannot read " + counts_in);
            }
            auto const t = quetschn::seqlz_build_tables(quetschn::read_seqlz_counts(in));
            if (want_lit_sets) {
                print_lit_sets(t.lit, "from " + counts_in);
            } else {
                print_lengths(t, "from " + counts_in);
            }
            return 0;
        }
        auto const c = quetschn::load_corpus(base);
        if (c.page_size != SEQLZ_PAGE) {
            throw std::invalid_argument("the corpus has pages of " + std::to_string(c.page_size) + " bytes, this build " +
                                        std::to_string(SEQLZ_PAGE));
        }
        auto counts = quetschn::seqlz_counts{};
        counts.token.assign(SEQLZ_TOKEN_SYMBOLS, 0.0);
        counts.ll.assign(SEQLZ_LEN_SYMBOLS, 0.0);
        counts.ml.assign(SEQLZ_LEN_SYMBOLS, 0.0);
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
                counts.token[seqlz_token(s.literals, s.match, cls)] += 1;
                if (s.literals >= SEQLZ_LL_CAP) {
                    counts.ll[seqlz_len_symbol(s.literals - SEQLZ_LL_CAP, &extra)] += 1;
                }
                if (s.match == 0) {
                    continue;
                }
                if (s.match - 4U >= SEQLZ_ML_CAP) {
                    counts.ml[seqlz_len_symbol(s.match - 4U - SEQLZ_ML_CAP, &extra)] += 1;
                }
                last = s.offset;
            }
        }
        auto const what = "trained on " + std::to_string(pages) + " pages of " + base;

        auto lit = lit_result{};
        if (want_lit_sets || !counts_out.empty()) {
            lit = lit_sets(page_lits, search);
        }
        if (!counts_out.empty()) {
            counts.lit = lit.sources;
            counts.lit_pages = lit.pages;
            counts.comments = {
                "The symbol counts seqlz's tables for " + std::to_string(SEQLZ_PAGE / 1024) +
                    " KiB pages are built from: quetschn-seqlz-train --from-counts <this file>",
                "[--lit-sets] gives the code lengths. Made by quetschn-seqlz-train --corpus " + base + " --counts,",
                std::to_string(pages) + " pages, " + std::to_string(page_lits.size()) +
                    " of them with more than 64 literals, k-means seeds " + std::to_string(search.first_seed) + " to " +
                    std::to_string(search.first_seed + search.seeds - 1) + ". token, ll and ml: how often",
                "each symbol occurred, by number; lit: a literal table's pages, then how often each byte",
                "occurred as a literal on them."};
            auto out = std::ofstream(counts_out);
            quetschn::write_seqlz_counts(out, counts);
            if (!out) {
                throw std::runtime_error("cannot write " + counts_out);
            }
        }
        if (want_lit_sets) {
            print_lit_sets(lit.sets,
                           "trained on the " + std::to_string(page_lits.size()) + " pages with more than 64 literals of " +
                               std::to_string(pages) + " pages of " + base);
        } else {
            print_lengths(quetschn::seqlz_build_tables(counts), what);
        }
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
