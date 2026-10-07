// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "seqlz.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace {

// The page size seqlz is built for, 4 KiB or 16 KiB (QUETSCHN_PAGE_BITS): the tests run with both.
constexpr unsigned page_bits = QUETSCHN_PAGE_BITS;
constexpr unsigned page_size = SEQLZ_PAGE;
// zram's huge_class_size, from which on a page is stored as it is (docs/plan.md §3.1 and §3.5)
constexpr unsigned zram_huge = page_bits == 12 ? 3625U : 14553U;

struct seqlz_page {
    std::vector<unsigned char> bytes;
    std::vector<seqlz_sequence> sequences;
    std::vector<unsigned char> literals;
};

// A page built from random sequences, so the sequences that describe it are known. kind picks what
// dominates: 0 short offsets 1 to 7, 1 repeat offsets, 2 long runs, 3 anything.
seqlz_page random_seqlz_page(std::mt19937_64& rng, int kind) {
    auto p = seqlz_page{};
    auto last = std::array<unsigned, 3>{1, 4, 8};
    while (true) {
        auto const room = page_size - p.bytes.size();
        auto lit = static_cast<unsigned>(rng() % (kind == 2 ? 3 : 20));
        if (rng() % 16 == 0) {
            lit += static_cast<unsigned>(rng() % 300);
        }
        if (p.bytes.empty() && lit == 0) {
            lit = 1;
        }
        if (lit + 4 > room || rng() % 200 == 0) {
            // the last sequence: literals up to the end of the page
            auto s = seqlz_sequence{static_cast<unsigned short>(room), 0, 0};
            for (std::size_t i = 0; i < room; ++i) {
                auto const b = static_cast<unsigned char>(rng());
                p.bytes.push_back(b);
                p.literals.push_back(b);
            }
            p.sequences.push_back(s);
            return p;
        }
        for (unsigned i = 0; i < lit; ++i) {
            auto const b = static_cast<unsigned char>(rng() % 4 == 0 ? 0 : rng());
            p.bytes.push_back(b);
            p.literals.push_back(b);
        }
        auto const have = static_cast<unsigned>(p.bytes.size());
        auto off = 0U;
        switch (kind) {
        case 0:
            off = 1 + static_cast<unsigned>(rng() % 7);
            break;
        case 1:
            off = last[rng() % 3];
            break;
        default:
            off = 1 + static_cast<unsigned>(rng() % have);
            break;
        }
        off = std::min(off, have);
        auto const max_len = static_cast<unsigned>(page_size - p.bytes.size());
        auto len = 4 + static_cast<unsigned>(rng() % (kind == 2 ? 3000 : 40));
        len = std::min(len, max_len);
        if (len < 4) {
            continue;
        }
        for (unsigned i = 0; i < len; ++i) {
            p.bytes.push_back(p.bytes[p.bytes.size() - off]);
        }
        last = {off, last[0], last[1]};
        p.sequences.push_back(seqlz_sequence{
            static_cast<unsigned short>(lit), static_cast<unsigned short>(len), static_cast<unsigned short>(off)});
        if (p.bytes.size() == page_size) {
            // ended with a match: an empty last sequence
            p.sequences.push_back(seqlz_sequence{0, 0, 0});
            return p;
        }
    }
}

std::unique_ptr<seqlz_tables, void (*)(seqlz_tables*)> default_tables(seqlz_lengths const& lengths = seqlz_default_own) {
    auto* t = static_cast<seqlz_tables*>(::operator new(seqlz_tables_size()));
    REQUIRE(seqlz_tables_init(t, &lengths) == 0);
    return {t, [](seqlz_tables* p) {
                ::operator delete(p);
            }};
}

} // namespace

TEST_CASE("seqlz: pages from random sequences come back, with every kind of copy") {
    auto const t = default_tables();
    auto rng = std::mt19937_64(31);
    for (int round = 0; round < 800; ++round) {
        CAPTURE(round);
        auto const p = random_seqlz_page(rng, round % 4);
        REQUIRE(p.bytes.size() == page_size);
        auto c = std::vector<unsigned char>(3 * page_size);
        auto const len = seqlz_encode(t.get(),
                                      p.sequences.data(),
                                      static_cast<unsigned>(p.sequences.size()),
                                      p.literals.data(),
                                      static_cast<unsigned>(p.literals.size()),
                                      c.data(),
                                      static_cast<unsigned>(c.size()),
                                      0);
        REQUIRE(len > 0);
        // exactly sized, so ASan sees any read past the end
        c.resize(len);
        auto out = std::vector<unsigned char>(page_size);
        REQUIRE(seqlz_decode(t.get(), c.data(), len, out.data(), nullptr) == 0);
        CHECK(out == p.bytes);
        // and a byte less is not a valid page
        CHECK(seqlz_decode(t.get(), c.data(), len - 1, out.data(), nullptr) == -1);
    }
}

TEST_CASE("seqlz: the token holds ll and ml - 4 up to their caps and the offset class") {
    auto constexpr ml_shift = SEQLZ_LL_BITS, cls_shift = SEQLZ_LL_BITS + SEQLZ_ML_BITS;
    CHECK(seqlz_token(0, 4, 0) == 0);
    CHECK(seqlz_token(2, 7, 1) == 2 + (3U << ml_shift) + (1U << cls_shift));
    CHECK(seqlz_token(SEQLZ_LL_CAP - 1, SEQLZ_ML_CAP + 3, 2) ==
          SEQLZ_LL_CAP - 1 + ((SEQLZ_ML_CAP - 1) << ml_shift) + (2U << cls_shift));
    CHECK(seqlz_token(SEQLZ_LL_CAP, SEQLZ_ML_CAP + 4, 0) == SEQLZ_LL_CAP + (SEQLZ_ML_CAP << ml_shift));
    CHECK(seqlz_token(4000, 3000, 2) == SEQLZ_LL_CAP + (SEQLZ_ML_CAP << ml_shift) + (2U << cls_shift));
    CHECK(seqlz_token(1, 0, 0) == 1);
    CHECK(seqlz_token(20, 0, 0) == SEQLZ_LL_CAP);
    auto bits = 0U;
    CHECK(seqlz_off_class(9, 9, &bits) == 0);
    CHECK(bits == 0);
    CHECK(seqlz_off_class(1, 9, &bits) == 1);
    CHECK(bits == 4);
    CHECK(seqlz_off_class(15, 9, &bits) == 1);
    CHECK(seqlz_off_class(17, 9, &bits) == 2);
    CHECK(bits == 8);
    CHECK(seqlz_off_class(255, 9, &bits) == 2);
    CHECK(seqlz_off_class(257, 9, &bits) == 3);
    CHECK(bits == page_bits);
    CHECK(seqlz_off_class(4095, 9, &bits) == 3);
    // the multiples of 8 from 16 on
    CHECK(seqlz_off_class(8, 9, &bits) == 1);
    CHECK(bits == 4);
    CHECK(seqlz_off_class(16, 9, &bits) == 4);
    CHECK(bits == 5);
    CHECK(seqlz_off_class(248, 9, &bits) == 4);
    CHECK(seqlz_off_class(256, 9, &bits) == 5);
    CHECK(bits == page_bits - 3);
    CHECK(seqlz_off_class(4088, 9, &bits) == 5);
    CHECK(seqlz_off_class(4088, 4088, &bits) == 0);
}

