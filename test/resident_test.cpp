// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "resident.h"

#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

using quetschn::collect_options;
using quetschn::collect_process;
using quetschn::corpus_writer;
using quetschn::is_anonymous_private;
using quetschn::parse_maps;

TEST_CASE("resident: parse_maps and the anonymous private filter") {
    auto const text =
        std::string("5581a1c00000-5581a1c21000 rw-p 00000000 00:00 0                          [heap]\n"
                    "7f0000000000-7f0000004000 rw-p 00000000 00:00 0 \n"
                    "7f0000004000-7f0000008000 r--p 00000000 00:00 0\n"
                    "7f0000008000-7f000000c000 ---p 00000000 00:00 0\n"
                    "7f000000c000-7f0000010000 rw-s 00000000 00:01 1234                       /dev/zero (deleted)\n"
                    "7f0000010000-7f0000014000 r--p 00001000 fd:01 5678                       /usr/lib/lib c.so\n"
                    "7f0000014000-7f0000018000 rw-p 00000000 00:00 0                          [anon:dalvik-main space]\n"
                    "7f0000018000-7f000001c000 rw-s 00000000 00:00 0                          [anon:shared]\n"
                    "7ffd00000000-7ffd00021000 rw-p 00000000 00:00 0                          [stack]\n"
                    "7ffd00100000-7ffd00102000 r--p 00000000 00:00 0                          [vvar]\n"
                    "7ffd00102000-7ffd00104000 r-xp 00000000 00:00 0                          [vdso]\n");
    auto const maps = parse_maps(text);
    REQUIRE(maps.size() == 11);

    CHECK(maps[0].start == 0x5581a1c00000);
    CHECK(maps[0].end == 0x5581a1c21000);
    CHECK(maps[0].perms == "rw-p");
    CHECK(maps[0].name == "[heap]");
    CHECK(maps[1].name.empty());
    CHECK(maps[4].inode == 1234);
    CHECK(maps[5].name == "/usr/lib/lib c.so");
    CHECK(maps[6].name == "[anon:dalvik-main space]");

    auto const expected = std::array{true, true, true, false, false, false, true, false, true, false, false};
    for (std::size_t i = 0; i < maps.size(); ++i) {
        CAPTURE(maps[i].name);
        CAPTURE(maps[i].perms);
        CHECK(is_anonymous_private(maps[i]) == expected[i]);
    }

    CHECK_THROWS((void)parse_maps("garbage\n"));
}

namespace {

constexpr std::size_t touched_pages = 8;
constexpr std::size_t untouched_pages = 4;
// Larger than the chunk the collector reads /proc/<pid>/pagemap in, so pages in later chunks are
// found too. Only a few pages are touched, the rest is just address space.
constexpr std::size_t big_pages = 200000;
constexpr std::size_t paged_out_pages = 4;
// Every other page touched, so PAGEMAP_SCAN reports ~1000 separate regions for this one mapping. That is
// more than its region buffer holds, so the scan has to continue where it stopped.
constexpr std::size_t striped_pages = 2000;
constexpr auto big_touched = std::array<std::size_t, 4>{0, 70000, 150000, big_pages - 1};

// Page i of the touched region: every byte is (i * 37 + offset * 11) % 251 + 1, which is never 0 and
// different for every page, so each one can be found in the corpus.
std::byte pattern(std::size_t page, std::size_t offset) {
    return static_cast<std::byte>((page * 37 + offset * 11) % 251 + 1);
}

bool has_swap() {
    auto in = std::ifstream("/proc/swaps");
    auto line = std::string();
    std::size_t lines = 0;
    while (std::getline(in, line)) {
        ++lines;
    }
    return lines > 1; // the first line is the header
}

std::size_t count_swapped(pid_t pid, std::uintptr_t start, std::size_t num_pages, std::size_t page_size) {
    // pread and not std::ifstream: pagemap_read() fails with EINVAL unless offset and size are multiples
    // of 8, and ifstream's buffered reads are not. That silently read 0 swapped pages here once.
    auto const path = "/proc/" + std::to_string(pid) + "/pagemap";
    auto const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    std::size_t n = 0;
    for (std::size_t i = 0; i < num_pages; ++i) {
        std::uint64_t e = 0;
        auto const offset = static_cast<off_t>((start / page_size + i) * sizeof(e));
        REQUIRE(::pread(fd, &e, sizeof(e), offset) == static_cast<ssize_t>(sizeof(e)));
        n += (e >> 62U) & 1U;
    }
    ::close(fd);
    return n;
}

struct addresses {
    std::uintptr_t touched;
    std::uintptr_t untouched;
    std::uintptr_t shared;
    std::uintptr_t big;
    std::uintptr_t paged_out;
    std::uintptr_t striped;
    bool paged_out_ok; // madvise(MADV_PAGEOUT) succeeded
};

// A child process with three regions: private pages with a known pattern, private pages that were
// never touched and so are not resident, and shared anonymous pages that zram never sees. It blocks
// until the parent closes the pipe.
class child_process {
public:
    child_process() {
        int to_parent[2];
        int to_child[2];
        REQUIRE(::pipe(to_parent) == 0);
        REQUIRE(::pipe(to_child) == 0);
        m_page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        m_pid = ::fork();
        REQUIRE(m_pid >= 0);
        if (m_pid == 0) {
            ::close(to_parent[0]);
            ::close(to_child[1]);
            run_child(to_parent[1], to_child[0]);
        }
        ::close(to_parent[1]);
        ::close(to_child[0]);
        m_release = to_child[1];
        REQUIRE(::read(to_parent[0], &m_addr, sizeof(m_addr)) == static_cast<ssize_t>(sizeof(m_addr)));
        ::close(to_parent[0]);
    }

