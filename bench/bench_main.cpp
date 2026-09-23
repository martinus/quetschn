// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// One binary per codec (QUETSCHN_CODEC is set by CMake), so that code layout and alignment of one
// codec cannot shift the numbers of another. See PLAN.md §5.2 for the metrics.

#include "harness.h"
#include "zsmalloc_cost.h"

#include <sched.h>
#include <unistd.h>

#include <charconv>
#include <cstdio>
#include <exception>
#include <fstream>
#include <string>
#include <string_view>

#ifndef QUETSCHN_CODEC
#    error "QUETSCHN_CODEC must name one of the quetschn_codec_* objects"
#endif

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: %s --corpus <base> [--repetitions <n>] [--cpu <n>] [--out <file.tsv>]\n"
                 "\n"
                 "Reads <base>.pages and <base>.tsv as written by quetschn-collect-resident.\n"
                 "--cpu pins the process to one CPU; set a fixed frequency yourself.\n"
                 "--out writes one line per page, for paired comparisons between codecs.\n",
                 QUETSCHN_CODEC.name);
}

template <typename T>
bool parse(std::string_view s, T& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

std::string first_line(std::string const& path) {
    auto in = std::ifstream(path);
    auto line = std::string();
    std::getline(in, line);
    return line.empty() ? "unknown" : line;
}

std::string cpu_model() {
    auto in = std::ifstream("/proc/cpuinfo");
    auto line = std::string();
    while (std::getline(in, line)) {
        if (line.starts_with("model name")) {
            return line.substr(line.find(':') + 2);
        }
    }
    return "unknown";
}

void print_latency(char const* what, quetschn::latency_summary const& l) {
    std::printf("%-22s %9.0f %9.0f %9.0f %9.0f %9.0f\n", what, l.p50, l.p90, l.p99, l.p999, l.max);
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    auto out_path = std::string();
    auto opts = quetschn::run_options{};
    int cpu = -1;
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--corpus" && has_value) {
            base = argv[++i];
        } else if (arg == "--out" && has_value) {
            out_path = argv[++i];
        } else if (arg == "--repetitions" && has_value && parse(argv[++i], opts.repetitions) && opts.repetitions > 0) {
        } else if (arg == "--cpu" && has_value && parse(argv[++i], cpu)) {
        } else {
            usage();
            return 2;
        }
    }
    if (base.empty()) {
        usage();
        return 2;
    }

    try {
        if (cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cpu, &set);
            if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
                std::perror("sched_setaffinity");
                return 1;
            }
        }
        auto const c = quetschn::load_corpus(base);
        auto const model = quetschn::zsmalloc_model(quetschn::zsmalloc_config{.page_size = c.page_size});

        auto const governor_cpu = cpu >= 0 ? cpu : ::sched_getcpu();
        std::printf("codec      %s\n", QUETSCHN_CODEC.name);
        std::printf("corpus     %s, %zu pages of %zu bytes\n", base.c_str(), c.size(), c.page_size);
        std::printf(
            "cpu        %s, pinned: %s, governor: %s\n",
            cpu_model().c_str(),
            cpu >= 0 ? std::to_string(cpu).c_str() : "no",
            first_line("/sys/devices/system/cpu/cpu" + std::to_string(governor_cpu) + "/cpufreq/scaling_governor").c_str());
        std::printf("method     median of %u runs per page, percentiles across pages, ns\n", opts.repetitions);

        auto const r = quetschn::run_codec(c, QUETSCHN_CODEC, model, opts);
        auto const s = quetschn::summarize(r, c.page_size);

        std::printf("\n");
        std::printf("pages                  %zu measured, %zu same-filled skipped\n", s.pages, s.same_filled);
        std::printf("zsmalloc cost          %.0f bytes, %.1f%% of uncompressed, %.1f bytes/page\n",
                    s.total_cost,
                    s.pages == 0 ? 0.0 : 100.0 * s.total_cost / s.total_uncompressed,
                    s.pages == 0 ? 0.0 : s.total_cost / static_cast<double>(s.pages));
        std::printf("stored uncompressed    %zu pages (comp_len >= %zu)\n", s.huge, model.huge_class_size());
        std::printf("workspace per CPU      %zu bytes\n", QUETSCHN_CODEC.workspace_size);
        std::printf("\n%-22s %9s %9s %9s %9s %9s\n", "latency ns", "p50", "p90", "p99", "p99.9", "max");
        print_latency("compress", s.compress);
        print_latency("decompress warm", s.decompress);
        print_latency("decompress cold", s.decompress_cold);

        if (!out_path.empty()) {
            auto out = std::ofstream(out_path);
            out << "page\tcomp_len\thuge\tcost\tcompress_ns\tdecompress_ns\tdecompress_cold_ns\n";
            for (auto const& p : r.pages) {
                out << p.page << '\t' << p.comp_len << '\t' << (p.huge ? 1 : 0) << '\t' << p.cost << '\t' << p.compress_ns
                    << '\t' << p.decompress_ns << '\t' << p.decompress_cold_ns << '\n';
            }
            if (!out) {
                std::fprintf(stderr, "error: cannot write %s\n", out_path.c_str());
                return 1;
            }
        }
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
