// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Trains the static Huffman tables of seqlz (explore/seqlz.h) on a corpus: the matches of lz4 or
// lz4hc on every page, split into seqlz's symbols, counted, and turned into code lengths of at most
// SEQLZ_MAX_BITS bits. Writes a C initializer for explore/seqlz_default_tables.c, or with --blob the
// 577 bytes that zram's dictionary parameter can carry.

#include "harness.h"
#include "kernel_codecs/zram_codec.h"
#include "lz_analysis.h"
#include "page_stats.h"
#include "seqlz.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Huffman code lengths, limited to max_bits by halving the counts until they fit.
std::vector<unsigned char> code_lengths(std::vector<double> counts, unsigned max_bits) {
    auto const n = counts.size();
    while (true) {
        // nodes: leaves 0..n-1, then inner nodes; parent[] to walk up
        auto parent = std::vector<std::size_t>(2 * n, 0);
        using item = std::pair<double, std::size_t>;
        auto q = std::priority_queue<item, std::vector<item>, std::greater<>>();
        for (std::size_t i = 0; i < n; ++i) {
            q.emplace(counts[i], i);
        }
        auto next = n;
        while (q.size() > 1) {
            auto const a = q.top();
            q.pop();
            auto const b = q.top();
            q.pop();
            parent[a.second] = next;
            parent[b.second] = next;
            q.emplace(a.first + b.first, next++);
        }
        auto const root = next - 1;
        auto lengths = std::vector<unsigned char>(n);
        auto longest = 0U;
        for (std::size_t i = 0; i < n; ++i) {
            auto depth = 0U;
            for (auto k = i; k != root; k = parent[k]) {
                ++depth;
            }
            lengths[i] = static_cast<unsigned char>(depth);
            longest = std::max(longest, depth);
        }
        if (longest <= max_bits) {
            return lengths;
        }
        for (auto& c : counts) {
            c = std::max(1.0, std::floor(c / 2));
        }
    }
}

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-seqlz-train --corpus <base> [--codec lz4|lz4hc|seqlz] [--level <n>] [--blob <file>]\n"
                 "\n"
                 "Counts seqlz's symbols over the matches of --codec (default lz4; seqlz is its own matcher) on\n"
                 "every page. Prints the\n"
                 "code lengths as a C initializer, or writes them to --blob for zram's dictionary parameter.\n");
}

} // namespace

