// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Turns a raw page dump into a corpus. The dump of a zram device is the real thing, the pages that
// reclaim swapped out (PLAN.md Phase 1, collector 2):
//
//   mkdir -m 700 -p ~/quetschn-corpus
//   sudo dd if=/dev/zram0 bs=1M iflag=direct status=progress |
//       dd of=$HOME/quetschn-corpus/zram0.raw bs=4096 iflag=fullblock conv=sparse

#include "import_raw.h"

#include <unistd.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-import-raw --in <dump> --out <base> [--name <process name>]\n"
                 "\n"
                 "Writes every page of the dump that is not all zero to <base>.pages and <base>.tsv, mode 0600.\n"
                 "--name is stored as the process name of every page, default: the dump's file name.\n");
}

} // namespace

int main(int argc, char** argv) {
    auto in = std::string();
    auto out = std::string();
    auto name = std::string();
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--in" && has_value) {
            in = argv[++i];
        } else if (arg == "--out" && has_value) {
            out = argv[++i];
        } else if (arg == "--name" && has_value) {
            name = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (in.empty() || out.empty()) {
        usage();
        return 2;
    }
    if (name.empty()) {
        name = std::filesystem::path(in).stem().string();
    }
    try {
        auto const r = quetschn::import_raw(in, out, name, static_cast<std::size_t>(::sysconf(_SC_PAGESIZE)));
        std::fprintf(stderr, "%zu pages written, %zu all-zero pages skipped\n", r.pages_written, r.zero_pages);
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
