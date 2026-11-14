/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
namespace ams::ncm::usb_limits {
    // Bounded post-mount allocations; never enlarge NCM's early static image.
    constexpr unsigned Titles=512;
    constexpr unsigned Content=8192;
    constexpr unsigned Magic=0x3350584E;
    constexpr unsigned Abi=1;
}
