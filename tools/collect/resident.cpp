// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "resident.h"

#include "page_stats.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace quetschn {

namespace {

// Bits of a /proc/<pid>/pagemap entry, see Documentation/admin-guide/mm/pagemap.rst. Without
// CAP_SYS_ADMIN the PFN bits read as 0, but these flags are still there.
constexpr auto pm_present = std::uint64_t{1} << 63U;
constexpr auto pm_swapped = std::uint64_t{1} << 62U;

[[noreturn]] void throw_errno(std::string const& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

std::string read_text(std::filesystem::path const& path) {
    auto in = std::ifstream(path);
    if (!in) {
        throw_errno("cannot read " + path.string());
    }
    auto ss = std::ostringstream();
    ss << in.rdbuf();
    return ss.str();
}

std::string_view next_field(std::string_view& line) {
    auto const begin = line.find_first_not_of(' ');
    if (begin == std::string_view::npos) {
        line = {};
        return {};
    }
    line.remove_prefix(begin);
    auto const end = std::min(line.find(' '), line.size());
    auto field = line.substr(0, end);
    line.remove_prefix(end);
    return field;
}

template <typename T>
T parse_number(std::string_view s, int base) {
    T value{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        throw std::runtime_error("parse_maps: bad number '" + std::string(s) + "'");
    }
    return value;
}

// The TSV must stay one line per page with a fixed column count, so tabs and newlines in names go.
std::string sanitize(std::string_view s) {
    auto out = std::string(s);
    std::replace_if(
        out.begin(),
        out.end(),
        [](char c) {
            return c == '\t' || c == '\n' || c == '\r';
        },
        ' ');
    return out;
}

std::FILE* create_private(std::filesystem::path const& path) {
    auto fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw_errno("cannot create " + path.string());
    }
    auto* f = ::fdopen(fd, "wb");
    if (f == nullptr) {
        ::close(fd);
        throw_errno("fdopen " + path.string());
    }
    return f;
}

enum class scan_result { ok, unsupported, failed };

// Calls on_page(address, swapped) for every present or swapped page of the mapping.
template <typename F>
scan_result scan_mapping([[maybe_unused]] int pagemap_fd,
                         [[maybe_unused]] mapping const& m,
                         [[maybe_unused]] std::size_t page_size,
                         [[maybe_unused]] F&& on_page) {
#ifdef PAGEMAP_SCAN
    auto regions = std::array<page_region, 512>{};
    auto start = static_cast<std::uint64_t>(m.start);
    while (start < m.end) {
        auto arg = pm_scan_arg{};
        arg.size = sizeof(arg);
        arg.start = start;
        arg.end = m.end;
        arg.vec = reinterpret_cast<std::uint64_t>(regions.data());
        arg.vec_len = regions.size();
        arg.category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED;
        arg.return_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED;
        auto const n = ::ioctl(pagemap_fd, PAGEMAP_SCAN, &arg);
        if (n < 0) {
            // ENOTTY: the kernel does not know the ioctl. EINVAL: it knows it, but not this struct size.
            return (errno == ENOTTY || errno == EINVAL) ? scan_result::unsupported : scan_result::failed;
        }
        for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
            auto const swapped = (regions[i].categories & PAGE_IS_SWAPPED) != 0;
            for (auto a = regions[i].start; a < regions[i].end; a += page_size) {
                on_page(static_cast<std::uintptr_t>(a), swapped);
            }
        }
        // The walk stops early when the region buffer is full, and continues from walk_end.
        if (arg.walk_end <= start) {
            break;
        }
        start = arg.walk_end;
    }
    return scan_result::ok;
#else
    return scan_result::unsupported;
#endif
}

