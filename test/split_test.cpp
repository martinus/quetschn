// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "harness.h"
#include "page_stats.h"
#include "resident.h"
#include "split.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr std::size_t page_size = 4096;

class temp_dir {
public:
    temp_dir() {
        auto tmpl = (std::filesystem::temp_directory_path() / "quetschn-split-XXXXXX").string();
        REQUIRE(::mkdtemp(tmpl.data()) != nullptr);
        m_path = tmpl;
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    temp_dir(temp_dir const&) = delete;
    temp_dir& operator=(temp_dir const&) = delete;

    [[nodiscard]] std::filesystem::path const& path() const {
        return m_path;
    }

private:
    std::filesystem::path m_path;
};

// Page number n of process `name`: the name and the number are written into the page, so every page
// is unique and says where it came from. Every 4th page is same-filled instead.
std::vector<std::byte> page_for(std::string const& name, std::size_t n) {
    if (n % 4 == 3) {
        return std::vector<std::byte>(page_size, std::byte{0x11});
    }
    auto p = std::vector<std::byte>(page_size);
    auto const text = name + "#" + std::to_string(n);
    std::memcpy(p.data(), text.data(), text.size());
    return p;
}

std::vector<std::string> names() {
    auto v = std::vector<std::string>();
    for (int i = 0; i < 20; ++i) {
        v.push_back("proc-" + std::to_string(i));
    }
    return v;
}

void write_corpus(std::filesystem::path const& base) {
    auto w = quetschn::corpus_writer(base, page_size);
    auto pid = 100;
    for (auto const& name : names()) {
        for (std::size_t n = 0; n < 8; ++n) {
            w.write(pid, name, "[heap]", 0x1000 * (n + 1), page_for(name, n));
        }
        ++pid;
    }
}

// process name of every page, from the page content
std::multiset<std::string> names_in(quetschn::corpus const& c) {
    auto result = std::multiset<std::string>();
    for (std::size_t i = 0; i < c.size(); ++i) {
        auto const p = c.page(i);
        if (quetschn::analyze_page(p).same_filled) {
            result.insert("<same-filled>");
            continue;
        }
        auto const text = std::string(reinterpret_cast<char const*>(p.data()));
        result.insert(text.substr(0, text.find('#')));
    }
    return result;
}

} // namespace

TEST_CASE("split: every process name is on exactly one side, and no page is lost") {
    auto dir = temp_dir();
    write_corpus(dir.path() / "all");
    auto const r = quetschn::split_corpus(dir.path() / "all", dir.path() / "train", dir.path() / "test", {});

    auto const train = names_in(quetschn::load_corpus(dir.path() / "train"));
    auto const test = names_in(quetschn::load_corpus(dir.path() / "test"));
    CHECK(r.train_pages + r.test_pages + r.same_filled_dropped == 20 * 8);
    CHECK(r.train_names + r.test_names == 20);
    CHECK(r.train_names > 0);
    CHECK(r.test_names > 0);

    for (auto const& name : names()) {
        CAPTURE(name);
        // 6 of the 8 pages carry the name, the other 2 are same-filled
        auto const n_train = train.count(name);
        auto const n_test = test.count(name);
        CHECK(n_train + n_test == 6);
        CHECK((n_train == 0 || n_test == 0));
        CHECK((n_test > 0) == quetschn::is_test_name(name, {}));
    }
}

TEST_CASE("split: same-filled pages are left out of the training side only") {
    auto dir = temp_dir();
    write_corpus(dir.path() / "all");
    auto const r = quetschn::split_corpus(dir.path() / "all", dir.path() / "train", dir.path() / "test", {});

    CHECK(names_in(quetschn::load_corpus(dir.path() / "train")).count("<same-filled>") == 0);
    CHECK(names_in(quetschn::load_corpus(dir.path() / "test")).count("<same-filled>") == 2 * r.test_names);
    CHECK(r.same_filled_dropped == 2 * r.train_names);
}

TEST_CASE("split: the side depends on name and seed only") {
    auto const a = quetschn::split_options{.test_fraction = 0.3, .seed = 1};
    auto const b = quetschn::split_options{.test_fraction = 0.3, .seed = 2};
    auto same = 0;
    auto test_a = 0;
    for (auto const& name : names()) {
        CHECK(quetschn::is_test_name(name, a) == quetschn::is_test_name(name, a));
        same += quetschn::is_test_name(name, a) == quetschn::is_test_name(name, b) ? 1 : 0;
        test_a += quetschn::is_test_name(name, a) ? 1 : 0;
    }
    // another seed gives another split, and 30% of 20 names is about 6
    CHECK(same < 20);
    CHECK(test_a >= 2);
    CHECK(test_a <= 12);

    // the extremes put everything on one side
    auto const none = quetschn::split_options{.test_fraction = 0.0, .seed = 1};
    auto const all = quetschn::split_options{.test_fraction = 1.0, .seed = 1};
    for (auto const& name : names()) {
        CHECK_FALSE(quetschn::is_test_name(name, none));
        CHECK(quetschn::is_test_name(name, all));
    }
}

TEST_CASE("split: an empty side is an error, before anything is written") {
    auto dir = temp_dir();
    write_corpus(dir.path() / "all");
    CHECK_THROWS_WITH_AS((void)quetschn::split_corpus(
                             dir.path() / "all", dir.path() / "train", dir.path() / "test", {.test_fraction = 0.0, .seed = 1}),
                         doctest::Contains("one side is empty"),
                         std::runtime_error);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "train.pages"));
    CHECK_FALSE(std::filesystem::exists(dir.path() / "test.pages"));
}
