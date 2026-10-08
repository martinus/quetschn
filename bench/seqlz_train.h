// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include <array>
#include <iosfwd>
#include <string>
#include <vector>

namespace quetschn {

// What seqlz's tables are built from: how often each symbol occurred on the training pages, summed over
// all of them. The literal tables have one histogram each, of the pages k-means gave that table in its
// last round. Counts only, no page and nothing per page.
struct seqlz_counts {
    std::vector<double> token;                // SEQLZ_TOKEN_SYMBOLS, without the escape
    std::vector<double> ll;                   // SEQLZ_LEN_SYMBOLS
    std::vector<double> ml;                   // SEQLZ_LEN_SYMBOLS
    std::vector<std::array<double, 256>> lit; // SEQLZ_LIT_SETS, the most used table first
    std::vector<double> lit_pages;            // pages per literal table
    std::vector<std::string> comments;        // the lines of the file's head, without "# "
};

// The code lengths built from seqlz_counts, as src/seqlz.h's struct seqlz_lengths and seqlz_lit_sets have
// them.
struct seqlz_built_tables {
    std::vector<unsigned char> token; // SEQLZ_TOKEN_SYMBOLS + 1, the escape last
    std::vector<unsigned char> ll;
    std::vector<unsigned char> ml;
    std::vector<std::vector<unsigned char>> lit;
};

// The optimal code lengths of at most max_bits bits for the counts.
[[nodiscard]] std::vector<unsigned char> seqlz_code_lengths(std::vector<double> const& counts, unsigned max_bits);

// The token lengths with an escape, for the counts of the tokens without the escape.
[[nodiscard]] std::vector<unsigned char> seqlz_token_lengths(std::vector<double> const& counts);

// A literal table for a histogram: every byte gets a code, one more than its count.
[[nodiscard]] std::vector<unsigned char> seqlz_lit_lengths(std::array<double, 256> const& histogram);

// The tables of the counts: each token, ll and ml count one more, so that a symbol that never occurred
// still gets a code.
[[nodiscard]] seqlz_built_tables seqlz_build_tables(seqlz_counts const& c);

// The counts as text: the comments as "# " lines, then one line per table, its name, how many numbers
// follow and the numbers: token, ll, ml, then lit with the pages of the table first. read_seqlz_counts()
// throws std::runtime_error for anything else, or for sizes that are not this build's.
void write_seqlz_counts(std::ostream& out, seqlz_counts const& c);
[[nodiscard]] seqlz_counts read_seqlz_counts(std::istream& in);

} // namespace quetschn
