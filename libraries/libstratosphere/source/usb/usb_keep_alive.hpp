/* Copyright (c) Atmosphere-NX. GPL-2.0-or-later. */
#pragma once

namespace ams::usb::impl {
    constexpr u64 KeepAliveIdleNs = 5'000'000'000ULL;
    // A cached-write/flash maintenance pause can outlast the old three-second
    // background budget. Allow the normal endpoint timeout, shared across the
    // entire command (not fifteen seconds per phase), before quarantining it.
    constexpr u64 KeepAliveBudgetNs = 15'000'000'000ULL;
    constexpr u64 KeepAliveSlowNs = 3'000'000'000ULL;
    // REQUEST SENSE is a different operation; do not extend its recovery wait.
    constexpr u64 SenseBudgetNs = 3'000'000'000ULL;

    inline u64 UsbNowNs() {
        return static_cast<u64>(os::GetSystemTick().ToTimeSpan().GetNanoSeconds());
    }

    constexpr bool IsKeepAliveDue(u64 now, u64 last_command) {
        return last_command != 0 && now >= last_command && now - last_command >= KeepAliveIdleNs;
    }

    constexpr u64 TransferWaitNs(u64 now, u64 deadline, u64 normal_timeout) {
        return deadline == 0 ? normal_timeout :
            (now >= deadline ? 0 : (deadline - now < normal_timeout ? deadline - now : normal_timeout));
    }
}
