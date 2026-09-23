// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "harness.h"

#include "page_stats.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#if defined(__x86_64__)
#    include <x86intrin.h>
#endif

namespace quetschn {

namespace {

constexpr std::size_t cache_line = 64;

#if defined(__x86_64__)

// TSC ticks, with lfence so that the read is not moved across the measured code. Converted to ns with
// a factor measured once against steady_clock.
std::uint64_t ticks() {
    _mm_lfence();
    auto t = __rdtsc();
    _mm_lfence();
    return t;
}

void flush(void const* p, std::size_t size) {
    auto const* b = static_cast<char const*>(p);
    for (std::size_t off = 0; off < size; off += cache_line) {
        _mm_clflush(b + off);
    }
    _mm_mfence();
}

#else

// TODO(arm64): the PMU cycle counter via perf_event_open, see PLAN.md §5.2. cntvct_el0 runs at only
// 19 to 54 MHz on many boards, which is too coarse for a single page.
std::uint64_t ticks() {
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

void flush(void const* p, std::size_t size) {
#    if defined(__aarch64__)
    auto const* b = static_cast<char const*>(p);
    for (std::size_t off = 0; off < size; off += cache_line) {
        asm volatile("dc civac, %0" ::"r"(b + off) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
#    else
    (void)p;
    (void)size;
#    endif
}

#endif

double ns_per_tick() {
    static double const factor = [] {
        using clock = std::chrono::steady_clock;
        auto const t0 = clock::now();
        auto const c0 = ticks();
        while (clock::now() - t0 < std::chrono::milliseconds(50)) {
        }
        auto const c1 = ticks();
        auto const ns = std::chrono::duration<double, std::nano>(clock::now() - t0).count();
        return ns / static_cast<double>(c1 - c0);
    }();
    return factor;
}

template <typename F>
double median_ns(unsigned repetitions, F&& op) {
    auto samples = std::vector<double>();
    samples.reserve(repetitions);
    for (unsigned r = 0; r < repetitions; ++r) {
        samples.push_back(op());
    }
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2), samples.end());
    return samples[samples.size() / 2];
}

[[noreturn]] void fail(quetschn_codec const& codec, std::size_t page, char const* what) {
    throw std::runtime_error(std::string(codec.name) + ": page " + std::to_string(page) + ": " + what);
}

} // namespace

std::size_t corpus::size() const {
    return page_size == 0 ? 0 : data.size() / page_size;
}

std::span<std::byte const> corpus::page(std::size_t i) const {
    return std::span<std::byte const>(data).subspan(i * page_size, page_size);
}

corpus load_corpus(std::filesystem::path const& base) {
    auto tsv_path = base;
    tsv_path += ".tsv";
    auto pages_path = base;
    pages_path += ".pages";

    auto c = corpus{};
    auto tsv = std::ifstream(tsv_path);
    auto line = std::string();
    constexpr auto prefix = std::string_view("# page_size=");
    if (!std::getline(tsv, line) || !line.starts_with(prefix)) {
        throw std::runtime_error("load_corpus: " + tsv_path.string() + " does not start with '# page_size='");
    }
    c.page_size = std::stoul(line.substr(prefix.size()));

    auto in = std::ifstream(pages_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("load_corpus: cannot read " + pages_path.string());
    }
    auto const raw = std::string(std::istreambuf_iterator<char>(in), {});
    if (c.page_size == 0 || raw.size() % c.page_size != 0) {
        throw std::runtime_error("load_corpus: " + pages_path.string() + " is not a whole number of pages");
    }
    c.data.resize(raw.size());
    std::memcpy(c.data.data(), raw.data(), raw.size());
    return c;
}

run_result run_codec(corpus const& c, quetschn_codec const& codec, zsmalloc_model const& model, run_options const& opts) {
    auto const page_size = c.page_size;
    if (page_size != model.config().page_size) {
        throw std::invalid_argument("run_codec: corpus and zsmalloc model have different page sizes");
    }
    auto const tick_ns = ns_per_tick();

    auto result = run_result{};
    result.level = opts.level;
    result.workspace_size = codec.workspace_size(&result.level, static_cast<unsigned int>(page_size));
    if (result.workspace_size == 0) {
        throw std::invalid_argument(std::string(codec.name) + ": zram rejects level " + std::to_string(opts.level));
    }

    // Separate allocations, aligned to cache lines like the kernel's page-sized buffers. The workspace is
    // zeroed, like zram's vzalloc.
    auto workspace = std::vector<std::byte>(result.workspace_size + cache_line);
    auto compressed = std::vector<std::byte>(2 * page_size + cache_line);
    auto restored = std::vector<std::byte>(page_size + cache_line);
    auto align = [](std::vector<std::byte>& v) {
        auto p = reinterpret_cast<std::uintptr_t>(v.data());
        return v.data() + ((cache_line - p % cache_line) % cache_line);
    };
    auto* dst = align(compressed);
    auto* out = align(restored);

    // One stream for the whole run, like one zram stream per CPU
    auto stream = quetschn_stream{};
    stream.level = result.level;
    stream.workspace = align(workspace);
    stream.workspace_size = result.workspace_size;
    if (codec.init(&stream, static_cast<unsigned int>(page_size)) != 0) {
        throw std::runtime_error(std::string(codec.name) + ": init failed");
    }

    for (std::size_t i = 0; i < c.size(); ++i) {
        auto const src = c.page(i);
        if (analyze_page(src).same_filled) {
            ++result.same_filled;
            continue;
        }

        auto compress_once = [&]() -> unsigned int {
            auto len = static_cast<unsigned int>(2 * page_size);
            if (codec.compress(&stream, src.data(), static_cast<unsigned int>(page_size), dst, &len) != 0) {
                fail(codec, i, "compress failed");
            }
            return len;
        };

        auto r = page_result{};
        r.page = i;
        r.comp_len = compress_once();
        r.huge = r.comp_len >= model.huge_class_size();
        r.cost = model.cost(r.comp_len);

        // Roundtrip check, also for pages that zram would store raw: the codec must still be correct.
        auto out_len = static_cast<unsigned int>(page_size);
        if (codec.decompress(&stream, dst, r.comp_len, out, &out_len) != 0) {
            fail(codec, i, "decompress failed");
        }
        if (out_len != page_size || std::memcmp(out, src.data(), page_size) != 0) {
            fail(codec, i, "roundtrip does not reproduce the page");
        }

        if (opts.measure_time) {
            r.compress_ns = median_ns(opts.repetitions, [&] {
                auto const t0 = ticks();
                (void)compress_once();
                return static_cast<double>(ticks() - t0) * tick_ns;
            });

            // What zram_read_from_zspool() does: memcpy for pages stored raw, decompress otherwise
            auto read_once = [&] {
                if (r.huge) {
                    std::memcpy(out, src.data(), page_size);
                    return;
                }
                auto len = static_cast<unsigned int>(page_size);
                (void)codec.decompress(&stream, dst, r.comp_len, out, &len);
            };
            auto const* read_src = r.huge ? static_cast<void const*>(src.data()) : dst;
            auto const read_len = r.huge ? page_size : r.comp_len;
            r.decompress_ns = median_ns(opts.repetitions, [&] {
                auto const t0 = ticks();
                read_once();
                return static_cast<double>(ticks() - t0) * tick_ns;
            });
            r.decompress_cold_ns = median_ns(opts.repetitions, [&] {
                flush(read_src, read_len);
                flush(out, page_size);
                auto const t0 = ticks();
                read_once();
                return static_cast<double>(ticks() - t0) * tick_ns;
            });
        }
        result.pages.push_back(r);
    }
    return result;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        throw std::invalid_argument("percentile: no values");
    }
    std::sort(values.begin(), values.end());
    // nearest rank: the smallest value with at least p% of all values at or below it
    auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(values.size())));
    rank = std::clamp<std::size_t>(rank, 1, values.size());
    return values[rank - 1];
}

latency_summary summarize_latency(std::vector<double> const& values) {
    if (values.empty()) {
        return {};
    }
    return {percentile(values, 50),
            percentile(values, 90),
            percentile(values, 99),
            percentile(values, 99.9),
            percentile(values, 100)};
}

run_summary summarize(run_result const& r, std::size_t page_size) {
    auto s = run_summary{};
    s.pages = r.pages.size();
    s.same_filled = r.same_filled;
    auto compress = std::vector<double>();
    auto decompress = std::vector<double>();
    auto decompress_cold = std::vector<double>();
    for (auto const& p : r.pages) {
        s.huge += p.huge ? 1U : 0U;
        s.total_cost += p.cost;
        compress.push_back(p.compress_ns);
        decompress.push_back(p.decompress_ns);
        decompress_cold.push_back(p.decompress_cold_ns);
    }
    s.total_uncompressed = static_cast<double>(s.pages * page_size);
    s.compress = summarize_latency(compress);
    s.decompress = summarize_latency(decompress);
    s.decompress_cold = summarize_latency(decompress_cold);
    return s;
}

} // namespace quetschn
