// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace ams::mitm::usb_storage::impl {

    struct UsbEnablement {
        bool content;
        bool storage;
    };

    // The monitor is the sole refresh writer. Request threads only read the
    // published flags: they never open the settings file or wait for a refresh.
    class UsbSettingsCache {
        static constexpr uint32_t Content = 1;
        static constexpr uint32_t Storage = 2;
        std::atomic<uint32_t> flags{0};
        uint64_t last_refresh_ns{};
        bool initialized{};

        public:
            static constexpr uint64_t RefreshIntervalNs = 5'000'000'000ULL;

            template<typename Reader>
            bool Refresh(uint64_t now_ns, Reader &&read_settings) {
                if (initialized && now_ns >= last_refresh_ns &&
                    now_ns - last_refresh_ns < RefreshIntervalNs) {
                    return false;
                }

                const UsbEnablement settings = read_settings();
                const uint32_t next = (settings.content ? Content : 0) |
                                      (settings.storage ? Storage : 0);
                flags.store(next, std::memory_order_release);
                last_refresh_ns = now_ns;
                initialized = true;
                return true;
            }

            bool IsContentEnabled() const {
                return (flags.load(std::memory_order_acquire) & Content) != 0;
            }

            bool IsEnabled() const {
                return flags.load(std::memory_order_acquire) != 0;
            }
    };

}
