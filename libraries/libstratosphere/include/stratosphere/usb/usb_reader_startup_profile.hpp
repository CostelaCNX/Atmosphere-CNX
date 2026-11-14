/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ams::usb::startup_profile {
    // C8 experimental pre-mount profile from the supplied device map. These
    // values identify a descriptor revision, NOT a physical dock port/card.
    // Never force an unreported LUN, endpoint, protocol or alternate setting.
    template<class Device, class Interface>
    bool Matches(const Device &d, const Interface &i) {
        const auto &v = i.interface_desc;
        if (d.idVendor != 0x2E4D || d.idProduct != 0x2372 || d.bcdDevice != 0x0064 || d.bcdUSB != 0x0320 ||
            v.bInterfaceClass != 8 || v.bInterfaceSubClass != 6 || v.bInterfaceProtocol != 0x50 ||
            v.bInterfaceNumber != 0 || v.bAlternateSetting != 0 || v.bNumEndpoints != 2) return false;
        unsigned in = 0, out = 0;
        for (const auto &e : i.input_endpoint_descs) {
            if (e.bDescriptorType != 5) continue;
            if (e.bEndpointAddress != 0x81 || e.bmAttributes != 2 || e.wMaxPacketSize != 1024) return false;
            ++in;
        }
        for (const auto &e : i.output_endpoint_descs) {
            if (e.bDescriptorType != 5) continue;
            if (e.bEndpointAddress != 0x02 || e.bmAttributes != 2 || e.wMaxPacketSize != 1024) return false;
            ++out;
        }
        return in == 1 && out == 1;
    }

    constexpr unsigned SettleMs = 250;
    inline std::atomic<uint32_t> attempts{}, fallbacks{}, initialized{}, stage{};
    inline std::atomic<uint32_t> first_result{UINT32_MAX}, final_result{UINT32_MAX};

    // Only called before capacity/filesystem acquisition. Initialize owns and
    // destroys failed sessions. Refresh must revalidate the SAME attachment;
    // failed revalidation never authorizes trying an unrelated replacement.
    template<class Initialize, class Refresh, class Stop>
    auto Attempt(bool mapped, bool already_reset, Initialize initialize, Refresh refresh, Stop stop) {
        if (!mapped || already_reset) return initialize(already_reset);
        attempts.fetch_add(1);
        stage.store(1); first_result.store(UINT32_MAX); final_result.store(UINT32_MAX);
        auto result = refresh();
        if (R_FAILED(result)) { stage.store(7); final_result.store(result.GetValue()); return result; }
        stage.store(2);
        result = initialize(true);
        first_result.store(result.GetValue()); final_result.store(result.GetValue());
        if (R_SUCCEEDED(result)) { initialized.fetch_add(1); stage.store(3); return result; }
        // One ordinary same-LUN fallback, not an unlimited retry and not a
        // replay on a timed-out endpoint. No live read/write path uses this.
        if (stop()) { stage.store(8); return result; }
        fallbacks.fetch_add(1); stage.store(4);
        result = refresh();
        if (R_FAILED(result)) { stage.store(7); final_result.store(result.GetValue()); return result; }
        stage.store(5);
        result = initialize(false);
        final_result.store(result.GetValue());
        if (R_SUCCEEDED(result)) initialized.fetch_add(1);
        stage.store(R_SUCCEEDED(result) ? 6 : 9);
        return result;
    }

    inline int Format(char *out, size_t size) {
        return std::snprintf(out, size,
            "usb_reader_startup_profile=C8\n"
            "usb_reader_profile_match=2E4D:2372/0064/0320/BOT0/81:02/1024\n"
            "usb_reader_profile_scope=pre_mount_only_not_presence\n"
            "usb_reader_profile_attempts=%u\nusb_reader_profile_fallbacks=%u\n"
            "usb_reader_profile_initialized=%u\nusb_reader_profile_stage=%u\n"
            "usb_reader_profile_first_result=0x%08X\nusb_reader_profile_final_result=0x%08X\n",
            attempts.load(), fallbacks.load(), initialized.load(), stage.load(), first_result.load(), final_result.load());
    }
}
