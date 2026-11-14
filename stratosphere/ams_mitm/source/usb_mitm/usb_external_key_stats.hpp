/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>

namespace ams::mitm::usb_storage::key_stats {
    /* Counts/results only: never persist keys, tickets, or rights-ID bytes. */
    inline std::atomic<uint64_t> es_sessions{}, registrations{}, accepted{}, mirrored{}, removals{}, hits{}, misses{}, last_client{};
    inline std::atomic<uint32_t> native_result{}, mirror_result{};
}
