// SPDX-License-Identifier: MIT OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace quetschn {

// What zsmalloc is built with. Defaults are a 64-bit kernel with 4 KiB pages and the default
// CONFIG_ZSMALLOC_CHAIN_SIZE. Everything else in the model is derived from these four numbers, the
// same way zs_create_pool() derives it in mm/zsmalloc.c.
struct zsmalloc_config {
    std::size_t page_size = 4096;
    std::size_t chain_size = 8;      // CONFIG_ZSMALLOC_CHAIN_SIZE, the kernel allows 4 to 16
    std::size_t handle_size = 8;     // ZS_HANDLE_SIZE, sizeof(unsigned long)
    std::size_t min_alloc_size = 32; // ZS_MIN_ALLOC_SIZE, 32 on all 64-bit configs
};

struct size_class {
    std::size_t index;            // index of the class in /sys/kernel/debug/zsmalloc/<pool>/classes
    std::size_t size;             // slot size
    std::size_t pages_per_zspage; // physical pages per zspage
    std::size_t objs_per_zspage;  // slots per zspage

    // Bytes of physical memory one object really uses: pages_per_zspage * page_size / objs_per_zspage.
    // This is more than `size` because a zspage usually does not divide evenly into slots, and the
    // tail is lost.
    double cost;
};

// Memory that zram uses for one page, depending on how many bytes the codec compressed it to.
//
// zram does not pay for compressed bytes: it pays for a zsmalloc slot, and the slot size depends on
// the merged class table, which is not a plain 16 byte grid. Pages at or above huge_class_size() are
// stored uncompressed. This is a port of the class setup in zs_create_pool() and of the size check in
// zram_write_page(), so it is only as correct as the kernel version it was ported from (986c24e0fe44).
//
// Not modelled: fragmentation from partly filled zspages. That depends on allocation history and only
// a real zram run shows it.
class zsmalloc_model {
public:
    explicit zsmalloc_model(zsmalloc_config const& cfg = {});

    // zram stores a page uncompressed when the codec output is at least this long.
    [[nodiscard]] std::size_t huge_class_size() const;

    // The class that zram ends up using for a page that was compressed to comp_len bytes.
    // comp_len must be > 0, zsmalloc rejects empty objects.
    [[nodiscard]] size_class const& class_for(std::size_t comp_len) const;

    // Bytes of physical memory for a page that was compressed to comp_len bytes, including the zspage
    // tail waste. This is the ratio metric of the benchmark.
    [[nodiscard]] double cost(std::size_t comp_len) const;

    // All classes after merging, smallest first. Same rows as the debugfs `classes` file.
    [[nodiscard]] std::span<size_class const> classes() const;

    [[nodiscard]] zsmalloc_config const& config() const;

private:
    zsmalloc_config m_cfg;
    std::size_t m_huge_class_size = 0;
    std::vector<size_class> m_classes;
    std::vector<std::size_t> m_class_of_index; // nominal class index => position in m_classes
};

} // namespace quetschn