TEST_CASE("seqlz: a sequence with long lengths and a large offset needs more bits than one refill") {
    // 1100 literals (value 1085, 10 extra bits), a match of 2100 (value 2081, 11 extra bits) at offset
    // 1050 (12 or 14 raw bits), then literals to the end of the page: token, two codes and 33 or 35 more
    // bits in one sequence
    auto const t = default_tables();
    auto rng = std::mt19937_64(41);
    auto p = seqlz_page{};
    for (int i = 0; i < 1100; ++i) {
        p.bytes.push_back(static_cast<unsigned char>(rng()));
    }
    p.literals = p.bytes;
    for (int i = 0; i < 2100; ++i) {
        p.bytes.push_back(p.bytes[p.bytes.size() - 1050]);
    }
    for (unsigned i = 0; i < page_size - 3200; ++i) {
        auto const b = static_cast<unsigned char>(rng());
        p.bytes.push_back(b);
        p.literals.push_back(b);
    }
    p.sequences = {{1100, 2100, 1050}, {page_size - 3200, 0, 0}};
    auto c = std::vector<unsigned char>(3 * page_size);
    auto const len = seqlz_encode(t.get(),
                                  p.sequences.data(),
                                  2,
                                  p.literals.data(),
                                  static_cast<unsigned>(p.literals.size()),
                                  c.data(),
                                  3 * page_size,
                                  0);
    REQUIRE(len > 0);
    c.resize(len);
    auto out = std::vector<unsigned char>(page_size);
    REQUIRE(seqlz_decode(t.get(), c.data(), len, out.data(), nullptr) == 0);
    CHECK(out == p.bytes);
}

TEST_CASE("seqlz: tables that are no prefix code are rejected") {
    auto t = default_tables();
    auto l = seqlz_default_own;
    CHECK(seqlz_tables_init(t.get(), &l) == 0);
    l.ll[0] = 10; // longer than SEQLZ_MAX_BITS
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    l = seqlz_default_own;
    l.ml[0] = 1; // together with the others more codes than fit: over-subscribed
    l.ml[1] = 1;
    l.ml[2] = 1;
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    l = seqlz_default_own;
    std::memset(l.ml, 0, sizeof(l.ml)); // no code at all
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    // A complete code: the tokens with 11 bits, then the first ones 10 bits, then 9, until they fill
    // the 11-bit table exactly.
    l = seqlz_default_own;
    auto complete = [&] {
        // tokens 3 to 2047 and the escape with 11 bits, the others without a code: they take the escape
        std::memset(l.token, 0, sizeof(l.token));
        std::memset(l.token + 3, 11, 2045);
        l.token[SEQLZ_ESCAPE] = 11;
        auto units = 2046U; // the tokens and the escape
        for (unsigned bits = 10; units < 2048; --bits) {
            for (unsigned i = 3; i < sizeof(l.token) && units < 2048; ++i) {
                units += 1U << (10 - bits);
                l.token[i] = static_cast<unsigned char>(bits);
            }
        }
    };
    auto with_bits = [&](unsigned char bits, unsigned nth) {
        auto* p = std::find(l.token, l.token + sizeof(l.token), bits);
        for (unsigned k = 0; k < nth; ++k) {
            p = std::find(p + 1, l.token + sizeof(l.token), bits);
        }
        REQUIRE(p != l.token + sizeof(l.token));
        return static_cast<std::size_t>(p - l.token);
    };
    complete();
    CHECK(seqlz_tables_init(t.get(), &l) == 0);
    l.token[with_bits(10, 0)] = 11;
    CHECK(seqlz_tables_init(t.get(), &l) == -1); // a gap of one 11-bit code
    complete();
    l.token[with_bits(11, 0)] = 12;
    CHECK(seqlz_tables_init(t.get(), &l) == -1); // longer than 11 bits
    // A complete code, and then one of 12 bits more: the Kraft sum over 11 bits is still complete,
    // only the length check stops the 12-bit code.
    complete();
    auto const a = with_bits(10, 0), b = with_bits(10, 1);
    l.token[a] = 0;
    l.token[b] = 9;
    CHECK(seqlz_tables_init(t.get(), &l) == 0);
    l.token[a] = 12;
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
    // a gap: every bit pattern must start a code, the decoder does not check
    l = seqlz_default_own;
    auto const shortest = std::min_element(l.ll, l.ll + SEQLZ_LEN_SYMBOLS);
    *shortest = static_cast<unsigned char>(*shortest + 1);
    CHECK(seqlz_tables_init(t.get(), &l) == -1);
}

TEST_CASE("seqlz: any input is safe for the decoder") {
    auto const t = default_tables();
    auto rng = std::mt19937_64(37);
    auto out = std::vector<unsigned char>(page_size);
    for (int round = 0; round < 3000; ++round) {
        CAPTURE(round);
        std::vector<unsigned char> c;
        if (round % 2 == 0) {
            // random bytes with a plausible header
            c.resize(8 + rng() % 3000);
            for (auto& b : c) {
                b = static_cast<unsigned char>(rng());
            }
            auto const n_lit = static_cast<unsigned>(rng() % (c.size() - 7)); // up to all bytes behind the header
            c[0] = static_cast<unsigned char>(1 + rng() % 200);
            c[1] = 0;
            c[2] = static_cast<unsigned char>(n_lit);
            c[3] = static_cast<unsigned char>(n_lit >> 8);
            auto const rest = static_cast<unsigned>(c.size() - 8 - n_lit);
            auto const a = rest == 0 ? 0U : static_cast<unsigned>(rng() % rest);
            c[4] = static_cast<unsigned char>(a);
            c[5] = static_cast<unsigned char>(a >> 8);
            c[6] = static_cast<unsigned char>((rest - a) / 2);
            c[7] = static_cast<unsigned char>(((rest - a) / 2) >> 8);
        } else {
            // a valid page with some bits flipped
            auto const p = random_seqlz_page(rng, round % 4);
            c.resize(3 * page_size);
            auto const len = seqlz_encode(t.get(),
                                          p.sequences.data(),
                                          static_cast<unsigned>(p.sequences.size()),
                                          p.literals.data(),
                                          static_cast<unsigned>(p.literals.size()),
                                          c.data(),
                                          static_cast<unsigned>(c.size()),
                                          0);
            REQUIRE(len > 0);
            c.resize(len);
            for (int f = 0; f < 3; ++f) {
                c[rng() % c.size()] ^= static_cast<unsigned char>(1U << (rng() % 8));
            }
        }
        auto const ret = seqlz_decode(t.get(), c.data(), static_cast<unsigned>(c.size()), out.data(), nullptr);
        CHECK((ret == 0 || ret == -1));
    }
}

namespace {

// the literals of a page for its sequences
std::vector<unsigned char> literals_of(std::vector<unsigned char> const& bytes, seqlz_sequence const* seq, unsigned n) {
    auto out = std::vector<unsigned char>();
    std::size_t pos = 0;
    for (unsigned i = 0; i < n; ++i) {
        out.insert(out.end(),
                   bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                   bytes.begin() + static_cast<std::ptrdiff_t>(pos + seq[i].literals));
        pos += seq[i].literals + seq[i].match;
    }
    return out;
}

} // namespace

