// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "harness.h"

#include "page_stats.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#if defined(__x86_64__)
#    include <x86intrin.h>
#endif

namespace quetschn {

namespace {

constexpr std::size_t cache_line = 64;

// Pages are checked and then timed in blocks of this many. Each repetition goes through the whole block,
// so before a codec runs on a page again it has run on the others: timing a page right after the same
// code ran on it let the branch predictor learn that page's branches, which a page fault does not get
// (docs/explored-designs.md). 16 pages of 2 * 2 pages of room each are 1 MiB per codec.
constexpr std::size_t block_pages = 16;

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

// every cache line of [p, p + size) read once
void touch(void const* p, std::size_t size) {
    auto const* b = static_cast<unsigned char const*>(p);
    auto sum = 0U;
    for (std::size_t off = 0; off < size; off += cache_line) {
        sum += *static_cast<unsigned char const volatile*>(b + off);
    }
    if (size > 0) {
        sum += *static_cast<unsigned char const volatile*>(b + size - 1);
    }
    static_cast<void>(sum);
}

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

    // one read straight into the buffer; istreambuf_iterator took a minute for a 1.9 GB corpus
    auto in = std::ifstream(pages_path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("load_corpus: cannot read " + pages_path.string());
    }
    auto const size = static_cast<std::size_t>(in.tellg());
    if (c.page_size == 0 || size % c.page_size != 0) {
        throw std::runtime_error("load_corpus: " + pages_path.string() + " is not a whole number of pages");
    }
    c.data.resize(size);
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(c.data.data()), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("load_corpus: cannot read " + pages_path.string());
    }
    return c;
}

run_result run_codec(corpus const& c, quetschn_codec const& codec, zsmalloc_model const& model, run_options const& opts) {
    auto const codecs = std::array<quetschn_codec const*, 1>{&codec};
    return std::move(run_interleaved(c, codecs, model, opts)[0]);
}

namespace {

// One codec as zram sets it up: setup_params once per device, create once per CPU. Both are released
// again in the destructor, also when a run fails halfway.
class codec_instance {
public:
    codec_instance(quetschn_codec const& codec, std::size_t page_size, run_options const& opts)
        : m_codec(codec)
        , m_slot_size(4 * page_size)
        , m_compressed(block_pages * 4 * page_size + page_size)
        , m_restored(2 * page_size) {
        m_params.dict = opts.dict.empty() ? nullptr : opts.dict.data();
        m_params.dict_size = opts.dict.size();
        m_params.level = opts.level;
        m_params.page_size = static_cast<unsigned int>(page_size);
        if (codec.setup_params(&m_params) != 0) {
            throw std::invalid_argument(std::string(codec.name) + ": zram rejects these parameters (level " +
                                        std::to_string(opts.level) + ", dictionary of " + std::to_string(opts.dict.size()) +
                                        " bytes)");
        }
        m_have_params = true;
        if (codec.create(&m_params, &m_stream) != 0) {
            // the destructor does not run for an object whose constructor throws
            codec.release_params(&m_params);
            throw std::runtime_error(std::string(codec.name) + ": create failed");
        }
        m_have_stream = true;
        // Separate allocations. The output is page aligned like the page zram decompresses into. The
        // compressed data moves, see set_page.
        m_compressed_base = align(m_compressed, page_size);
        dst = m_compressed_base;
        out = align(m_restored, page_size);
    }

    ~codec_instance() {
        if (m_have_stream) {
            m_codec.destroy(&m_stream);
        }
        if (m_have_params) {
            m_codec.release_params(&m_params);
        }
    }

    codec_instance(codec_instance const&) = delete;
    codec_instance& operator=(codec_instance const&) = delete;

    [[nodiscard]] quetschn_codec const& codec() const {
        return m_codec;
    }

    // zsmalloc objects start at many different offsets within a page, and the offset changes which
    // loads and stores alias in the low 12 address bits. So the compressed data of page i starts at an
    // offset that depends on i, a multiple of 16 like zsmalloc's size classes, and the same for all
    // codecs: every codec sees the same mix of offsets, and every run the same.
    // Each page of a block has its own slot, so that its compressed data stays until it is timed.
    void set_page(std::size_t slot, std::size_t i) {
        dst = m_compressed_base + slot * m_slot_size + (i * 2704U) % 4096U / 16U * 16U;
    }

