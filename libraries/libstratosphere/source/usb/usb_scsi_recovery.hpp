/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>

namespace ams::usb::impl {
    // Applies only after a complete CSW and current NOT READY / 04 / 01 sense.
    // No replay of writes, media-change/reset attention, or unknown outcomes.
    inline bool PrepareReadyRetry(u64 &deadline, u32 &retries, u64 outer_deadline = 0) {
        constexpr u64 budget = 5'000'000'000ULL;
        constexpr u64 delay = 250'000'000ULL;
        const u64 now = UsbNowNs();
        if (deadline == 0) {
            deadline = now + budget;
            if (outer_deadline != 0 && outer_deadline < deadline) deadline = outer_deadline;
        }
        if (retries >= 20 || now >= deadline || deadline - now <= delay) return false;
        ++retries;
        os::SleepThread(TimeSpan::FromMilliSeconds(250));
        return UsbNowNs() < deadline;
    }
    enum class SenseAction { Fatal, Refused, BecomingReady, ResetDuringProbe };
    // Only current, complete sense is actionable. Deferred errors may refer
    // to a previous write and must never be treated as a retryable read.
    constexpr SenseAction ClassifySense(const std::uint8_t *p, std::size_t n) {
        if (!p || n < 8) return SenseAction::Fatal;
        unsigned key, asc, ascq;
        if ((p[0] & 0x7f) == 0x70) {
            if (n < 14 || p[7] < 6 || n < 8u + p[7]) return SenseAction::Fatal;
            key = p[2] & 0xf; asc = p[12]; ascq = p[13];
        } else if ((p[0] & 0x7f) == 0x72) {
            if (n < 8u + p[7]) return SenseAction::Fatal;
            key = p[1] & 0xf; asc = p[2]; ascq = p[3];
        } else return SenseAction::Fatal;
        if (key == 7 || key == 5) return SenseAction::Refused; // protection / illegal request
        if (key == 2 && asc == 4 && ascq == 1) return SenseAction::BecomingReady;
        if (key == 6 && asc == 0x29) return SenseAction::ResetDuringProbe;
        return SenseAction::Fatal; // includes medium change, loss and write errors
    }
    constexpr bool CanReplayRead(std::uint8_t opcode) {
        return opcode == 0x00 || opcode == 0x12 || opcode == 0x25 ||
               opcode == 0x28 || opcode == 0x88 || opcode == 0x9e;
    }
}
