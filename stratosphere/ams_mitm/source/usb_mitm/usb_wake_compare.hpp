/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ams::mitm::usb_storage::wake_compare {
    constexpr std::size_t MaxRead = 64 * 1024;
    constexpr std::uint32_t NotAttempted = UINT32_MAX;
    enum class Stage : std::uint32_t { Idle, Queued, Running, TooLarge, Stale, OpenFailed, ReadFailed, Verified, NoMemory };
    inline std::atomic<std::uint32_t> ready_epoch{};

    constexpr bool Eligible(std::uint32_t open_epoch, std::uint32_t read_epoch, std::uint32_t current_epoch,
                            std::int64_t offset, std::size_t size) {
        return read_epoch > open_epoch && current_epoch == read_epoch && offset >= 0 && size > 0 &&
            size <= static_cast<std::uint64_t>(INT64_MAX - offset);
    }

    struct Results {
        std::uint32_t open = NotAttempted, read = NotAttempted;
    };
    template<class Verify, class Open, class Read>
    Stage Compare(Results &results, Verify valid, Open open, Read read) {
        if (!valid()) return Stage::Stale;
        results.open = open();
        if (!valid()) return Stage::Stale;
        if (results.open) return Stage::OpenFailed;
        results.read = read();
        if (!valid()) return Stage::Stale;
        return results.read ? Stage::ReadFailed : Stage::Verified;
    }
    int Format(char *out, std::size_t capacity);
}
