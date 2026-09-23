// SPDX-License-Identifier: MIT OR GPL-2.0-only
#include "zsmalloc_cost.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace quetschn {

namespace {

// Port of calculate_zspage_chain_size(): the chain length with the least tail waste, the shortest
// one on a tie.
std::size_t chain_size_for(std::size_t class_size, zsmalloc_config const& cfg) {
    if (std::has_single_bit(class_size)) {
        return 1;
    }
    auto min_waste = std::numeric_limits<std::size_t>::max();
    std::size_t chain_size = 1;
    for (std::size_t i = 1; i <= cfg.chain_size; ++i) {
        auto waste = (i * cfg.page_size) % class_size;
        if (waste < min_waste) {
            min_waste = waste;
            chain_size = i;
        }
    }
    return chain_size;
}

void validate(zsmalloc_config const& cfg) {
    // Kernel pages are 4 KiB to 64 KiB. Smaller sizes would make ZS_SIZE_CLASS_DELTA meaningless.
    if (!std::has_single_bit(cfg.page_size) || cfg.page_size < 4096 || cfg.page_size > 65536) {
        throw std::invalid_argument("zsmalloc_config: page_size must be a power of two in [4096, 65536]");
    }
    if (cfg.chain_size < 4 || cfg.chain_size > 16) {
        throw std::invalid_argument("zsmalloc_config: chain_size must be in [4, 16], like CONFIG_ZSMALLOC_CHAIN_SIZE");
    }
    if (cfg.handle_size == 0 || cfg.min_alloc_size == 0 || cfg.min_alloc_size >= cfg.page_size) {
        throw std::invalid_argument("zsmalloc_config: handle_size and min_alloc_size must be > 0, min_alloc_size < page_size");
    }
}

} // namespace

zsmalloc_model::zsmalloc_model(zsmalloc_config const& cfg)
    : m_cfg(cfg) {
    validate(m_cfg);

    // ZS_SIZE_CLASS_DELTA and ZS_SIZE_CLASSES
    auto const delta = m_cfg.page_size >> 8U;
    auto const num_classes = (m_cfg.page_size - m_cfg.min_alloc_size + delta - 1) / delta + 1;
    m_class_of_index.resize(num_classes);

    // Same walk as zs_create_pool(): from the largest class down, and a class that has the same
    // (pages_per_zspage, objs_per_zspage) as the previous one is merged into it. Merging always goes
    // to the larger class, so an object in a merged class pays for the larger slot.
    std::vector<size_class> descending;
    for (auto i = num_classes; i-- > 0;) {
        auto size = std::min(m_cfg.min_alloc_size + i * delta, m_cfg.page_size);
        auto pages_per_zspage = chain_size_for(size, m_cfg);
        auto objs_per_zspage = pages_per_zspage * m_cfg.page_size / size;

        // The largest class that shares zspages is the watermark. The kernel subtracts
        // ZS_HANDLE_SIZE - 1 because zs_malloc() adds the handle before the class lookup.
        if (pages_per_zspage != 1 && objs_per_zspage != 1 && m_huge_class_size == 0) {
            m_huge_class_size = size - (m_cfg.handle_size - 1);
        }

        if (!descending.empty() && descending.back().pages_per_zspage == pages_per_zspage &&
            descending.back().objs_per_zspage == objs_per_zspage) {
            m_class_of_index[i] = descending.size() - 1;
            continue;
        }
        auto cost = static_cast<double>(pages_per_zspage * m_cfg.page_size) / static_cast<double>(objs_per_zspage);
        descending.push_back({i, size, pages_per_zspage, objs_per_zspage, cost});
        m_class_of_index[i] = descending.size() - 1;
    }
    if (m_huge_class_size == 0) {
        throw std::logic_error("zsmalloc_model: no class shares a zspage, this cannot happen for valid configs");
    }

    // store ascending, and translate the positions accordingly
    m_classes.assign(descending.rbegin(), descending.rend());
    for (auto& pos : m_class_of_index) {
        pos = m_classes.size() - 1 - pos;
    }
}

std::size_t zsmalloc_model::huge_class_size() const {
    return m_huge_class_size;
}

size_class const& zsmalloc_model::class_for(std::size_t comp_len) const {
    if (comp_len == 0) {
        throw std::invalid_argument("zsmalloc_model::class_for: comp_len must be > 0");
    }
    // zram_write_page() hands everything at or above the watermark to write_incompressible_page(),
    // which stores the whole page. Codec output larger than a page ends up there too. For every valid
    // config this gives the same class as the plain lookup below, because all classes above the
    // watermark have 1 page and 1 object per zspage and so merge into the page sized class. It is
    // here to mirror zram, not because the result depends on it.
    auto const obj_size = comp_len >= m_huge_class_size ? m_cfg.page_size : comp_len;

    // lookup_size_class() and get_size_class_index()
    auto const size = obj_size + m_cfg.handle_size;
    auto const delta = m_cfg.page_size >> 8U;
    std::size_t idx = 0;
    if (size > m_cfg.min_alloc_size) {
        idx = (size - m_cfg.min_alloc_size + delta - 1) / delta;
    }
    idx = std::min(idx, m_class_of_index.size() - 1);
    return m_classes[m_class_of_index[idx]];
}

double zsmalloc_model::cost(std::size_t comp_len) const {
    return class_for(comp_len).cost;
}

std::span<size_class const> zsmalloc_model::classes() const {
    return m_classes;
}

zsmalloc_config const& zsmalloc_model::config() const {
    return m_cfg;
}

} // namespace quetschn
