// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "score.h"

#include "../tools/swap_bursts.h"

#include <doctest/doctest.h>

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using quetschn::best_for_some_lambda;
using quetschn::codec_cost;
using quetschn::read_bench_results;
using quetschn::read_vm_results;
using quetschn::score_weights;
using quetschn::us_per_page;

namespace {

// the lines of tools/zram-vm/init.c for two devices, one with recompression, as run.sh prints them
char const* const vm_log =
    "\x1b[?7l\x1b[2JRESULT lz4      mm_stat 81920000 33296726 35090432        0 35090432      416        0      852      852\n"
    "RESULT a+b     mm_stat 81920000 26224238 27971584        0 27971584      416        0      749      749\n"
    "RESULT lz4      write, same page before    : p50 5880 p90 8230 p99 9400 mean 6100 ns\n"
    "RESULT a+b     write, same page before    : p50 7550 p90 9781 p99 11080 mean 7900 ns\n"
    "RESULT lz4      write, other page before   : p50 5981 p90 8310 p99 9500 mean 6200 ns\n"
    "RESULT a+b     write, other page before   : p50 7680 p90 9909 p99 11149 mean 8000 ns\n"
    "RESULT a+b     recompress: 242842 ns per page\n"
    "RESULT a+b     mm_stat after recompress 81920000 21946548 23511040        0 28102656      416     2010      512     5243\n"
    "RESULT lz4      warm                       prefetch 8: p50 1930 p90 2460 p99 3540 mean 2000 ns\n"
    "RESULT lz4      flushed, other page first  prefetch 0: p50 2541 p90 3850 p99 5010 mean 2700 ns\n"
    "RESULT a+b     flushed, other page first  prefetch 0: p50 3380 p90 5180 p99 6940 mean 3500 ns\n"
    "RESULT lz4      flushed, other page first  prefetch 8: p50 2560 p90 3950 p99 5080 mean 2600 ns\n"
    "RESULT a+b     flushed, other page first  prefetch 8: p50 3259 p90 4840 p99 6020 mean 3400 ns\n"
    "some other line of the kernel\n";

codec_cost point(char const* name, double us, double bytes) {
    auto c = codec_cost{};
    c.name = name;
    c.write_ns = us * 1000.0;
    c.bytes_per_page = bytes;
    return c;
}

std::vector<std::string> names(std::vector<codec_cost> const& c, std::vector<std::size_t> const& idx) {
    auto v = std::vector<std::string>();
    for (auto k : idx) {
        v.push_back(c[k].name);
    }
    return v;
}

} // namespace

TEST_CASE("score: the devices of a run of tools/zram-vm, memory after recompression and the means") {
    auto in = std::istringstream(vm_log);
    auto const c = read_vm_results(in, "run1 ");
    REQUIRE(c.size() == 2);
    CHECK(c[0].name == "run1 lz4");
    CHECK(c[0].pages == 20000.0);
    CHECK(c[0].bytes_per_page == doctest::Approx(35090432.0 / 20000.0));
    CHECK(c[0].write_ns == 6200.0);
    CHECK(c[0].read_ns == 2600.0);
    CHECK(c[0].recompress_ns == 0.0);
    CHECK(c[1].name == "run1 a+b");
    CHECK(c[1].bytes_per_page == doctest::Approx(23511040.0 / 20000.0));
    CHECK(c[1].write_ns == 8000.0);
    CHECK(c[1].read_ns == 3400.0);
    CHECK(c[1].recompress_ns == 242842.0);
    // 8 + 0.5 * 3.4 + 0.25 * 242.842
    CHECK(us_per_page(c[1], score_weights{0.5, 0.25}) == doctest::Approx(70.4105));
}

TEST_CASE("score: a log from before the means is rejected, not scored with 0") {
    auto const mm = std::string("RESULT lz4 mm_stat 81920000 1 2000000 0\n");
    auto const write = std::string("RESULT lz4 write, other page before   : p50 5981 p90 8310 p99 9500");
    auto const read = std::string("RESULT lz4 flushed, other page first  prefetch 8: p50 2560 p90 3950 p99 5080");
    auto in = std::istringstream(mm + write + " mean 6000 ns\n" + read + " mean 2600 ns\n");
    CHECK(read_vm_results(in, "").size() == 1);
    auto no_write = std::istringstream(mm + write + " ns\n" + read + " mean 2600 ns\n");
    CHECK_THROWS_AS((void)read_vm_results(no_write, ""), std::runtime_error);
    auto no_read = std::istringstream(mm + write + " mean 6000 ns\n" + read + " ns\n");
    CHECK_THROWS_AS((void)read_vm_results(no_read, ""), std::runtime_error);
}

