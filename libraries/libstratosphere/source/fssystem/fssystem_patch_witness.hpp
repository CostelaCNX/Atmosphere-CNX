/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ams::fssystem::patch_witness {
    using Digest = std::array<std::uint8_t, 32>;
    struct Sample {
        std::int64_t offset{-1};
        std::size_t size{};
        Digest expected{}, cipher{}, plain{};
        bool cipher_seen{}, plain_seen{};
        bool Verified() const { return plain_seen && expected == plain; }
    };
    using Lookup = Sample *(*)(const void *);
    inline std::atomic<Lookup> lookup{};
    inline Sample *Find(const void *reader) {
        const auto get = lookup.load(std::memory_order_acquire);
        return get ? get(reader) : nullptr;
    }

    // Assemble just the requested bytes. The alignment wrapper may read the
    // head, body and tail separately, including padding or in a different order.
    template<std::size_t N>
    struct Input {
        std::int64_t offset;
        std::uint8_t bytes[N]{}, seen[N]{};
        std::size_t count{};
        void Observe(std::int64_t at, const void *buffer, std::size_t size) {
            if (at < 0 || size > static_cast<std::uint64_t>(INT64_MAX - at)) return;
            const auto start = std::max(at, offset);
            const auto end = std::min(at + static_cast<std::int64_t>(size), offset + static_cast<std::int64_t>(N));
            if (start >= end) return;
            const auto index = static_cast<std::size_t>(start - offset);
            const auto length = static_cast<std::size_t>(end - start);
            std::memcpy(bytes + index, static_cast<const std::uint8_t *>(buffer) + (start - at), length);
            for (auto i = index; i < index + length; ++i) {
                if (!seen[i]) { seen[i] = 1; ++count; }
            }
        }
    };
}
