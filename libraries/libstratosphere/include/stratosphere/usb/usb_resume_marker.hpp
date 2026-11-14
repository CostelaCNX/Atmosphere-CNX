/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>
#include <cstring>
#include <stratosphere/usb/usb_reader_policy.hpp>

namespace ams::usb::sleep {
    inline constexpr char MarkerPath[] = "/NEXUS.RSM";
    inline constexpr std::uint8_t MarkerSignature[8] = {'N','X','S','L','E','E','P',2};
    struct VolumeMarker {
        std::uint8_t signature[8];
        std::uint8_t challenge[32];
    };
    static_assert(sizeof(VolumeMarker) == 40);
    inline bool IsVolumeMarker(const VolumeMarker &marker) {
        return std::memcmp(marker.signature, MarkerSignature, sizeof(MarkerSignature)) == 0;
    }
    template<class Interface>
    bool IsRebindCandidate(const Interface &candidate, const Interface &saved) {
        const auto &current = candidate.inf.interface_desc;
        const auto &previous = saved.inf.interface_desc;
        // Re-enumeration can change packet sizes, companion descriptors and
        // the chosen alternate. Only the later on-disk proof admits game I/O.
        return candidate.inf.ID >= 0 &&
            candidate.device_desc.idVendor == saved.device_desc.idVendor &&
            candidate.device_desc.idProduct == saved.device_desc.idProduct &&
            current.bInterfaceNumber == previous.bInterfaceNumber &&
            current.bInterfaceSubClass == previous.bInterfaceSubClass &&
            reader::Supported(current.bInterfaceClass, current.bInterfaceSubClass, current.bInterfaceProtocol);
    }
    template<class Interface>
    bool DescriptorMatch(const Interface &left, const Interface &right) {
        return std::memcmp(&left.device_desc, &right.device_desc, sizeof(left.device_desc)) == 0 &&
            std::memcmp(&left.config_desc, &right.config_desc, sizeof(left.config_desc)) == 0 &&
            std::memcmp(&left.inf.interface_desc, &right.inf.interface_desc, sizeof(left.inf.interface_desc)) == 0 &&
            std::memcmp(left.inf.input_endpoint_descs, right.inf.input_endpoint_descs, sizeof(left.inf.input_endpoint_descs)) == 0 &&
            std::memcmp(left.inf.output_endpoint_descs, right.inf.output_endpoint_descs, sizeof(left.inf.output_endpoint_descs)) == 0 &&
            std::memcmp(left.inf.input_ss_endpoint_companion_descs, right.inf.input_ss_endpoint_companion_descs, sizeof(left.inf.input_ss_endpoint_companion_descs)) == 0 &&
            std::memcmp(left.inf.output_ss_endpoint_companion_descs, right.inf.output_ss_endpoint_companion_descs, sizeof(left.inf.output_ss_endpoint_companion_descs)) == 0;
    }
}