    int compress(std::span<std::byte const> src, unsigned int& len) {
        len = static_cast<unsigned int>(2 * src.size());
        return m_codec.compress(&m_params, &m_stream, src.data(), static_cast<unsigned int>(src.size()), dst, &len);
    }

    int decompress(unsigned int comp_len, unsigned int& len) {
        return m_codec.decompress(&m_params, &m_stream, dst, comp_len, out, &len);
    }

    [[nodiscard]] int level() const {
        return m_params.level;
    }
    [[nodiscard]] std::size_t params_bytes() const {
        return m_params.allocated;
    }
    [[nodiscard]] std::size_t stream_bytes() const {
        return m_stream.allocated;
    }

    std::byte* dst = nullptr;
    std::byte* out = nullptr;

private:
    static std::byte* align(std::vector<std::byte>& v, std::size_t alignment) {
        auto p = reinterpret_cast<std::uintptr_t>(v.data());
        return v.data() + ((alignment - p % alignment) % alignment);
    }

    quetschn_codec const& m_codec;
    std::size_t m_slot_size;
    quetschn_params m_params{};
    quetschn_stream m_stream{};
    bool m_have_params = false;
    bool m_have_stream = false;
    std::vector<std::byte> m_compressed;
    std::byte* m_compressed_base = nullptr;
    std::vector<std::byte> m_restored;
};

} // namespace

