// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "seqlz_train.h"

// seqlz.h is C, written for the kernel
extern "C" {
#include "seqlz.h"
}

#include <algorithm>
#include <cstddef>
#include <istream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace quetschn {

// The optimal code lengths of at most max_bits bits, by package-merge (Larmore and Hirschberg): the
// lists of the levels from max_bits up to 1 each hold the symbols and the pairs of the level below,
// sorted by weight; a symbol's length is how often it is among the 2n - 2 lightest items of the top
// list, counted through the pairs. With 1536 tokens and 11 bits, halving all counts until a Huffman
// code fits cost 18% more token bits than no limit at all.
std::vector<unsigned char> seqlz_code_lengths(std::vector<double> const& counts, unsigned max_bits) {
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
std::vector<unsigned char> seqlz_token_lengths(std::vector<double> const& counts) {
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
        auto const l = seqlz_code_lengths(kept, SEQLZ_TOKEN_BITS);
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

std::vector<unsigned char> seqlz_lit_lengths(std::array<double, 256> const& histogram) {
    // every byte gets a code, the encoder needs one: + 1
    auto c = std::vector<double>(256);
    for (std::size_t s = 0; s < 256; ++s) {
        c[s] = histogram[s] + 1.0;
    }
    return seqlz_code_lengths(c, SEQLZ_LIT_BITS);
}

seqlz_built_tables seqlz_build_tables(seqlz_counts const& c) {
    auto plus_one = [](std::vector<double> v) {
        for (auto& x : v) {
            x += 1.0;
        }
        return v;
    };
    auto t = seqlz_built_tables{};
    t.token = seqlz_token_lengths(plus_one(c.token));
    t.ll = seqlz_code_lengths(plus_one(c.ll), SEQLZ_MAX_BITS);
    t.ml = seqlz_code_lengths(plus_one(c.ml), SEQLZ_MAX_BITS);
    for (auto const& h : c.lit) {
        t.lit.push_back(seqlz_lit_lengths(h));
    }
    return t;
}

namespace {

void write_line(std::ostream& out, char const* name, double const* v, std::size_t n) {
    out << name << ' ' << n;
    for (std::size_t i = 0; i < n; ++i) {
        out << ' ' << v[i];
    }
    out << '\n';
}

std::vector<double> read_line(std::istream& in, std::string const& name, std::size_t n) {
    auto line = std::string();
    while (std::getline(in, line) && (line.empty() || line[0] == '#')) {
    }
    auto ls = std::istringstream(line);
    auto got = std::string();
    auto size = std::size_t{0};
    if (!(ls >> got >> size) || got != name || size != n) {
        throw std::runtime_error("seqlz counts: expected " + name + " with " + std::to_string(n) + " numbers, got \"" +
                                 line.substr(0, 40) + "\"");
    }
    auto v = std::vector<double>(n);
    for (auto& x : v) {
        if (!(ls >> x) || x < 0.0) {
            throw std::runtime_error("seqlz counts: " + name + " has a number missing or below 0");
        }
    }
    auto rest = std::string();
    if (ls >> rest) {
        throw std::runtime_error("seqlz counts: " + name + " has more than " + std::to_string(n) + " numbers");
    }
    return v;
}

} // namespace

void write_seqlz_counts(std::ostream& out, seqlz_counts const& c) {
    auto const precision = out.precision(17);
    for (auto const& line : c.comments) {
        out << "# " << line << '\n';
    }
    write_line(out, "token", c.token.data(), c.token.size());
    write_line(out, "ll", c.ll.data(), c.ll.size());
    write_line(out, "ml", c.ml.data(), c.ml.size());
    for (std::size_t k = 0; k < c.lit.size(); ++k) {
        auto v = std::vector<double>{c.lit_pages[k]};
        v.insert(v.end(), c.lit[k].begin(), c.lit[k].end());
        write_line(out, "lit", v.data(), v.size());
    }
    out.precision(precision);
}

seqlz_counts read_seqlz_counts(std::istream& in) {
    auto c = seqlz_counts{};
    while (in.peek() == '#') {
        auto line = std::string();
        std::getline(in, line);
        c.comments.push_back(line.size() > 2 ? line.substr(2) : std::string());
    }
    c.token = read_line(in, "token", SEQLZ_TOKEN_SYMBOLS);
    c.ll = read_line(in, "ll", SEQLZ_LEN_SYMBOLS);
    c.ml = read_line(in, "ml", SEQLZ_LEN_SYMBOLS);
    for (unsigned k = 0; k < SEQLZ_LIT_SETS; ++k) {
        auto const v = read_line(in, "lit", 257);
        c.lit_pages.push_back(v[0]);
        auto h = std::array<double, 256>{};
        std::copy(v.begin() + 1, v.end(), h.begin());
        c.lit.push_back(h);
    }
    auto line = std::string();
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] != '#') {
            throw std::runtime_error("seqlz counts: more lines than the tables");
        }
    }
    return c;
}

} // namespace quetschn
