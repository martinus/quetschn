// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "compare.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using quetschn::compare;
using quetschn::compare_options;
using quetschn::page_result;
using quetschn::percentile;

namespace {

// Pages 0, 2, 4, ... like a run that skipped the same-filled odd pages. Costs vary a lot from page to
// page, the latencies are 1000 + i ns.
std::vector<page_result> baseline_run(std::size_t n) {
    auto pages = std::vector<page_result>();
    for (std::size_t i = 0; i < n; ++i) {
        auto p = page_result{};
        p.page = 2 * i;
        p.cost = static_cast<double>(32 + (i * 7919) % 4000);
        p.comp_len = static_cast<unsigned int>(p.cost);
        p.compress_ns = 1000.0 + static_cast<double>(i);
        p.decompress_ns = 1000.0 + static_cast<double>(i);
        p.decompress_cold_ns = 1000.0 + static_cast<double>(i);
        pages.push_back(p);
    }
    return pages;
}

compare_options fast() {
    auto o = compare_options{};
    o.resamples = 200;
    return o;
}

} // namespace

TEST_CASE("compare: page results read back exactly as written") {
    auto pages = baseline_run(3);
    pages[1].huge = true;
    pages[1].cost = 585.142857142857; // not representable in 6 digits
    pages[2].decompress_cold_ns = 0.1 + 0.2;

    auto out = std::stringstream();
    quetschn::write_page_results(out, pages);
    auto const back = quetschn::read_page_results(out);
    REQUIRE(back.size() == pages.size());
    for (std::size_t i = 0; i < pages.size(); ++i) {
        CAPTURE(i);
        CHECK(back[i].page == pages[i].page);
        CHECK(back[i].comp_len == pages[i].comp_len);
        CHECK(back[i].huge == pages[i].huge);
        CHECK(back[i].cost == pages[i].cost);
        CHECK(back[i].compress_ns == pages[i].compress_ns);
        CHECK(back[i].decompress_ns == pages[i].decompress_ns);
        CHECK(back[i].decompress_cold_ns == pages[i].decompress_cold_ns);
    }
}

TEST_CASE("compare: files that are not page results are rejected") {
    auto const header = std::string("page\tcomp_len\thuge\tcost\tcompress_ns\tdecompress_ns\tdecompress_cold_ns\n");
    for (auto const* text : {
             "",
             "page\tpid\n",
             "0\t80\t0\t96\t250\t80\t320\n", // no header
         }) {
        auto in = std::istringstream(text);
        CHECK_THROWS_AS((void)quetschn::read_page_results(in), std::runtime_error);
    }
    for (auto const* line : {"0\t80\t0\t96\t250\t80\n",         // a field missing
                             "0\t80\t0\t96\t250\t80\t320\t1\n", // one too many
                             "0\t80\t2\t96\t250\t80\t320\n",    // huge is 0 or 1
                             "0\t80\t0\t96x\t250\t80\t320\n",
                             "0\t80\t0\t\t250\t80\t320\n"}) {
        CAPTURE(std::string(line));
        auto in = std::istringstream(header + line);
        CHECK_THROWS_AS((void)quetschn::read_page_results(in), std::runtime_error);
    }
    auto in = std::istringstream(header + "0\t80\t0\t96\t250.5\t80\t320\n");
    CHECK(quetschn::read_page_results(in).size() == 1);
}

TEST_CASE("compare: only runs on the same pages can be paired") {
    auto const a = baseline_run(10);
    auto shorter = a;
    shorter.pop_back();
    CHECK_THROWS_AS((void)compare(a, shorter, fast()), std::invalid_argument);

    auto other = a;
    other[4].page = 9;
    CHECK_THROWS_WITH_AS((void)compare(a, other, fast()), doctest::Contains("page 9"), std::invalid_argument);

    CHECK_THROWS_AS((void)compare({}, {}, fast()), std::invalid_argument);
    auto none = fast();
    none.resamples = 0;
    CHECK_THROWS_AS((void)compare(a, a, none), std::invalid_argument);
}

TEST_CASE("compare: the same run against itself saves nothing, with no uncertainty") {
    auto const a = baseline_run(1000);
    auto const r = compare(a, a, fast());
    CHECK(r.pages == 1000);
    CHECK(r.saving.value == 0.0);
    CHECK(r.saving.low == 0.0);
    CHECK(r.saving.high == 0.0);
    CHECK(r.same == 1000);
    CHECK(r.cheaper == 0);
    CHECK(r.costlier == 0);
    CHECK(r.largest_regression == 0.0);
    REQUIRE(r.latency.size() == 3);
    for (auto const& l : r.latency) {
        CHECK(l.decompress_cold.low == 0.0);
        CHECK(l.decompress_cold.high == 0.0);
    }
}

