// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "harness.h"
#include "resident.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using quetschn::corpus;
using quetschn::percentile;
using quetschn::run_codec;
using quetschn::run_options;
using quetschn::summarize_latency;
using quetschn::zsmalloc_model;

namespace {

constexpr std::size_t page_size = 4096;

// A trivial codec for testing the harness: 2 bytes length k, then the first k bytes of the page, where
// k is the position after the last non-zero byte. Decompression fills the rest with zeros. So the
// compressed length of a page is known exactly: k + 2.
unsigned int trimmed_length(void const* src, unsigned int len) {
    auto const* p = static_cast<unsigned char const*>(src);
    while (len > 0 && p[len - 1] == 0) {
        --len;
    }
    return len;
}

// How often each part of the lifecycle ran, so the tests can check that the harness releases what it set up.
struct lifecycle {
    int setup = 0;
    int release = 0;
    int create = 0;
    int destroy = 0;
};
lifecycle calls;

// Accepts levels 0 to 9, default 5. Pretends to hold 100 bytes per level per CPU, and the dictionary once
// per device, so the tests can see the harness report both.
int trim_setup_params(quetschn_params* p) {
    ++calls.setup;
    if (p->level == QUETSCHN_LEVEL_DEFAULT) {
        p->level = 5;
    }
    if (p->level < 0 || p->level > 9) {
        return -1;
    }
    p->allocated = p->dict_size;
    return 0;
}

void trim_release_params(quetschn_params* p) {
    ++calls.release;
    p->allocated = 0;
}

// The stream remembers the level it was created for, so compress can check it got the same params.
int trim_create(quetschn_params* p, quetschn_stream* s) {
    ++calls.create;
    s->allocated = 100 * static_cast<std::size_t>(p->level + 1);
    s->context = &calls;
    return 0;
}

int failing_create(quetschn_params*, quetschn_stream*) {
    ++calls.create;
    return -1;
}

void trim_destroy(quetschn_stream* s) {
    ++calls.destroy;
    s->allocated = 0;
    s->context = nullptr;
}

int trim_compress(
    quetschn_params* p, quetschn_stream* s, void const* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    if (s->context != &calls || s->allocated != 100 * static_cast<std::size_t>(p->level + 1)) {
        return -1;
    }
    auto const k = trimmed_length(src, src_len);
    if (k + 2 > *dst_len) {
        return -1;
    }
    auto* d = static_cast<unsigned char*>(dst);
    d[0] = static_cast<unsigned char>(k & 0xff);
    d[1] = static_cast<unsigned char>(k >> 8);
    std::memcpy(d + 2, src, k);
    *dst_len = k + 2;
    return 0;
}

int trim_decompress(
    quetschn_params*, quetschn_stream*, void const* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    auto const* s = static_cast<unsigned char const*>(src);
    auto const k = static_cast<unsigned int>(s[0] | (s[1] << 8));
    if (src_len != k + 2 || *dst_len < page_size) {
        return -1;
    }
    std::memcpy(dst, s + 2, k);
    std::memset(static_cast<unsigned char*>(dst) + k, 0, page_size - k);
    *dst_len = page_size;
    return 0;
}

int broken_decompress(
    quetschn_params* p, quetschn_stream* s, void const* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    auto ret = trim_decompress(p, s, src, src_len, dst, dst_len);
    static_cast<unsigned char*>(dst)[7] ^= 1;
    return ret;
}

int failing_compress(quetschn_params*, quetschn_stream*, void const*, unsigned int, void*, unsigned int*) {
    return -1;
}

// like trim, with 8 more bytes of zeros at the end, so its sizes differ from trim's
int pad_compress(
    quetschn_params* p, quetschn_stream* s, void const* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    if (trim_compress(p, s, src, src_len, dst, dst_len) != 0 || *dst_len + 8 > 2 * page_size) {
        return -1;
    }
    std::memset(static_cast<unsigned char*>(dst) + *dst_len, 0, 8);
    *dst_len += 8;
    return 0;
}

int pad_decompress(
    quetschn_params* p, quetschn_stream* s, void const* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    return src_len < 8 ? -1 : trim_decompress(p, s, src, src_len - 8, dst, dst_len);
}

quetschn_codec const trim_codec{
    "trim", trim_setup_params, trim_release_params, trim_create, trim_destroy, trim_compress, trim_decompress};
quetschn_codec const pad_codec{
    "pad", trim_setup_params, trim_release_params, trim_create, trim_destroy, pad_compress, pad_decompress};
quetschn_codec const broken_codec{
    "broken", trim_setup_params, trim_release_params, trim_create, trim_destroy, trim_compress, broken_decompress};
quetschn_codec const failing_codec{
    "failing", trim_setup_params, trim_release_params, trim_create, trim_destroy, failing_compress, trim_decompress};
quetschn_codec const failing_create_codec{
    "failing-create", trim_setup_params, trim_release_params, failing_create, trim_destroy, trim_compress, trim_decompress};

// page with the first `nonzero` bytes set to non-zero values, the rest zero
std::vector<std::byte> page_with_prefix(std::size_t nonzero) {
    auto p = std::vector<std::byte>(page_size);
    for (std::size_t i = 0; i < nonzero; ++i) {
        p[i] = static_cast<std::byte>(i % 200 + 1);
    }
    return p;
}

// run_options without timing, and optionally a level and a dictionary
run_options untimed(int level = QUETSCHN_LEVEL_DEFAULT, std::vector<std::byte> dict = {}) {
    auto o = run_options{};
    o.measure_time = false;
    o.level = level;
    o.dict = std::move(dict);
    return o;
}

corpus make_corpus(std::vector<std::vector<std::byte>> const& pages) {
    auto c = corpus{};
    c.page_size = page_size;
    for (auto const& p : pages) {
        c.data.insert(c.data.end(), p.begin(), p.end());
    }
    return c;
}

} // namespace

