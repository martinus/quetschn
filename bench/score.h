// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace quetschn {

// What a page costs in memory and in time, from one zram device of a run of tools/zram-vm/run.sh or
// one codec of quetschn-bench-*.
struct codec_cost {
    std::string name;
    double pages = 0.0;          // VM: orig_data_size / 4096, same-filled pages included; bench: measured
    double bytes_per_page = 0.0; // VM: mem_used_total / pages, after recompression; bench: zsmalloc cost
    double write_ns = 0.0;       // mean; VM: "write, other page before"; bench: compress
    double read_ns = 0.0;        // mean; VM: "flushed, other page first", prefetch 8; bench: decompress cold
    double recompress_ns = 0.0;  // VM, per page, 0 without a secondary algorithm
};

// The RESULT lines of one run, in the order of the devices; other lines are skipped. prefix goes in
// front of every name, to tell runs apart. Throws std::runtime_error when a device lacks one of the
// numbers, e.g. a log from before the means.
[[nodiscard]] std::vector<codec_cost> read_vm_results(std::istream& in, std::string const& prefix);

// The codecs of the output of quetschn-bench-* with timing, same-filled pages excluded, in the order
// they appear. Throws std::runtime_error when a codec lacks its cost or the mean of compress or of
// decompress cold.
[[nodiscard]] std::vector<codec_cost> read_bench_results(std::istream& in, std::string const& prefix);

// PLAN.md §1.1. All times per page written to zram.
struct score_weights {
    double reads_per_write = 0.34; // pswpin / pswpout, 27.5 days of a desktop
    double recompress_weight = 1.0;
};

// Time per page written: its write, the reads of it, its recompression. In us.
[[nodiscard]] double us_per_page(codec_cost const& c, score_weights const& w);

// The codecs that have the lowest bytes_per_page + lambda * us_per_page for some lambda >= 0, as
// indices into codecs: the lower left convex hull of the points (us, bytes), from the fastest to the
// smallest. Between neighbours k and k + 1 the exchange rate is (bytes k - bytes k + 1) / (us k + 1 -
// us k) bytes per us: above it the faster one wins, below it the smaller one.
[[nodiscard]] std::vector<std::size_t> best_for_some_lambda(std::vector<codec_cost> const& codecs, score_weights const& w);

} // namespace quetschn
