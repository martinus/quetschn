// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// The kernel's lz4 and lzo, built in userspace from QUETSCHN_KERNEL_TREE. Only compiled when that is set.

#include "kernel_codecs/zram_codec.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t page_size = 4096;

using page = std::vector<std::uint8_t>;

page random_page(std::uint64_t seed) {
    auto rng = std::mt19937_64(seed);
    auto p = page(page_size);
    for (auto& b : p) {
        b = static_cast<std::uint8_t>(rng());
    }
    return p;
}

// like a heap page: 64-bit words, many of them pointers into the same region, some small integers
page pointer_page() {
    auto rng = std::mt19937_64(7);
    auto p = page(page_size);
    for (std::size_t i = 0; i < page_size; i += 8) {
        std::uint64_t w = 0;
        switch (rng() % 4) {
        case 0:
            w = 0x00007f3a12000000ULL + (rng() % 0x100000) * 16;
            break;
        case 1:
            w = rng() % 100;
            break;
        case 2:
            w = 0;
            break;
        default:
            w = rng();
            break;
        }
        std::memcpy(&p[i], &w, 8);
    }
    return p;
}

page text_page() {
    auto const words = std::vector<std::string>{"page ", "zram ", "compress ", "the ", "kernel ", "swap ", "memory "};
    auto rng = std::mt19937_64(3);
    auto p = page();
    while (p.size() < page_size) {
        auto const& w = words[rng() % words.size()];
        p.insert(p.end(), w.begin(), w.end());
    }
    p.resize(page_size);
    return p;
}

// long zero runs, which is what lzo-rle was made for
page zero_runs_page() {
    auto p = page(page_size);
    for (std::size_t i = 0; i < page_size; i += 512) {
        p[i] = static_cast<std::uint8_t>(i / 512 + 1);
    }
    return p;
}

std::vector<page> test_pages() {
    return {random_page(1), random_page(2), pointer_page(), text_page(), zero_runs_page()};
}

std::vector<quetschn_codec const*> codecs() {
    return {&quetschn_codec_lz4, &quetschn_codec_lzo, &quetschn_codec_lzo_rle, &quetschn_codec_zstd};
}

// A zram stream: workspace allocated and zeroed once, then set up by the codec.
class stream {
public:
    explicit stream(quetschn_codec const& codec, int level = QUETSCHN_LEVEL_DEFAULT)
        : m_codec(codec) {
        m_s.level = level;
        m_s.workspace_size = codec.workspace_size(&m_s.level, page_size);
        REQUIRE(m_s.workspace_size > 0);
        m_workspace.resize(m_s.workspace_size + 64);
        auto const p = reinterpret_cast<std::uintptr_t>(m_workspace.data());
        m_s.workspace = m_workspace.data() + (64 - p % 64) % 64;
        REQUIRE(codec.init(&m_s, page_size) == 0);
    }

    page compress(page const& src) {
        auto dst = page(2 * page_size);
        auto len = static_cast<unsigned int>(dst.size());
        REQUIRE(m_codec.compress(&m_s, src.data(), static_cast<unsigned int>(src.size()), dst.data(), &len) == 0);
        dst.resize(len);
        return dst;
    }

    // returns the codec's result, out holds the decompressed bytes
    int decompress(page const& src, page& out) {
        out.assign(page_size, 0);
        auto len = static_cast<unsigned int>(out.size());
        auto const ret = m_codec.decompress(&m_s, src.data(), static_cast<unsigned int>(src.size()), out.data(), &len);
        out.resize(len);
        return ret;
    }

    [[nodiscard]] quetschn_stream const& raw() const {
        return m_s;
    }

private:
    quetschn_codec const& m_codec;
    quetschn_stream m_s{};
    std::vector<std::uint8_t> m_workspace;
};

page compress(quetschn_codec const& codec, page const& src) {
    return stream(codec).compress(src);
}

