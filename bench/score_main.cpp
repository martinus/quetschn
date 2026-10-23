// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Memory against time per page, from runs of tools/zram-vm/run.sh or seqlz-bench-*, see docs/plan.md
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
#include <utility>
#include <vector>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: seqlz-score [--reads-per-write <r>] [--recompress-weight <b>] [--lambda <bytes per us>]\n"
                 "                      [--what-if <codec>:<factor>] ... [--per-cpu <codec>:<bytes>] ...\n"
                 "                      [--device <cpus>:<MiB>] ... <run.log>[:<prefix>] ...\n"
                 "\n"
                 "Each log is the output of tools/zram-vm/run.sh, with BOOTS=n the mean over the boots, or of\n"
                 "seqlz-bench-* with timing, all from the same corpus; <prefix> goes in front of its codec\n"
                 "names, to tell runs apart. Time per page written =\n"
                 "write + r * cold read + b * recompression, the means over the pages; in a bench log write is\n"
                 "compress and read is decompress cold, in a log of MODE=swap the swap-out with one call per page\n"
                 "and the swap-in with the compressed data flushed. --reads-per-write default 0.73, a phone's\n"
                 "(a desktop's is 0.34, docs/plan.md §1.1), --recompress-weight 1.\n"
                 "Lists the codecs with the lowest bytes + lambda * us for some lambda, with the exchange rates\n"
                 "between them; with --lambda also that score for each codec.\n"
                 "--what-if adds a codec with the bytes of <codec> (its name with the prefix) and its own time times\n"
                 "<factor>, e.g. lz4 in hardware. In a log of MODE=swap the time of a same-filled page is the kernel's\n"
                 "part and stays; elsewhere the whole time is scaled.\n"
                 "Memory per CPU: a bench log has it (\"memory per CPU\"), a log of run.sh does not, --per-cpu gives\n"
                 "it for a codec (its name with the prefix). With it for every codec, the codecs are listed again\n"
                 "for each --device, a machine with <cpus> CPUs and <MiB> of pages in zram, with memory per CPU\n"
                 "* cpus / pages added to the bytes per page. Default devices: 8 and 32 CPUs, 64 and 1024 MiB each.\n");
}

// the codecs that win for some lambda, with the exchange rates between neighbours
void print_hull(std::vector<bench::codec_cost> const& codecs, bench::score_weights const& w) {
    auto const hull = bench::best_for_some_lambda(codecs, w);
    for (std::size_t k = 0; k < hull.size(); ++k) {
        auto const& c = codecs[hull[k]];
        std::printf("%-32s %10.1f", c.name.c_str(), c.bytes_per_page);
        if (k + 1 < hull.size()) {
            auto const& n = codecs[hull[k + 1]];
            auto const rate = (c.bytes_per_page - n.bytes_per_page) / (bench::us_per_page(n, w) - bench::us_per_page(c, w));
            std::printf("  -> %s: %.1f bytes for %.2f us more, %.1f bytes per us",
                        n.name.c_str(),
                        c.bytes_per_page - n.bytes_per_page,
                        bench::us_per_page(n, w) - bench::us_per_page(c, w),
                        rate);
        }
        std::printf("\n");
    }
}

bool parse(std::string_view s, double& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

} // namespace

int main(int argc, char** argv) {
    auto w = bench::score_weights{};
    auto lambda = -1.0;
    auto codecs = std::vector<bench::codec_cost>();
    auto what_ifs = std::vector<std::pair<std::string, double>>();
    auto per_cpu = std::vector<std::pair<std::string, double>>();
    auto devices = std::vector<bench::zram_device>();
    // "<name>:<number>", the number >= 0
    auto const named = [](std::string_view spec, char const* option) {
        auto const colon = spec.rfind(':');
        auto x = 0.0;
        if (colon == std::string_view::npos || colon == 0 || !parse(spec.substr(colon + 1), x) || x < 0.0) {
            throw std::invalid_argument(std::string(option) + " needs <name>:<number>, the number >= 0");
        }
        return std::pair(std::string(spec.substr(0, colon)), x);
    };
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
            } else if (arg == "--what-if" || arg == "--per-cpu" || arg == "--device") {
                auto const spec = named(i + 1 < argc ? argv[i + 1] : "", argv[i]);
                if (arg == "--what-if") {
                    what_ifs.push_back(spec);
                } else if (arg == "--per-cpu") {
                    per_cpu.push_back(spec);
                } else {
                    auto cpus = 0.0;
                    if (!parse(spec.first, cpus) || cpus < 1.0 || spec.second <= 0.0) {
                        throw std::invalid_argument("--device needs <cpus>:<MiB>, both > 0");
                    }
                    devices.push_back({cpus, spec.second});
                }
                ++i;
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
                for (auto& c : vm ? bench::read_vm_boots(in, prefix) : bench::read_bench_results(in, prefix)) {
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
    for (auto const& [name, bytes] : per_cpu) {
        auto found = false;
        for (auto& c : codecs) {
            if (c.name == name) {
                c.per_cpu_bytes = bytes;
                found = true;
            }
        }
        if (!found) {
            std::fprintf(stderr, "error: --per-cpu: no codec %s in the logs\n", name.c_str());
            return 2;
        }
    }
    for (auto const& [name, factor] : what_ifs) {
        auto const n = codecs.size();
        for (std::size_t k = 0; k < n; ++k) {
            if (codecs[k].name == name) {
                codecs.push_back(bench::what_if(codecs[k], factor));
            }
        }
        if (codecs.size() == n) {
            std::fprintf(stderr, "error: --what-if: no codec %s in the logs\n", name.c_str());
            return 2;
        }
    }

    std::printf("time per page written = write + %.2f * cold read + %.2f * recompression, means, us\n\n",
                w.reads_per_write,
                w.recompress_weight);
    std::printf("%-32s %10s %9s %9s %11s %9s %9s", "codec", "bytes/page", "write", "read", "recompress", "us/page", "per CPU");
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
                    bench::us_per_page(c, w));
        if (c.per_cpu_bytes >= 0.0) {
            std::printf(" %9.0f", c.per_cpu_bytes);
        } else {
            std::printf(" %9s", "-");
        }
        if (lambda >= 0.0) {
            std::printf(" %13.1f", c.bytes_per_page + lambda * bench::us_per_page(c, w));
        }
        std::printf("\n");
    }

    std::printf("\nbest for some lambda, without memory per CPU, from the fastest to the smallest; bytes per page,\n"
                "lambda in bytes per us\n");
    print_hull(codecs, w);

    auto missing = std::string();
    for (auto const& c : codecs) {
        if (c.per_cpu_bytes < 0.0) {
            missing += " " + c.name;
        }
    }
    if (!missing.empty()) {
        std::printf("\nno memory per CPU for%s: --per-cpu <codec>:<bytes> gives it\n", missing.c_str());
        return 0;
    }
    if (devices.empty()) {
        devices = {{8, 64}, {8, 1024}, {32, 64}, {32, 1024}};
    }
    for (auto const& d : devices) {
        std::printf("\nbest for some lambda, %.0f CPUs and %.0f MiB of pages in zram, bytes per page with memory per CPU\n",
                    d.cpus,
                    d.stored_mib);
        print_hull(bench::on_device(codecs, d), w);
    }
    return 0;
}