TEST_CASE("seqlz: the compressor's pages come back, with a state shared over many pages") {
    // One state for all pages, as zram has one per CPU: nothing of one page may change the next.
    auto const t = default_tables(seqlz_default_own);
    auto st = std::make_unique<seqlz_state>();
    auto rng = std::mt19937_64(43);
    auto c = std::vector<unsigned char>(2 * page_size);
    auto out = std::vector<unsigned char>(page_size);
    for (int round = 0; round < 800; ++round) {
        CAPTURE(round);
        auto const p = random_seqlz_page(rng, round % 4);
        auto const len = seqlz_compress(t.get(), st.get(), p.bytes.data(), c.data(), static_cast<unsigned>(c.size()), 0);
        REQUIRE(len > 0);
        auto exact = std::vector<unsigned char>(c.begin(), c.begin() + len);
        REQUIRE(seqlz_decode(t.get(), exact.data(), len, out.data(), nullptr) == 0);
        CHECK(out == p.bytes);
    }
}

TEST_CASE("seqlz: the compressor writes what seqlz_encode writes for seqlz_find's sequences") {
    auto const t = default_tables(seqlz_default_own);
    // two states with the same history, one for each side
    auto a = std::make_unique<seqlz_state>();
    auto b = std::make_unique<seqlz_state>();
    auto rng = std::mt19937_64(47);
    auto seq = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
    for (int round = 0; round < 300; ++round) {
        CAPTURE(round);
        auto const p = random_seqlz_page(rng, round % 4);
        auto const n = seqlz_find(a.get(), p.bytes.data(), seq.data());
        auto const lits = literals_of(p.bytes, seq.data(), n);
        auto expected = std::vector<unsigned char>(2 * page_size);
        auto const elen = seqlz_encode(
            t.get(), seq.data(), n, lits.data(), static_cast<unsigned>(lits.size()), expected.data(), 2 * page_size, 0);
        auto got = std::vector<unsigned char>(2 * page_size);
        auto const glen = seqlz_compress(t.get(), b.get(), p.bytes.data(), got.data(), 2 * page_size, 0);
        REQUIRE(elen > 0);
        REQUIRE(glen == elen);
        CHECK(std::equal(got.begin(), got.begin() + glen, expected.begin()));
    }
}

TEST_CASE("seqlz: the matcher finds a repeat with its whole length") {
    // Random bytes, and bytes 1 to 1 + len again at 62, with other bytes around both copies. Within the
    // first 64 bytes, where the matcher looks at every position. It hashes 5 bytes, so a repeat of 4 is
    // not found.
    auto state = std::make_unique<seqlz_state>();
    auto seq = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
    auto rng = std::mt19937_64(67);
    for (unsigned len = 4; len <= 56; ++len) {
        CAPTURE(len);
        auto bytes = std::vector<unsigned char>(page_size);
        for (auto& b : bytes) {
            b = static_cast<unsigned char>(rng());
        }
        std::copy_n(bytes.begin() + 1, len, bytes.begin() + 62);
        bytes[61] = static_cast<unsigned char>(bytes[0] + 1);
        bytes[62 + len] = static_cast<unsigned char>(bytes[1 + len] + 1);
        auto const n = seqlz_find(state.get(), bytes.data(), seq.data());
        if (len == 4) {
            CHECK(n == 1);
            continue;
        }
        REQUIRE(n == 2);
        CHECK(seq[0].literals == 62);
        CHECK(seq[0].match == len);
        CHECK(seq[0].offset == 61);
    }
}

TEST_CASE("seqlz: the matcher never takes a position of an earlier page") {
    // The state is shared over the pages, as zram has one per CPU. Page b repeats nothing of its own,
    // only 8 bytes of page a, so it has no match.
    auto seq = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
    auto rng = std::mt19937_64(73);
    auto random_page = [&] {
        auto bytes = std::vector<unsigned char>(SEQLZ_PAGE);
        for (auto& b : bytes) {
            b = static_cast<unsigned char>(rng());
        }
        return bytes;
    };
    auto state = std::make_unique<seqlz_state>();
    for (int round = 0; round < 20; ++round) {
        CAPTURE(round);
        auto const a = random_page();
        REQUIRE(seqlz_find(state.get(), a.data(), seq.data()) == 1);
        auto b = random_page();
        std::copy_n(a.begin() + 500, 8, b.begin() + 1000);
        CHECK(seqlz_find(state.get(), b.data(), seq.data()) == 1);
    }
}

