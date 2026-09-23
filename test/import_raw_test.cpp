// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "harness.h"
#include "import_raw.h"

#include <doctest/doctest.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr std::size_t page_size = 4096;

class temp_dir {
public:
    temp_dir() {
        auto tmpl = (std::filesystem::temp_directory_path() / "quetschn-import-XXXXXX").string();
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

std::vector<std::byte> page_with(unsigned char value) {
    auto p = std::vector<std::byte>(page_size);
    for (std::size_t i = 0; i < page_size; i += 7) {
        p[i] = std::byte{value};
    }
    return p;
}

// a sparse file like `dd conv=sparse` writes: 128 pages, data at pages 0, 3 and 100, an explicitly written
// zero page at 5, holes everywhere else
void write_dump(std::filesystem::path const& path) {
    auto const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, 128 * page_size) == 0);
    auto put = [&](std::size_t index, std::vector<std::byte> const& p) {
        REQUIRE(::pwrite(fd, p.data(), page_size, static_cast<off_t>(index * page_size)) == static_cast<ssize_t>(page_size));
    };
    put(0, page_with(1));
    put(3, page_with(3));
    put(5, std::vector<std::byte>(page_size));
    put(100, page_with(100));
    ::close(fd);
}

// the address column of the TSV, in order
std::vector<std::uintptr_t> addresses(std::filesystem::path const& base) {
    auto tsv_path = base;
    tsv_path += ".tsv";
    auto in = std::ifstream(tsv_path);
    auto line = std::string();
    std::getline(in, line);
    std::getline(in, line);
    auto result = std::vector<std::uintptr_t>();
    while (std::getline(in, line)) {
        // page, pid, comm, address
        auto pos = std::size_t{0};
        for (int i = 0; i < 3; ++i) {
            pos = line.find('\t', pos) + 1;
        }
        result.push_back(std::stoull(line.substr(pos, line.find('\t', pos) - pos), nullptr, 16));
    }
    return result;
}

} // namespace

TEST_CASE("import_raw: every non-zero page of a sparse dump, at its offset") {
    auto dir = temp_dir();
    write_dump(dir.path() / "zram0.raw");
    auto const r = quetschn::import_raw(dir.path() / "zram0.raw", dir.path() / "c", "zram0", page_size);

    CHECK(r.pages_written == 3);
    auto const c = quetschn::load_corpus(dir.path() / "c");
    REQUIRE(c.size() == 3);
    CHECK(std::memcmp(c.page(0).data(), page_with(1).data(), page_size) == 0);
    CHECK(std::memcmp(c.page(1).data(), page_with(3).data(), page_size) == 0);
    CHECK(std::memcmp(c.page(2).data(), page_with(100).data(), page_size) == 0);
    CHECK(addresses(dir.path() / "c") == std::vector<std::uintptr_t>{0, 3 * page_size, 100 * page_size});

    // The written zero page is read and skipped. The holes are not read at all, otherwise all 125 other
    // pages would be counted here. Some file systems allocate a little around written blocks, so allow a few.
    CHECK(r.zero_pages >= 1);
    CHECK(r.zero_pages < 10);
}

TEST_CASE("import_raw: a dump that is not a whole number of pages is an error") {
    auto dir = temp_dir();
    std::ofstream(dir.path() / "bad.raw") << "not a page";
    CHECK_THROWS_WITH_AS((void)quetschn::import_raw(dir.path() / "bad.raw", dir.path() / "c", "x", page_size),
                         doctest::Contains("whole number"),
                         std::runtime_error);
}

TEST_CASE("import_raw: a dump that is all holes gives an empty corpus") {
    auto dir = temp_dir();
    auto const path = dir.path() / "empty.raw";
    auto const fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, 64 * page_size) == 0);
    ::close(fd);
    auto const r = quetschn::import_raw(path, dir.path() / "c", "x", page_size);
    CHECK(r.pages_written == 0);
    CHECK(quetschn::load_corpus(dir.path() / "c").size() == 0);
}
