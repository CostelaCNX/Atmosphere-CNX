/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>

namespace ams::mitm::usb_storage::content_stats {
    inline std::atomic<uint64_t> bridge_opens{}, create_attempts{}, creates{}, writes{}, bytes{}, renames{}, changes{};
    inline std::atomic<uint32_t> last_result{};
    inline void ResultRecorded(uint32_t result) {
        last_result.store(result, std::memory_order_relaxed);
        changes.fetch_add(1, std::memory_order_release);
    }
}
