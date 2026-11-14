/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace ams::usb::endpoint_profile {
    // C11 candidate from the captured 05E3:0764 descriptors. This is not a
    // generic VID quirk or a claim that the card/physical port is present.
    template<class Endpoint, class Companion>
    bool EndpointMatches(const Endpoint &e, const Companion &c, unsigned address) {
        return e.bDescriptorType == 5 && e.bEndpointAddress == address &&
            e.bmAttributes == 2 && e.wMaxPacketSize == 1024 &&
            c.bLength == 6 && c.bDescriptorType == 0x30 && c.bMaxBurst == 4 &&
            c.bmAttributes == 0 && c.wBytesPerInterval == 0;
    }

    template<class Device, class Interface>
    bool Matches(const Device &d, const Interface &i) {
        const auto &v = i.interface_desc;
        if (d.idVendor != 0x05E3 || d.idProduct != 0x0764 || d.bcdDevice != 0x2961 || d.bcdUSB != 0x0320 ||
            v.bInterfaceClass != 8 || v.bInterfaceSubClass != 6 || v.bInterfaceProtocol != 0x50 ||
            v.bInterfaceNumber != 0 || v.bAlternateSetting != 0 || v.bNumEndpoints != 2) return false;
        unsigned in = 0, out = 0;
        for (size_t n = 0; n < sizeof(i.input_endpoint_descs) / sizeof(i.input_endpoint_descs[0]); ++n) {
            const auto &e = i.input_endpoint_descs[n];
            if (e.bDescriptorType == 0) continue;
            if (!EndpointMatches(e, i.input_ss_endpoint_companion_descs[n], 0x81)) return false;
            ++in;
        }
        for (size_t n = 0; n < sizeof(i.output_endpoint_descs) / sizeof(i.output_endpoint_descs[0]); ++n) {
            const auto &e = i.output_endpoint_descs[n];
            if (e.bDescriptorType == 0) continue;
            if (!EndpointMatches(e, i.output_ss_endpoint_companion_descs[n], 0x02)) return false;
            ++out;
        }
        return in == 1 && out == 1;
    }

    struct Settings { uint16_t urbs; uint32_t in_size, out_size; };
    inline std::atomic<uint32_t> vid{}, pid{}, selected{}, attempts{}, urbs{}, in_size{}, out_size{};

    inline void Begin(uint16_t vendor, uint16_t product) {
        vid.store(vendor); pid.store(product); selected.store(0);
        urbs.store(0); in_size.store(0); out_size.store(0);
    }

    inline Settings Select(bool matched, uint16_t default_urbs, uint32_t default_size) {
        // Endpoint allocation only, as used by libusbhsfs. Do not shrink
        // submitted data transfers, replay failed commands, or change resets.
        const Settings s = matched ? Settings{1, 1024, 1024} : Settings{default_urbs, default_size, default_size};
        if (matched) attempts.fetch_add(1);
        selected.store(matched ? 1 : 0); urbs.store(s.urbs);
        in_size.store(s.in_size); out_size.store(s.out_size);
        return s;
    }

    inline int Format(char *out, size_t size) {
        return std::snprintf(out, size,
            "usb_endpoint_profile=C11\nusb_endpoint_profile_scope=last_BOT_open_attempt_not_presence\n"
            "usb_endpoint_profile_vid=0x%04X\nusb_endpoint_profile_pid=0x%04X\n"
            "usb_endpoint_profile_selected=%u\nusb_endpoint_profile_attempts=%u\n"
            "usb_endpoint_profile_urbs=%u\nusb_endpoint_profile_in_bytes=%u\nusb_endpoint_profile_out_bytes=%u\n",
            vid.load(), pid.load(), selected.load(), attempts.load(), urbs.load(), in_size.load(), out_size.load());
    }
}
