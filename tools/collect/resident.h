// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace quetschn {

// One line of /proc/<pid>/maps
struct mapping {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
    std::string perms; // e.g. "rw-p"
    std::uint64_t inode = 0;
    std::string name; // empty for plain anonymous memory, "[heap]", "[anon:...]", a file path, ...
};

[[nodiscard]] std::vector<mapping> parse_maps(std::string_view text);

// Readable, private, and named like anonymous memory: no name, [heap], [stack...] or [anon:...]. File
// mappings always have their path as the name, so they are out without looking at the inode. This is the memory that reclaim
// can push to zram. Shared memory and file pages go elsewhere, and the kernel's own [vdso]/[vvar] pages never move.
[[nodiscard]] bool is_anonymous_private(mapping const& m);

// Writes a corpus as two files:
//
// - <base>.pages: the raw pages, concatenated, nothing else.
// - <base>.tsv: one line per page, in the same order, with where the page came from and its stats.
//
// Both files are created with mode 0600 and never overwritten, because the pages contain whatever the
// processes had in memory: keys, passwords, personal data.
class corpus_writer {
public:
    corpus_writer(std::filesystem::path const& base, std::size_t page_size);
    ~corpus_writer();
    corpus_writer(corpus_writer const&) = delete;
    corpus_writer& operator=(corpus_writer const&) = delete;

    void write(pid_t pid,
               std::string_view comm,
               std::string_view mapping_name,
               std::uintptr_t address,
               std::span<std::byte const> page);

    [[nodiscard]] std::size_t pages_written() const;
    [[nodiscard]] std::size_t page_size() const;

private:
    std::FILE* m_pages = nullptr;
    std::FILE* m_tsv = nullptr;
    std::size_t m_page_size = 0;
    std::size_t m_pages_written = 0;
};

struct collect_options {
    // 0 means all resident pages. Otherwise a random subset of this size per process, so that one big
    // process does not dominate the corpus.
    std::size_t max_pages_per_process = 0;
    std::uint64_t seed = 1;
    // Use the PAGEMAP_SCAN ioctl when the kernel has it (6.7+). Off only to test the fallback.
    bool use_pagemap_scan = true;
};

struct collect_result {
    std::size_t resident = 0;    // resident anonymous pages found
    std::size_t swapped = 0;     // pages already in swap, not read: reading them would swap them back in
    std::size_t written = 0;     // pages that ended up in the corpus
    std::size_t read_failed = 0; // resident when checked, but gone when read, e.g. the process freed them
    bool used_pagemap_scan = false;
};

// Reads the resident anonymous pages of one process into the corpus. Needs the same permissions as
// ptrace: own processes, and with kernel.yama.ptrace_scope=1 only descendants. Throws
// std::system_error when /proc/<pid>/maps or /proc/<pid>/pagemap cannot be read.
collect_result collect_process(pid_t pid, collect_options const& opts, corpus_writer& out);

} // namespace quetschn