// LZ4 block format, decoded straight from the format description (lz4_Block_format.md): a token with
// literal length (high nibble) and match length - 4 (low nibble), 15 meaning "more length bytes follow",
// the literals, then a 2 byte little-endian offset. The last sequence has literals only. Independent of
// the kernel's decoder, so it checks that the userspace build of the kernel's compressor writes real LZ4.
page lz4_reference_decode(page const& in) {
    auto out = page();
    std::size_t pos = 0;
    auto length = [&](std::size_t n) {
        if (n == 15) {
            std::uint8_t b = 0;
            do {
                b = in.at(pos++);
                n += b;
            } while (b == 255);
        }
        return n;
    };
    while (pos < in.size()) {
        auto const token = in.at(pos++);
        auto const lit = length(token >> 4U);
        for (std::size_t i = 0; i < lit; ++i) {
            out.push_back(in.at(pos++));
        }
        if (pos == in.size()) {
            break;
        }
        auto const offset = static_cast<std::size_t>(in.at(pos) | (in.at(pos + 1) << 8U));
        pos += 2;
        REQUIRE(offset > 0);
        REQUIRE(offset <= out.size());
        auto const match = length(token & 15U) + 4;
        for (std::size_t i = 0; i < match; ++i) {
            out.push_back(out[out.size() - offset]);
        }
    }
    return out;
}

} // namespace

TEST_CASE("kernel codecs: every page roundtrips through every codec") {
    for (auto const* codec : codecs()) {
        auto pages = test_pages();
        for (std::size_t i = 0; i < pages.size(); ++i) {
            CAPTURE(codec->name);
            CAPTURE(i);
            auto s = stream(*codec);
            auto const c = s.compress(pages[i]);
            auto out = page();
            REQUIRE(s.decompress(c, out) == 0);
            CHECK(out == pages[i]);
        }
    }
}

TEST_CASE("kernel codecs: lz4 output is valid LZ4 for an independent decoder") {
    for (auto const& p : test_pages()) {
        CHECK(lz4_reference_decode(compress(quetschn_codec_lz4, p)) == p);
    }
}

TEST_CASE("kernel codecs: compressed sizes are plausible") {
    // random data does not compress, so every codec expands it a little, but stays within zram's buffer
    for (auto const* codec : codecs()) {
        CAPTURE(codec->name);
        auto const n = compress(*codec, random_page(1)).size();
        CHECK(n > page_size);
        CHECK(n < page_size + page_size / 16);
        CHECK(compress(*codec, zero_runs_page()).size() < 200);
        CHECK(compress(*codec, text_page()).size() < page_size / 2);
    }
}

TEST_CASE("kernel codecs: lzo-rle writes the lzo-rle stream, lzo does not") {
    // lzo1x_compress.c starts an lzo-rle stream with the marker 17 and the bitstream version 1, which
    // lzo1x_decompress_safe() checks for. Plain lzo output of a page starting with a literal does not.
    auto const rle = compress(quetschn_codec_lzo_rle, zero_runs_page());
    auto const plain = compress(quetschn_codec_lzo, zero_runs_page());
    REQUIRE(rle.size() >= 2);
    CHECK(rle[0] == 17);
    CHECK(rle[1] == 1);
    CHECK_FALSE((plain[0] == 17 && plain[1] == 1));
}

TEST_CASE("kernel codecs: workspace sizes match PLAN.md §3.3") {
    auto size = [](quetschn_codec const& codec) {
        return stream(codec).raw().workspace_size;
    };
    CHECK(size(quetschn_codec_lz4) == 16416);
    CHECK(size(quetschn_codec_lzo) == 16384);
    CHECK(size(quetschn_codec_lzo_rle) == 16384);
    // "far larger": a cctx and a dctx per stream
    CHECK(size(quetschn_codec_zstd) > 4 * 16416);
}

TEST_CASE("kernel codecs: truncated or corrupted input is rejected, not a crash") {
    for (auto const* codec : codecs()) {
        CAPTURE(codec->name);
        auto s = stream(*codec);
        auto c = s.compress(text_page());
        c.resize(c.size() / 2);
        auto out = page();
        auto const ret = s.decompress(c, out);
        // either an error, or at least not a full page: the second half of the input is missing
        CHECK((ret != 0 || out.size() != page_size));
    }
}

// From the kernel's include/linux/lz4.h, which cannot be included here: it needs the kernel's headers.
extern "C" int
LZ4_compress_fast(char const* source, char* dest, int input_size, int max_output_size, int acceleration, void* wrkmem);

