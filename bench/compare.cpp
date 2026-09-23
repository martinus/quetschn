// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "compare.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace quetschn {

namespace {

constexpr auto header = std::string_view("page\tcomp_len\thuge\tcost\tcompress_ns\tdecompress_ns\tdecompress_cold_ns");

constexpr auto latency_percentiles = std::array<double, 3>{50.0, 99.0, 99.9};

constexpr auto latency_metrics = std::array<double page_result::*, 3>{
    &page_result::compress_ns, &page_result::decompress_ns, &page_result::decompress_cold_ns};

// splitmix64: gives the same sequence on every platform, std::uniform_int_distribution does not.
class splitmix {
    std::uint64_t m_state;

public:
    explicit splitmix(std::uint64_t seed)
        : m_state(seed) {}

    std::uint64_t next() {
        auto z = (m_state += 0x9e3779b97f4a7c15U);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9U;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebU;
        return z ^ (z >> 31U);
    }

    // in [0, n). The modulo bias is below 1e-13 for a million pages.
    std::size_t below(std::size_t n) {
        return static_cast<std::size_t>(next() % n);
    }
};

// The nearest-rank latency_percentiles of values. Reorders values.
std::array<double, latency_percentiles.size()> select_percentiles(std::vector<double>& values) {
    auto out = std::array<double, latency_percentiles.size()>{};
    auto first = values.begin();
    for (std::size_t i = 0; i < latency_percentiles.size(); ++i) {
        auto const nth = values.begin() + static_cast<std::ptrdiff_t>(nearest_rank(latency_percentiles[i], values.size()) - 1);
        if (nth >= first) {
            std::nth_element(first, nth, values.end());
            first = nth + 1;
        }
        out[i] = *nth;
    }
    return out;
}

using latency_differences = std::array<std::array<double, latency_percentiles.size()>, latency_metrics.size()>;

// One bootstrap resample: saving and latency differences for the pages that index(i) picks. Both runs
// get the same pages, that is what makes the comparison paired: a page that is expensive for both
// codecs moves both sums together, and only the difference is left as noise.
class resampler {
    std::vector<page_result> const& m_baseline;
    std::vector<page_result> const& m_candidate;
    std::array<std::vector<double>, latency_metrics.size()> m_b;
    std::array<std::vector<double>, latency_metrics.size()> m_c;

public:
    resampler(std::vector<page_result> const& baseline, std::vector<page_result> const& candidate)
        : m_baseline(baseline)
        , m_candidate(candidate) {
        for (std::size_t m = 0; m < latency_metrics.size(); ++m) {
            m_b[m].resize(baseline.size());
            m_c[m].resize(baseline.size());
        }
    }

    template <typename Index>
    std::pair<double, latency_differences> run(Index&& index) {
        auto b_cost = 0.0;
        auto c_cost = 0.0;
        for (std::size_t i = 0; i < m_baseline.size(); ++i) {
            auto const j = index(i);
            b_cost += m_baseline[j].cost;
            c_cost += m_candidate[j].cost;
            for (std::size_t m = 0; m < latency_metrics.size(); ++m) {
                m_b[m][i] = m_baseline[j].*latency_metrics[m];
                m_c[m][i] = m_candidate[j].*latency_metrics[m];
            }
        }
        auto d = latency_differences{};
        for (std::size_t m = 0; m < latency_metrics.size(); ++m) {
            auto const b = select_percentiles(m_b[m]);
            auto const c = select_percentiles(m_c[m]);
            for (std::size_t k = 0; k < latency_percentiles.size(); ++k) {
                d[m][k] = c[k] - b[k];
            }
        }
        return {1.0 - c_cost / b_cost, d};
    }
};

estimate interval(double value, std::vector<double> const& resampled, double confidence) {
    return {value,
            percentile(resampled, 100.0 * (1.0 - confidence) / 2.0),
            percentile(resampled, 100.0 * (1.0 + confidence) / 2.0)};
}

template <typename T>
T parse_field(std::string_view& line, std::size_t line_no) {
    auto const end = line.find('\t');
    auto const field = line.substr(0, end);
    line = end == std::string_view::npos ? std::string_view() : line.substr(end + 1);
    auto value = T{};
    auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (field.empty() || ec != std::errc{} || ptr != field.data() + field.size()) {
        throw std::runtime_error("read_page_results: line " + std::to_string(line_no) + ": cannot parse '" +
                                 std::string(field) + "'");
    }
    return value;
}

} // namespace