    ~child_process() {
        ::close(m_release);
        int status = 0;
        ::waitpid(m_pid, &status, 0);
    }

    child_process(child_process const&) = delete;
    child_process& operator=(child_process const&) = delete;

    [[nodiscard]] pid_t pid() const {
        return m_pid;
    }
    [[nodiscard]] addresses const& addr() const {
        return m_addr;
    }
    [[nodiscard]] std::size_t page_size() const {
        return m_page_size;
    }

private:
    [[noreturn]] void run_child(int to_parent, int to_child) const {
        auto* touched = static_cast<std::byte*>(
            ::mmap(nullptr, touched_pages * m_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        auto* untouched =
            ::mmap(nullptr, untouched_pages * m_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        auto* shared =
            static_cast<std::byte*>(::mmap(nullptr, m_page_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        auto* big = static_cast<std::byte*>(::mmap(
            nullptr, big_pages * m_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
        auto* paged_out = static_cast<std::byte*>(
            ::mmap(nullptr, paged_out_pages * m_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        auto* striped = static_cast<std::byte*>(
            ::mmap(nullptr, striped_pages * m_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (touched == MAP_FAILED || untouched == MAP_FAILED || shared == MAP_FAILED || big == MAP_FAILED ||
            paged_out == MAP_FAILED || striped == MAP_FAILED) {
            ::_exit(1);
        }
        for (std::size_t p = 0; p < touched_pages; ++p) {
            for (std::size_t i = 0; i < m_page_size; ++i) {
                touched[p * m_page_size + i] = pattern(p, i);
            }
        }
        std::memset(shared, 0x5a, m_page_size);
        for (std::size_t p = 0; p < striped_pages; p += 2) {
            striped[p * m_page_size] = std::byte{1};
        }
        std::memset(paged_out, 0x77, paged_out_pages * m_page_size);
        // Pushes the pages to swap right away, if there is swap. Needs Linux 5.4.
        auto const paged_out_ok = ::madvise(paged_out, paged_out_pages * m_page_size, MADV_PAGEOUT) == 0;
        for (auto p : big_touched) {
            for (std::size_t i = 0; i < m_page_size; ++i) {
                big[p * m_page_size + i] = pattern(p + touched_pages, i);
            }
        }
        auto const a = addresses{reinterpret_cast<std::uintptr_t>(touched),
                                 reinterpret_cast<std::uintptr_t>(untouched),
                                 reinterpret_cast<std::uintptr_t>(shared),
                                 reinterpret_cast<std::uintptr_t>(big),
                                 reinterpret_cast<std::uintptr_t>(paged_out),
                                 reinterpret_cast<std::uintptr_t>(striped),
                                 paged_out_ok};
        if (::write(to_parent, &a, sizeof(a)) != static_cast<ssize_t>(sizeof(a))) {
            ::_exit(1);
        }
        char c = 0;
        while (::read(to_child, &c, 1) > 0) {
        }
        ::_exit(0);
    }

    pid_t m_pid = -1;
    int m_release = -1;
    addresses m_addr{};
    std::size_t m_page_size = 0;
};

class temp_dir {
public:
    temp_dir() {
        auto tmpl = (std::filesystem::temp_directory_path() / "quetschn-test-XXXXXX").string();
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

struct tsv_row {
    std::uintptr_t address;
    int same_filled;
};

// Reads the corpus back, the way a consumer of the files would.
struct corpus {
    std::size_t page_size = 0;
    std::vector<std::byte> pages;
    std::vector<tsv_row> rows;

    explicit corpus(std::filesystem::path const& base) {
        auto pages_path = base;
        pages_path += ".pages";
        auto tsv_path = base;
        tsv_path += ".tsv";

        auto in = std::ifstream(pages_path, std::ios::binary);
        auto const raw = std::string(std::istreambuf_iterator<char>(in), {});
        pages.resize(raw.size());
        std::memcpy(pages.data(), raw.data(), raw.size());

        auto tsv = std::ifstream(tsv_path);
        auto line = std::string();
        REQUIRE(std::getline(tsv, line));
        REQUIRE(line.starts_with("# page_size="));
        page_size = std::stoul(line.substr(12));
        REQUIRE(std::getline(tsv, line)); // column names
        while (std::getline(tsv, line)) {
            // split by hand: the last column is empty for unnamed mappings, and getline would drop it
            auto fields = std::vector<std::string>(1);
            for (auto ch : line) {
                if (ch == '\t') {
                    fields.emplace_back();
                } else {
                    fields.back() += ch;
                }
            }
            REQUIRE(fields.size() == 9);
            rows.push_back({static_cast<std::uintptr_t>(std::stoull(fields[3], nullptr, 16)), std::stoi(fields[5])});
        }
        REQUIRE(rows.size() * page_size == pages.size());
    }

    [[nodiscard]] std::byte const* page_at(std::uintptr_t address) const {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].address == address) {
                return pages.data() + i * page_size;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t count_in(std::uintptr_t start, std::size_t num_pages) const {
        std::size_t n = 0;
        for (auto const& r : rows) {
            if (r.address >= start && r.address < start + num_pages * page_size) {
                ++n;
            }
        }
        return n;
    }
};

} // namespace

// Runs the test body once with PAGEMAP_SCAN and once with the direct pagemap read. Returns the options
// for the current subcase.
collect_options both_scan_methods() {
    auto opts = collect_options{};
    SUBCASE("PAGEMAP_SCAN") {
        opts.use_pagemap_scan = true;
    }
    SUBCASE("direct pagemap read") {
        opts.use_pagemap_scan = false;
    }
    return opts;
}

#if defined(__SANITIZE_ADDRESS__)
constexpr bool asan = true;
#elif defined(__has_feature)
#    if __has_feature(address_sanitizer)
constexpr bool asan = true;
#    else
constexpr bool asan = false;
#    endif
#else
constexpr bool asan = false;
#endif

// The direct read produces one pagemap entry per page of address space. Under ASan that includes a 15 TB
// shadow mapping, which takes minutes.
bool direct_read_too_slow(collect_options const& opts) {
    if (asan && !opts.use_pagemap_scan) {
        MESSAGE("skipped under ASan: the direct pagemap read would walk the whole shadow mapping");
        return true;
    }
    return false;
}

// PAGEMAP_SCAN exists since Linux 6.7
bool kernel_has_pagemap_scan() {
    auto u = utsname{};
    REQUIRE(::uname(&u) == 0);
    int major = 0;
    int minor = 0;
    REQUIRE(std::sscanf(u.release, "%d.%d", &major, &minor) == 2);
    return major > 6 || (major == 6 && minor >= 7);
}

TEST_CASE("resident: collects exactly the resident private anonymous pages of a process") {
    auto const opts = both_scan_methods();
    if (direct_read_too_slow(opts)) {
        return;
    }
    auto child = child_process();
    auto dir = temp_dir();
    auto const base = dir.path() / "corpus";

    auto result = quetschn::collect_result{};
    {
        auto writer = corpus_writer(base, child.page_size());
        result = collect_process(child.pid(), opts, writer);
        CHECK(writer.pages_written() == result.written);
    }
    CHECK(result.used_pagemap_scan == (opts.use_pagemap_scan && kernel_has_pagemap_scan()));
    CHECK(result.written >= touched_pages);
    CHECK(result.resident == result.written + result.read_failed);

    auto const c = corpus(base);
    CHECK(c.page_size == child.page_size());
    CHECK(c.rows.size() == result.written);

    // every touched page is there, with its content
    for (std::size_t p = 0; p < touched_pages; ++p) {
        CAPTURE(p);
        auto const* page = c.page_at(child.addr().touched + p * child.page_size());
        REQUIRE(page != nullptr);
        bool same = true;
        for (std::size_t i = 0; i < child.page_size(); ++i) {
            same = same && page[i] == pattern(p, i);
        }
        CHECK(same);
    }
    CHECK(c.count_in(child.addr().untouched, untouched_pages) == 0);

    CHECK(c.count_in(child.addr().striped, striped_pages) == striped_pages / 2);

    // the big mapping: exactly the touched pages, also those after the first pagemap chunk
    CHECK(c.count_in(child.addr().big, big_pages) == big_touched.size());
    for (auto p : big_touched) {
        CAPTURE(p);
        auto const* page = c.page_at(child.addr().big + p * child.page_size());
        REQUIRE(page != nullptr);
        bool same = true;
        for (std::size_t i = 0; i < child.page_size(); ++i) {
            same = same && page[i] == pattern(p + touched_pages, i);
        }
        CHECK(same);
    }
    CHECK(c.count_in(child.addr().shared, 1) == 0);
}

TEST_CASE("resident: swapped pages are counted, not read, and stay in swap") {
    auto const opts = both_scan_methods();
    if (direct_read_too_slow(opts)) {
        return;
    }
    if (!has_swap()) {
        MESSAGE("no swap on this machine, the test cannot push pages out");
        return;
    }
    auto child = child_process();
    REQUIRE(child.addr().paged_out_ok);
    auto const page_size = child.page_size();
    auto const swapped_before = count_swapped(child.pid(), child.addr().paged_out, paged_out_pages, page_size);
    // MADV_PAGEOUT is a request, reclaim may keep a page. At least one has to go out for this to test anything.
    REQUIRE(swapped_before > 0);

    auto dir = temp_dir();
    auto const base = dir.path() / "corpus";
    auto result = quetschn::collect_result{};
    {
        auto writer = corpus_writer(base, page_size);
        result = collect_process(child.pid(), opts, writer);
    }
    CHECK(result.swapped >= swapped_before);
    CHECK(corpus(base).count_in(child.addr().paged_out, paged_out_pages) == paged_out_pages - swapped_before);
    CHECK(count_swapped(child.pid(), child.addr().paged_out, paged_out_pages, page_size) == swapped_before);
}

TEST_CASE("resident: max_pages_per_process takes a sample") {
    auto child = child_process();
    auto dir = temp_dir();
    auto const base = dir.path() / "corpus";

    auto result = quetschn::collect_result{};
    {
        auto writer = corpus_writer(base, child.page_size());
        result = collect_process(child.pid(), collect_options{.max_pages_per_process = 3}, writer);
    }
    CHECK(result.resident > 3);
    CHECK(result.written == 3);

    auto const c = corpus(base);
    REQUIRE(c.rows.size() == 3);
    CHECK(c.rows[0].address < c.rows[1].address);
    CHECK(c.rows[1].address < c.rows[2].address);
}

TEST_CASE("resident: the sample depends on the seed and covers the whole process") {
    // With 3 of many resident pages per run, 20 seeds must pick more than 3 different pages. A sampler
    // that ignores the seed, or always takes the first pages it finds, picks the same 3 every time.
    auto child = child_process();
    auto dir = temp_dir();
    auto seen = std::vector<std::uintptr_t>();
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        auto const base = dir.path() / ("corpus" + std::to_string(seed));
        {
            auto writer = corpus_writer(base, child.page_size());
            (void)collect_process(child.pid(), collect_options{.max_pages_per_process = 3, .seed = seed}, writer);
        }
        for (auto const& r : corpus(base).rows) {
            if (std::find(seen.begin(), seen.end(), r.address) == seen.end()) {
                seen.push_back(r.address);
            }
        }
    }
    CHECK(seen.size() > 10);
}

TEST_CASE("resident: corpus files are private and never overwritten") {
    auto dir = temp_dir();
    auto const base = dir.path() / "corpus";
    {
        auto writer = corpus_writer(base, 4096);
    }
    for (auto const* ext : {".pages", ".tsv"}) {
        auto path = base;
        path += ext;
        struct stat st{};
        REQUIRE(::stat(path.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0600);
    }
    CHECK_THROWS_AS(corpus_writer(base, 4096), std::system_error);
}

TEST_CASE("resident: same-filled pages are flagged in the TSV") {
    auto dir = temp_dir();
    auto const base = dir.path() / "corpus";
    {
        auto writer = corpus_writer(base, 4096);
        auto page = std::vector<std::byte>(4096, std::byte{0x42});
        writer.write(1, "a", "", 0x1000, page);
        page[100] = std::byte{0};
        writer.write(1, "a", "", 0x2000, page);
    }
    auto const c = corpus(base);
    REQUIRE(c.rows.size() == 2);
    CHECK(c.rows[0].same_filled == 1);
    CHECK(c.rows[1].same_filled == 0);
}

TEST_CASE("resident: a process that cannot be read throws") {
    auto dir = temp_dir();
    auto writer = corpus_writer(dir.path() / "corpus", 4096);
    // pid 0 has no /proc entry
    CHECK_THROWS_AS(collect_process(0, collect_options{}, writer), std::system_error);
}
