/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace ams::usb::impl {
    constexpr u64 StartupBudgetNs = 30'000'000'000ULL;
    constexpr u64 StartupCommandBudgetNs = 15'000'000'000ULL;
    constexpr size_t StartupMaxPolls = 120;
    constexpr u64 StartupRecoveryBudgetNs = 3'000'000'000ULL;

    // Worker-local cooldown, not a long blocking sleep. Only used after an
    // unsuccessful probe cycle with device-initialization failures.
    class StartupProbeBackoff {
        u32 m_delay_ms{0};
        u64 m_until{0};
        public:
            bool IsWaiting(u64 now) const { return now < m_until; }
            u32 Failed(u64 now) {
                m_delay_ms = m_delay_ms == 0 ? 2000 : (m_delay_ms < 15000 ? m_delay_ms * 2 : 30000);
                m_until = now + static_cast<u64>(m_delay_ms) * 1'000'000;
                return m_delay_ms;
            }
            void Reset() { m_delay_ms = 0; m_until = 0; }
            // Empty readers are not failing disks. Recheck for a newly
            // inserted card without accumulating the transport-fault backoff.
            void EmptyReader(u64 now) { m_delay_ms = 0; m_until = now + 1'000'000'000ULL; }
    };

    // Valid CURRENT sense only; no deferred, incomplete or vendor responses.
    // Packed value: 0x01kkAAqq, or zero if it is not safe to interpret.
    inline u32 StartupSense(const u8 *p, size_t n) {
        if (!p || n < 8 || n < 8u + p[7]) return 0;
        if ((p[0] & 0x7f) == 0x70) {
            if (n < 14 || p[7] < 6) return 0;
            return 0x01000000u | ((p[2] & 0xf) << 16) | (p[12] << 8) | p[13];
        }
        if ((p[0] & 0x7f) == 0x72)
            return 0x01000000u | ((p[1] & 0xf) << 16) | (p[2] << 8) | p[3];
        return 0;
    }

    inline bool StartupMediumAbsent(u32 sense) {
        // NOT READY / MEDIUM NOT PRESENT, including tray open/closed.
        return sense == 0x01023A00 || sense == 0x01023A01 || sense == 0x01023A02;
    }
}