TEST_CASE("score: the codecs of a quetschn-bench run, with a level where there is one") {
    auto in = std::istringstream("\n"
                                 "codec      zstd, level -1\n"
                                 "pages                  1974 measured, 26 same-filled skipped\n"
                                 "zsmalloc cost          2114284 bytes, 26.1% of uncompressed, 1067.8 bytes/page\n"
                                 "\n"
                                 "latency ns                   p50       p90       p99     p99.9       max      mean\n"
                                 "compress                   11025     14000     20000     22000     25000     11700\n"
                                 "decompress warm             2970      4000      5000      6000      7000      3100\n"
                                 "decompress cold             3300      4400      5500      6600      7700      3450\n"
                                 "\n"
                                 "codec      seqlz, no level (zram ignores it for this codec)\n"
                                 "pages                  1974 measured, 26 same-filled skipped\n"
                                 "zsmalloc cost          1873182 bytes, 23.1% of uncompressed, 946.1 bytes/page\n"
                                 "latency ns                   p50       p90       p99     p99.9       max      mean\n"
                                 "compress                   17820     19000     21000     22000     23000     18000\n"
                                 "decompress warm             5130      6000      7000      8000      9000      5200\n"
                                 "decompress cold             5500      6500      7500      8500      9500      5600\n");
    auto const c = read_bench_results(in, "");
    REQUIRE(c.size() == 2);
    CHECK(c[0].name == "zstd:-1");
    CHECK(c[0].pages == 1974.0);
    CHECK(c[0].bytes_per_page == 1067.8);
    CHECK(c[0].write_ns == 11700.0);
    CHECK(c[0].read_ns == 3450.0);
    CHECK(c[1].name == "seqlz");
    CHECK(c[1].bytes_per_page == 946.1);
    CHECK(c[1].read_ns == 5600.0);
    // without the timing there is no mean
    auto no_timing = std::istringstream("codec      lz4, no level\n"
                                        "zsmalloc cost          2114284 bytes, 26.1% of uncompressed, 1067.8 bytes/page\n");
    CHECK_THROWS_AS((void)read_bench_results(no_timing, ""), std::runtime_error);
}

TEST_CASE("score: the codecs that win for some lambda are the lower left convex hull") {
    // d is slower and larger than c; e is on the Pareto front between b and c but above the line from
    // b to c, so it loses to one of them at every lambda
    auto const c = std::vector<codec_cost>{
        point("c", 30, 1000), point("a", 5, 2000), point("d", 40, 1500), point("b", 10, 1300), point("e", 20, 1200)};
    CHECK(names(c, best_for_some_lambda(c, score_weights{})) == std::vector<std::string>{"a", "b", "c"});
    // e moved below the line from b to c: now it wins between them
    auto d = c;
    d[4] = point("e", 20, 1100);
    CHECK(names(d, best_for_some_lambda(d, score_weights{})) == std::vector<std::string>{"a", "b", "e", "c"});
    CHECK(best_for_some_lambda({}, score_weights{}).empty());
}

TEST_CASE("swap bursts: runs of intervals with swap-ins, with short gaps, by size") {
    auto b = swap_bursts{};
    b.gap = 1;
    // 3 and 2 with one empty interval between: one burst of 5; then 2 empty ones end it; then 1 alone
    for (auto n : {3ULL, 0ULL, 2ULL, 0ULL, 0ULL, 1ULL, 0ULL, 0ULL}) {
        swap_bursts_add(&b, n);
    }
    swap_bursts_end(&b);
    CHECK(b.bursts[0] == 1);
    CHECK(b.pages[0] == 1);
    CHECK(b.bursts[2] == 1);
    CHECK(b.pages[2] == 5);
    CHECK(b.bursts[1] == 0);
    CHECK(swap_burst_class(1) == 0);
    CHECK(swap_burst_class(3) == 1);
    CHECK(swap_burst_class(4) == 2);
    CHECK(swap_burst_class(1ULL << 40) == SWAP_BURST_CLASSES - 1);
}