TEST_CASE("harness: sizes, costs and the uncompressed-page threshold come from the codec output") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({
        page_with_prefix(100),                              // comp_len 102
        std::vector<std::byte>(page_size, std::byte{0x42}), // same-filled, skipped
        page_with_prefix(page_size),                        // comp_len 4098: stored uncompressed
        page_with_prefix(3000),                             // comp_len 3002
        page_with_prefix(model.huge_class_size() - 2),      // exactly at the threshold
    });
    auto const r = run_codec(c, trim_codec, model, untimed());

    CHECK(r.same_filled == 1);
    REQUIRE(r.pages.size() == 4);
    CHECK(r.pages[0].page == 0);
    CHECK(r.pages[1].page == 2);
    CHECK(r.pages[2].page == 3);
    CHECK(r.pages[3].page == 4);

    CHECK(r.pages[0].comp_len == 102);
    CHECK(r.pages[1].comp_len == 4098);
    CHECK(r.pages[2].comp_len == 3002);
    CHECK(r.pages[3].comp_len == model.huge_class_size());

    CHECK_FALSE(r.pages[0].huge);
    CHECK(r.pages[1].huge);
    CHECK_FALSE(r.pages[2].huge);
    CHECK(r.pages[3].huge);

    // 102 + 8 handle = 110 -> 112 byte class, 7 pages per zspage hold 256 of them exactly
    CHECK(r.pages[0].cost == doctest::Approx(112.0));
    CHECK(r.pages[1].cost == doctest::Approx(4096.0));
    CHECK(r.pages[3].cost == doctest::Approx(4096.0));

    auto const s = quetschn::summarize(r, page_size);
    CHECK(s.pages == 4);
    CHECK(s.same_filled == 1);
    CHECK(s.huge == 2);
    CHECK(s.total_cost == doctest::Approx(112.0 + 4096.0 + model.cost(3002) + 4096.0));
    CHECK(s.total_uncompressed == doctest::Approx(4.0 * page_size));
}

TEST_CASE("harness: a codec that does not reproduce the page is an error, not a number") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({page_with_prefix(100)});
    CHECK_THROWS_WITH_AS(
        (void)run_codec(c, broken_codec, model, untimed()), doctest::Contains("roundtrip"), std::runtime_error);
    CHECK_THROWS_WITH_AS(
        (void)run_codec(c, failing_codec, model, untimed()), doctest::Contains("compress failed"), std::runtime_error);
}

TEST_CASE("harness: level and dictionary reach the codec, and its memory is reported") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({page_with_prefix(100)});

    auto const def = run_codec(c, trim_codec, model, untimed());
    CHECK(def.level == 5);
    CHECK(def.stream_bytes == 600);
    CHECK(def.params_bytes == 0);

    auto const l2 = run_codec(c, trim_codec, model, untimed(2, std::vector<std::byte>(1000)));
    CHECK(l2.level == 2);
    CHECK(l2.stream_bytes == 300);
    CHECK(l2.params_bytes == 1000);
    CHECK(l2.pages.size() == 1);

    CHECK_THROWS_WITH_AS(
        (void)run_codec(c, trim_codec, model, untimed(10)), doctest::Contains("zram rejects"), std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        (void)run_codec(c, failing_create_codec, model, untimed()), doctest::Contains("create failed"), std::runtime_error);
}

