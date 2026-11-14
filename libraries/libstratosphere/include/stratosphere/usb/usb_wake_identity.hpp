/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>

namespace ams::usb::sleep {
    // These optional USB string witnesses identify the bridge/device. The
    // volume-specific witness is still necessary for a removable card.
    constexpr bool IsStringDescriptor(const std::uint8_t *p, std::size_t bytes) {
        return p && bytes >= 4 && bytes <= 254 && (bytes % 2) == 0 &&
               p[0] == bytes && p[1] == 3;
    }

    constexpr std::uint16_t SerialLanguage(const std::uint8_t *p, std::size_t bytes) {
        if (!IsStringDescriptor(p, bytes)) return 0;
        return static_cast<std::uint16_t>(p[2] | (static_cast<unsigned>(p[3]) << 8));
    }

    constexpr bool HasUsefulSerial(const std::uint8_t *p, std::size_t bytes) {
        if (!IsStringDescriptor(p, bytes) || bytes < 10) return false;
        bool varied = false;
        for (std::size_t i = 2; i < bytes; i += 2) {
            const auto ch = static_cast<unsigned>(p[i]) | (static_cast<unsigned>(p[i + 1]) << 8);
            if (ch < 0x20 || ch == 0xffff) return false;
            varied |= p[i] != p[2] || p[i + 1] != p[3];
        }
        return varied;
    }

    // Called after validating the UAS status IU type and current command tag.
    // CHECK CONDITION is the only SCSI status that supplies wake retry evidence.
    constexpr std::size_t WakeSenseSize(std::uint8_t status, std::size_t reported,
                                        std::size_t transferred, std::size_t capacity) {
        return status == 2 && reported >= 8 && reported <= capacity &&
               transferred >= 16 && reported <= transferred - 16 ? reported : 0;
    }
}
