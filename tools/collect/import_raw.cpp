// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "import_raw.h"

#include "resident.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace quetschn {

namespace {

// Closes the file on every exit
struct fd_guard {
    int fd;
    ~fd_guard() {
        ::close(fd);
    }
};

[[noreturn]] void throw_errno(std::string const& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

import_result import_raw(std::filesystem::path const& dump,
                         std::filesystem::path const& out_base,
                         std::string const& name,
                         std::size_t page_size) {
    auto const fd = ::open(dump.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        throw_errno("cannot open " + dump.string());
    }
    auto const guard = fd_guard{fd};
    auto const size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) {
        throw_errno("cannot seek in " + dump.string());
    }
    if (static_cast<std::size_t>(size) % page_size != 0) {
        throw std::runtime_error("import_raw: " + dump.string() + " is not a whole number of " + std::to_string(page_size) +
                                 " byte pages");
    }

    auto result = import_result{};
    auto out = corpus_writer(out_base, page_size);
    auto page = std::vector<std::byte>(page_size);

    // A dump of an 8 GiB zram device with 2 GiB in use is mostly holes when written with conv=sparse.
    // SEEK_DATA jumps over them; on a file system without hole support it just returns the offset.
    auto pos = off_t{0};
    while (pos < size) {
        auto const data = ::lseek(fd, pos, SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) {
                break; // only holes left
            }
            throw_errno("SEEK_DATA in " + dump.string());
        }
        auto hole = ::lseek(fd, data, SEEK_HOLE);
        if (hole < 0) {
            throw_errno("SEEK_HOLE in " + dump.string());
        }
        // holes are reported at file system block granularity, so round to whole pages
        auto const ps = static_cast<off_t>(page_size);
        pos = data / ps * ps;
        hole = std::min<off_t>((hole + ps - 1) / ps * ps, size);
        for (; pos < hole; pos += ps) {
            auto const got = ::pread(fd, page.data(), page_size, pos);
            if (got != static_cast<ssize_t>(page_size)) {
                throw_errno("cannot read " + dump.string());
            }
            if (std::all_of(page.begin(), page.end(), [](std::byte b) {
                    return b == std::byte{0};
                })) {
                ++result.zero_pages;
                continue;
            }
            out.write(0, name, "", static_cast<std::uintptr_t>(pos), page);
        }
    }
    result.pages_written = out.pages_written();
    return result;
}

} // namespace quetschn
