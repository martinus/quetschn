// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// One binary per codec (QUETSCHN_CODEC is set by CMake), so that code layout and alignment of one
// codec cannot shift the numbers of another. See PLAN.md §5.2 for the metrics.

#include "compare.h"
#include "harness.h"
#include "zsmalloc_cost.h"

#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#if !defined(QUETSCHN_CODEC) && !defined(QUETSCHN_INTERLEAVED)
#    error "QUETSCHN_CODEC must name one of the quetschn_codec_* objects, or QUETSCHN_INTERLEAVED be set"
#endif

namespace {

#ifdef QUETSCHN_INTERLEAVED
// All codecs in one binary, for comparisons that must not suffer from drift between separate runs.
// The price: the code layout of one codec can shift another's numbers, which one binary per codec
// avoids. The order of --codecs changes the layout, so run it twice in two orders when it matters.
auto const all_codecs = std::to_array<quetschn_codec const*>({
    &quetschn_codec_lz4,
    &quetschn_codec_lz4hc,
    &quetschn_codec_lzo,
    &quetschn_codec_lzo_rle,
    &quetschn_codec_zstd,
    &quetschn_codec_spike_switch,
    &quetschn_codec_spike_branchless,
    &quetschn_codec_spike_zeroskip,
    &quetschn_codec_spike_slots,
    &quetschn_codec_shuffle_lz4,
    &quetschn_codec_bdelta,
    &quetschn_codec_zstd_nolit,
    &quetschn_codec_seqlz,
    &quetschn_codec_seqlz_hc,
#    ifdef QUETSCHN_HAVE_MEMLZ
    &quetschn_codec_memlz,
#    endif
});
auto const* const program = "quetschn-bench-interleaved";
#else
auto const* const program = QUETSCHN_CODEC.name;
#endif

void usage() {
    std::fprintf(stderr,
#ifdef QUETSCHN_INTERLEAVED
                 "usage: %s --codecs <a[:level],b,...> --corpus <base> [--level <n>] [--dict <file>] [--repetitions <n>]\n"
                 "          [--cpu <n>] [--out <dir>]\n"
                 "\n"
                 "Runs all codecs on every page, with the timing interleaved: each repetition runs each codec\n"
                 "once, starting with another codec each time. --out writes <dir>/<codec>.tsv, or\n"
                 "<dir>/<codec>-level<n>.tsv for a codec with its own level, e.g. zstd:-1.\n"
#else
                 "usage: %s --corpus <base> [--level <n>] [--dict <file>] [--repetitions <n>] [--cpu <n>]\n"
                 "          [--out <file.tsv>]\n"
                 "\n"
                 "--out writes one line per page, for quetschn-compare.\n"
#endif
                 "Reads <base>.pages and <base>.tsv as written by quetschn-collect-resident.\n"
                 "--level is zram's algorithm_params level, default: zram's default for the codec.\n"
                 "--dict is zram's algorithm_params dict: a dictionary file, e.g. from zstd --train.\n"
                 "--cpu pins the process to one CPU; set a fixed frequency yourself.\n"
                 "--no-timing only compresses and checks the roundtrip: sizes and zsmalloc cost, fast.\n"
                 "--decode-loop <n> decodes every page n times and nothing else, for perf.\n",
                 program);
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

// For perf: compresses every page once, then decompresses all of them loops times and nothing else,
// so that a profile or perf stat shows only the decoder. No timing, no output but a checksum.
int decode_loop(quetschn::corpus const& c, quetschn_codec const& codec, quetschn::run_options const& opts, unsigned loops) {
    auto params = quetschn_params{};
    params.dict = opts.dict.empty() ? nullptr : opts.dict.data();
    params.dict_size = opts.dict.size();
    params.level = opts.levels.empty() ? opts.level : opts.levels.front();
    params.page_size = static_cast<unsigned int>(c.page_size);
    auto stream = quetschn_stream{};
    if (codec.setup_params(&params) != 0 || codec.create(&params, &stream) != 0) {
        std::fprintf(stderr, "error: %s: setup failed\n", codec.name);
        return 1;
    }
    auto compressed = std::vector<std::vector<std::byte>>();
    auto buf = std::vector<std::byte>(2 * c.page_size);
    for (std::size_t i = 0; i < c.size(); ++i) {
        auto len = static_cast<unsigned int>(buf.size());
        if (codec.compress(&params, &stream, c.page(i).data(), static_cast<unsigned int>(c.page_size), buf.data(), &len) !=
            0) {
            std::fprintf(stderr, "error: %s: compress failed\n", codec.name);
            return 1;
        }
        compressed.emplace_back(buf.begin(), buf.begin() + len);
    }
    auto out = std::vector<std::byte>(c.page_size);
    auto sum = std::uint64_t{0};
    for (unsigned l = 0; l < loops; ++l) {
        for (auto const& p : compressed) {
            auto len = static_cast<unsigned int>(out.size());
            if (codec.decompress(&params, &stream, p.data(), static_cast<unsigned int>(p.size()), out.data(), &len) != 0) {
                std::fprintf(stderr, "error: %s: decompress failed\n", codec.name);
                return 1;
            }
            sum += static_cast<std::uint64_t>(out[len / 3]);
        }
    }
    codec.destroy(&stream);
    codec.release_params(&params);
    std::printf("%s: %zu pages decoded %u times, checksum %llu\n",
                codec.name,
                compressed.size(),
                loops,
                static_cast<unsigned long long>(sum));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    auto out_path = std::string();
    auto dict_path = std::string();
    auto opts = quetschn::run_options{};
    int cpu = -1;
    unsigned decode_loops = 0;
    auto codecs = std::vector<quetschn_codec const*>();
    auto codec_levels = std::vector<int>();
#ifndef QUETSCHN_INTERLEAVED
    codecs.push_back(&QUETSCHN_CODEC);
#endif
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
#ifdef QUETSCHN_INTERLEAVED
        if (arg == "--codecs" && has_value) {
            auto names = std::string_view(argv[++i]);
            while (!names.empty()) {
                auto const spec = names.substr(0, names.find(','));
                names.remove_prefix(std::min(names.size(), spec.size() + 1));
                // name or name:level
                auto const name = spec.substr(0, spec.find(':'));
                auto level = QUETSCHN_LEVEL_DEFAULT;
                if (name.size() < spec.size() && !parse(spec.substr(name.size() + 1), level)) {
                    usage();
                    return 2;
                }
                codec_levels.push_back(level);
                auto const it = std::find_if(all_codecs.begin(), all_codecs.end(), [&](auto const* codec) {
                    return name == codec->name;
                });
                if (it == all_codecs.end()) {
                    std::fprintf(stderr, "error: unknown codec '%.*s'\n", static_cast<int>(name.size()), name.data());
                    return 2;
                }
                codecs.push_back(*it);
            }
            continue;
        }
#endif
        if (arg == "--corpus" && has_value) {
            base = argv[++i];
        } else if (arg == "--out" && has_value) {
            out_path = argv[++i];
        } else if (arg == "--decode-loop" && has_value && parse(argv[++i], decode_loops) && decode_loops > 0) {
        } else if (arg == "--no-timing") {
            opts.measure_time = false;
        } else if (arg == "--repetitions" && has_value && parse(argv[++i], opts.repetitions) && opts.repetitions > 0) {
        } else if (arg == "--cpu" && has_value && parse(argv[++i], cpu)) {
        } else if (arg == "--level" && has_value && parse(argv[++i], opts.level)) {
        } else if (arg == "--dict" && has_value) {
            dict_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (base.empty() || codecs.empty()) {
        usage();
        return 2;
    }
    // a codec without :level gets --level
    if (std::any_of(codec_levels.begin(), codec_levels.end(), [](int l) {
            return l != QUETSCHN_LEVEL_DEFAULT;
        })) {
        for (auto& l : codec_levels) {
            l = l == QUETSCHN_LEVEL_DEFAULT ? opts.level : l;
        }
        opts.levels = codec_levels;
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
        if (!dict_path.empty()) {
            auto in = std::ifstream(dict_path, std::ios::binary);
            auto const raw = std::string(std::istreambuf_iterator<char>(in), {});
            if ((!in && !in.eof()) || raw.empty()) {
                // zram refuses an empty dictionary file as well
                std::fprintf(stderr, "error: cannot read %s, or it is empty\n", dict_path.c_str());
                return 1;
            }
            opts.dict.resize(raw.size());
            std::memcpy(opts.dict.data(), raw.data(), raw.size());
        }
        auto const model = quetschn::zsmalloc_model(quetschn::zsmalloc_config{.page_size = c.page_size});
        if (decode_loops > 0) {
            return decode_loop(c, *codecs.front(), opts, decode_loops);
        }

        auto const governor_cpu = cpu >= 0 ? cpu : ::sched_getcpu();
        std::printf("corpus     %s, %zu pages of %zu bytes\n", base.c_str(), c.size(), c.page_size);
        auto const cpufreq = "/sys/devices/system/cpu/cpu" + std::to_string(governor_cpu) + "/cpufreq/";
        std::printf("cpu        %s, pinned: %s, governor: %s\n",
                    cpu_model().c_str(),
                    cpu >= 0 ? std::to_string(cpu).c_str() : "no",
                    first_line(cpufreq + "scaling_governor").c_str());
        // Cold latencies depend on the clock, see docs/explored-designs.md. Fixed means min == max and
        // boost off.
        std::printf("frequency  %s to %s kHz, boost: %s\n",
                    first_line(cpufreq + "scaling_min_freq").c_str(),
                    first_line(cpufreq + "scaling_max_freq").c_str(),
                    first_line("/sys/devices/system/cpu/cpufreq/boost").c_str());
        if (opts.measure_time) {
            std::printf("method     median of %u runs per page, percentiles across pages, ns\n", opts.repetitions);
        } else {
            std::printf("method     no timing, sizes only\n");
        }

        auto const results = quetschn::run_interleaved(c, codecs, model, opts);
        for (std::size_t k = 0; k < codecs.size(); ++k) {
            auto const* codec = codecs[k];
            auto const& r = results[k];
            auto const s = quetschn::summarize(r, c.page_size);
            std::printf("\n");
            if (r.level == QUETSCHN_LEVEL_DEFAULT) {
                std::printf("codec      %s, no level (zram ignores it for this codec)\n", codec->name);
            } else {
                std::printf("codec      %s, level %d\n", codec->name, r.level);
            }
            std::printf("pages                  %zu measured, %zu same-filled skipped\n", s.pages, s.same_filled);
            std::printf("zsmalloc cost          %.0f bytes, %.1f%% of uncompressed, %.1f bytes/page\n",
                        s.total_cost,
                        s.pages == 0 ? 0.0 : 100.0 * s.total_cost / s.total_uncompressed,
                        s.pages == 0 ? 0.0 : s.total_cost / static_cast<double>(s.pages));
            std::printf("stored uncompressed    %zu pages (comp_len >= %zu)\n", s.huge, model.huge_class_size());
            std::printf("memory per CPU         %zu bytes\n", r.stream_bytes);
            std::printf("memory per device      %zu bytes (dictionary %s, %zu bytes)\n",
                        r.params_bytes,
                        dict_path.empty() ? "none" : dict_path.c_str(),
                        opts.dict.size());
            if (opts.measure_time) {
                std::printf("\n%-22s %9s %9s %9s %9s %9s\n", "latency ns", "p50", "p90", "p99", "p99.9", "max");
                print_latency("compress", s.compress);
                print_latency("decompress warm", s.decompress);
                print_latency("decompress cold", s.decompress_cold);
            }

            if (!out_path.empty()) {
#ifdef QUETSCHN_INTERLEAVED
                auto const path = out_path + "/" + codec->name +
                                  (opts.levels.empty() || opts.levels[k] == QUETSCHN_LEVEL_DEFAULT
                                       ? std::string()
                                       : "-level" + std::to_string(opts.levels[k])) +
                                  ".tsv";
#else
                auto const& path = out_path;
#endif
                auto out = std::ofstream(path);
                quetschn::write_page_results(out, r.pages);
                if (!out) {
                    std::fprintf(stderr, "error: cannot write %s\n", path.c_str());
                    return 1;
                }
            }
        }
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
