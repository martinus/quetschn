// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "zsmalloc_cost.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <stdexcept>
#include <vector>

using quetschn::zsmalloc_config;
using quetschn::zsmalloc_model;

namespace {

struct row {
    std::size_t index;
    std::size_t size;
    std::size_t pages_per_zspage;
};

// All classes with a slot size >= min_size, in the same shape as the debugfs `classes` file.
std::vector<row> rows_from(zsmalloc_model const& model, std::size_t min_size) {
    std::vector<row> rows;
    for (auto const& c : model.classes()) {
        if (c.size >= min_size) {
            rows.push_back({c.index, c.size, c.pages_per_zspage});
        }
    }
    return rows;
}

void check_rows(std::vector<row> const& actual, std::vector<row> const& expected) {
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(actual[i].index == expected[i].index);
        CHECK(actual[i].size == expected[i].size);
        CHECK(actual[i].pages_per_zspage == expected[i].pages_per_zspage);
    }
}

} // namespace

// Expected values in this file come from outside the model: the listings and the table in the kernel's
// Documentation/mm/zsmalloc.rst, and arithmetic done by hand. The only exception is marked.

TEST_CASE("zsmalloc: huge class watermark matches Documentation/mm/zsmalloc.rst for every chain size") {
    // "huge size class watermark" column of the chain size table in the docs. zram's huge_class_size
    // is the watermark minus ZS_HANDLE_SIZE - 1.
    struct entry {
        std::size_t chain_size;
        std::size_t watermark;
    };
    auto const table = std::vector<entry>{
        {4, 3264},
        {5, 3408},
        {6, 3504},
        {7, 3584},
        {8, 3632},
        {9, 3680},
        {10, 3712},
        {11, 3744},
        {12, 3776},
        {13, 3792},
        {14, 3808},
        {15, 3840},
        {16, 3840},
    };
    for (auto const& e : table) {
        CAPTURE(e.chain_size);
        auto model = zsmalloc_model(zsmalloc_config{.chain_size = e.chain_size});
        CHECK(model.huge_class_size() == e.watermark - 7);
    }
}

TEST_CASE("zsmalloc: top of the class table matches the docs listing for chain size 4") {
    // "Let's take a closer look at the bottom of /sys/kernel/debug/zsmalloc/zramX/classes"
    auto model = zsmalloc_model(zsmalloc_config{.chain_size = 4});
    check_rows(rows_from(model, 3264), {{202, 3264, 4}, {254, 4096, 1}});
}

TEST_CASE("zsmalloc: top of the class table matches the docs listing for chain size 8") {
    auto model = zsmalloc_model(zsmalloc_config{.chain_size = 8});
    check_rows(rows_from(model, 3264),
               {
                   {202, 3264, 4},
                   {211, 3408, 5},
                   {217, 3504, 6},
                   {222, 3584, 7},
                   {225, 3632, 8},
                   {254, 4096, 1},
               });
}

TEST_CASE("zsmalloc: top of the class table matches the docs listing for chain size 16") {
    // One row differs from the docs. The docs list `212 3424 16`, but by the kernel's own rules that
    // row cannot appear: 3424 and 3440 both waste the least with 16 pages (480 and 176 bytes of tail),
    // so both are (16 pages, 19 objects), and zs_create_pool() merges #212 into the larger #213.
    // I don't know why the docs differ, maybe the listing is from an earlier version of the series.
    auto model = zsmalloc_model(zsmalloc_config{.chain_size = 16});
    // one row per line, like the listing in the docs
    // clang-format off
    check_rows(rows_from(model, 3264),
               {
                   {202, 3264, 4},
                   {206, 3328, 13},
                   {207, 3344, 9},
                   {208, 3360, 14},
                   {211, 3408, 5},
                   {213, 3440, 16}, // docs: {212, 3424, 16}, see above
                   {214, 3456, 11},
                   {217, 3504, 6},
                   {219, 3536, 13},
                   {222, 3584, 7},
                   {223, 3600, 15},
                   {225, 3632, 8},
                   {228, 3680, 9},
                   {230, 3712, 10},
                   {232, 3744, 11},
                   {234, 3776, 12},
                   {235, 3792, 13},
                   {236, 3808, 14},
                   {238, 3840, 15},
                   {254, 4096, 1},
               });
    // clang-format on
}

