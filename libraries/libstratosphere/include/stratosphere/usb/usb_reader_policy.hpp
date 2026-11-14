/* Copyright (c) Atmosphere-NX. GPL-2.0-or-later. */
#pragma once
#include <cstdint>

namespace ams::usb::reader {
    // No VID/PID guesses. RBC uses the same transparent block command path;
    // UFI/ATAPI, CBI, vendor protocols and non-storage interfaces do not.
    constexpr bool Supported(uint8_t device_class, uint8_t subclass, uint8_t protocol) {
        return device_class == 0x08 &&
            ((subclass == 0x06 && (protocol == 0x50 || protocol == 0x62)) ||
             (subclass == 0x01 && protocol == 0x50));
    }

    class InventoryCadence {
        uint64_t m_next{};
        public:
            bool Due(uint64_t now) {
                if (now < m_next) return false;
                m_next = now + 5'000'000'000ULL;
                return true;
            }
    };
}