TEST_CASE("harness: what was set up is released, also when the run fails") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({page_with_prefix(100)});
    for (auto const* codec : {&trim_codec, &broken_codec, &failing_codec, &failing_create_codec}) {
        CAPTURE(codec->name);
        calls = lifecycle{};
        try {
            (void)run_codec(c, *codec, model, untimed());
        } catch (std::exception const&) {
        }
        CHECK(calls.setup == 1);
        CHECK(calls.release == 1);
        CHECK(calls.create == 1);
        // a stream that could not be created is not destroyed, like in zram
        CHECK(calls.destroy == (codec == &failing_create_codec ? 0 : 1));
    }
    // and nothing is created when the parameters are rejected
    calls = lifecycle{};
    CHECK_THROWS((void)run_codec(c, trim_codec, model, untimed(10)));
    CHECK(calls.create == 0);
}

TEST_CASE("harness: interleaved runs give every codec the result of its own run") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({
        page_with_prefix(100),
        std::vector<std::byte>(page_size, std::byte{0x42}),
        page_with_prefix(page_size),
        page_with_prefix(3000),
    });
    auto const codecs = std::array<quetschn_codec const*, 2>{&trim_codec, &pad_codec};
    auto const both = quetschn::run_interleaved(c, codecs, model, untimed());
    REQUIRE(both.size() == 2);
    CHECK(both[0].pages[0].comp_len == 102);
    CHECK(both[1].pages[0].comp_len == 110);
    for (std::size_t k = 0; k < 2; ++k) {
        CAPTURE(k);
        auto const& r = both[k];
        auto const alone = run_codec(c, *codecs[k], model, untimed());
        CHECK(r.same_filled == alone.same_filled);
        REQUIRE(r.pages.size() == alone.pages.size());
        for (std::size_t i = 0; i < r.pages.size(); ++i) {
            CAPTURE(i);
            CHECK(r.pages[i].page == alone.pages[i].page);
            CHECK(r.pages[i].comp_len == alone.pages[i].comp_len);
            CHECK(r.pages[i].huge == alone.pages[i].huge);
            CHECK(r.pages[i].cost == alone.pages[i].cost);
        }
    }
}

TEST_CASE("harness: interleaved, each codec can have its own level") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({page_with_prefix(100)});
    auto const codecs = std::array<quetschn_codec const*, 2>{&trim_codec, &trim_codec};
    auto opts = untimed(4);
    opts.levels = {2, 7};
    auto const r = quetschn::run_interleaved(c, codecs, model, opts);
    CHECK(r[0].level == 2);
    CHECK(r[1].level == 7);
    CHECK(r[0].stream_bytes == 300);
    CHECK(r[1].stream_bytes == 800);

    opts.levels = {};
    auto const same = quetschn::run_interleaved(c, codecs, model, opts);
    CHECK(same[0].level == 4);
    CHECK(same[1].level == 4);

    opts.levels = {2};
    CHECK_THROWS_AS((void)quetschn::run_interleaved(c, codecs, model, opts), std::invalid_argument);
}

TEST_CASE("harness: interleaved, every codec is timed on every page") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({page_with_prefix(100), page_with_prefix(page_size), page_with_prefix(2000)});
    auto const codecs = std::array<quetschn_codec const*, 3>{&trim_codec, &trim_codec, &trim_codec};
    auto opts = run_options{};
    opts.repetitions = 2;
    auto const results = quetschn::run_interleaved(c, codecs, model, opts);
    REQUIRE(results.size() == 3);
    for (std::size_t k = 0; k < results.size(); ++k) {
        CAPTURE(k);
        REQUIRE(results[k].pages.size() == 3);
        for (auto const& p : results[k].pages) {
            CAPTURE(p.page);
            CHECK(p.compress_ns > 0.0);
            CHECK(p.decompress_ns > 0.0);
            CHECK(p.decompress_cold_ns > 0.0);
        }
    }
}

TEST_CASE("harness: interleaved, a failing codec releases the others too") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({page_with_prefix(100)});
    for (auto const* second : {&broken_codec, &failing_codec, &failing_create_codec}) {
        CAPTURE(second->name);
        calls = lifecycle{};
        auto const codecs = std::array<quetschn_codec const*, 2>{&trim_codec, second};
        CHECK_THROWS((void)quetschn::run_interleaved(c, codecs, model, untimed()));
        CHECK(calls.setup == 2);
        CHECK(calls.release == 2);
        CHECK(calls.create == 2);
        CHECK(calls.destroy == (second == &failing_create_codec ? 1 : 2));
    }
}

TEST_CASE("harness: every measured page gets a latency, stored-uncompressed pages included") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({page_with_prefix(100), page_with_prefix(page_size)});
    auto const r = run_codec(c, trim_codec, model, [] {
        auto o = run_options{};
        o.repetitions = 3;
        return o;
    }());
    REQUIRE(r.pages.size() == 2);
    for (auto const& p : r.pages) {
        CAPTURE(p.page);
        CHECK(p.compress_ns > 0.0);
        CHECK(p.decompress_ns > 0.0);
        CHECK(p.decompress_cold_ns > 0.0);
    }
}

