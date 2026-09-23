// SPDX-License-Identifier: MIT OR GPL-2.0-only
//
// Samples resident anonymous memory of running processes into a page corpus. See PLAN.md, Phase 1,
// collector 1. This corpus is biased: zram stores cold, reclaimed pages, and these are resident ones.
// Good enough to shake out the harness, not for headline numbers.

#include "resident.h"

#include <unistd.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: quetschn-collect-resident --out <base> [--pid <pid>]... [--max-per-process <n>] [--seed <n>]\n"
                 "\n"
                 "Writes <base>.pages and <base>.tsv, mode 0600. Existing files are never overwritten.\n"
                 "Without --pid, all processes this user may read are sampled, except this one.\n"
                 "\n"
                 "The pages contain whatever was in memory: keys, passwords, personal data.\n"
                 "Never commit or publish them.\n");
}

template <typename T>
bool parse(std::string_view s, T& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

std::vector<pid_t> all_pids() {
    auto pids = std::vector<pid_t>();
    for (auto const& entry : std::filesystem::directory_iterator("/proc")) {
        pid_t pid = 0;
        if (parse(entry.path().filename().string(), pid) && pid != ::getpid()) {
            pids.push_back(pid);
        }
    }
    return pids;
}

} // namespace

int main(int argc, char** argv) {
    auto out = std::string();
    auto pids = std::vector<pid_t>();
    auto opts = quetschn::collect_options{};
    for (int i = 1; i < argc; ++i) {
        auto const arg = std::string_view(argv[i]);
        auto const has_value = i + 1 < argc;
        if (arg == "--out" && has_value) {
            out = argv[++i];
        } else if (arg == "--pid" && has_value) {
            pid_t pid = 0;
            if (!parse(argv[++i], pid)) {
                usage();
                return 2;
            }
            pids.push_back(pid);
        } else if (arg == "--max-per-process" && has_value) {
            if (!parse(argv[++i], opts.max_pages_per_process)) {
                usage();
                return 2;
            }
        } else if (arg == "--seed" && has_value) {
            if (!parse(argv[++i], opts.seed)) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (out.empty()) {
        usage();
        return 2;
    }
    auto const explicit_pids = !pids.empty();
    if (!explicit_pids) {
        pids = all_pids();
    }

    try {
        auto const page_size = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        auto writer = quetschn::corpus_writer(out, page_size);
        auto total = quetschn::collect_result{};
        std::size_t processes = 0;
        std::size_t skipped = 0;
        for (auto pid : pids) {
            try {
                auto r = quetschn::collect_process(pid, opts, writer);
                total.resident += r.resident;
                total.swapped += r.swapped;
                total.written += r.written;
                total.read_failed += r.read_failed;
                total.used_pagemap_scan = total.used_pagemap_scan || r.used_pagemap_scan;
                ++processes;
            } catch (std::system_error const& e) {
                // Other users' processes, kernel threads, processes that exited: expected when
                // sampling everything, an error when the pid was asked for.
                if (explicit_pids) {
                    std::fprintf(stderr, "pid %d: %s\n", static_cast<int>(pid), e.what());
                    return 1;
                }
                ++skipped;
            }
        }
        std::fprintf(stderr,
                     "%zu processes read, %zu skipped. %zu pages written, %zu resident, %zu already swapped (not read), "
                     "%zu gone before read. Resident pages found with %s.\n",
                     processes,
                     skipped,
                     total.written,
                     total.resident,
                     total.swapped,
                     total.read_failed,
                     total.used_pagemap_scan ? "PAGEMAP_SCAN" : "direct pagemap reads");
    } catch (std::exception const& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