TEST_CASE("compare: pages are resampled together, so a constant ratio has no uncertainty") {
    // The costs vary from 32 to 4031 bytes, so an unpaired bootstrap that draws other pages for the
    // candidate would give a wide interval. Paired, every resample saves exactly 20%.
    auto const a = baseline_run(1000);
    auto b = a;
    for (auto& p : b) {
        p.cost *= 0.8;
        p.decompress_ns += 100.0;
    }
    auto const r = compare(a, b, fast());
    CHECK(r.baseline_cost > r.candidate_cost);
    CHECK(r.saving.value == doctest::Approx(0.2));
    CHECK(r.saving.low == doctest::Approx(0.2));
    CHECK(r.saving.high == doctest::Approx(0.2));
    CHECK(r.cheaper == 1000);
    for (auto const& l : r.latency) {
        CAPTURE(l.p);
        CHECK(l.decompress.value == doctest::Approx(100.0));
        CHECK(l.decompress.low == doctest::Approx(100.0));
        CHECK(l.decompress.high == doctest::Approx(100.0));
        CHECK(l.compress.value == 0.0);
    }
}

TEST_CASE("compare: estimates, regressions and intervals of a mixed result") {
    auto const a = baseline_run(2000);
    auto b = a;
    // every 4th page gets 100 bytes more expensive, the others 10% cheaper; page 2 * 7 the most
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i].cost = i % 4 == 3 ? b[i].cost + 100.0 : b[i].cost * 0.9;
    }
    b[7].cost += 50.0;
    // the slowest 1% of pages get slower, which moves p99.9 but not p50
    for (auto& p : b) {
        // and warm decompression takes twice as long above 1000 ns, so the difference depends on the rank
        p.decompress_ns = 1000.0 + 2.0 * (p.decompress_ns - 1000.0);
        if (p.decompress_cold_ns >= 1000.0 + 1980.0) {
            p.decompress_cold_ns += 1000.0;
        }
    }

    auto const r = compare(a, b, fast());
    auto sum = [](std::vector<page_result> const& v) {
        auto s = 0.0;
        for (auto const& p : v) {
            s += p.cost;
        }
        return s;
    };
    CHECK(r.baseline_cost == doctest::Approx(sum(a)));
    CHECK(r.candidate_cost == doctest::Approx(sum(b)));
    CHECK(r.saving.value == doctest::Approx(1.0 - sum(b) / sum(a)));
    CHECK(r.saving.low < r.saving.value);
    CHECK(r.saving.high > r.saving.value);
    CHECK(r.saving.low > 0.0);
    CHECK(r.costlier == 500);
    CHECK(r.cheaper == 1500);
    CHECK(r.same == 0);
    CHECK(r.largest_regression == doctest::Approx(150.0));
    CHECK(r.largest_regression_page == 14);

    REQUIRE(r.latency.size() == 3);
    CHECK(r.latency[0].p == 50.0);
    CHECK(r.latency[1].p == 99.0);
    CHECK(r.latency[2].p == 99.9);
    auto cold = [](std::vector<page_result> const& v) {
        auto out = std::vector<double>();
        for (auto const& p : v) {
            out.push_back(p.decompress_cold_ns);
        }
        return out;
    };
    for (auto const& l : r.latency) {
        CAPTURE(l.p);
        CHECK(l.decompress_cold.value == percentile(cold(b), l.p) - percentile(cold(a), l.p));
        CHECK(l.decompress_cold.low <= l.decompress_cold.value);
        CHECK(l.decompress_cold.high >= l.decompress_cold.value);
    }
    CHECK(r.latency[0].decompress_cold.value == 0.0);
    CHECK(r.latency[2].decompress_cold.value == 1000.0);
    // nearest rank of 2000 values: p50 is the 1000th, p99 the 1980th, p99.9 the 1998th
    CHECK(r.latency[0].decompress.value == 999.0);
    CHECK(r.latency[1].decompress.value == 1979.0);
    CHECK(r.latency[2].decompress.value == 1997.0);
}

TEST_CASE("compare: the seed makes the intervals reproducible") {
    auto const a = baseline_run(500);
    auto b = a;
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i].cost = i % 2 == 0 ? b[i].cost * 0.5 : b[i].cost + 10.0;
    }
    auto const r1 = compare(a, b, fast());
    auto const r2 = compare(a, b, fast());
    CHECK(r1.saving.low == r2.saving.low);
    CHECK(r1.saving.high == r2.saving.high);

    auto other = fast();
    other.seed = 2;
    auto const r3 = compare(a, b, other);
    CHECK(r3.saving.value == r1.saving.value);
    CHECK((r3.saving.low != r1.saving.low || r3.saving.high != r1.saving.high));
}

TEST_CASE("compare: the intervals do not depend on the number of threads") {
    auto const a = baseline_run(300);
    auto b = a;
    for (std::size_t i = 0; i < b.size(); ++i) {
        b[i].cost = i % 3 == 0 ? b[i].cost * 0.5 : b[i].cost + 10.0;
        b[i].decompress_cold_ns += static_cast<double>(i % 7) * 30.0;
    }
    auto one = fast();
    one.threads = 1;
    auto three = fast();
    three.threads = 3;
    auto const r1 = compare(a, b, one);
    auto const r3 = compare(a, b, three);
    CHECK(r1.saving.low == r3.saving.low);
    CHECK(r1.saving.high == r3.saving.high);
    for (std::size_t k = 0; k < r1.latency.size(); ++k) {
        CAPTURE(k);
        CHECK(r1.latency[k].decompress_cold.low == r3.latency[k].decompress_cold.low);
        CHECK(r1.latency[k].decompress_cold.high == r3.latency[k].decompress_cold.high);
    }
}
