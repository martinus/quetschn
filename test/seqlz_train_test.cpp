// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "seqlz_train.h"

// seqlz.h is C, written for the kernel
extern "C" {
#include "seqlz.h"
}

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using quetschn::read_seqlz_counts;
using quetschn::seqlz_build_tables;
using quetschn::seqlz_counts;
using quetschn::write_seqlz_counts;

namespace {

seqlz_counts committed_counts() {
    auto const path = std::string(QUETSCHN_SOURCE_DIR) + "/bench/seqlz_counts_" + (SEQLZ_PAGE == 4096 ? "4k" : "16k") + ".txt";
    auto in = std::ifstream(path);
    REQUIRE_MESSAGE(in.good(), "cannot read ", path);
    return read_seqlz_counts(in);
}

} // namespace

TEST_CASE("seqlz_train: the tables compiled in are built from the counts in bench/") {
    auto const t = seqlz_build_tables(committed_counts());
    CHECK(t.token == std::vector<unsigned char>(seqlz_default_own.token, seqlz_default_own.token + SEQLZ_TOKEN_SYMBOLS + 1));
    CHECK(t.ll == std::vector<unsigned char>(seqlz_default_own.ll, seqlz_default_own.ll + SEQLZ_LEN_SYMBOLS));
    CHECK(t.ml == std::vector<unsigned char>(seqlz_default_own.ml, seqlz_default_own.ml + SEQLZ_LEN_SYMBOLS));
    REQUIRE(t.lit.size() == SEQLZ_LIT_SETS);
    for (unsigned k = 0; k < SEQLZ_LIT_SETS; ++k) {
        CAPTURE(k);
        CHECK(t.lit[k] == std::vector<unsigned char>(seqlz_lit_sets[k], seqlz_lit_sets[k] + 256));
    }
}

TEST_CASE("seqlz_train: each literal table's counts are summed over many pages, none is one page's") {
    auto const c = committed_counts();
    for (auto const pages : c.lit_pages) {
        CHECK(pages >= 100);
    }
}

TEST_CASE("seqlz_train: the counts come back as they were written, other sizes are refused") {
    auto c = committed_counts();
    c.token[7] = 0.25; // a reseeded literal table's histogram is not whole numbers
    c.lit[3][200] = 1.0 / 3.0;
    auto out = std::ostringstream();
    write_seqlz_counts(out, c);
    auto in = std::istringstream(out.str());
    auto const back = read_seqlz_counts(in);
    CHECK(back.comments == c.comments);
    CHECK(back.token == c.token);
    CHECK(back.ll == c.ll);
    CHECK(back.ml == c.ml);
    CHECK(back.lit == c.lit);
    CHECK(back.lit_pages == c.lit_pages);

    c.ll.pop_back();
    out = std::ostringstream();
    write_seqlz_counts(out, c);
    in = std::istringstream(out.str());
    CHECK_THROWS_AS((void)read_seqlz_counts(in), std::runtime_error);

    in = std::istringstream(out.str().substr(0, out.str().size() / 2));
    CHECK_THROWS_AS((void)read_seqlz_counts(in), std::runtime_error);

    in = std::istringstream(std::string("token 3 1 2 3\n"));
    CHECK_THROWS_AS((void)read_seqlz_counts(in), std::runtime_error);

    // a number too many on the token line
    out = std::ostringstream();
    write_seqlz_counts(out, committed_counts());
    auto text = out.str();
    text.insert(text.find('\n', text.find("\ntoken ") + 1), " 5");
    in = std::istringstream(text);
    CHECK_THROWS_AS((void)read_seqlz_counts(in), std::runtime_error);
}