TEST_CASE("kernel codecs: lz4 uses zram's default acceleration") {
    // backend_lz4.c uses LZ4_ACCELERATION_DEFAULT (1) unless a level is configured. Higher acceleration
    // is faster and compresses worse, so it would make lz4 look better on speed and worse on size.
    auto const p = pointer_page();
    auto direct = [&](int acceleration) {
        auto workspace = std::vector<std::uint8_t>(16416);
        auto dst = page(2 * page_size);
        auto const n = LZ4_compress_fast(reinterpret_cast<char const*>(p.data()),
                                         reinterpret_cast<char*>(dst.data()),
                                         static_cast<int>(p.size()),
                                         static_cast<int>(dst.size()),
                                         acceleration,
                                         workspace.data());
        REQUIRE(n > 0);
        dst.resize(static_cast<std::size_t>(n));
        return dst;
    };
    // the page has to tell the two apart, otherwise this test proves nothing
    REQUIRE(direct(1) != direct(8));
    CHECK(compress(quetschn_codec_lz4, p) == direct(1));
    // and a configured level reaches LZ4_compress_fast() as the acceleration
    CHECK(stream(quetschn_codec_lz4, 8).compress(p) == direct(8));
}

TEST_CASE("kernel codecs: levels zram rejects are rejected") {
    // lz4_setup_params(): below LZ4_ACCELERATION_DEFAULT. zstd_setup_params(): outside
    // [zstd_min_clevel(), zstd_max_clevel()], and the maximum is 22.
    for (int level : {0, -1}) {
        CAPTURE(level);
        CHECK(quetschn_codec_lz4.workspace_size(&level, page_size) == 0);
    }
    for (int level : {23, 100}) {
        CAPTURE(level);
        CHECK(quetschn_codec_zstd.workspace_size(&level, page_size) == 0);
    }
    for (int level : {-1, 1, 3, 22}) {
        CAPTURE(level);
        auto l = level;
        CHECK(quetschn_codec_zstd.workspace_size(&l, page_size) > 0);
        CHECK(l == level);
    }
}

TEST_CASE("kernel codecs: zstd defaults to level 3, and the level changes the output") {
    // ZSTD_CLEVEL_DEFAULT is 3, and zram uses zstd_default_clevel() when no level is configured
    CHECK(stream(quetschn_codec_zstd).raw().level == 3);

    auto const p = pointer_page();
    auto const fast = stream(quetschn_codec_zstd, -1).compress(p);
    auto const l3 = stream(quetschn_codec_zstd, 3).compress(p);
    auto const l19 = stream(quetschn_codec_zstd, 19).compress(p);
    CHECK(fast != l3);
    CHECK(l19.size() <= l3.size());
    CHECK(l3.size() < fast.size());
    for (auto const& c : {fast, l3, l19}) {
        auto s = stream(quetschn_codec_zstd);
        auto out = page();
        REQUIRE(s.decompress(c, out) == 0);
        CHECK(out == p);
    }
}

namespace {

// Runs the zstd command line tool on a frame. It is a separate process with its own libzstd, so it
// checks the kernel's zstd built in userspace without sharing any code with it. Returns false if there
// is no zstd binary.
bool zstd_cli_decompress(page const& frame, page& out) {
    auto const dir = std::filesystem::temp_directory_path();
    auto const in_path = dir / ("quetschn-zstd-" + std::to_string(::getpid()) + ".zst");
    auto const out_path = dir / ("quetschn-zstd-" + std::to_string(::getpid()) + ".out");
    {
        auto f = std::ofstream(in_path, std::ios::binary);
        f.write(reinterpret_cast<char const*>(frame.data()), static_cast<std::streamsize>(frame.size()));
    }
    auto const cmd = "zstd -q -d -f -o '" + out_path.string() + "' '" + in_path.string() + "' 2>/dev/null";
    auto const rc = std::system(cmd.c_str());
    auto in = std::ifstream(out_path, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(in), {});
    std::error_code ec;
    std::filesystem::remove(in_path, ec);
    std::filesystem::remove(out_path, ec);
    return rc == 0;
}

bool have_zstd_cli() {
    return std::system("zstd --version >/dev/null 2>&1") == 0;
}

} // namespace

TEST_CASE("kernel codecs: zstd output is a valid zstd frame for the zstd command line tool") {
    if (!have_zstd_cli()) {
        MESSAGE("no zstd command line tool, skipped");
        return;
    }
    for (int level : {-1, 1, 3}) {
        auto s = stream(quetschn_codec_zstd, level);
        for (auto const& p : test_pages()) {
            CAPTURE(level);
            auto out = page();
            REQUIRE(zstd_cli_decompress(s.compress(p), out));
            CHECK(out == p);
        }
    }
}
