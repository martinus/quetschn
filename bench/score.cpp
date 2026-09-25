// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "score.h"

#include <algorithm>
#include <istream>
#include <sstream>
#include <stdexcept>

namespace quetschn {

namespace {

// the number behind key in line, e.g. "mean" in "... p99 9500 mean 6012 ns"; -1 if it is not there
double value_after(std::string const& line, std::string const& key) {
    auto words = std::istringstream(line);
    auto w = std::string();
    while (words >> w) {
        if (w == key) {
            auto v = 0.0;
            if (words >> v) {
                return v;
            }
            return -1.0;
        }
    }
    return -1.0;
}

// the fields of a mm_stat line behind "mm_stat" or "mm_stat after recompress"
std::vector<double> mm_stat(std::string const& rest) {
    auto in = std::istringstream(rest);
    auto v = std::vector<double>();
    auto x = 0.0;
    while (in >> x) {
        v.push_back(x);
    }
    return v;
}

} // namespace

std::vector<codec_cost> read_vm_results(std::istream& in, std::string const& prefix) {
    struct seen {
        codec_cost c;
        bool mm = false, write = false, read = false;
    };
    auto devices = std::vector<seen>();
    auto device = [&](std::string const& name) -> seen& {
        for (auto& d : devices) {
            if (d.c.name == prefix + name) {
                return d;
            }
        }
        devices.push_back({});
        devices.back().c.name = prefix + name;
        return devices.back();
    };
    auto line = std::string();
    while (std::getline(in, line)) {
        // the VM's console puts escape sequences in front of the first line
        auto const at = line.find("RESULT ");
        if (at == std::string::npos) {
            continue;
        }
        auto words = std::istringstream(line.substr(at + 7));
        auto name = std::string();
        words >> name;
        auto rest = std::string();
        std::getline(words, rest);
        auto const what = rest.substr(std::min(rest.find_first_not_of(' '), rest.size()));
        auto& d = device(name);
        if (what.rfind("mm_stat after recompress ", 0) == 0 || what.rfind("mm_stat ", 0) == 0) {
            auto const after = what.rfind("mm_stat after recompress ", 0) == 0;
            auto const v = mm_stat(what.substr(after ? 25 : 8));
            if (v.size() < 3 || v[0] <= 0.0) {
                throw std::runtime_error("mm_stat without orig_data_size and mem_used_total: " + line);
            }
            // the one after recompression wins, it comes later
            d.c.pages = v[0] / 4096.0;
            d.c.bytes_per_page = v[2] / d.c.pages;
            d.mm = true;
        } else if (what.rfind("write, other page before", 0) == 0) {
            d.c.write_ns = value_after(what, "mean");
            d.write = d.c.write_ns >= 0.0;
        } else if (what.rfind("flushed, other page first", 0) == 0 && value_after(what, "prefetch") == 8.0) {
            d.c.read_ns = value_after(what, "mean");
            d.read = d.c.read_ns >= 0.0;
        } else if (what.rfind("recompress:", 0) == 0) {
            d.c.recompress_ns = value_after(what, "recompress:");
        }
    }
    auto out = std::vector<codec_cost>();
    for (auto const& d : devices) {
        if (!d.mm || !d.write || !d.read) {
            throw std::runtime_error(d.c.name + ": no mm_stat, or no mean of the writes or the cold reads");
        }
        out.push_back(d.c);
    }
    return out;
}

std::vector<codec_cost> read_bench_results(std::istream& in, std::string const& prefix) {
    auto out = std::vector<codec_cost>();
    auto complete = std::vector<int>();
    auto line = std::string();
    auto const last_number = [](std::string const& rest) {
        auto words = std::istringstream(rest);
        auto v = -1.0, x = 0.0;
        while (words >> x) {
            v = x;
        }
        return v;
    };
    while (std::getline(in, line)) {
        if (line.rfind("codec ", 0) == 0) {
            // "codec      zstd, level -1" or "codec      seqlz, no level (...)"
            auto words = std::istringstream(line.substr(6));
            auto name = std::string();
            words >> name;
            if (!name.empty() && name.back() == ',') {
                name.pop_back();
            }
            auto level = std::string();
            if (words >> level && level == "level") {
                words >> level;
                name += ":" + level;
            }
            out.push_back({});
            out.back().name = prefix + name;
            complete.push_back(0);
            continue;
        }
        if (out.empty()) {
            continue;
        }
        auto& c = out.back();
        if (line.rfind("pages ", 0) == 0) {
            auto words = std::istringstream(line.substr(5));
            words >> c.pages;
        } else if (line.rfind("zsmalloc cost ", 0) == 0) {
            // "zsmalloc cost          1873182 bytes, 23.1% of uncompressed, 946.1 bytes/page"
            auto const at = line.rfind(" bytes/page");
            auto const from = line.rfind(' ', at - 1);
            c.bytes_per_page = std::stod(line.substr(from + 1, at - from - 1));
            complete.back() |= 1;
        } else if (line.rfind("compress ", 0) == 0) {
            c.write_ns = last_number(line.substr(9));
            complete.back() |= c.write_ns >= 0.0 ? 2 : 0;
        } else if (line.rfind("decompress cold ", 0) == 0) {
            c.read_ns = last_number(line.substr(16));
            complete.back() |= c.read_ns >= 0.0 ? 4 : 0;
        }
    }
    for (std::size_t k = 0; k < out.size(); ++k) {
        if (complete[k] != 7) {
            throw std::runtime_error(out[k].name + ": no zsmalloc cost, or no mean of compress or decompress cold");
        }
    }
    return out;
}

double us_per_page(codec_cost const& c, score_weights const& w) {
    return (c.write_ns + w.reads_per_write * c.read_ns + w.recompress_weight * c.recompress_ns) / 1000.0;
}

std::vector<std::size_t> best_for_some_lambda(std::vector<codec_cost> const& codecs, score_weights const& w) {
    auto idx = std::vector<std::size_t>(codecs.size());
    for (std::size_t k = 0; k < idx.size(); ++k) {
        idx[k] = k;
    }
    auto const x = [&](std::size_t k) {
        return us_per_page(codecs[k], w);
    };
    auto const y = [&](std::size_t k) {
        return codecs[k].bytes_per_page;
    };
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        return x(a) != x(b) ? x(a) < x(b) : y(a) < y(b);
    });
    auto hull = std::vector<std::size_t>();
    for (auto k : idx) {
        // only points below the last one: the others lose at every lambda >= 0
        if (!hull.empty() && y(k) >= y(hull.back())) {
            continue;
        }
        // drop the last one while it is not below the line from the one before to k
        while (hull.size() >= 2) {
            auto const a = hull[hull.size() - 2], b = hull.back();
            auto const cross = (x(b) - x(a)) * (y(k) - y(a)) - (y(b) - y(a)) * (x(k) - x(a));
            if (cross > 0.0) {
                break;
            }
            hull.pop_back();
        }
        hull.push_back(k);
    }
    return hull;
}

} // namespace quetschn
