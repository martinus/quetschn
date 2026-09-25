// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Memory against time per page, from runs of tools/zram-vm/run.sh or quetschn-bench-*, see PLAN.md
// §1.1.

#include "score.h"

#include <charconv>
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-score [--reads-per-write <r>] [--recompress-weight <b>] [--lambda <bytes per us>]\n"
                 "                      <run.log>[:<prefix>] ...\n"
                 "\n"
                 "Each log is the output of tools/zram-vm/run.sh or of quetschn-bench-* with timing, all from the\n"
                 "same corpus; <prefix> goes in front of its codec names, to tell runs apart. Time per page written =\n"
                 "write + r * cold read + b * recompression, the means over the pages; in a bench log write is\n"
                 "compress and read is decompress cold. --reads-per-write default 0.34, --recompress-weight 1.\n"
                 "Lists the codecs with the lowest bytes + lambda * us for some lambda, with the exchange rates\n"
                 "between them; with --lambda also that score for each codec.\n");
}

bool parse(std::string_view s, double& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

} // namespace

int main(int argc, char** argv) {
    auto w = quetschn::score_weights{};
    auto lambda = -1.0;
    auto codecs = std::vector<quetschn::codec_cost>();
    try {
        for (int i = 1; i < argc; ++i) {
            auto const arg = std::string_view(argv[i]);
            auto const value = [&](double& out) {
                if (i + 1 >= argc || !parse(argv[i + 1], out) || out < 0.0) {
                    throw std::invalid_argument(std::string(arg) + " needs a number >= 0");
                }
                ++i;
            };
            if (arg == "--reads-per-write") {
                value(w.reads_per_write);
            } else if (arg == "--recompress-weight") {
                value(w.recompress_weight);
            } else if (arg == "--lambda") {
                value(lambda);
            } else if (arg.starts_with("-")) {
                usage();
                return 2;
            } else {
                auto const colon = arg.rfind(':');
                auto const path = std::string(colon == std::string_view::npos ? arg : arg.substr(0, colon));
                auto const prefix = colon == std::string_view::npos ? std::string() : std::string(arg.substr(colon + 1)) + " ";
                auto file = std::ifstream(path);
                if (!file) {
                    throw std::runtime_error("cannot read " + path);
                }
                auto text = std::stringstream();
                text << file.rdbuf();
                auto in = std::istringstream(text.str());
                auto const vm = text.str().find("RESULT ") != std::string::npos;
                for (auto& c : vm ? quetschn::read_vm_results(in, prefix) : quetschn::read_bench_results(in, prefix)) {
                    codecs.push_back(std::move(c));
                }
            }
        }
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    if (codecs.empty()) {
        usage();
        return 2;
    }

    std::printf("time per page written = write + %.2f * cold read + %.2f * recompression, means, us\n\n",
                w.reads_per_write,
                w.recompress_weight);
    std::printf("%-32s %10s %9s %9s %11s %9s", "codec", "bytes/page", "write", "read", "recompress", "us/page");
    if (lambda >= 0.0) {
        std::printf(" %13s", "score");
    }
    std::printf("\n");
    for (auto const& c : codecs) {
        std::printf("%-32s %10.1f %9.2f %9.2f %11.2f %9.2f",
                    c.name.c_str(),
                    c.bytes_per_page,
                    c.write_ns / 1000.0,
                    c.read_ns / 1000.0,
                    c.recompress_ns / 1000.0,
                    quetschn::us_per_page(c, w));
        if (lambda >= 0.0) {
            std::printf(" %13.1f", c.bytes_per_page + lambda * quetschn::us_per_page(c, w));
        }
        std::printf("\n");
    }

    auto const hull = quetschn::best_for_some_lambda(codecs, w);
    std::printf("\nbest for some lambda, from the fastest to the smallest; lambda in bytes per us\n");
    for (std::size_t k = 0; k < hull.size(); ++k) {
        auto const& c = codecs[hull[k]];
        std::printf("%-32s", c.name.c_str());
        if (k + 1 < hull.size()) {
            auto const& n = codecs[hull[k + 1]];
            auto const rate =
                (c.bytes_per_page - n.bytes_per_page) / (quetschn::us_per_page(n, w) - quetschn::us_per_page(c, w));
            std::printf("  -> %s: %.1f bytes for %.2f us more, %.1f bytes per us",
                        n.name.c_str(),
                        c.bytes_per_page - n.bytes_per_page,
                        quetschn::us_per_page(n, w) - quetschn::us_per_page(c, w),
                        rate);
        }
        std::printf("\n");
    }
    return 0;
}
