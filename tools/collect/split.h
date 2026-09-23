// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace quetschn {

struct split_options {
    double test_fraction = 0.3; // share of process names that go to the test side
    std::uint64_t seed = 1;
};

struct split_result {
    std::size_t train_pages = 0;
    std::size_t test_pages = 0;
    std::size_t train_names = 0;
    std::size_t test_names = 0;
    std::size_t same_filled_dropped = 0; // same-filled pages not written to the training side
};

// Splits a corpus into a training and a test corpus, by process name: all pages of one name end up on
// the same side, so a dictionary trained on one side never saw the programs of the other. Which side a
// name goes to depends only on the name and the seed. Same-filled pages are left out of the training
// side, because zram stores them without a codec and they would only teach the dictionary zeros; the
// test side keeps them, the harness skips them anyway.
//
// A process name stands in for a workload until the scripted VM workloads of PLAN.md Phase 1 exist.
// Throws std::runtime_error if one side would be empty.
split_result split_corpus(std::filesystem::path const& base,
                          std::filesystem::path const& train_base,
                          std::filesystem::path const& test_base,
                          split_options const& opts);

struct training_result {
    std::size_t pages = 0;               // written to the training side
    std::size_t same_filled_dropped = 0; // left out, as in split_corpus
    std::size_t excluded = 0;            // left out because exclude_base has a page with the same content
};

// Writes the pages of base to train_base, to train a dictionary that is measured on another corpus,
// exclude_base. Pages whose content also is in exclude_base are left out: a cold page can sit in zram
// through two dumps taken days apart, and a dictionary must not have seen the pages it is measured on.
// Same-filled pages are left out like in split_corpus. Throws std::runtime_error when the corpora
// cannot be read or have different page sizes.
training_result write_training_corpus(std::filesystem::path const& base,
                                      std::filesystem::path const& exclude_base,
                                      std::filesystem::path const& train_base);

// Which side a process name goes to: true for test. Exposed for tests.
[[nodiscard]] bool is_test_name(std::string const& name, split_options const& opts);

} // namespace quetschn
