// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Writes a fixed random sample of a corpus, for fast latency measurements. See split.h.

#include "split.h"

#include <charconv>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-sample-corpus --corpus <base> --out <base> [--pages <n>] [--seed <n>]\n"
                 "\n"
                 "A uniform random sample of --pages pages (default 20000), in their original order. The same\n"
                 "seed gives the same sample. Output files are created with mode 0600.\n");
}

template <typename T>
bool parse(std::string_view s, T& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    auto out = std::string();
    auto pages = std::size_t{20000};
    auto seed = std::uint64_t{1};
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--corpus" && has_value) {
            base = argv[++i];
        } else if (arg == "--out" && has_value) {
            out = argv[++i];
        } else if (arg == "--pages" && has_value && parse(argv[++i], pages) && pages > 0) {
        } else if (arg == "--seed" && has_value && parse(argv[++i], seed)) {
        } else {
            usage();
            return 2;
        }
    }
    if (base.empty() || out.empty()) {
        usage();
        return 2;
    }
    try {
        std::printf("%zu pages\n", quetschn::sample_corpus(base, out, pages, seed));
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
