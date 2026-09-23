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

// Which side a process name goes to: true for test. Exposed for tests.
[[nodiscard]] bool is_test_name(std::string const& name, split_options const& opts);

} // namespace quetschn
