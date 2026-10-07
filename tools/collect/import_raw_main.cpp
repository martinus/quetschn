// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Turns a raw page dump into a corpus. The dump of a zram device is the real thing, the pages that
// reclaim swapped out (docs/plan.md Phase 1, collector 2):
//
//   mkdir -m 700 -p ~/quetschn-corpus
//   sudo dd if=/dev/zram0 bs=1M iflag=direct status=progress |
//       dd of=$HOME/quetschn-corpus/zram0.raw bs=4096 iflag=fullblock conv=sparse

#include "import_raw.h"

#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-import-raw --in <dump> --out <base> [--name <process name>] [--page-size <bytes>]\n"
                 "\n"
                 "Writes every page of the dump that is not all zero to <base>.pages and <base>.tsv, mode 0600.\n"
                 "--name is stored as the process name of every page, default: the dump's file name.\n"
                 "--page-size is the page size of the machine the dump is from, e.g. 16384 for a zram device of\n"
                 "an Android with 16 KiB pages; default: this machine's.\n");
}

} // namespace

int main(int argc, char** argv) {
    auto in = std::string();
    auto out = std::string();
    auto name = std::string();
    auto page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--in" && has_value) {
            in = argv[++i];
        } else if (arg == "--out" && has_value) {
            out = argv[++i];
        } else if (arg == "--name" && has_value) {
            name = argv[++i];
        } else if (arg == "--page-size" && has_value) {
            page_size = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
            if (page_size < 4096 || (page_size & (page_size - 1)) != 0) {
                usage();
                return 2;
            }
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
        auto const r = quetschn::import_raw(in, out, name, page_size);
        std::fprintf(stderr, "%zu pages written, %zu all-zero pages skipped\n", r.pages_written, r.zero_pages);
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
