/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <cstdint>
#include <cstddef>

namespace ams::mitm::usb_storage::memory_diagnostics {
    constexpr std::int64_t IntervalMs = 5000;
    constexpr std::size_t Capacity = 16;
    constexpr std::size_t PairCount = 6;
    // SVC failures remain explicit; zero is never interpreted as available memory.
    struct Value {
        std::uint64_t bytes{};
        std::uint32_t result{0xFFFFFFFF};
        bool Valid() const { return result == 0; }
    };
    struct Pair {
        Value total{}, used{};
        bool Valid() const { return total.Valid() && used.Valid() && used.bytes <= total.bytes; }
        std::uint64_t Free() const { return Valid() ? total.bytes - used.bytes : 0; }
    };
    struct Sample {
        std::int64_t at_ms{};
        std::uint64_t sequence{};
        Pair pairs[PairCount]{}; // self physical, self page-table resource, app/appet/system/unsafe pools
        Value free_threads{};
    };
    class History {
        Sample m_samples[Capacity]{};
        std::size_t m_next{}, m_count{};
        std::uint64_t m_sequence{}, m_self_peak{};
        std::uint64_t m_min_free[PairCount]{};
        std::uint32_t m_min_valid{};
        std::int64_t m_last_ms{};
      public:
        bool Due(std::int64_t now) const {
            return m_count == 0 || now < m_last_ms || now - m_last_ms >= IntervalMs;
        }
        // Single publisher thread owns this object. No heap walk, allocation,
        // service session, USB probe, resource-limit handle, or new mutex.
        template<typename Query> bool Poll(std::int64_t now, Query query) {
            if (!Due(now)) return false;
            Sample sample{}; sample.at_ms = now; sample.sequence = ++m_sequence;
            for (std::size_t i = 0; i < PairCount; ++i) {
                sample.pairs[i].total = query(i, false);
                sample.pairs[i].used = query(i, true);
                const auto &p = sample.pairs[i];
                if (p.Valid()) {
                    if (!(m_min_valid & (1u << i)) || p.Free() < m_min_free[i]) m_min_free[i] = p.Free();
                    m_min_valid |= 1u << i;
                }
            }
            sample.free_threads = query(PairCount, false);
            if (sample.pairs[0].used.Valid() && sample.pairs[0].used.bytes > m_self_peak)
                m_self_peak = sample.pairs[0].used.bytes;
            m_samples[m_next] = sample; m_next = (m_next + 1) % Capacity;
            if (m_count < Capacity) ++m_count;
            m_last_ms = now;
            return true;
        }
        std::size_t Count() const { return m_count; }
        const Sample &At(std::size_t i) const { return m_samples[(m_next + Capacity - m_count + i) % Capacity]; }
        const Sample &Latest() const { return At(m_count - 1); }
        std::uint64_t SelfPeak() const { return m_self_peak; }
        bool MinimumValid(std::size_t i) const { return (m_min_valid & (1u << i)) != 0; }
        std::uint64_t MinimumFree(std::size_t i) const { return m_min_free[i]; }
    };
    static_assert(sizeof(History) < 8192);
}
