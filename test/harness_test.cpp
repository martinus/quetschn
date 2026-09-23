// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "harness.h"
#include "resident.h"

#include <doctest/doctest.h>

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

int trim_compress(void const* src, unsigned int src_len, void* dst, unsigned int* dst_len, void* /*workspace*/) {
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

int trim_decompress(void const* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
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

int broken_decompress(void const* src, unsigned int src_len, void* dst, unsigned int* dst_len) {
    auto ret = trim_decompress(src, src_len, dst, dst_len);
    static_cast<unsigned char*>(dst)[7] ^= 1;
    return ret;
}

int failing_compress(void const*, unsigned int, void*, unsigned int*, void*) {
    return -1;
}

quetschn_codec const trim_codec{"trim", 0, trim_compress, trim_decompress};
quetschn_codec const broken_codec{"broken", 0, trim_compress, broken_decompress};
quetschn_codec const failing_codec{"failing", 0, failing_compress, trim_decompress};

// page with the first `nonzero` bytes set to non-zero values, the rest zero
std::vector<std::byte> page_with_prefix(std::size_t nonzero) {
    auto p = std::vector<std::byte>(page_size);
    for (std::size_t i = 0; i < nonzero; ++i) {
        p[i] = static_cast<std::byte>(i % 200 + 1);
    }
    return p;
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
    auto const r = run_codec(c, trim_codec, model, run_options{.measure_time = false});

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
    CHECK_THROWS_WITH_AS((void)run_codec(c, broken_codec, model, run_options{.measure_time = false}),
                         doctest::Contains("roundtrip"),
                         std::runtime_error);
    CHECK_THROWS_WITH_AS((void)run_codec(c, failing_codec, model, run_options{.measure_time = false}),
                         doctest::Contains("compress failed"),
                         std::runtime_error);
}

TEST_CASE("harness: every measured page gets a latency, stored-uncompressed pages included") {
    auto const model = zsmalloc_model();
    auto const c = make_corpus({page_with_prefix(100), page_with_prefix(page_size)});
    auto const r = run_codec(c, trim_codec, model, run_options{.repetitions = 3, .measure_time = true});
    REQUIRE(r.pages.size() == 2);
    for (auto const& p : r.pages) {
        CAPTURE(p.page);
        CHECK(p.compress_ns > 0.0);
        CHECK(p.decompress_ns > 0.0);
        CHECK(p.decompress_cold_ns > 0.0);
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
    CHECK_THROWS_AS((void)percentile({}, 50), std::invalid_argument);
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