TEST_CASE("zsmalloc: merged classes use the larger slot, docs example for chain size 4") {
    // From the docs: "Size classes #95-99 are merged with size class #100", which uses 2 pages per
    // zspage and "can hold a total of 5 objects". Class #94 (1536 bytes) stays separate with 3 pages.
    auto model = zsmalloc_model(zsmalloc_config{.chain_size = 4});

    // 1536 + 8 byte handle = 1544 > 1536, so the smallest payload that lands in #95 is 1529
    auto const& c94 = model.class_for(1528);
    CHECK(c94.index == 94);
    CHECK(c94.size == 1536);
    CHECK(c94.pages_per_zspage == 3);

    for (std::size_t comp_len = 1529; comp_len <= 1632 - 8; ++comp_len) {
        CAPTURE(comp_len);
        auto const& c = model.class_for(comp_len);
        REQUIRE(c.index == 100);
        REQUIRE(c.size == 1632);
        REQUIRE(c.pages_per_zspage == 2);
        REQUIRE(c.objs_per_zspage == 5);
    }
    CHECK(model.class_for(1632 - 7).index > 100);
}

TEST_CASE("zsmalloc: cost includes the zspage tail waste") {
    auto model = zsmalloc_model();

    // By hand: 100 + 8 = 108 bytes, class index ceil((108 - 32) / 16) = 5, slot 32 + 5 * 16 = 112.
    // 7 pages are 28672 = 256 * 112 bytes, so there is no tail and the cost is exactly the slot.
    CHECK(model.class_for(100).size == 112);
    CHECK(model.class_for(100).pages_per_zspage == 7);
    CHECK(model.cost(100) == doctest::Approx(112.0));

    // Class #225 from the docs listing: 3632 bytes, 8 pages. 32768 / 3632 = 9 slots, tail 80 bytes.
    // Cost per object is 32768 / 9.
    CHECK(model.class_for(3624).size == 3632);
    CHECK(model.cost(3624) == doctest::Approx(32768.0 / 9.0));
}

TEST_CASE("zsmalloc: pages at the watermark are stored as a whole page") {
    auto model = zsmalloc_model();
    auto const huge = model.huge_class_size();
    REQUIRE(huge == 3625);

    CHECK(model.class_for(huge - 1).size == 3632);
    CHECK(model.cost(huge - 1) < 3700.0);

    // write_incompressible_page() stores the page itself in the 4096 byte class, 1 page per object
    for (auto comp_len : {huge, huge + 1, std::size_t{4000}, std::size_t{4096}, std::size_t{8192}}) {
        CAPTURE(comp_len);
        CHECK(model.class_for(comp_len).size == 4096);
        CHECK(model.cost(comp_len) == doctest::Approx(4096.0));
    }
}

TEST_CASE("zsmalloc: cost never decreases when the compressed page gets longer") {
    // Not a kernel guarantee, but the benchmark treats "smaller output is never more expensive" as true.
    // If a config ever violates it, the cost column has to be read differently, so this should fail
    // loudly.
    for (std::size_t page_size : {4096U, 16384U, 65536U}) {
        for (std::size_t chain_size = 4; chain_size <= 16; ++chain_size) {
            CAPTURE(page_size);
            CAPTURE(chain_size);
            auto model = zsmalloc_model(zsmalloc_config{.page_size = page_size, .chain_size = chain_size});
            double prev = 0.0;
            for (std::size_t comp_len = 1; comp_len <= page_size; ++comp_len) {
                auto cost = model.cost(comp_len);
                if (cost < prev) {
                    CAPTURE(comp_len);
                    FAIL("cost decreased");
                }
                prev = cost;
            }
        }
    }
}

