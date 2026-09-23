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
    auto tsv_path = base;
    tsv_path += ".tsv";
    auto pages_path = base;
    pages_path += ".pages";
    auto tsv = std::ifstream(tsv_path);
    auto pages = std::ifstream(pages_path, std::ios::binary);
    if (!tsv || !pages) {
        throw std::runtime_error("split_corpus: cannot read " + base.string() + ".{tsv,pages}");
    }

    auto line = std::string();
    constexpr auto prefix = std::string_view("# page_size=");
    if (!std::getline(tsv, line) || !line.starts_with(prefix)) {
        throw std::runtime_error("split_corpus: " + tsv_path.string() + " does not start with '# page_size='");
    }
    auto const page_size = std::stoul(line.substr(prefix.size()));
    std::getline(tsv, line); // column names

    // Decide first, so that a split with an empty side fails before any file is written.
    struct row {
        int pid;
        std::string comm;
        std::uintptr_t address;
        std::string mapping;
    };
    auto rows = std::vector<row>();
    auto train_names = std::set<std::string>();
    auto test_names = std::set<std::string>();
    while (std::getline(tsv, line)) {
        auto const f = split_tabs(line);
        if (f.size() != 9) {
            throw std::runtime_error("split_corpus: bad line in " + tsv_path.string());
        }
        auto r = row{
            std::stoi(std::string(f[1])), std::string(f[2]), std::stoull(std::string(f[3]), nullptr, 16), std::string(f[8])};
        (is_test_name(r.comm, opts) ? test_names : train_names).insert(r.comm);
        rows.push_back(std::move(r));
    }
    if (train_names.empty() || test_names.empty()) {
        throw std::runtime_error("split_corpus: one side is empty; " + std::to_string(train_names.size()) + " training and " +
                                 std::to_string(test_names.size()) + " test process names");
    }

    auto result = split_result{};
    result.train_names = train_names.size();
    result.test_names = test_names.size();
    auto train = corpus_writer(train_base, page_size);
    auto test = corpus_writer(test_base, page_size);
    auto buf = std::vector<std::byte>(page_size);
    for (auto const& r : rows) {
        if (!pages.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(page_size))) {
            throw std::runtime_error("split_corpus: " + pages_path.string() + " has fewer pages than the TSV");
        }
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

} // namespace quetschn