std::vector<run_result> run_interleaved(corpus const& c,
                                        std::span<quetschn_codec const* const> codecs,
                                        zsmalloc_model const& model,
                                        run_options const& opts) {
    auto const page_size = c.page_size;
    if (page_size != model.config().page_size) {
        throw std::invalid_argument("run_codec: corpus and zsmalloc model have different page sizes");
    }
    auto const tick_ns = ns_per_tick();

    if (!opts.levels.empty() && opts.levels.size() != codecs.size()) {
        throw std::invalid_argument("run_interleaved: " + std::to_string(opts.levels.size()) + " levels for " +
                                    std::to_string(codecs.size()) + " codecs");
    }
    auto instances = std::vector<std::unique_ptr<codec_instance>>();
    for (std::size_t k = 0; k < codecs.size(); ++k) {
        auto codec_opts = opts;
        if (!opts.levels.empty()) {
            codec_opts.level = opts.levels[k];
        }
        instances.push_back(std::make_unique<codec_instance>(*codecs[k], page_size, codec_opts));
    }
    auto results = std::vector<run_result>(codecs.size());
    for (std::size_t k = 0; k < codecs.size(); ++k) {
        results[k].level = instances[k]->level();
    }

    auto const n = codecs.size();
    auto const reps = opts.measure_time ? opts.repetitions : 0U;
    // per page of the block, per codec: compress, decompress warm and cold, one value per repetition
    auto samples = std::vector<std::vector<std::array<std::vector<double>, 3>>>(
        block_pages, std::vector<std::array<std::vector<double>, 3>>(n));
    for (auto& page : samples) {
        for (auto& s : page) {
            for (auto& v : s) {
                v.resize(reps);
            }
        }
    }
    auto median = [](std::vector<double>& v) {
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return v[v.size() / 2];
    };

    // the block's pages that zram compresses, and their results per codec
    auto todo = std::vector<std::size_t>();
    auto pages = std::vector<std::vector<page_result>>(block_pages, std::vector<page_result>(n));
    for (std::size_t start = 0; start < c.size(); start += block_pages) {
        auto const end = std::min(c.size(), start + block_pages);
        todo.clear();
        for (std::size_t i = start; i < end; ++i) {
            auto const src = c.page(i);
            if (analyze_page(src).same_filled) {
                for (auto& r : results) {
                    ++r.same_filled;
                }
                continue;
            }
            auto const slot = todo.size();
            todo.push_back(i);
            for (std::size_t k = 0; k < n; ++k) {
                auto& inst = *instances[k];
                inst.set_page(slot, i);
                auto& r = pages[slot][k];
                r = page_result{};
                r.page = i;
                if (inst.compress(src, r.comp_len) != 0) {
                    fail(inst.codec(), i, "compress failed");
                }
                r.huge = r.comp_len >= model.huge_class_size();
                r.cost = model.cost(r.comp_len);

                // Roundtrip check, also for pages that zram would store raw: the codec must still be correct.
                auto out_len = static_cast<unsigned int>(page_size);
                if (inst.decompress(r.comp_len, out_len) != 0) {
                    fail(inst.codec(), i, "decompress failed");
                }
                if (out_len != page_size || std::memcmp(inst.out, src.data(), page_size) != 0) {
                    fail(inst.codec(), i, "roundtrip does not reproduce the page");
                }
            }
        }

        // Every repetition runs every codec once per page, starting with another codec each time, so
        // that a change of CPU frequency or temperature during the run hits all codecs alike. First the
        // compressions of the block, then the warm and then the cold decompressions, each pass over all
        // pages, so that no timed run comes right after a run on the same page. A compressor with a
        // large workspace, like lz4hc's 256 KiB, evicts what a cold decompression would otherwise still
        // find in the cache, but the cold pass flushes the data and follows other decompressions only.
        for (unsigned rep = 0; rep < reps; ++rep) {
            for (std::size_t slot = 0; slot < todo.size(); ++slot) {
                auto const i = todo[slot];
                for (std::size_t o = 0; o < n; ++o) {
                    auto const k = (o + rep) % n;
                    instances[k]->set_page(slot, i);
                    auto const t0 = ticks();
                    auto len = 0U;
                    (void)instances[k]->compress(c.page(i), len);
                    samples[slot][k][0][rep] = static_cast<double>(ticks() - t0) * tick_ns;
                }
            }
            for (auto const cold : {false, true}) {
                for (std::size_t slot = 0; slot < todo.size(); ++slot) {
                    auto const i = todo[slot];
                    auto const src = c.page(i);
                    for (std::size_t o = 0; o < n; ++o) {
                        auto const k = (o + rep) % n;
                        auto& inst = *instances[k];
                        auto const& r = pages[slot][k];
                        inst.set_page(slot, i);
                        auto const* data = r.huge ? static_cast<void const*>(src.data()) : inst.dst;
                        auto const data_len = r.huge ? page_size : r.comp_len;
                        if (cold) {
                            flush(data, data_len);
                            flush(inst.out, page_size);
                        } else {
                            // the data read and the output written, without decoding
                            touch(data, data_len);
                            std::memset(inst.out, 0, page_size);
                        }
                        // What zram_read_from_zspool() does: memcpy for pages stored raw, decompress otherwise
                        auto const t0 = ticks();
                        if (r.huge) {
                            std::memcpy(inst.out, src.data(), page_size);
                        } else {
                            auto out_len = static_cast<unsigned int>(page_size);
                            (void)inst.decompress(r.comp_len, out_len);
                        }
                        samples[slot][k][cold ? 2 : 1][rep] = static_cast<double>(ticks() - t0) * tick_ns;
                    }
                }
            }
        }
        for (std::size_t slot = 0; slot < todo.size(); ++slot) {
            for (std::size_t k = 0; k < n; ++k) {
                auto& r = pages[slot][k];
                if (reps > 0) {
                    r.compress_ns = median(samples[slot][k][0]);
                    r.decompress_ns = median(samples[slot][k][1]);
                    r.decompress_cold_ns = median(samples[slot][k][2]);
                }
                results[k].pages.push_back(r);
            }
        }
    }
    // zstd allocates some of its per-stream memory lazily during the first compression
    for (std::size_t k = 0; k < n; ++k) {
        results[k].stream_bytes = instances[k]->stream_bytes();
        results[k].params_bytes = instances[k]->params_bytes();
    }
    return results;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        throw std::invalid_argument("percentile: no values");
    }
    std::sort(values.begin(), values.end());
    return values[nearest_rank(p, values.size()) - 1];
}

std::size_t nearest_rank(double p, std::size_t n) {
    // the smallest value with at least p% of all values at or below it. p is not exact in binary, e.g.
    // 99.9 / 100 * 2000 is 1998.0000000000002, so a result that close to an integer is that integer.
    auto const x = p / 100.0 * static_cast<double>(n);
    auto const nearest = std::round(x);
    auto const rank = static_cast<std::size_t>(std::abs(x - nearest) <= 1e-9 * std::max(1.0, x) ? nearest : std::ceil(x));
    return std::clamp<std::size_t>(rank, 1, n);
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
