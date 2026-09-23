// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "split.h"

#include "page_stats.h"
#include "resident.h"

#include <cstring>
#include <fstream>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace quetschn {

namespace {

// FNV-1a over the seed and the name. Fixed, so a split is the same on every machine and compiler.
std::uint64_t name_hash(std::string const& name, std::uint64_t seed) {
    auto h = std::uint64_t{0xcbf29ce484222325};
    auto mix = [&](unsigned char c) {
        h ^= c;
        h *= 0x100000001b3;
    };
    for (int i = 0; i < 8; ++i) {
        mix(static_cast<unsigned char>(seed >> (8 * i)));
    }
    for (auto c : name) {
        mix(static_cast<unsigned char>(c));
    }
    return h;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    auto fields = std::vector<std::string_view>();
    while (true) {
        auto const tab = line.find('\t');
        fields.push_back(line.substr(0, tab));
        if (tab == std::string_view::npos) {
            return fields;
        }
        line.remove_prefix(tab + 1);
    }
}

struct row {
    int pid;
    std::string comm;
    std::uintptr_t address;
    std::string mapping;
};

// A corpus as quetschn-collect-resident writes it: all of the TSV, and the pages to read one by one.
struct corpus_reader {
    std::filesystem::path pages_path;
    std::size_t page_size = 0;
    std::vector<row> rows;
    std::ifstream pages;

    void read(std::vector<std::byte>& buf) {
        if (!pages.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(page_size))) {
            throw std::runtime_error(pages_path.string() + " has fewer pages than the TSV");
        }
    }
};

corpus_reader open_corpus(std::filesystem::path const& base) {
    auto tsv_path = base;
    tsv_path += ".tsv";
    auto c = corpus_reader{};
    c.pages_path = base;
    c.pages_path += ".pages";
    auto tsv = std::ifstream(tsv_path);
    c.pages.open(c.pages_path, std::ios::binary);
    if (!tsv || !c.pages) {
        throw std::runtime_error("cannot read " + base.string() + ".{tsv,pages}");
    }

    auto line = std::string();
    constexpr auto prefix = std::string_view("# page_size=");
    if (!std::getline(tsv, line) || !line.starts_with(prefix)) {
        throw std::runtime_error(tsv_path.string() + " does not start with '# page_size='");
    }
    c.page_size = std::stoul(line.substr(prefix.size()));
    std::getline(tsv, line); // column names
    while (std::getline(tsv, line)) {
        auto const f = split_tabs(line);
        if (f.size() != 9) {
            throw std::runtime_error("bad line in " + tsv_path.string());
        }
        c.rows.push_back(
            {std::stoi(std::string(f[1])), std::string(f[2]), std::stoull(std::string(f[3]), nullptr, 16), std::string(f[8])});
    }
    return c;
}

// 64 bits of the page content. A collision only drops a training page that would have been fine.
std::uint64_t page_hash(std::span<std::byte const> page) {
    auto h = std::uint64_t{0xcbf29ce484222325};
    for (std::size_t i = 0; i + 8 <= page.size(); i += 8) {
        auto w = std::uint64_t{};
        std::memcpy(&w, page.data() + i, 8);
        h = (h ^ w) * 0x9e3779b97f4a7c15U;
        h ^= h >> 29U;
    }
    return h;
}

} // namespace

bool is_test_name(std::string const& name, split_options const& opts) {
    // top 53 bits as a uniform number in [0, 1)
    auto const u = static_cast<double>(name_hash(name, opts.seed) >> 11U) * 0x1.0p-53;
    return u < opts.test_fraction;
}

split_result split_corpus(std::filesystem::path const& base,
                          std::filesystem::path const& train_base,
                          std::filesystem::path const& test_base,
                          split_options const& opts) {
    auto in = open_corpus(base);

    // Decide first, so that a split with an empty side fails before any file is written.
    auto train_names = std::set<std::string>();
    auto test_names = std::set<std::string>();
    for (auto const& r : in.rows) {
        (is_test_name(r.comm, opts) ? test_names : train_names).insert(r.comm);
    }
    if (train_names.empty() || test_names.empty()) {
        throw std::runtime_error("split_corpus: one side is empty; " + std::to_string(train_names.size()) + " training and " +
                                 std::to_string(test_names.size()) + " test process names");
    }

    auto result = split_result{};
    result.train_names = train_names.size();
    result.test_names = test_names.size();
    auto train = corpus_writer(train_base, in.page_size);
    auto test = corpus_writer(test_base, in.page_size);
    auto buf = std::vector<std::byte>(in.page_size);
    for (auto const& r : in.rows) {
        in.read(buf);
        if (test_names.contains(r.comm)) {
            test.write(r.pid, r.comm, r.mapping, r.address, buf);
        } else if (analyze_page(buf).same_filled) {
            ++result.same_filled_dropped;
        } else {
            train.write(r.pid, r.comm, r.mapping, r.address, buf);
        }
    }
    result.train_pages = train.pages_written();
    result.test_pages = test.pages_written();
    return result;
}

training_result write_training_corpus(std::filesystem::path const& base,
                                      std::filesystem::path const& exclude_base,
                                      std::filesystem::path const& train_base) {
    auto in = open_corpus(base);
    auto exclude = open_corpus(exclude_base);
    if (exclude.page_size != in.page_size) {
        throw std::runtime_error("write_training_corpus: " + base.string() + " and " + exclude_base.string() +
                                 " have different page sizes");
    }
    auto excluded = std::unordered_set<std::uint64_t>();
    auto buf = std::vector<std::byte>(in.page_size);
    for (std::size_t i = 0; i < exclude.rows.size(); ++i) {
        exclude.read(buf);
        excluded.insert(page_hash(buf));
    }

    auto result = training_result{};
    auto train = corpus_writer(train_base, in.page_size);
    for (auto const& r : in.rows) {
        in.read(buf);
        if (analyze_page(buf).same_filled) {
            ++result.same_filled_dropped;
        } else if (excluded.contains(page_hash(buf))) {
            ++result.excluded;
        } else {
            train.write(r.pid, r.comm, r.mapping, r.address, buf);
        }
    }
    result.pages = train.pages_written();
    return result;
}

} // namespace quetschn
