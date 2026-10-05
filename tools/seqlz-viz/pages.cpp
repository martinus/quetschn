// SPDX-License-Identifier: MIT OR GPL-2.0-only
// pages <text file> <dir>: two real memory pages of this program for the decoder visualization, with
// nothing private in them. heap.page is a page of its heap after it built a hash map of the words of
// the text file, each a struct with a string, two counters, a pointer and a double. relro.page is a
// page of libstdc++'s data that the dynamic linker wrote the addresses into.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Symbol {
    std::string name;
    std::uint32_t kind;
    std::uint32_t refs;
    Symbol* next;
    double weight;
};

void dump(std::uintptr_t addr, std::string const& file) {
    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (f == nullptr || std::fwrite(reinterpret_cast<void const*>(addr), 1, 4096, f) != 4096) {
        std::perror(file.c_str());
        std::exit(1);
    }
    std::fclose(f);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: pages <text file> <dir>\n");
        return 1;
    }
    std::ifstream in(argv[1]);
    std::stringstream ss;
    ss << in.rdbuf();
    std::vector<std::string> words;
    std::string w;
    for (char c : ss.str()) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            w += c;
        } else if (!w.empty()) {
            words.push_back(w);
            w.clear();
        }
    }

    std::unordered_map<std::string, std::unique_ptr<Symbol>> table;
    Symbol* prev = nullptr;
    std::uint32_t k = 0;
    for (auto const& x : words) {
        auto& s = table[x];
        if (!s) {
            s = std::make_unique<Symbol>(Symbol{x, k % 7, 0, prev, 1.0 / (1 + k % 13)});
            prev = s.get();
            ++k;
        }
        s->refs++;
    }
    // the heap page around the middle symbol
    std::vector<Symbol*> all;
    for (auto& [name, s] : table) {
        all.push_back(s.get());
    }
    std::string const dir = argv[2];
    dump(reinterpret_cast<std::uintptr_t>(all[all.size() / 2]) & ~std::uintptr_t{4095}, dir + "/heap.page");

    // libstdc++'s mappings; the second to last is its relocated read-only data
    std::ifstream maps("/proc/self/maps");
    std::string line;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> lib;
    while (std::getline(maps, line)) {
        if (line.find("libstdc++") != std::string::npos) {
            unsigned long a = 0;
            unsigned long b = 0;
            std::sscanf(line.c_str(), "%lx-%lx", &a, &b);
            lib.emplace_back(a, b);
        }
    }
    if (lib.size() < 2) {
        std::fprintf(stderr, "libstdc++ is not mapped as expected\n");
        return 1;
    }
    auto [a, b] = lib[lib.size() - 2];
    dump(a + 4096 * 3 < b ? a + 4096 * 3 : a, dir + "/relro.page");
    std::printf("%zu words, %zu symbols\n", words.size(), table.size());
}
