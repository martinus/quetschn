// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Splits a page corpus into a training side for dictionaries and a test side for measuring, by process
// name. See split.h, and PLAN.md §5.3 for why the dictionary must not see the pages it is measured on.

#include "split.h"

#include <charconv>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-split-corpus --corpus <base> --train <base> --test <base> [--test-fraction <f>]\n"
                 "                             [--seed <n>]\n"
                 "\n"
                 "All pages of a process name go to the same side. --test-fraction is the share of process\n"
                 "names on the test side, default 0.3. Output files are created with mode 0600.\n"
                 "\n"
                 "A dictionary for zram, like Honor's in kernel commit f0f6f7871430, from the training side:\n"
                 "  zstd --train <train>.pages -B4096 --maxdict=64KB -o dict\n");
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    auto train = std::string();
    auto test = std::string();
    auto opts = quetschn::split_options{};
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--corpus" && has_value) {
            base = argv[++i];
        } else if (arg == "--train" && has_value) {
            train = argv[++i];
        } else if (arg == "--test" && has_value) {
            test = argv[++i];
        } else if (arg == "--test-fraction" && has_value) {
            auto const v = std::string_view(argv[++i]);
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), opts.test_fraction);
            if (ec != std::errc{} || ptr != v.data() + v.size() || opts.test_fraction <= 0 || opts.test_fraction >= 1) {
                usage();
                return 2;
            }
        } else if (arg == "--seed" && has_value) {
            auto const v = std::string_view(argv[++i]);
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), opts.seed);
            if (ec != std::errc{} || ptr != v.data() + v.size()) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (base.empty() || train.empty() || test.empty()) {
        usage();
        return 2;
    }
    try {
        auto const r = quetschn::split_corpus(base, train, test, opts);
        std::printf("train: %zu pages from %zu process names (%zu same-filled pages left out)\n",
                    r.train_pages,
                    r.train_names,
                    r.same_filled_dropped);
        std::printf("test:  %zu pages from %zu process names\n", r.test_pages, r.test_names);
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
