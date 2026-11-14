/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

namespace ams::mitm::usb_storage::impl {
    // SC1: the existing USB-application header policy is part of this build.
    // Index loss must not change it. The NcaReader still restricts the exception
    // to application IDs; section hashes, keys and data verification are intact.
    constexpr bool UseEmbeddedUsbHeaderCompatibility(bool content_enabled) {
        return content_enabled;
    }
}