// Same as scan_mapping, by reading one pagemap entry per page. Returns false if the read fails.
// Mappings can be huge reservations with few resident pages, so this reads in chunks instead of one
// buffer per mapping.
template <typename F>
bool read_mapping(int pagemap_fd, mapping const& m, std::size_t page_size, F&& on_page) {
    constexpr std::size_t chunk_pages = 64 * 1024;
    auto entries = std::vector<std::uint64_t>(chunk_pages);
    auto const num_pages = (m.end - m.start) / page_size;
    for (std::size_t first = 0; first < num_pages; first += chunk_pages) {
        auto const n = std::min(chunk_pages, num_pages - first);
        auto const offset = static_cast<off_t>((m.start / page_size + first) * sizeof(std::uint64_t));
        auto const got = ::pread(pagemap_fd, entries.data(), n * sizeof(std::uint64_t), offset);
        if (got < 0) {
            return false;
        }
        // The mapping can shrink while we look at it. What was not returned counts as not resident.
        auto const valid = static_cast<std::size_t>(got) / sizeof(std::uint64_t);
        for (std::size_t i = 0; i < valid; ++i) {
            auto const e = entries[i];
            if ((e & pm_swapped) != 0) {
                on_page(m.start + (first + i) * page_size, true);
            } else if ((e & pm_present) != 0) {
                on_page(m.start + (first + i) * page_size, false);
            }
        }
        if (valid < n) {
            break;
        }
    }
    return true;
}

} // namespace

std::vector<mapping> parse_maps(std::string_view text) {
    // "start-end perms offset dev inode   name", where name may contain spaces
    auto result = std::vector<mapping>();
    while (!text.empty()) {
        auto const eol = std::min(text.find('\n'), text.size());
        auto line = text.substr(0, eol);
        text.remove_prefix(std::min(eol + 1, text.size()));
        if (line.empty()) {
            continue;
        }

        auto m = mapping{};
        auto range = next_field(line);
        auto const dash = range.find('-');
        if (dash == std::string_view::npos) {
            throw std::runtime_error("parse_maps: bad range '" + std::string(range) + "'");
        }
        m.start = parse_number<std::uintptr_t>(range.substr(0, dash), 16);
        m.end = parse_number<std::uintptr_t>(range.substr(dash + 1), 16);
        m.perms = std::string(next_field(line));
        next_field(line); // offset
        next_field(line); // dev
        m.inode = parse_number<std::uint64_t>(next_field(line), 10);
        auto const name_begin = line.find_first_not_of(' ');
        if (name_begin != std::string_view::npos) {
            m.name = std::string(line.substr(name_begin));
        }
        if (m.perms.size() != 4 || m.end <= m.start) {
            throw std::runtime_error("parse_maps: bad line");
        }
        result.push_back(std::move(m));
    }
    return result;
}

bool is_anonymous_private(mapping const& m) {
    if (m.perms[0] != 'r' || m.perms[3] != 'p') {
        return false;
    }
    // Android names anonymous memory, e.g. "[anon:dalvik-main space]", and so does
    // prctl(PR_SET_VMA_ANON_NAME) on Linux.
    return m.name.empty() || m.name == "[heap]" || m.name.starts_with("[stack") || m.name.starts_with("[anon:");
}

corpus_writer::corpus_writer(std::filesystem::path const& base, std::size_t page_size)
    : m_page_size(page_size) {
    auto pages_path = base;
    pages_path += ".pages";
    auto tsv_path = base;
    tsv_path += ".tsv";
    m_pages = create_private(pages_path);
    try {
        m_tsv = create_private(tsv_path);
    } catch (...) {
        std::fclose(m_pages);
        throw;
    }
    std::fprintf(m_tsv, "# page_size=%zu\n", m_page_size);
    std::fprintf(m_tsv, "page\tpid\tcomm\taddress\tzero_bytes\tsame_filled\tfill_value\tentropy\tmapping\n");
}

corpus_writer::~corpus_writer() {
    std::fclose(m_pages);
    std::fclose(m_tsv);
}

void corpus_writer::write(
    pid_t pid, std::string_view comm, std::string_view mapping_name, std::uintptr_t address, std::span<std::byte const> page) {
    if (page.size() != m_page_size) {
        throw std::invalid_argument("corpus_writer::write: wrong page size");
    }
    if (std::fwrite(page.data(), 1, page.size(), m_pages) != page.size()) {
        throw_errno("write pages");
    }
    auto const s = analyze_page(page);
    auto const c = sanitize(comm);
    auto const n = sanitize(mapping_name);
    std::fprintf(m_tsv,
                 "%zu\t%d\t%s\t%jx\t%zu\t%d\t%016jx\t%.4f\t%s\n",
                 m_pages_written,
                 static_cast<int>(pid),
                 c.c_str(),
                 static_cast<std::uintmax_t>(address),
                 s.zero_bytes,
                 s.same_filled ? 1 : 0,
                 static_cast<std::uintmax_t>(s.fill_value),
                 s.entropy_bits,
                 n.c_str());
    ++m_pages_written;
}