namespace {

// the trimmed length of each page trim's compress and decompress got, in order
std::vector<unsigned int> compressed_pages, decompressed_pages;

int log_compress(
    quetschn_params* p, quetschn_stream* s, void const* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    compressed_pages.push_back(trimmed_length(src, src_len));
    return trim_compress(p, s, src, src_len, dst, dst_len);
}

int log_decompress(
    quetschn_params* p, quetschn_stream* s, void const* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    auto const* b = static_cast<unsigned char const*>(src);
    decompressed_pages.push_back(static_cast<unsigned int>(b[0] | (b[1] << 8)));
    return trim_decompress(p, s, src, src_len, dst, dst_len);
}

quetschn_codec const log_codec{
    "log", trim_setup_params, trim_release_params, trim_create, trim_destroy, log_compress, log_decompress};

} // namespace

TEST_CASE("harness: a codec never runs on the same page twice in a row") {
    // Timed right after the same code ran on the same page, the branch predictor has learned it.
    auto const model = zsmalloc_model();
    auto const c = make_corpus({page_with_prefix(10), page_with_prefix(20), page_with_prefix(30), page_with_prefix(40)});
    auto opts = run_options{};
    opts.repetitions = 3;
    compressed_pages.clear();
    decompressed_pages.clear();
    auto const r = run_codec(c, log_codec, model, opts);
    REQUIRE(r.pages.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(r.pages[i].page == i);
        CHECK(r.pages[i].comp_len == 10 * (i + 1) + 2);
    }
    // the check once, then 3 compressions and 6 decompressions (warm and cold) per page
    CHECK(compressed_pages.size() == 4 * 4);
    CHECK(decompressed_pages.size() == 4 * 7);
    for (std::size_t k = 1; k < compressed_pages.size(); ++k) {
        CAPTURE(k);
        CHECK(compressed_pages[k] != compressed_pages[k - 1]);
    }
    for (std::size_t k = 1; k < decompressed_pages.size(); ++k) {
        CAPTURE(k);
        CHECK(decompressed_pages[k] != decompressed_pages[k - 1]);
    }
}

TEST_CASE("harness: nearest-rank percentile") {
    auto v = std::vector<double>();
    for (int i = 100; i >= 1; --i) {
        v.push_back(i);
    }
    CHECK(percentile(v, 50) == 50);
    CHECK(percentile(v, 90) == 90);
    CHECK(percentile(v, 99) == 99);
    CHECK(percentile(v, 99.9) == 100);
    CHECK(percentile(v, 100) == 100);
    CHECK(percentile(v, 0) == 1);
    CHECK(percentile({7.0}, 99) == 7.0);
    CHECK(percentile({1.0, 2.0, 3.0}, 50) == 2.0);

    // 99.9 / 100 * 2000 is 1998.0000000000002 in doubles, the rank is still 1998
    auto w = std::vector<double>();
    for (int i = 1; i <= 2000; ++i) {
        w.push_back(i);
    }
    CHECK(percentile(w, 99.9) == 1998);
    CHECK(percentile(w, 99.95) == 1999);
    CHECK_THROWS_AS((void)percentile({}, 50), std::invalid_argument);
}

TEST_CASE("harness: the latency summary has the mean, which a few slow pages move and the p50 does not") {
    auto v = std::vector<double>(100, 1000.0);
    v[3] = 51000.0;
    auto const s = summarize_latency(v);
    CHECK(s.p50 == 1000.0);
    CHECK(s.mean == 1500.0);
    CHECK(s.max == 51000.0);
    CHECK(summarize_latency({}).mean == 0.0);
}

TEST_CASE("harness: a corpus written by the collector loads back unchanged") {
    auto tmpl = (std::filesystem::temp_directory_path() / "quetschn-harness-XXXXXX").string();
    REQUIRE(::mkdtemp(tmpl.data()) != nullptr);
    auto const dir = std::filesystem::path(tmpl);
    auto const base = dir / "corpus";

    auto const a = page_with_prefix(10);
    auto const b = page_with_prefix(4000);
    {
        auto writer = quetschn::corpus_writer(base, page_size);
        writer.write(1, "x", "", 0x1000, a);
        writer.write(1, "x", "", 0x2000, b);
    }
    auto const c = quetschn::load_corpus(base);
    CHECK(c.page_size == page_size);
    REQUIRE(c.size() == 2);
    CHECK(std::memcmp(c.page(0).data(), a.data(), page_size) == 0);
    CHECK(std::memcmp(c.page(1).data(), b.data(), page_size) == 0);

    // a TSV without the page size header is rejected
    auto tsv = base;
    tsv += ".tsv";
    std::ofstream(tsv) << "page\tpid\n";
    CHECK_THROWS_AS((void)quetschn::load_corpus(base), std::runtime_error);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
