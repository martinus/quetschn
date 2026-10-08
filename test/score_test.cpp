// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "score.h"

#include "../tools/swap-bursts/swap_bursts.h"

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
using quetschn::what_if;

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

// the lines of one device of MODE=swap, as run.sh prints them; zram adds the swap header to the pages
char const* const swap_log =
    "\x1b[?7l\x1b[2JRESULT swap: 22000 pages, 2262 of them same-filled (2000 added)\n"
    "RESULT lz4             mm_stat 90116096 27239813 29007872        0 29007872     2262        0      472      472\n"
    "RESULT lz4       run 0 per page, flushed: 0 of 22000 pages still resident, 22000 reads, flush 221 ns per page\n"
    "RESULT lz4       swap-out, one call for all pages: median of 3 runs 5346 ns per page\n"
    "RESULT lz4       swap-out, one call per page        n  19738: p50 6600 p90 8620 p99 10470 mean 6436 ns\n"
    "RESULT lz4       swap-out, same-filled              n   2262: p50 2480 p90 2631 p99 2850 mean 2485 ns\n"
    "RESULT lz4       swap-in, warm                      n  19738: p50 3151 p90 3789 p99 5110 mean 3152 ns\n"
    "RESULT lz4       swap-in, warm, same-filled         n   2262: p50 1770 p90 1870 p99 3380 mean 1819 ns\n"
    "RESULT lz4       swap-in, flushed                   n  19738: p50 3240 p90 3919 p99 5229 mean 3239 ns\n"
    "RESULT lz4       swap-in, flushed, same-filled      n   2262: p50 1760 p90 1870 p99 3370 mean 1803 ns\n"
    "RESULT lz4       swap-in, flushed, decompress timed: same-filled 1960, others 3511, of it zcomp_decompress() "
    "1423 ns, means, median of 3 runs\n"
    "RESULT PAGE 0 0 6000 3000 3100\n";

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

TEST_CASE("score: MODE=swap, the corpus's pages without the same-filled ones the mode adds") {
    auto in = std::istringstream(swap_log);
    auto const c = read_vm_results(in, "");
    REQUIRE(c.size() == 1);
    CHECK(c[0].name == "lz4");
    // 22001 pages stored, the swap header among them, 2000 of them added
    CHECK(c[0].pages == 20001.0);
    CHECK(c[0].bytes_per_page == doctest::Approx(29007872.0 / 20001.0));
    // 19738 compressed pages and 262 same-filled ones of the corpus
    CHECK(c[0].write_ns == doctest::Approx((19738.0 * 6436.0 + 262.0 * 2485.0) / 20000.0));
    CHECK(c[0].read_ns == doctest::Approx((19738.0 * 3239.0 + 262.0 * 1803.0) / 20000.0));
    auto const log = std::string(swap_log);
    auto const line = log.find("RESULT lz4       swap-in, flushed, same-filled");
    auto incomplete = std::istringstream(log.substr(0, line) + log.substr(log.find('\n', line) + 1));
    CHECK_THROWS_AS((void)read_vm_results(incomplete, ""), std::runtime_error);
}

TEST_CASE("score: a codec made faster keeps its bytes and the kernel's part of the time") {
    auto in = std::istringstream(swap_log);
    auto const c = read_vm_results(in, "");
    REQUIRE(c.size() == 1);
    // the same-filled pages are the kernel's part: zram stores them without the codec
    CHECK(c[0].write_kernel_ns == 2485.0);
    CHECK(c[0].read_kernel_ns == 1803.0);
    auto const half = what_if(c[0], 0.5);
    CHECK(half.name == "lz4 x0.5");
    CHECK(half.bytes_per_page == c[0].bytes_per_page);
    CHECK(half.write_ns == doctest::Approx(2485.0 + 0.5 * (c[0].write_ns - 2485.0)));
    CHECK(half.read_ns == doctest::Approx(1803.0 + 0.5 * (c[0].read_ns - 1803.0)));
    auto const zero = what_if(c[0], 0.0);
    CHECK(zero.name == "lz4 x0");
    CHECK(zero.write_ns == 2485.0);
    CHECK(zero.read_ns == 1803.0);
    // without a kernel's part, e.g. a bench log, the whole time is scaled
    auto const p = what_if(point("b", 10, 1000), 0.25);
    CHECK(p.write_ns == doctest::Approx(2500.0));
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