TEST_CASE("zsmalloc: every payload fits its slot but not the next smaller one") {
    // Property from zs_malloc(): the payload plus the handle fits the slot, and it would not fit the
    // next smaller class, otherwise the lookup picked the wrong one.
    for (std::size_t page_size : {4096U, 16384U}) {
        auto model = zsmalloc_model(zsmalloc_config{.page_size = page_size});
        auto const classes = model.classes();
        for (std::size_t comp_len = 1; comp_len < model.huge_class_size(); ++comp_len) {
            auto const& c = model.class_for(comp_len);
            REQUIRE(comp_len + 8 <= c.size);
            if (&c != &classes.front()) {
                REQUIRE(comp_len + 8 > (&c - 1)->size);
            }
        }
    }
}

TEST_CASE("zsmalloc: 16 KiB pages") {
    auto model = zsmalloc_model(zsmalloc_config{.page_size = 16384});

    // ZS_SIZE_CLASS_DELTA is 16384 >> 8 = 64, so every slot is 32 + n * 64, except the top class,
    // which zs_create_pool() clamps to PAGE_SIZE
    auto const classes = model.classes();
    for (auto const& c : classes.first(classes.size() - 1)) {
        CHECK((c.size - 32) % 64 == 0);
    }
    CHECK(classes.back().size == 16384);

    // Regression pin, not an independent oracle: this value comes from a separate Python port of the
    // same kernel code, written by the same author. It is in PLAN.md §3.5.
    CHECK(model.huge_class_size() == 14553);
    CHECK(model.classes().size() == 121);
}

TEST_CASE("zsmalloc: invalid input is rejected") {
    CHECK_THROWS_AS((void)zsmalloc_model().class_for(0), std::invalid_argument);
    CHECK_THROWS_AS(zsmalloc_model(zsmalloc_config{.page_size = 5000}), std::invalid_argument);
    CHECK_THROWS_AS(zsmalloc_model(zsmalloc_config{.page_size = 2048}), std::invalid_argument);
    CHECK_THROWS_AS(zsmalloc_model(zsmalloc_config{.chain_size = 3}), std::invalid_argument);
    CHECK_THROWS_AS(zsmalloc_model(zsmalloc_config{.chain_size = 17}), std::invalid_argument);
    CHECK_THROWS_AS(zsmalloc_model(zsmalloc_config{.handle_size = 0}), std::invalid_argument);
}

TEST_CASE("zsmalloc: the cost table in PLAN.md §3.1") {
    // Regression pin for the numbers PLAN.md publishes. Rows 100, 3624 and 3625 are checked against
    // independent values above; the others come from the same Python port as the 16 KiB pin.
    struct entry {
        std::size_t comp_len;
        std::size_t slot;
        std::size_t pages_per_zspage;
        std::size_t objs_per_zspage;
        double cost;
    };
    auto const table = std::vector<entry>{
        {100, 112, 7, 256, 112.0},
        {111, 128, 1, 32, 128.0},
        {1024, 1056, 8, 31, 1057.0},
        {2048, 2176, 8, 15, 2184.5},
        {3255, 3264, 4, 5, 3276.8},
        {3624, 3632, 8, 9, 3640.9},
        {3625, 4096, 1, 1, 4096.0},
        {4000, 4096, 1, 1, 4096.0},
    };
    auto model = zsmalloc_model();
    for (auto const& e : table) {
        CAPTURE(e.comp_len);
        auto const& c = model.class_for(e.comp_len);
        CHECK(c.size == e.slot);
        CHECK(c.pages_per_zspage == e.pages_per_zspage);
        CHECK(c.objs_per_zspage == e.objs_per_zspage);
        CHECK(c.cost == doctest::Approx(e.cost).epsilon(0.0001));
    }
    CHECK(model.classes().size() == 119);
}