int main(int argc, char** argv) {
    auto base = std::string();
    auto blob = std::string();
    auto const* codec = &quetschn_codec_lz4;
    auto own_matcher = false;
    int level = QUETSCHN_LEVEL_DEFAULT;
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--corpus" && has_value) {
            base = argv[++i];
        } else if (arg == "--blob" && has_value) {
            blob = argv[++i];
        } else if (arg == "--codec" && has_value) {
            auto const name = std::string_view(argv[++i]);
            if (name == "seqlz") {
                own_matcher = true;
            } else if (name == "lz4hc") {
                codec = &quetschn_codec_lz4hc;
            } else if (name != "lz4") {
                usage();
                return 2;
            }
        } else if (arg == "--level" && has_value) {
            auto const v = std::string_view(argv[++i]);
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), level);
            if (ec != std::errc{} || ptr != v.data() + v.size()) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (base.empty()) {
        usage();
        return 2;
    }
    try {
        auto const c = quetschn::load_corpus(base);
        auto params = quetschn_params{};
        params.level = level;
        params.page_size = static_cast<unsigned int>(c.page_size);
        if (codec->setup_params(&params) != 0) {
            throw std::invalid_argument(std::string(codec->name) + " rejects level " + std::to_string(level));
        }
        auto stream = quetschn_stream{};
        if (codec->create(&params, &stream) != 0) {
            throw std::runtime_error(std::string(codec->name) + ": create failed");
        }

        // start at 1: a symbol that never occurs here must still get a code
        auto token = std::vector<double>(SEQLZ_TOKEN_SYMBOLS, 1.0);
        auto ll = std::vector<double>(SEQLZ_LEN_SYMBOLS, 1.0);
        auto ml = std::vector<double>(SEQLZ_LEN_SYMBOLS, 1.0);
        auto off = std::vector<double>(SEQLZ_OFF_SYMBOLS, 1.0);
        auto dst = std::vector<std::uint8_t>(2 * c.page_size);
        auto state = std::make_unique<seqlz_state>();
        auto seqs = std::vector<seqlz_sequence>(SEQLZ_MAX_SEQUENCES);
        auto pages = std::size_t{0};
        for (std::size_t i = 0; i < c.size(); ++i) {
            auto const src = c.page(i);
            if (quetschn::analyze_page(src).same_filled) {
                continue;
            }
            ++pages;
            auto parsed = quetschn::parsed_page{};
            if (own_matcher) {
                // seqlz's own matcher, as seqlz-fast uses it
                auto const n = seqlz_find(state.get(), src.data(), seqs.data());
                for (unsigned k = 0; k < n; ++k) {
                    parsed.sequences.push_back({seqs[k].literals, seqs[k].match, seqs[k].offset});
                }
            } else {
                auto len = static_cast<unsigned int>(dst.size());
                if (codec->compress(&params, &stream, src.data(), static_cast<unsigned int>(src.size()), dst.data(), &len) !=
                    0) {
                    throw std::runtime_error(std::string(codec->name) + ": compress failed");
                }
                parsed = quetschn::parse_lz4(dst.data(), len);
            }
            // the same symbols and the same repeat offsets as seqlz_encode()
            auto rep = std::array<unsigned, 3>{1, 4, 8};
            auto extra = 0U;
            for (auto const& s : parsed.sequences) {
                token[seqlz_token(s.literals, s.match)] += 1;
                if (s.literals >= SEQLZ_LL_CAP) {
                    ll[seqlz_len_symbol(s.literals - SEQLZ_LL_CAP, &extra)] += 1;
                }
                if (s.match == 0) {
                    continue;
                }
                if (s.match - 4 >= SEQLZ_ML_CAP) {
                    ml[seqlz_len_symbol(s.match - 4 - SEQLZ_ML_CAP, &extra)] += 1;
                }
                auto r = 0U;
                while (r < 3 && rep[r] != s.offset) {
                    ++r;
                }
                if (r < 3) {
                    off[r] += 1;
                    for (; r > 0; --r) {
                        rep[r] = rep[r - 1];
                    }
                } else {
                    off[seqlz_off_bucket(s.offset, &extra)] += 1;
                    rep[2] = rep[1];
                    rep[1] = rep[0];
                }
                rep[0] = s.offset;
            }
        }
        codec->destroy(&stream);
        codec->release_params(&params);

        auto lengths = seqlz_lengths{};
        auto const l_token = code_lengths(token, SEQLZ_TOKEN_BITS);
        auto const l_ll = code_lengths(ll, SEQLZ_MAX_BITS);
        auto const l_ml = code_lengths(ml, SEQLZ_MAX_BITS);
        auto const l_off = code_lengths(off, SEQLZ_MAX_BITS);
        std::copy(l_token.begin(), l_token.end(), lengths.token);
        std::copy(l_ll.begin(), l_ll.end(), lengths.ll);
        std::copy(l_ml.begin(), l_ml.end(), lengths.ml);
        std::copy(l_off.begin(), l_off.end(), lengths.off);

        if (!blob.empty()) {
            auto out = std::ofstream(blob, std::ios::binary);
            out.write(reinterpret_cast<char const*>(&lengths), sizeof(lengths));
            if (!out) {
                throw std::runtime_error("cannot write " + blob);
            }
            std::printf("%zu pages, %s level %d, %zu bytes to %s\n",
                        pages,
                        own_matcher ? "seqlz" : codec->name,
                        params.level,
                        sizeof(lengths),
                        blob.c_str());
            return 0;
        }
        auto print = [](char const* name, unsigned char const* l, unsigned n) {
            std::printf("    .%s = {", name);
            for (unsigned i = 0; i < n; ++i) {
                std::printf("%s%u", i == 0 ? "" : ", ", l[i]);
            }
            std::printf("},\n");
        };
        std::printf("/* trained on %zu pages of %s, %s level %d */\n{\n",
                    pages,
                    base.c_str(),
                    own_matcher ? "seqlz" : codec->name,
                    params.level);
        print("token", lengths.token, SEQLZ_TOKEN_SYMBOLS);
        print("ll", lengths.ll, SEQLZ_LEN_SYMBOLS);
        print("ml", lengths.ml, SEQLZ_LEN_SYMBOLS);
        print("off", lengths.off, SEQLZ_OFF_SYMBOLS);
        std::printf("}\n");
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
