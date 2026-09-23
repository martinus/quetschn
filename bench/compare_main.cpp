// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Paired comparison of two quetschn-bench-* --out files from the same corpus, see PLAN.md §5.2.

#include "compare.h"

#include <charconv>
#include <cstdio>
#include <exception>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-compare --baseline <file.tsv> --candidate <file.tsv> [--resamples <n>] [--seed <n>]\n"
                 "                        [--threads <n>]\n"
                 "\n"
                 "Both files come from quetschn-bench-* --out on the same corpus. Confidence intervals are 95%%,\n"
                 "from a bootstrap that resamples pages, the same pages for both runs. --resamples default 1000.\n"
                 "--threads default one per CPU, each needs 48 bytes per page.\n");
}

template <typename T>
bool parse(std::string_view s, T& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

std::vector<quetschn::page_result> read(std::string const& path) {
    auto in = std::ifstream(path);
    if (!in) {
        throw std::runtime_error("cannot read " + path);
    }
    return quetschn::read_page_results(in);
}

void print_latency(char const* what, double p, quetschn::estimate const& e) {
    std::printf("%-16s p%-5g %+9.0f   [%+.0f, %+.0f]\n", what, p, e.value, e.low, e.high);
}

} // namespace

int main(int argc, char** argv) {
    auto baseline_path = std::string();
    auto candidate_path = std::string();
    auto opts = quetschn::compare_options{};
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--baseline" && has_value) {
            baseline_path = argv[++i];
        } else if (arg == "--candidate" && has_value) {
            candidate_path = argv[++i];
        } else if (arg == "--resamples" && has_value && parse(argv[++i], opts.resamples) && opts.resamples > 0) {
        } else if (arg == "--seed" && has_value && parse(argv[++i], opts.seed)) {
        } else if (arg == "--threads" && has_value && parse(argv[++i], opts.threads)) {
        } else {
            usage();
            return 2;
        }
    }
    if (baseline_path.empty() || candidate_path.empty()) {
        usage();
        return 2;
    }

    try {
        auto const r = quetschn::compare(read(baseline_path), read(candidate_path), opts);
        std::printf("baseline     %s\n", baseline_path.c_str());
        std::printf("candidate    %s\n", candidate_path.c_str());
        std::printf(
            "method       %zu paired pages, %u bootstrap resamples, 95%% confidence intervals\n\n", r.pages, opts.resamples);

        std::printf("zsmalloc cost          %.0f -> %.0f bytes\n", r.baseline_cost, r.candidate_cost);
        std::printf("candidate saves        %.2f%%   [%.2f%%, %.2f%%]\n",
                    100.0 * r.saving.value,
                    100.0 * r.saving.low,
                    100.0 * r.saving.high);
        std::printf("per page               %zu cheaper, %zu same, %zu costlier\n", r.cheaper, r.same, r.costlier);
        if (r.costlier > 0) {
            std::printf("largest regression     +%.0f bytes, page %zu\n", r.largest_regression, r.largest_regression_page);
        }

        std::printf("\nlatency ns, candidate - baseline, negative is faster\n");
        for (auto const& l : r.latency) {
            print_latency("compress", l.p, l.compress);
        }
        for (auto const& l : r.latency) {
            print_latency("decompress warm", l.p, l.decompress);
        }
        for (auto const& l : r.latency) {
            print_latency("decompress cold", l.p, l.decompress_cold);
        }
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