void write_page_results(std::ostream& out, std::vector<page_result> const& pages) {
    out << header << '\n';
    // to_chars writes the shortest text that reads back as the same double, so sums over the file are exact
    auto buf = std::array<char, 32>{};
    auto num = [&](double v) {
        auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
        return std::string_view(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
    };
    for (auto const& p : pages) {
        out << p.page << '\t' << p.comp_len << '\t' << (p.huge ? 1 : 0) << '\t' << num(p.cost) << '\t';
        out << num(p.compress_ns) << '\t';
        out << num(p.decompress_ns) << '\t';
        out << num(p.decompress_cold_ns) << '\n';
    }
}

std::vector<page_result> read_page_results(std::istream& in) {
    auto line = std::string();
    if (!std::getline(in, line) || line != header) {
        throw std::runtime_error("read_page_results: the first line is not the header of quetschn-bench-* --out");
    }
    auto pages = std::vector<page_result>();
    for (std::size_t line_no = 2; std::getline(in, line); ++line_no) {
        auto rest = std::string_view(line);
        auto p = page_result{};
        p.page = parse_field<std::size_t>(rest, line_no);
        p.comp_len = parse_field<unsigned int>(rest, line_no);
        auto const huge = parse_field<int>(rest, line_no);
        p.cost = parse_field<double>(rest, line_no);
        p.compress_ns = parse_field<double>(rest, line_no);
        p.decompress_ns = parse_field<double>(rest, line_no);
        p.decompress_cold_ns = parse_field<double>(rest, line_no);
        if ((huge != 0 && huge != 1) || !rest.empty()) {
            throw std::runtime_error("read_page_results: line " + std::to_string(line_no) + " is not a page result");
        }
        p.huge = huge == 1;
        pages.push_back(p);
    }
    return pages;
}

comparison
compare(std::vector<page_result> const& baseline, std::vector<page_result> const& candidate, compare_options const& opts) {
    if (baseline.empty()) {
        throw std::invalid_argument("compare: no pages");
    }
    if (baseline.size() != candidate.size()) {
        throw std::invalid_argument("compare: " + std::to_string(baseline.size()) + " and " +
                                    std::to_string(candidate.size()) + " pages, the runs are not on the same corpus");
    }
    if (opts.resamples == 0 || !(opts.confidence > 0.0 && opts.confidence < 1.0)) {
        throw std::invalid_argument("compare: resamples must be > 0 and confidence in (0, 1)");
    }
    auto const n = baseline.size();

    auto r = comparison{};
    r.pages = n;
    for (std::size_t i = 0; i < n; ++i) {
        auto const& b = baseline[i];
        auto const& c = candidate[i];
        if (b.page != c.page) {
            throw std::invalid_argument("compare: line " + std::to_string(i + 2) + " has page " + std::to_string(b.page) +
                                        " and page " + std::to_string(c.page) + ", the runs are not on the same corpus");
        }
        r.baseline_cost += b.cost;
        r.candidate_cost += c.cost;
        if (c.cost < b.cost) {
            ++r.cheaper;
        } else if (c.cost > b.cost) {
            ++r.costlier;
            if (c.cost - b.cost > r.largest_regression) {
                r.largest_regression = c.cost - b.cost;
                r.largest_regression_page = b.page;
            }
        } else {
            ++r.same;
        }
    }

    // Each resample has its own generator, seeded from one sequence, so the result does not depend on
    // how the resamples are spread over threads.
    auto seeds = std::vector<std::uint64_t>(opts.resamples);
    auto seeder = splitmix(opts.seed);
    for (auto& s : seeds) {
        s = seeder.next();
    }

    auto const [saving_value, latency_value] = resampler(baseline, candidate).run([](std::size_t i) {
        return i;
    });
    auto saving = std::vector<double>(opts.resamples);
    auto latency = std::array<std::array<std::vector<double>, latency_percentiles.size()>, latency_metrics.size()>{};
    for (auto& per_metric : latency) {
        for (auto& v : per_metric) {
            v.resize(opts.resamples);
        }
    }
    auto work = [&](unsigned first, unsigned step) {
        auto rs = resampler(baseline, candidate);
        for (auto s = first; s < opts.resamples; s += step) {
            auto rng = splitmix(seeds[s]);
            auto const [sv, d] = rs.run([&](std::size_t) {
                return rng.below(n);
            });
            saving[s] = sv;
            for (std::size_t m = 0; m < latency_metrics.size(); ++m) {
                for (std::size_t k = 0; k < latency_percentiles.size(); ++k) {
                    latency[m][k][s] = d[m][k];
                }
            }
        }
    };
    auto const threads =
        std::min(opts.threads == 0 ? std::max(1U, std::thread::hardware_concurrency()) : opts.threads, opts.resamples);
    {
        auto pool = std::vector<std::jthread>();
        for (unsigned t = 1; t < threads; ++t) {
            pool.emplace_back(work, t, threads);
        }
        work(0, threads);
    }

    r.saving = interval(saving_value, saving, opts.confidence);
    for (std::size_t k = 0; k < latency_percentiles.size(); ++k) {
        auto l = latency_difference{};
        l.p = latency_percentiles[k];
        l.compress = interval(latency_value[0][k], latency[0][k], opts.confidence);
        l.decompress = interval(latency_value[1][k], latency[1][k], opts.confidence);
        l.decompress_cold = interval(latency_value[2][k], latency[2][k], opts.confidence);
        r.latency.push_back(l);
    }
    return r;
}

} // namespace quetschn
