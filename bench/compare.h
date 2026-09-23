// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include "harness.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

namespace quetschn {

// The per-page lines of quetschn-bench-* --out, one page_result per line after a header line.
void write_page_results(std::ostream& out, std::vector<page_result> const& pages);

// Throws std::runtime_error when the header or a line is not what write_page_results writes.
[[nodiscard]] std::vector<page_result> read_page_results(std::istream& in);

struct compare_options {
    // Bootstrap: pages are drawn with replacement, the same draw for both runs, this many times.
    unsigned resamples = 1000;
    std::uint64_t seed = 1;
    double confidence = 0.95;
    // 0 for one per CPU. Each thread needs 48 bytes per page. The result does not depend on it.
    unsigned threads = 0;
};

// A statistic of the measured pages, and its bootstrap confidence interval (percentile method).
struct estimate {
    double value = 0.0;
    double low = 0.0;
    double high = 0.0;
};

// Percentile of the candidate minus the same percentile of the baseline, in ns. Negative is faster.
struct latency_difference {
    double p = 0.0;
    estimate compress;
    estimate decompress;
    estimate decompress_cold;
};

struct comparison {
    std::size_t pages = 0;
    double baseline_cost = 0.0;  // Σ zsmalloc cost
    double candidate_cost = 0.0; // Σ zsmalloc cost
    estimate saving;             // 1 - candidate_cost / baseline_cost, positive when the candidate needs less
    std::size_t cheaper = 0;     // pages where the candidate costs less
    std::size_t same = 0;
    std::size_t costlier = 0;
    double largest_regression = 0.0; // largest cost increase of one page, bytes, 0 when there is none
    std::size_t largest_regression_page = 0;
    std::vector<latency_difference> latency; // p50, p99, p99.9
};

// Pairs the pages of two runs on the same corpus. Both must list the same pages in the same order,
// otherwise std::invalid_argument. The runs must not be empty.
[[nodiscard]] comparison
compare(std::vector<page_result> const& baseline, std::vector<page_result> const& candidate, compare_options const& opts);

} // namespace quetschn
