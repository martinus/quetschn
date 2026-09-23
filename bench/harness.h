// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include "kernel_codecs/zram_codec.h"
#include "zsmalloc_cost.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace quetschn {

// A page corpus as quetschn-collect-resident writes it: <base>.pages and <base>.tsv.
struct corpus {
    std::size_t page_size = 0;
    std::vector<std::byte> data;

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::span<std::byte const> page(std::size_t i) const;
};

// Reads <base>.pages, and the page size from the first line of <base>.tsv.
[[nodiscard]] corpus load_corpus(std::filesystem::path const& base);

struct run_options {
    // Each page is timed this many times, and its latency is the median of those. Percentiles are
    // taken across pages afterwards, so they describe slow pages and not slow measurements.
    unsigned repetitions = 5;
    bool measure_time = true;
    // zram's algorithm_params level, QUETSCHN_LEVEL_DEFAULT for the codec's default
    int level = QUETSCHN_LEVEL_DEFAULT;
};

struct page_result {
    std::size_t page = 0;            // index in the corpus
    unsigned int comp_len = 0;       // codec output
    bool huge = false;               // zram stores the page uncompressed, comp_len >= huge_class_size
    double cost = 0.0;               // zsmalloc bytes, see zsmalloc_model::cost
    double compress_ns = 0.0;        // median; zram always compresses, even pages it then stores raw
    double decompress_ns = 0.0;      // median, warm cache. For huge pages zram does a memcpy, so that is timed.
    double decompress_cold_ns = 0.0; // same, with source and destination flushed from all caches first
};

struct run_result {
    int level = 0;                  // after the codec applied its default
    std::size_t workspace_size = 0; // what zram allocates per CPU for this codec and level
    std::size_t same_filled = 0;    // skipped: zram stores these without a codec
    std::vector<page_result> pages;
};

// Compresses every page that is not same-filled with the codec, checks that it decompresses to the
// same bytes, and measures both directions. Buffer sizes are zram's: 2 * page_size for the compressed
// output, page_size for the decompressed one. Throws std::runtime_error when the codec fails or the
// roundtrip does not reproduce the page, std::invalid_argument when zram would reject the level.
[[nodiscard]] run_result
run_codec(corpus const& c, quetschn_codec const& codec, zsmalloc_model const& model, run_options const& opts);

// Nearest-rank percentile, p in [0, 100]. values must not be empty.
[[nodiscard]] double percentile(std::vector<double> values, double p);

struct latency_summary {
    double p50 = 0.0;
    double p90 = 0.0;
    double p99 = 0.0;
    double p999 = 0.0;
    double max = 0.0;
};

[[nodiscard]] latency_summary summarize_latency(std::vector<double> const& values);

struct run_summary {
    std::size_t pages = 0; // measured, same-filled pages excluded
    std::size_t same_filled = 0;
    std::size_t huge = 0;
    double total_cost = 0.0;         // sum of zsmalloc bytes
    double total_uncompressed = 0.0; // pages * page_size
    latency_summary compress;
    latency_summary decompress;
    latency_summary decompress_cold;
};

[[nodiscard]] run_summary summarize(run_result const& r, std::size_t page_size);

} // namespace quetschn
