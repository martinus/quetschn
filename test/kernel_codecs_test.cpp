// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// The kernel's lz4, lzo and zstd, built in userspace from QUETSCHN_KERNEL_TREE. Only compiled when that is set.

#include "kernel_codecs/zram_codec.h"

#include <doctest/doctest.h>

#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <system_error>
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
page pointer_page(std::uint64_t seed = 7) {
    auto rng = std::mt19937_64(seed);
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

page text_page(std::uint64_t seed = 3) {
    auto const words = std::vector<std::string>{"page ", "zram ", "compress ", "the ", "kernel ", "swap ", "memory "};
    auto rng = std::mt19937_64(seed);
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

// Pages that share a structure but no bytes in the same place: a record layout with a fixed header and
// varying fields. A dictionary built from some of them helps with the others.
page record_page(std::uint64_t seed) {
    auto rng = std::mt19937_64(seed);
    auto p = page();
    while (p.size() < page_size) {
        auto const header = std::string("struct task_info { pid=");
        p.insert(p.end(), header.begin(), header.end());
        auto const n = std::to_string(rng() % 100000);
        p.insert(p.end(), n.begin(), n.end());
        auto const mid = std::string(", state=RUNNING, flags=0x");
        p.insert(p.end(), mid.begin(), mid.end());
        auto const f = std::to_string(rng() % 1000);
        p.insert(p.end(), f.begin(), f.end());
        auto const tail = std::string(" }\n");
        p.insert(p.end(), tail.begin(), tail.end());
    }
    p.resize(page_size);
    return p;
}

std::vector<page> test_pages() {
    return {random_page(1), random_page(2), pointer_page(), text_page(), zero_runs_page(), record_page(1)};
}

std::vector<quetschn_codec const*> codecs() {
    return {&quetschn_codec_lz4,
            &quetschn_codec_lz4hc,
            &quetschn_codec_lzo,
            &quetschn_codec_lzo_rle,
            &quetschn_codec_zstd,
            &quetschn_codec_shuffle_lz4,
            &quetschn_codec_bdelta,
            &quetschn_codec_zstd_nolit,
            &quetschn_codec_seqlz,
            &quetschn_codec_seqlz_hc,
            &quetschn_codec_seqlz_fast};
}

// A zram device (params) with one per-CPU stream, set up the way zram does it.
class device {
public:
    explicit device(quetschn_codec const& codec, int level = QUETSCHN_LEVEL_DEFAULT, page dict = {})
        : m_codec(codec)
        , m_dict(std::move(dict)) {
        m_params.dict = m_dict.empty() ? nullptr : m_dict.data();
        m_params.dict_size = m_dict.size();
        m_params.level = level;
        m_params.page_size = page_size;
        REQUIRE(codec.setup_params(&m_params) == 0);
        REQUIRE(codec.create(&m_params, &m_stream) == 0);
    }

    ~device() {
        m_codec.destroy(&m_stream);
        m_codec.release_params(&m_params);
    }

    device(device const&) = delete;
    device& operator=(device const&) = delete;

    page compress(page const& src) {
        auto dst = page(2 * page_size);
        auto len = static_cast<unsigned int>(dst.size());
        REQUIRE(m_codec.compress(&m_params, &m_stream, src.data(), static_cast<unsigned int>(src.size()), dst.data(), &len) ==
                0);
        dst.resize(len);
        return dst;
    }

    // returns the codec's result, out holds the decompressed bytes
    int decompress(page const& src, page& out) {
        out.assign(page_size, 0);
        auto len = static_cast<unsigned int>(out.size());
        auto const ret =
            m_codec.decompress(&m_params, &m_stream, src.data(), static_cast<unsigned int>(src.size()), out.data(), &len);
        out.resize(len);
        return ret;
    }

    [[nodiscard]] quetschn_params const& params() const {
        return m_params;
    }
    [[nodiscard]] quetschn_stream const& stream() const {
        return m_stream;
    }

private:
    quetschn_codec const& m_codec;
    page m_dict;
    quetschn_params m_params{};
    quetschn_stream m_stream{};
};

page compress(quetschn_codec const& codec, page const& src) {
    return device(codec).compress(src);
}

bool rejects(quetschn_codec const& codec, int level) {
    auto p = quetschn_params{};
    p.level = level;
    p.page_size = page_size;
    auto const failed = codec.setup_params(&p) != 0;
    if (!failed) {
        codec.release_params(&p);
    }
    return failed;
}

// LZ4 block format, decoded straight from the format description (lz4_Block_format.md): a token with
// literal length (high nibble) and match length - 4 (low nibble), 15 meaning "more length bytes follow",
// the literals, then a 2 byte little-endian offset. The last sequence has literals only. With a dictionary
// the offsets may reach back into it, as if it came right before the output. Independent of the kernel's
// decoder, so it checks that the userspace build of the kernel's compressor writes real LZ4.
page lz4_reference_decode(page const& in, page const& dict = {}) {
    auto out = dict;
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
    return page(out.begin() + static_cast<std::ptrdiff_t>(dict.size()), out.end());
}

// a raw content dictionary: the bytes of a few pages like the ones that are compressed later
page record_dict() {
    auto d = page();
    for (std::uint64_t seed = 100; seed < 104; ++seed) {
        auto const p = record_page(seed);
        d.insert(d.end(), p.begin(), p.end());
    }
    return d;
}

std::filesystem::path temp_path(std::string const& name) {
    return std::filesystem::temp_directory_path() / ("quetschn-" + std::to_string(::getpid()) + "-" + name);
}

void write_file(std::filesystem::path const& path, page const& data) {
    auto f = std::ofstream(path, std::ios::binary);
    f.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
}

page read_file(std::filesystem::path const& path) {
    auto in = std::ifstream(path, std::ios::binary);
    return page(std::istreambuf_iterator<char>(in), {});
}

bool have_zstd_cli() {
    return std::system("zstd --version >/dev/null 2>&1") == 0;
}

// Runs the zstd command line tool on a frame. It is a separate process with its own libzstd, so it
// checks the kernel's zstd built in userspace without sharing any code with it.
bool zstd_cli_decompress(page const& frame, page& out, page const& dict = {}) {
    auto const in_path = temp_path("frame.zst");
    auto const out_path = temp_path("frame.out");
    auto const dict_path = temp_path("frame.dict");
    write_file(in_path, frame);
    auto cmd = "zstd -q -d -f -o '" + out_path.string() + "' '" + in_path.string() + "'";
    if (!dict.empty()) {
        write_file(dict_path, dict);
        cmd += " -D '" + dict_path.string() + "'";
    }
    cmd += " 2>/dev/null";
    auto const rc = std::system(cmd.c_str());
    out = read_file(out_path);
    std::error_code ec;
    for (auto const& p : {in_path, out_path, dict_path}) {
        std::filesystem::remove(p, ec);
    }
    return rc == 0;
}

} // namespace

TEST_CASE("kernel codecs: every page roundtrips through every codec, with and without dictionary") {
    for (auto const* codec : codecs()) {
        for (auto const& dict : {page(), record_dict()}) {
            auto pages = test_pages();
            for (std::size_t i = 0; i < pages.size(); ++i) {
                auto const name = std::string(codec->name);
                CAPTURE(name);
                CAPTURE(dict.size());
                CAPTURE(i);
                auto d = device(*codec, QUETSCHN_LEVEL_DEFAULT, dict);
                auto const c = d.compress(pages[i]);
                auto out = page();
                REQUIRE(d.decompress(c, out) == 0);
                CHECK(out == pages[i]);
            }
        }
    }
}

TEST_CASE("kernel codecs: lz4 output is valid LZ4 for an independent decoder") {
    auto const dict = record_dict();
    for (auto const& p : test_pages()) {
        CHECK(lz4_reference_decode(compress(quetschn_codec_lz4, p)) == p);
        CHECK(lz4_reference_decode(device(quetschn_codec_lz4, QUETSCHN_LEVEL_DEFAULT, dict).compress(p), dict) == p);
    }
}

TEST_CASE("kernel codecs: compressed sizes are plausible") {
    // random data does not compress, so every codec expands it a little, but stays within zram's buffer
    for (auto const* codec : codecs()) {
        auto const name = std::string(codec->name);
        CAPTURE(name);
        auto const n = compress(*codec, random_page(1)).size();
        CHECK(n > page_size);
        CHECK(n < page_size + page_size / 16);
        CHECK(compress(*codec, zero_runs_page()).size() < 200);
        // the byte-oriented kernel codecs find the repeats in text, the word-oriented candidates do not
        if (codec != &quetschn_codec_shuffle_lz4 && codec != &quetschn_codec_bdelta) {
            CHECK(compress(*codec, text_page()).size() < page_size / 2);
        }
    }
}

TEST_CASE("kernel codecs: a dictionary of similar pages makes lz4 and zstd output smaller") {
    auto const dict = record_dict();
    for (auto const* codec : {&quetschn_codec_lz4, &quetschn_codec_zstd}) {
        auto const name = std::string(codec->name);
        CAPTURE(name);
        auto const p = record_page(1);
        auto const without = device(*codec).compress(p).size();
        auto const with = device(*codec, QUETSCHN_LEVEL_DEFAULT, dict).compress(p).size();
        // 15% for lz4 and 9% for zstd on these pages; ignoring the dictionary gives 0%
        CHECK(with < without * 95 / 100);
    }
}

TEST_CASE("kernel codecs: every page is compressed on its own, also with a dictionary") {
    // zram resets the stream per page (lz4: a copy of the dictionary template, zstd: the cdict). If
    // history leaked from one page to the next, the second compression of the same page would differ,
    // and zram could not decompress pages in any order.
    auto const dict = record_dict();
    for (auto const* codec : {&quetschn_codec_lz4, &quetschn_codec_zstd}) {
        auto const name = std::string(codec->name);
        CAPTURE(name);
        auto d = device(*codec, QUETSCHN_LEVEL_DEFAULT, dict);
        auto const a = record_page(1);
        auto const b = record_page(2);
        auto const first = d.compress(a);
        (void)d.compress(b);
        CHECK(d.compress(a) == first);
        // and decompression works in any order too
        auto out = page();
        REQUIRE(d.decompress(d.compress(b), out) == 0);
        REQUIRE(d.decompress(first, out) == 0);
        CHECK(out == a);
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

TEST_CASE("kernel codecs: memory per CPU and per device") {
    // PLAN.md §3.3: LZ4_MEM_COMPRESS = 16416, LZO1X_MEM_COMPRESS = 16384, zstd "far larger". On top,
    // lz4 and zstd allocate a small context struct of pointers per stream.
    auto per_cpu = [](quetschn_codec const& codec, page dict = {}) {
        auto d = device(codec, QUETSCHN_LEVEL_DEFAULT, std::move(dict));
        (void)d.compress(text_page()); // zstd allocates part of it lazily
        return d.stream().allocated;
    };
    CHECK(per_cpu(quetschn_codec_lzo) == 16384);
    CHECK(per_cpu(quetschn_codec_lzo_rle) == 16384);
    CHECK(per_cpu(quetschn_codec_lz4) >= 16416);
    CHECK(per_cpu(quetschn_codec_lz4) < 16416 + 64);
    CHECK(per_cpu(quetschn_codec_zstd) > 4 * 16416);

    // With a dictionary lz4 holds a compression and a decompression stream per CPU instead of the
    // workspace, and the prepared dictionary once per device.
    auto const dict = record_dict();
    CHECK(per_cpu(quetschn_codec_lz4, dict) >= 16416);
    CHECK(device(quetschn_codec_lz4, QUETSCHN_LEVEL_DEFAULT, dict).params().allocated == 16416);
    CHECK(device(quetschn_codec_lz4).params().allocated == 0);
    // zstd with a dictionary still has a cctx and a dctx per CPU, allocated by zstd itself, and about as
    // large as the workspaces without one (186 112 bytes both at level 3 on the development machine)
    CHECK(per_cpu(quetschn_codec_zstd, dict) > per_cpu(quetschn_codec_zstd) * 9 / 10);
    CHECK(per_cpu(quetschn_codec_zstd, dict) < per_cpu(quetschn_codec_zstd) * 11 / 10);
    CHECK(device(quetschn_codec_zstd, QUETSCHN_LEVEL_DEFAULT, dict).params().allocated > 0);
}

TEST_CASE("kernel codecs: the allocator aligns like the kernel, and counts") {
    auto counter = std::size_t{0};
    auto* small = quetschn_zalloc(100, &counter);
    auto* page_block = quetschn_zalloc(4096, &counter);
    auto* large = quetschn_zalloc(70000, &counter);
    REQUIRE(small != nullptr);
    REQUIRE(page_block != nullptr);
    REQUIRE(large != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(small) % 64 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(page_block) % 4096 == 0);
    CHECK(reinterpret_cast<std::uintptr_t>(large) % 4096 == 0);
    CHECK(counter == 100 + 4096 + 70000);
    // zeroed, and all of it writable (ASan)
    auto const* bytes = static_cast<unsigned char const*>(large);
    CHECK(std::all_of(bytes, bytes + 70000, [](unsigned char b) {
        return b == 0;
    }));
    std::memset(large, 1, 70000);
    quetschn_free(large, &counter);
    quetschn_free(small, &counter);
    quetschn_free(page_block, &counter);
    quetschn_free(nullptr, &counter);
    CHECK(counter == 0);
}

TEST_CASE("kernel codecs: every allocation is released again") {
    for (auto const* codec : codecs()) {
        for (auto const& dict : {page(), record_dict()}) {
            auto const name = std::string(codec->name);
            CAPTURE(name);
            CAPTURE(dict.size());
            auto p = quetschn_params{};
            p.dict = dict.empty() ? nullptr : dict.data();
            p.dict_size = dict.size();
            p.level = QUETSCHN_LEVEL_DEFAULT;
            p.page_size = page_size;
            REQUIRE(codec->setup_params(&p) == 0);
            auto s = quetschn_stream{};
            REQUIRE(codec->create(&p, &s) == 0);
            auto dst = page(2 * page_size);
            auto len = static_cast<unsigned int>(dst.size());
            auto const src = text_page();
            REQUIRE(codec->compress(&p, &s, src.data(), page_size, dst.data(), &len) == 0);
            codec->destroy(&s);
            codec->release_params(&p);
            CHECK(s.allocated == 0);
            CHECK(p.allocated == 0);
        }
    }
}

TEST_CASE("kernel codecs: truncated or corrupted input is rejected, not a crash") {
    for (auto const* codec : codecs()) {
        auto const name = std::string(codec->name);
        CAPTURE(name);
        auto d = device(*codec);
        auto c = d.compress(text_page());
        c.resize(c.size() / 2);
        auto out = page();
        auto const ret = d.decompress(c, out);
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
    CHECK(device(quetschn_codec_lz4, 8).compress(p) == direct(8));
}

TEST_CASE("kernel codecs: lz4hc writes lz4, defaults to level 9, and zram's level range") {
    // lz4hc_setup_params(): LZ4HC_DEFAULT_CLEVEL (9) unless set, levels 1 to LZ4HC_MAX_CLEVEL (16)
    auto const dict = record_dict();
    for (auto const& p : test_pages()) {
        CHECK(lz4_reference_decode(compress(quetschn_codec_lz4hc, p)) == p);
        CHECK(lz4_reference_decode(device(quetschn_codec_lz4hc, QUETSCHN_LEVEL_DEFAULT, dict).compress(p), dict) == p);
    }
    CHECK(device(quetschn_codec_lz4hc).params().level == 9);
    CHECK(rejects(quetschn_codec_lz4hc, 0));
    CHECK(rejects(quetschn_codec_lz4hc, 17));
    CHECK_FALSE(rejects(quetschn_codec_lz4hc, 1));
    CHECK_FALSE(rejects(quetschn_codec_lz4hc, 16));
    // the level reaches the compressor: on the text page, level 1 is larger than 9, and 9 is smaller than lz4
    auto const t = text_page();
    auto const hc1 = device(quetschn_codec_lz4hc, 1).compress(t).size();
    auto const hc9 = compress(quetschn_codec_lz4hc, t).size();
    CHECK(hc9 < hc1);
    CHECK(hc9 < compress(quetschn_codec_lz4, t).size());
}

TEST_CASE("kernel codecs: zstd-nolit is zstd without Huffman coded literals") {
    // 16 different bytes in random order: few matches, and 4 bits of entropy per literal byte, so
    // Huffman coding the literals nearly halves them
    auto rng = std::mt19937_64(17);
    auto p = page(page_size);
    for (auto& b : p) {
        b = static_cast<std::uint8_t>('a' + rng() % 16);
    }
    auto const with = device(quetschn_codec_zstd, 3).compress(p).size();
    auto const without = device(quetschn_codec_zstd_nolit, 3).compress(p).size();
    CHECK(with < 2600);
    CHECK(without > 3900);
    // zstd itself never Huffman codes literals at negative levels, so there both are the same
    CHECK(device(quetschn_codec_zstd_nolit, -1).compress(p) == device(quetschn_codec_zstd, -1).compress(p));
    CHECK(device(quetschn_codec_zstd_nolit).params().level == 3);
    CHECK(rejects(quetschn_codec_zstd_nolit, 23));
}

TEST_CASE("kernel codecs: levels zram rejects are rejected") {
    // lz4_setup_params(): below LZ4_ACCELERATION_DEFAULT. zstd_setup_params(): outside
    // [zstd_min_clevel(), zstd_max_clevel()], and the maximum is 22.
    CHECK(rejects(quetschn_codec_lz4, 0));
    CHECK(rejects(quetschn_codec_lz4, -1));
    CHECK_FALSE(rejects(quetschn_codec_lz4, 1));
    CHECK(rejects(quetschn_codec_zstd, 23));
    CHECK(rejects(quetschn_codec_zstd, 100));
    for (int level : {-1, 1, 3, 22}) {
        CAPTURE(level);
        CHECK_FALSE(rejects(quetschn_codec_zstd, level));
        CHECK(device(quetschn_codec_zstd, level).params().level == level);
    }
}

TEST_CASE("kernel codecs: zstd defaults to level 3, and the level changes the output") {
    // ZSTD_CLEVEL_DEFAULT is 3, and zram uses zstd_default_clevel() when no level is configured
    CHECK(device(quetschn_codec_zstd).params().level == 3);

    auto const p = pointer_page();
    auto const fast = device(quetschn_codec_zstd, -1).compress(p);
    auto const l3 = device(quetschn_codec_zstd, 3).compress(p);
    auto const l19 = device(quetschn_codec_zstd, 19).compress(p);
    CHECK(fast != l3);
    CHECK(l19.size() <= l3.size());
    CHECK(l3.size() < fast.size());
    for (auto const& c : {fast, l3, l19}) {
        auto d = device(quetschn_codec_zstd);
        auto out = page();
        REQUIRE(d.decompress(c, out) == 0);
        CHECK(out == p);
    }
}

TEST_CASE("kernel codecs: zstd output is a valid zstd frame for the zstd command line tool") {
    if (!have_zstd_cli()) {
        MESSAGE("no zstd command line tool, skipped");
        return;
    }
    auto const dict = record_dict();
    for (int level : {-1, 1, 3}) {
        auto plain = device(quetschn_codec_zstd, level);
        auto with_dict = device(quetschn_codec_zstd, level, dict);
        for (auto const& p : test_pages()) {
            CAPTURE(level);
            auto out = page();
            REQUIRE(zstd_cli_decompress(plain.compress(p), out));
            CHECK(out == p);
            REQUIRE(zstd_cli_decompress(with_dict.compress(p), out, dict));
            CHECK(out == p);
        }
    }
}

TEST_CASE("kernel codecs: a dictionary trained by zstd --train works for zstd and lz4") {
    // What Honor's f0f6f7871430 does, and what zram's documentation suggests: train with the zstd tool,
    // then use the same file for either codec. -B4096 cuts the samples into pages; the --split=4096 from
    // that commit message is not an option zstd 1.5.7 knows. A trained dictionary has a zstd header and entropy
    // tables, which lz4 treats as plain content, while zstd parses them.
    if (!have_zstd_cli()) {
        MESSAGE("no zstd command line tool, skipped");
        return;
    }
    auto const samples = temp_path("samples");
    auto const dict_path = temp_path("trained.dict");
    {
        auto all = page();
        for (std::uint64_t seed = 1000; seed < 1200; ++seed) {
            auto const p = record_page(seed);
            all.insert(all.end(), p.begin(), p.end());
        }
        write_file(samples, all);
    }
    auto const cmd =
        "zstd -q -f --train '" + samples.string() + "' -B4096 --maxdict=16KB -o '" + dict_path.string() + "' 2>/dev/null";
    REQUIRE(std::system(cmd.c_str()) == 0);
    auto const dict = read_file(dict_path);
    std::error_code ec;
    std::filesystem::remove(samples, ec);
    std::filesystem::remove(dict_path, ec);

    // zstd checks for its dictionary magic number, 0xEC30A437 little-endian
    REQUIRE(dict.size() > 8);
    CHECK(dict[0] == 0x37);
    CHECK(dict[1] == 0xa4);
    CHECK(dict[2] == 0x30);
    CHECK(dict[3] == 0xec);

    auto const p = record_page(5);
    for (auto const* codec : {&quetschn_codec_lz4, &quetschn_codec_zstd}) {
        auto const name = std::string(codec->name);
        CAPTURE(name);
        auto d = device(*codec, QUETSCHN_LEVEL_DEFAULT, dict);
        auto const c = d.compress(p);
        CHECK(c.size() < device(*codec).compress(p).size());
        auto out = page();
        REQUIRE(d.decompress(c, out) == 0);
        CHECK(out == p);
    }
    auto out = page();
    REQUIRE(zstd_cli_decompress(device(quetschn_codec_zstd, QUETSCHN_LEVEL_DEFAULT, dict).compress(p), out, dict));
    CHECK(out == p);
}