TEST_CASE("seqlz: the matcher reads nothing behind the page") {
    // The page ends where the memory ends, as a page in the kernel can, and the matcher has to look at
    // its last positions: random bytes, which have no match. The mapping is rounded up to the system's
    // pages, which may be smaller than seqlz's, and a page without access follows it.
    auto const ps = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto const span = (SEQLZ_PAGE + ps - 1) / ps * ps;
    auto* const mem =
        static_cast<unsigned char*>(mmap(nullptr, span + ps, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    REQUIRE(mem != MAP_FAILED);
    REQUIRE(mprotect(mem + span, ps, PROT_NONE) == 0);
    unsigned char* const page = mem + span - SEQLZ_PAGE;
    auto const t = default_tables(seqlz_default_own);
    auto st = std::make_unique<seqlz_state>();
    auto seq = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
    auto c = std::vector<unsigned char>(2 * SEQLZ_PAGE);
    auto rng = std::mt19937_64(79);
    for (int round = 0; round < 20; ++round) {
        for (unsigned k = 0; k < SEQLZ_PAGE; ++k) {
            page[k] = static_cast<unsigned char>(rng());
        }
        CHECK(seqlz_find(st.get(), page, seq.data()) == 1);
        CHECK(seqlz_compress(t.get(), st.get(), page, c.data(), static_cast<unsigned>(c.size()), 1) > 0);
    }
    munmap(mem, span + ps);
}

TEST_CASE("seqlz: the matcher looks at every position, also far behind the last match") {
    // Random bytes with a match of 8 bytes at offset 20 at the start, and at p, 600 to 615 bytes into the
    // page, a repeat of exactly 4 bytes at the same offset, with other bytes around it. Only the check of
    // the last offset finds 4 bytes, and only at p: one position later 3 are left. lz4's step, which
    // grows with the literals since the last match, jumps over most of these positions.
    auto state = std::make_unique<seqlz_state>();
    auto seq = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
    auto rng = std::mt19937_64(71);
    for (unsigned p = 600; p < 616; ++p) {
        CAPTURE(p);
        auto bytes = std::vector<unsigned char>(page_size);
        for (auto& b : bytes) {
            b = static_cast<unsigned char>(rng());
        }
        std::copy_n(bytes.begin() + 4, 8, bytes.begin() + 24);
        bytes[23] = static_cast<unsigned char>(bytes[3] + 1);
        bytes[32] = static_cast<unsigned char>(bytes[12] + 1);
        std::copy_n(bytes.begin() + p - 20, 4, bytes.begin() + p);
        bytes[p - 1] = static_cast<unsigned char>(bytes[p - 21] + 1);
        bytes[p + 4] = static_cast<unsigned char>(bytes[p - 16] + 1);
        auto const n = seqlz_find(state.get(), bytes.data(), seq.data());
        REQUIRE(n == 3);
        CHECK(seq[0].literals == 24);
        CHECK(seq[0].match == 8);
        CHECK(seq[0].offset == 20);
        CHECK(seq[1].literals == p - 32);
        CHECK(seq[1].match == 4);
        CHECK(seq[1].offset == 20);
    }
}

TEST_CASE("seqlz: the most bits per page fit into two pages, less than two pages is an error") {
    // The most bits per page byte: matches of 4 bytes without literals, each with an offset that is not
    // one of the last three. 12 literals, then matches up to the end of the page, cycling through 4
    // offsets.
    auto const t = default_tables();
    auto rng = std::mt19937_64(59);
    auto bytes = std::vector<unsigned char>();
    for (int i = 0; i < 12; ++i) {
        bytes.push_back(static_cast<unsigned char>(rng()));
    }
    auto seq = std::vector<seqlz_sequence>{{12, 4, 5}};
    unsigned short const offsets[4] = {5, 6, 7, 9};
    for (unsigned i = 1; i < (page_size - 12) / 4; ++i) {
        seq.push_back({0, 4, offsets[i % 4]});
    }
    seq.push_back({0, 0, 0});
    for (auto const& q : seq) {
        for (unsigned k = 0; k < q.match; ++k) {
            bytes.push_back(bytes[bytes.size() - q.offset]);
        }
    }
    REQUIRE(bytes.size() == page_size);
    auto const literals = std::vector<unsigned char>(bytes.begin(), bytes.begin() + 12);
    // exactly two pages, so ASan sees any write past them
    auto c = std::vector<unsigned char>(2 * page_size);
    auto const len =
        seqlz_encode(t.get(), seq.data(), static_cast<unsigned>(seq.size()), literals.data(), 12, c.data(), 2 * page_size, 0);
    REQUIRE(len > 0);
    c.resize(len);
    auto out = std::vector<unsigned char>(page_size);
    REQUIRE(seqlz_decode(t.get(), c.data(), len, out.data(), nullptr) == 0);
    CHECK(out == bytes);

    auto st = std::make_unique<seqlz_state>();
    auto small = std::vector<unsigned char>(2 * page_size - 1);
    CHECK(seqlz_compress(t.get(), st.get(), bytes.data(), small.data(), static_cast<unsigned>(small.size()), 0) == 0);
    CHECK(seqlz_encode(t.get(),
                       seq.data(),
                       static_cast<unsigned>(seq.size()),
                       literals.data(),
                       12,
                       small.data(),
                       static_cast<unsigned>(small.size()),
                       0) == 0);
}

TEST_CASE("seqlz: the compressor needs a code for every symbol") {
    // a complete code where one match length symbol has none: fine for the decoder, not for the
    // encoder. 4 KiB pages: two codes of 2 bits, ten of 5 and twelve of 6, 2/4 + 10/32 + 12/64 = 1. 16 KiB
    // pages: two of 2 bits, eight of 5 and sixteen of 6, 2/4 + 8/32 + 16/64 = 1. The last symbol none.
    auto l = seqlz_default_own;
#if QUETSCHN_PAGE_BITS == 12
    unsigned char const ml[SEQLZ_LEN_SYMBOLS] = {2, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 0};
#else
    unsigned char const ml[SEQLZ_LEN_SYMBOLS] = {2, 2, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6,
                                                 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 0};
#endif
    std::memcpy(l.ml, ml, sizeof(ml));
    auto* t = static_cast<seqlz_tables*>(::operator new(seqlz_tables_size()));
    auto const init = seqlz_tables_init(t, &l);
    if (init == 0) {
        auto st = std::make_unique<seqlz_state>();
        auto bytes = std::vector<unsigned char>(page_size, 7);
        bytes[100] = 1;
        auto c = std::vector<unsigned char>(2 * page_size);
        CHECK(seqlz_all_symbols(t) == 0);
        CHECK(seqlz_compress(t, st.get(), bytes.data(), c.data(), 2 * page_size, 0) == 0);
    }
    ::operator delete(t);
    CHECK(init == 0); // the lengths above are still a complete code, otherwise this test tests nothing
}

namespace {

// seqlz written from its description in seqlz.h alone, to pin the format: encoder and decoder share
// code, so a change to that code would still roundtrip and only change the format. last_mlf and
// last_cls write the last sequence's token with other fields than the format allows.
std::vector<unsigned char> reference_encode(seqlz_lengths const& lengths,
                                            std::vector<seqlz_sequence> const& seq,
                                            std::vector<unsigned char> const& literals,
                                            unsigned last_mlf = 0,
                                            unsigned last_cls = 0) {
    // canonical Huffman codes, the first code of each length after the codes of all shorter lengths
    auto codes = [](unsigned char const* len, unsigned n) {
        auto count = std::array<unsigned, 17>{};
        for (unsigned s = 0; s < n; ++s) {
            ++count[len[s]];
        }
        count[0] = 0;
        auto next = std::array<unsigned, 17>{};
        auto code = 0U;
        for (unsigned l = 1; l <= 16; ++l) {
            code = (code + count[l - 1]) << 1;
            next[l] = code;
        }
        auto out = std::vector<unsigned>(n);
        for (unsigned s = 0; s < n; ++s) {
            out[s] = next[len[s]]++;
        }
        return out;
    };
    auto const token = codes(lengths.token, SEQLZ_TOKEN_SYMBOLS + 1);
    auto const ll = codes(lengths.ll, SEQLZ_LEN_SYMBOLS);
    auto const ml = codes(lengths.ml, SEQLZ_LEN_SYMBOLS);

    // the bitstream most significant bit first: of each code and number, and of each byte
    auto bits = std::vector<bool>();
    auto put = [&](unsigned v, unsigned n) {
        for (unsigned i = n; i-- > 0;) {
            bits.push_back(((v >> i) & 1U) != 0);
        }
    };
    auto value = [&](std::vector<unsigned> const& code, unsigned char const* len, unsigned v) {
        if (v < 16) {
            put(code[v], len[v]);
            return;
        }
        auto const b = static_cast<unsigned>(std::bit_width(v) - 1);
        put(code[12 + b], len[12 + b]);
        put(v - (1U << b), b);
    };
    auto last_offset = 1U;
    for (std::size_t i = 0; i < seq.size(); ++i) {
        auto const last = i + 1 == seq.size();
        auto const l = static_cast<unsigned>(seq[i].literals);
        auto const m = last ? 0U : static_cast<unsigned>(seq[i].match);
        auto const o = static_cast<unsigned>(seq[i].offset);
        // class 0: the last offset, 1: below 16 in 4 raw bits, 2: below 256 in 8, 3: in the page bits; 4 and
        // 5 the multiples of 8 from 16, divided by 8, in 5 and the page bits - 3
        auto const aligned = o >= 16 && o % 8 == 0;
        auto const cls = last               ? last_cls
                         : o == last_offset ? 0U
                         : o < 16           ? 1U
                         : o < 256          ? (aligned ? 4U : 2U)
                                            : (aligned ? 5U : 3U);
        auto const mlf = last ? last_mlf : std::min(m - 4U, SEQLZ_ML_CAP);
        auto const tok = std::min(l, SEQLZ_LL_CAP) + (mlf << SEQLZ_LL_BITS) + (cls << (SEQLZ_LL_BITS + SEQLZ_ML_BITS));
        if (lengths.token[tok] != 0) {
            put(token[tok], lengths.token[tok]);
        } else {
            // no code: the escape, then the token in 12 bits
            put(token[SEQLZ_ESCAPE], lengths.token[SEQLZ_ESCAPE]);
            put(tok, SEQLZ_ESCAPE_BITS);
        }
        put(cls >= 4 ? o / 8 : o, std::array<unsigned, 6>{0, 4, 8, page_bits, 5, page_bits - 3}[cls]);
        if (l >= SEQLZ_LL_CAP) {
            value(ll, lengths.ll, l - SEQLZ_LL_CAP);
        }
        if (last) {
            break;
        }
        if (m - 4 >= SEQLZ_ML_CAP) {
            value(ml, lengths.ml, m - 4 - SEQLZ_ML_CAP);
        }
        last_offset = o;
    }
    auto out = std::vector<unsigned char>{static_cast<unsigned char>(literals.size()),
                                          static_cast<unsigned char>(literals.size() >> 8)};
    out.insert(out.end(), literals.begin(), literals.end());
    for (std::size_t i = 0; i < bits.size(); i += 8) {
        auto byte = 0U;
        for (std::size_t k = 0; k < 8 && i + k < bits.size(); ++k) {
            byte |= (bits[i + k] ? 1U : 0U) << (7 - k);
        }
        out.push_back(static_cast<unsigned char>(byte));
    }
    return out;
}

} // namespace

TEST_CASE("seqlz: the encoder writes the format as seqlz.h describes it") {
    auto const t = default_tables();
    auto rng = std::mt19937_64(61);
    for (int round = 0; round < 400; ++round) {
        CAPTURE(round);
        auto const p = random_seqlz_page(rng, round % 4);
        auto got = std::vector<unsigned char>(2 * page_size);
        auto const len = seqlz_encode(t.get(),
                                      p.sequences.data(),
                                      static_cast<unsigned>(p.sequences.size()),
                                      p.literals.data(),
                                      static_cast<unsigned>(p.literals.size()),
                                      got.data(),
                                      2 * page_size,
                                      0);
        got.resize(len);
        CHECK(got == reference_encode(seqlz_default_own, p.sequences, p.literals));
    }
}

TEST_CASE("seqlz: the last sequence has ml - 4 = 0 and class 0, any other is invalid") {
    // 8 literals, the page but 16 bytes from 3 back, 8 literals that end the page
    auto const t = default_tables();
    auto literals = std::vector<unsigned char>(16);
    for (unsigned i = 0; i < 16; ++i) {
        literals[i] = static_cast<unsigned char>(i * 37 + 5);
    }
    auto const seq = std::vector<seqlz_sequence>{{8, page_size - 16, 3}, {8, 0, 8}};
    auto page = std::vector<unsigned char>(literals.begin(), literals.begin() + 8);
    for (unsigned i = 0; i < page_size - 16; ++i) {
        page.push_back(page[page.size() - 3]);
    }
    page.insert(page.end(), literals.begin() + 8, literals.end());
    auto out = std::vector<unsigned char>(page_size);
    auto const good = reference_encode(seqlz_default_own, seq, literals);
    REQUIRE(seqlz_decode(t.get(), good.data(), static_cast<unsigned>(good.size()), out.data(), nullptr) == 0);
    CHECK(out == page);
    for (auto const& [mlf, cls] : {std::pair{1U, 0U}, {31U, 0U}, {0U, 1U}, {0U, 3U}, {0U, 4U}, {0U, 5U}, {2U, 2U}}) {
        CAPTURE(mlf);
        CAPTURE(cls);
        auto const bad = reference_encode(seqlz_default_own, seq, literals, mlf, cls);
        CHECK(seqlz_decode(t.get(), bad.data(), static_cast<unsigned>(bad.size()), out.data(), nullptr) == -1);
    }
}

TEST_CASE("seqlz: sequences that need more literals than the header has are rejected, without reading past the input") {
    // 16 literals, then n sequences of 10 literals and 4 bytes from 1 back: the second one has too
    // few literals left. Decoded from a buffer of exactly the page's size, so that the sanitizer sees
    // any read behind it; n moves the end of the input across the literals that would be read.
    auto const t = default_tables();
    auto out = std::vector<unsigned char>(page_size);
    auto const literals = std::vector<unsigned char>(16, 7);
    for (unsigned n = 2; n < 80; ++n) {
        CAPTURE(n);
        auto const seq = std::vector<seqlz_sequence>(n, seqlz_sequence{10, 4, 1});
        auto const encoded = reference_encode(seqlz_default_own, seq, literals);
        auto const c = std::make_unique<unsigned char[]>(encoded.size());
        std::copy(encoded.begin(), encoded.end(), c.get());
        CHECK(seqlz_decode(t.get(), c.get(), static_cast<unsigned>(encoded.size()), out.data(), nullptr) == -1);
    }
}

TEST_CASE("seqlz: pages with coded literals come back, only with scratch, and are safe to decode") {
    auto const t = default_tables(seqlz_default_own);
    auto rng = std::mt19937_64(89);
    auto out = std::vector<unsigned char>(page_size);
    auto scratch = std::vector<unsigned char>(SEQLZ_SCRATCH);
    auto coded_pages = 0;
    auto shortest = std::array<unsigned char, 256>{};
    for (unsigned k = 0; k < 256; ++k) {
        shortest[k] = static_cast<unsigned char>(k);
    }
    std::stable_sort(shortest.begin(), shortest.end(), [](auto a, auto b) {
        return seqlz_lit_sets[0][a] < seqlz_lit_sets[0][b];
    });
    for (int round = 0; round < 1500; ++round) {
        CAPTURE(round);
        auto p = random_seqlz_page(rng, round % 4);
        // literals from the 16 bytes with the shortest codes, so that coding them pays
        for (auto& b : p.literals) {
            b = shortest[rng() % 16];
        }
        {
            auto in = std::size_t{0}, pos = std::size_t{0};
            for (auto const& s : p.sequences) {
                for (unsigned k = 0; k < s.literals; ++k) {
                    p.bytes[pos++] = p.literals[in++];
                }
                for (unsigned k = 0; k < s.match; ++k, ++pos) {
                    p.bytes[pos] = p.bytes[pos - s.offset];
                }
            }
        }
        auto c = std::vector<unsigned char>(2 * page_size);
        auto const len = seqlz_encode(t.get(),
                                      p.sequences.data(),
                                      static_cast<unsigned>(p.sequences.size()),
                                      p.literals.data(),
                                      static_cast<unsigned>(p.literals.size()),
                                      c.data(),
                                      2 * page_size,
                                      1);
        REQUIRE(len > 0);
        c.resize(len);
        auto const coded = (c[1] & 0x80) != 0 && len > 1;
        coded_pages += coded ? 1 : 0;
        REQUIRE(seqlz_decode(t.get(), c.data(), len, out.data(), scratch.data()) == 0);
        CHECK(out == p.bytes);
        CHECK(seqlz_decode(t.get(), c.data(), len, out.data(), nullptr) == (coded ? -1 : 0));
        // flipped bits: never unsafe
        for (int f = 0; f < 3; ++f) {
            c[rng() % c.size()] ^= static_cast<unsigned char>(1U << (rng() % 8));
        }
        auto const ret = seqlz_decode(t.get(), c.data(), len, out.data(), scratch.data());
        CHECK((ret == 0 || ret == -1));
    }
    CHECK(coded_pages > 1000);
}

// A page from random sequences whose literals are drawn from a few bytes, so that coding them pays.
seqlz_page page_with_literals_from(std::mt19937_64& rng, int kind, std::vector<unsigned char> const& bytes) {
    auto p = random_seqlz_page(rng, kind);
    for (auto& b : p.literals) {
        b = bytes[rng() % bytes.size()];
    }
    auto in = std::size_t{0}, pos = std::size_t{0};
    for (auto const& s : p.sequences) {
        for (unsigned k = 0; k < s.literals; ++k) {
            p.bytes[pos++] = p.literals[in++];
        }
        for (unsigned k = 0; k < s.match; ++k, ++pos) {
            p.bytes[pos] = p.bytes[pos - s.offset];
        }
    }
    return p;
}

// a page of bytes drawn as literal table k codes them: byte b with probability 2^-length
std::vector<unsigned char> bytes_as_coded_by(std::mt19937_64& rng, unsigned k) {
    auto weights = std::vector<double>(256);
    for (unsigned b = 0; b < 256; ++b) {
        weights[b] = std::ldexp(1.0, -static_cast<int>(seqlz_lit_sets[k][b]));
    }
    auto draw = std::discrete_distribution<unsigned>(weights.begin(), weights.end());
    auto bytes = std::vector<unsigned char>(page_size);
    for (auto& b : bytes) {
        b = static_cast<unsigned char>(draw(rng));
    }
    return bytes;
}

std::vector<unsigned char> encode_coded(seqlz_tables const* t, seqlz_page const& p) {
    auto c = std::vector<unsigned char>(2 * page_size);
    auto const len = seqlz_encode(t,
                                  p.sequences.data(),
                                  static_cast<unsigned>(p.sequences.size()),
                                  p.literals.data(),
                                  static_cast<unsigned>(p.literals.size()),
                                  c.data(),
                                  2 * page_size,
                                  1);
    REQUIRE(len > 0);
    c.resize(len);
    return c;
}

TEST_CASE("seqlz: coded literals use the literal table with the fewest bits for them") {
    auto const t = default_tables(seqlz_default_own);
    auto rng = std::mt19937_64(5);
    auto chosen = std::array<int, SEQLZ_LIT_SETS>{};
    for (int round = 0; round < 400; ++round) {
        CAPTURE(round);
        // literals as one of the tables expects them; in every other page every 8th literal, the first
        // stream, as another table expects them
        auto const want = static_cast<unsigned>(round) % SEQLZ_LIT_SETS;
        auto p = page_with_literals_from(rng, round % 4, bytes_as_coded_by(rng, want));
        if (round % 2 == 1) {
            auto const other = bytes_as_coded_by(rng, (want + 1) % SEQLZ_LIT_SETS);
            auto in = std::size_t{0}, pos = std::size_t{0};
            for (auto const& sq : p.sequences) {
                for (unsigned k = 0; k < sq.literals; ++k, ++in) {
                    if (in % 8 == 0) {
                        p.literals[in] = other[in % other.size()];
                    }
                    p.bytes[pos++] = p.literals[in];
                }
                for (unsigned k = 0; k < sq.match; ++k, ++pos) {
                    p.bytes[pos] = p.bytes[pos - sq.offset];
                }
            }
        }
        auto const c = encode_coded(t.get(), p);
        if ((c[1] & 0x80) == 0) {
            continue;
        }
        auto best = 0U;
        auto best_bits = ~0ULL;
        for (unsigned k = 0; k < SEQLZ_LIT_SETS; ++k) {
            auto bits = 0ULL;
            for (auto b : p.literals) {
                bits += seqlz_lit_sets[k][b];
            }
            if (bits < best_bits) {
                best = k;
                best_bits = bits;
            }
        }
        CHECK((c[2] & 7U) == best);
        ++chosen[c[2] & 7U];
    }
    // not always the same table
    CHECK(std::count_if(chosen.begin(), chosen.end(), [](int n) {
              return n > 0;
          }) >= 4);
}

// The canonical code of a literal table, most significant bit first: codes of the same length in the
// order of the bytes, shorter ones first. Returns code -> byte per length.
std::vector<std::vector<std::pair<unsigned, unsigned char>>> canonical_codes(unsigned char const* lengths) {
    auto by_length = std::vector<std::vector<std::pair<unsigned, unsigned char>>>(SEQLZ_LIT_BITS + 1);
    auto code = 0U;
    for (unsigned l = 1; l <= SEQLZ_LIT_BITS; ++l) {
        for (unsigned b = 0; b < 256; ++b) {
            if (lengths[b] == l) {
                by_length[l].emplace_back(code++, static_cast<unsigned char>(b));
            }
        }
        code <<= 1;
    }
    return by_length;
}

// In a page with coded literals (docs/format.md): the width of its stream sizes, the length of its header, and
// the size of literal stream st, width bits from bit st * width of the bytes behind byte 2.
unsigned size_width(std::vector<unsigned char> const& c) {
    return SEQLZ_SIZE_BITS_MIN + ((c[2] >> 3U) & 7U);
}

std::size_t lit_header(std::vector<unsigned char> const& c) {
    return SEQLZ_LIT_HEADER(size_width(c));
}

unsigned stream_size(std::vector<unsigned char> const& c, unsigned st) {
    auto v = 0U;
    for (unsigned i = 0; i < size_width(c); ++i) {
        auto const bit = st * size_width(c) + i;
        v |= ((c[3 + bit / 8] >> (bit % 8)) & 1U) << i;
    }
    return v;
}

void set_stream_size(std::vector<unsigned char>& c, unsigned st, unsigned size) {
    for (unsigned i = 0; i < size_width(c); ++i) {
        auto const bit = st * size_width(c) + i;
        auto const mask = static_cast<unsigned char>(1U << (bit % 8));
        c[3 + bit / 8] = static_cast<unsigned char>((size >> i) & 1U ? c[3 + bit / 8] | mask : c[3 + bit / 8] & ~mask);
    }
}

// the page c with its stream sizes written in width bits instead
std::vector<unsigned char> with_size_width(std::vector<unsigned char> const& c, unsigned width) {
    auto out = std::vector<unsigned char>(c.begin(), c.begin() + 3);
    out[2] = static_cast<unsigned char>((c[2] & 7U) | (width - SEQLZ_SIZE_BITS_MIN) << 3);
    out.resize(SEQLZ_LIT_HEADER(width));
    for (unsigned st = 0; st < 8; ++st) {
        set_stream_size(out, st, stream_size(c, st));
    }
    out.insert(out.end(), c.begin() + static_cast<std::ptrdiff_t>(lit_header(c)), c.end());
    return out;
}

TEST_CASE("seqlz: the stream sizes take as many bits as the largest needs, 5 to 12") {
    auto const t = default_tables(seqlz_default_own);
    auto rng = std::mt19937_64(31);
    auto out = std::vector<unsigned char>(page_size);
    auto scratch = std::vector<unsigned char>(SEQLZ_SCRATCH);
    auto state = std::make_unique<seqlz_state>();
    auto widths = std::set<unsigned>();
    for (int round = 0; round < 200; ++round) {
        CAPTURE(round);
        // from 1/64 of a page to a page of bytes drawn as a literal table codes them, then zeros: streams of
        // all sizes
        auto const bytes = bytes_as_coded_by(rng, static_cast<unsigned>(round) % SEQLZ_LIT_SETS);
        auto page = std::vector<unsigned char>(page_size);
        std::copy_n(bytes.begin(), std::size_t{page_size} >> (6 - round % 7), page.begin());
        auto c = std::vector<unsigned char>(2 * page_size);
        c.resize(seqlz_compress(t.get(), state.get(), page.data(), c.data(), 2 * page_size, 1));
        REQUIRE(c.size() > 0);
        if ((c[1] & 0x80) == 0) {
            continue;
        }
        auto largest = 0U;
        for (unsigned st = 0; st < 8; ++st) {
            largest = std::max(largest, stream_size(c, st));
        }
        auto const width = size_width(c);
        CAPTURE(width);
        CHECK(largest < 1U << width);
        CHECK((width == SEQLZ_SIZE_BITS_MIN || largest >= 1U << (width - 1)));
        widths.insert(width);
        // coded only where that saves 1/16 of the literals and 51 bytes, whatever the header takes
        auto coded = 0U;
        for (unsigned st = 0; st < 8; ++st) {
            coded += stream_size(c, st);
        }
        auto const n_lit = static_cast<unsigned>(c[0] | (c[1] & 0x7f) << 8);
        CHECK(coded + 51 < n_lit - n_lit / 16);
        // a decoder takes any width from 5 to 12, and bits 6 and 7 of byte 2 must be 0
        for (unsigned w = width; w <= SEQLZ_SIZE_BITS_MAX; ++w) {
            auto const wider = with_size_width(c, w);
            REQUIRE(seqlz_decode(t.get(), wider.data(), static_cast<unsigned>(wider.size()), out.data(), scratch.data()) == 0);
            CHECK(out == page);
        }
        for (unsigned bit = 6; bit < 8; ++bit) {
            auto reserved = c;
            reserved[2] = static_cast<unsigned char>(reserved[2] | 1U << bit);
            CHECK(seqlz_decode(t.get(), reserved.data(), static_cast<unsigned>(reserved.size()), out.data(), scratch.data()) ==
                  -1);
        }
    }
    CHECK(widths.size() >= 4);
}

TEST_CASE("seqlz: a literal stream without its last byte is rejected") {
    // The codes of a stream's literals have to fit into its size. The decoder decodes past a stream
    // into the next one, and once accepted up to 50 bits more than the size, for the symbols it decodes
    // behind the last literal: then a stream one byte too short decoded with the next stream's bits.
    auto const t = default_tables(seqlz_default_own);
    auto rng = std::mt19937_64(23);
    auto out = std::vector<unsigned char>(page_size);
    auto scratch = std::vector<unsigned char>(SEQLZ_SCRATCH);
    auto checked = 0;
    for (int round = 0; round < 100; ++round) {
        CAPTURE(round);
        auto const p =
            page_with_literals_from(rng, round % 4, bytes_as_coded_by(rng, static_cast<unsigned>(round) % SEQLZ_LIT_SETS));
        auto const c = encode_coded(t.get(), p);
        if ((c[1] & 0x80) == 0) {
            continue;
        }
        REQUIRE(seqlz_decode(t.get(), c.data(), static_cast<unsigned>(c.size()), out.data(), scratch.data()) == 0);
        auto end = lit_header(c);
        for (unsigned st = 0; st < 8; ++st) {
            CAPTURE(st);
            auto const size = stream_size(c, st);
            end += size;
            if (size == 0) {
                continue;
            }
            auto shorter = c;
            set_stream_size(shorter, st, size - 1);
            shorter.erase(shorter.begin() + static_cast<std::ptrdiff_t>(end - 1));
            CHECK(seqlz_decode(t.get(), shorter.data(), static_cast<unsigned>(shorter.size()), out.data(), scratch.data()) ==
                  -1);
            ++checked;
        }
    }
    CHECK(checked > 500);
}

TEST_CASE("seqlz: coded literals are 8 streams of canonical codes, most significant bit first") {
    auto const t = default_tables(seqlz_default_own);
    auto rng = std::mt19937_64(11);
    auto checked = 0;
    for (int round = 0; round < 300; ++round) {
        CAPTURE(round);
        auto const p =
            page_with_literals_from(rng, round % 4, bytes_as_coded_by(rng, static_cast<unsigned>(round) % SEQLZ_LIT_SETS));
        auto const c = encode_coded(t.get(), p);
        if ((c[1] & 0x80) == 0) {
            continue;
        }
        ++checked;
        // header: u16 0x8000 | literal bytes, u8 the table | width << 3, 8 stream sizes of width bits
        auto const n_lit = static_cast<unsigned>(c[0] | (c[1] & 0x7f) << 8);
        REQUIRE(n_lit == p.literals.size());
        auto const codes = canonical_codes(seqlz_lit_sets[c[2] & 7U]);
        auto pos = lit_header(c);
        auto literals = std::vector<unsigned char>(n_lit);
        for (unsigned st = 0; st < 8; ++st) {
            auto const size = std::size_t{stream_size(c, st)};
            REQUIRE(pos + size <= c.size());
            auto bit = std::size_t{0};
            auto next = [&] {
                REQUIRE(bit < 8 * size);
                auto const v = (c[pos + bit / 8] >> (7 - bit % 8)) & 1U;
                ++bit;
                return v;
            };
            for (auto k = std::size_t{st}; k < n_lit; k += 8) {
                auto code = 0U;
                auto found = false;
                for (unsigned l = 1; l <= SEQLZ_LIT_BITS && !found; ++l) {
                    code = code << 1 | next();
                    for (auto const& [cd, b] : codes[l]) {
                        if (cd == code) {
                            literals[k] = b;
                            found = true;
                        }
                    }
                }
                REQUIRE(found);
            }
            // the stream ends with the byte of its last bits
            CHECK((bit + 7) / 8 == size);
            pos += size;
        }
        CHECK(literals == p.literals);
        // then the sequences' bitstream, as in the page with raw literals
        auto raw = std::vector<unsigned char>(2 * page_size);
        auto const raw_len = seqlz_encode(t.get(),
                                          p.sequences.data(),
                                          static_cast<unsigned>(p.sequences.size()),
                                          p.literals.data(),
                                          static_cast<unsigned>(p.literals.size()),
                                          raw.data(),
                                          2 * page_size,
                                          0);
        REQUIRE(raw_len == 2 + n_lit + (c.size() - pos));
        CHECK(std::equal(c.begin() + static_cast<std::ptrdiff_t>(pos), c.end(), raw.begin() + 2 + n_lit));
    }
    CHECK(checked > 100);
}

TEST_CASE("seqlz: any page with coded literals is safe for the decoder") {
    auto const t = default_tables(seqlz_default_own);
    auto st = std::make_unique<seqlz_state>();
    auto rng = std::mt19937_64(23);
    auto out = std::vector<unsigned char>(page_size);
    auto scratch = std::vector<unsigned char>(SEQLZ_SCRATCH);
    for (int round = 0; round < 20000; ++round) {
        CAPTURE(round);
        auto c = std::vector<unsigned char>();
        if (round % 2 == 0) {
            // a coded header and random streams
            c.resize(1 + rng() % 5000);
            for (auto& b : c) {
                b = static_cast<unsigned char>(rng());
            }
            // any byte 2, also with the bits that must be 0
            auto const b2 = static_cast<unsigned char>(rng());
            auto const width = SEQLZ_SIZE_BITS_MIN + ((b2 >> 3U) & 7U);
            if (c.size() >= SEQLZ_LIT_HEADER(width)) {
                auto const n_lit = static_cast<unsigned>(rng() % (page_size + 1));
                auto const rest = static_cast<unsigned>(c.size() - SEQLZ_LIT_HEADER(width));
                c[0] = static_cast<unsigned char>(n_lit);
                c[1] = static_cast<unsigned char>(0x80 | n_lit >> 8);
                c[2] = rng() % 4 == 0 ? b2 : static_cast<unsigned char>(b2 & 0x3fU);
                for (unsigned k = 0; k < 8; ++k) {
                    auto const size = static_cast<unsigned>(rng() % (rest / 8 + 2));
                    set_stream_size(c, k, size);
                }
            }
        } else {
            // a page with coded literals, some bytes replaced and maybe cut short
            auto few = std::vector<unsigned char>(3 + rng() % 30);
            for (auto& b : few) {
                b = static_cast<unsigned char>(rng());
            }
            auto const p = page_with_literals_from(rng, round % 4, few);
            c = std::vector<unsigned char>(2 * page_size);
            auto const len = seqlz_compress(t.get(), st.get(), p.bytes.data(), c.data(), 2 * page_size, 1);
            REQUIRE(len > 0);
            c.resize(len);
            for (auto f = 1 + rng() % 8; f > 0; --f) {
                c[rng() % c.size()] = static_cast<unsigned char>(rng());
            }
            if (rng() % 4 == 0) {
                c.resize(1 + rng() % c.size());
            }
        }
        auto const ret = seqlz_decode(t.get(), c.data(), static_cast<unsigned>(c.size()), out.data(), scratch.data());
        CHECK((ret == 0 || ret == -1));
    }
}

// A page of bytes drawn as literal table k codes them, with a few blocks copied from earlier in the
// page: few sequences and many literals that pay to code.
std::vector<unsigned char> skewed_page(std::mt19937_64& rng) {
    auto bytes = bytes_as_coded_by(rng, static_cast<unsigned>(rng() % SEQLZ_LIT_SETS));
    for (int k = 0; k < 8; ++k) {
        auto const len = 50 + rng() % 250;
        auto const to = 1 + rng() % (page_size - len);
        auto const from = rng() % to;
        for (std::size_t i = 0; i < len; ++i) {
            bytes[to + i] = bytes[from + i];
        }
    }
    return bytes;
}

TEST_CASE("seqlz: the compressor with coded literals, pages come back") {
    auto const t = default_tables(seqlz_default_own);
    auto st = std::make_unique<seqlz_state>();
    auto rng = std::mt19937_64(97);
    auto out = std::vector<unsigned char>(page_size);
    auto scratch = std::vector<unsigned char>(SEQLZ_SCRATCH);
    auto coded_pages = 0;
    for (int round = 0; round < 800; ++round) {
        CAPTURE(round);
        auto bytes = std::vector<unsigned char>();
        if (round % 2 == 0) {
            bytes = skewed_page(rng);
        } else {
            // most bytes zero: many sequences
            bytes = random_seqlz_page(rng, round % 4).bytes;
            for (auto& b : bytes) {
                if (rng() % 4 != 0) {
                    b = 0;
                }
            }
        }
        auto c = std::vector<unsigned char>(2 * page_size);
        auto const len = seqlz_compress(t.get(), st.get(), bytes.data(), c.data(), 2 * page_size, 1);
        REQUIRE(len > 0);
        coded_pages += (c[1] & 0x80) != 0 ? 1 : 0;
        REQUIRE(seqlz_decode(t.get(), c.data(), len, out.data(), scratch.data()) == 0);
        CHECK(out == bytes);
    }
    CHECK(coded_pages > 350);
}

TEST_CASE("seqlz: the compressor codes the literals of every page where that pays, also with many sequences") {
    auto const t = default_tables(seqlz_default_own);
    auto st = std::make_unique<seqlz_state>();
    auto rng = std::mt19937_64(3);
    auto c = std::vector<unsigned char>(2 * page_size);
    auto e = std::vector<unsigned char>(2 * page_size);
    for (int round = 0; round < 20; ++round) {
        CAPTURE(round);
        // table 4 gives 00 2 bits: literals that save clearly more than the 1/16 and 51 bytes of the rule
        auto const skewed = bytes_as_coded_by(rng, 4);
        // a page of records of 4 skewed literals and 4 bytes that are the same in every record, one
        // sequence each: a page that takes long to compress, which the budget of before kept raw
        auto many = std::vector<unsigned char>(page_size);
        for (std::size_t r = 0; r < page_size / 8; ++r) {
            for (std::size_t k = 0; k < 4; ++k) {
                many[8 * r + k] = skewed[4 * r + k];
                many[8 * r + 4 + k] = static_cast<unsigned char>(0xa5 + k);
            }
        }
        auto const n_many = seqlz_compress(t.get(), st.get(), many.data(), c.data(), 2 * page_size, 1);
        REQUIRE(n_many > 0);
        CHECK((c[1] & 0x80) != 0);
        // the same bytes as seqlz_encode() with coded literals writes for the matcher's sequences
        auto seq = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
        auto const n = seqlz_find(st.get(), many.data(), seq.data());
        auto lits = std::vector<unsigned char>();
        auto pos = std::size_t{0};
        for (unsigned i = 0; i < n; ++i) {
            lits.insert(lits.end(),
                        many.begin() + static_cast<std::ptrdiff_t>(pos),
                        many.begin() + static_cast<std::ptrdiff_t>(pos + seq[i].literals));
            pos += seq[i].literals + seq[i].match;
        }
        CHECK(n > 400 * (page_size / 4096));
        auto const len =
            seqlz_encode(t.get(), seq.data(), n, lits.data(), static_cast<unsigned>(lits.size()), e.data(), 2 * page_size, 1);
        REQUIRE(len == n_many);
        CHECK(std::equal(c.begin(), c.begin() + n_many, e.begin()));
    }
}

TEST_CASE("seqlz: a page without matches but with literals that code well gets them coded") {
    auto const t = default_tables(seqlz_default_own);
    auto st = std::make_unique<seqlz_state>();
    auto rng = std::mt19937_64(59);
    auto scratch = std::vector<unsigned char>(SEQLZ_SCRATCH);
    auto out = std::vector<unsigned char>(page_size);
    auto c = std::vector<unsigned char>(2 * page_size);
    for (int round = 0; round < 50; ++round) {
        CAPTURE(round);
        // bytes as table 0 codes them, almost no repeats of 5 bytes: the raw literals are about a page,
        // their bitstream does not fit next to them in the first page of dst
        auto const bytes = bytes_as_coded_by(rng, 0);
        auto const len = seqlz_compress(t.get(), st.get(), bytes.data(), c.data(), 2 * page_size, 1);
        REQUIRE(len > 0);
        CHECK((c[1] & 0x80) != 0);
        CHECK(len < zram_huge); // not stored raw by zram
        REQUIRE(seqlz_decode(t.get(), c.data(), len, out.data(), scratch.data()) == 0);
        CHECK(out == bytes);
        // random bytes do not code: the page stays raw, and whole
        auto random = std::vector<unsigned char>(page_size);
        for (auto& b : random) {
            b = static_cast<unsigned char>(rng());
        }
        auto const rlen = seqlz_compress(t.get(), st.get(), random.data(), c.data(), 2 * page_size, 1);
        REQUIRE(rlen > 0);
        CHECK((c[1] & 0x80) == 0);
        REQUIRE(seqlz_decode(t.get(), c.data(), rlen, out.data(), nullptr) == 0);
        CHECK(out == random);
    }
}
