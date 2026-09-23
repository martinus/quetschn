// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace quetschn {

struct import_result {
    std::size_t pages_written = 0;
    std::size_t zero_pages = 0; // skipped: free zram slots read as zeros, and zram's own zero pages look the same
};

// Turns a raw page dump, e.g. `dd if=/dev/zram0`, into a corpus. Every page-sized block that is not all
// zero becomes one page, with `name` as its process name and its offset in the dump as its address.
// Holes of a sparse file are skipped without reading them. Throws std::runtime_error if the dump is not a
// whole number of pages.
import_result import_raw(std::filesystem::path const& dump,
                         std::filesystem::path const& out_base,
                         std::string const& name,
                         std::size_t page_size);

} // namespace quetschn