std::size_t corpus_writer::pages_written() const {
    return m_pages_written;
}

std::size_t corpus_writer::page_size() const {
    return m_page_size;
}

collect_result collect_process(pid_t pid, collect_options const& opts, corpus_writer& out) {
    auto const proc = std::filesystem::path("/proc") / std::to_string(pid);
    auto const page_size = out.page_size();

    auto comm = read_text(proc / "comm");
    if (!comm.empty() && comm.back() == '\n') {
        comm.pop_back();
    }
    auto const maps = parse_maps(read_text(proc / "maps"));

    auto pagemap_fd = ::open((proc / "pagemap").c_str(), O_RDONLY | O_CLOEXEC);
    if (pagemap_fd < 0) {
        throw_errno("cannot open " + (proc / "pagemap").string());
    }

    // Find resident pages first. Reading a page that is not resident would fault it in, and for a
    // swapped page that means swapping it back in, which changes the system that is being measured.
    struct candidate {
        std::uintptr_t address;
        std::size_t mapping_index;
    };
    auto result = collect_result{};
    auto candidates = std::vector<candidate>();
    auto rng = std::mt19937_64(opts.seed ^ static_cast<std::uint64_t>(pid));
    auto add_candidate = [&](candidate c) {
        ++result.resident;
        if (opts.max_pages_per_process == 0 || candidates.size() < opts.max_pages_per_process) {
            candidates.push_back(c);
            return;
        }
        // Reservoir sampling: a uniform sample without holding every resident page of a 100 GB process.
        auto const j = std::uniform_int_distribution<std::size_t>(0, result.resident - 1)(rng);
        if (j < candidates.size()) {
            candidates[j] = c;
        }
    };

    // PAGEMAP_SCAN (Linux 6.7) only reports populated ranges. Reading pagemap directly returns one entry
    // per page of address space, which takes minutes for a sanitizer's 15 TB shadow mapping. Older
    // kernels, e.g. on phones, only have the direct read.
    auto use_scan = opts.use_pagemap_scan;
    for (std::size_t mi = 0; mi < maps.size(); ++mi) {
        auto const& m = maps[mi];
        if (!is_anonymous_private(m)) {
            continue;
        }
        auto on_page = [&](std::uintptr_t address, bool swapped) {
            if (swapped) {
                ++result.swapped;
            } else {
                add_candidate({address, mi});
            }
        };
        if (use_scan) {
            auto const r = scan_mapping(pagemap_fd, m, page_size, on_page);
            if (r == scan_result::ok) {
                result.used_pagemap_scan = true;
                continue;
            }
            if (r == scan_result::failed) {
                ::close(pagemap_fd);
                throw_errno("PAGEMAP_SCAN " + (proc / "pagemap").string());
            }
            use_scan = false; // not supported, fall through to the direct read for this and all others
        }
        if (!read_mapping(pagemap_fd, m, page_size, on_page)) {
            ::close(pagemap_fd);
            throw_errno("cannot read " + (proc / "pagemap").string());
        }
    }
    ::close(pagemap_fd);
    std::sort(candidates.begin(), candidates.end(), [](candidate const& a, candidate const& b) {
        return a.address < b.address;
    });

    // One page per call. Batching would be faster, but process_vm_readv() stops at the first page
    // that is gone, and the corpus is collected once, so simple wins.
    auto buf = std::vector<std::byte>(page_size);
    for (auto const& c : candidates) {
        auto local = iovec{buf.data(), page_size};
        auto remote = iovec{reinterpret_cast<void*>(c.address), page_size};
        auto const got = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (got != static_cast<ssize_t>(page_size)) {
            if (got < 0 && errno == EPERM) {
                throw_errno("process_vm_readv pid " + std::to_string(pid));
            }
            if (got < 0 && errno == ESRCH) {
                // the process exited, keep what we have
                result.read_failed += static_cast<std::size_t>(&candidates.back() - &c) + 1;
                break;
            }
            ++result.read_failed;
            continue;
        }
        out.write(pid, comm, maps[c.mapping_index].name, c.address, buf);
        ++result.written;
    }
    return result;
}

} // namespace quetschn
