/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstdint>

namespace ams::fssystem {
    /* Explicit per-reader USB exception, never a global crypto setting.
     * System titles and out-of-range identifiers cannot opt out. This does
     * not claim an invalid signature is valid or change any data hashes. */
    constexpr bool AcceptNcaHeaderSignature(bool valid, bool allow_unsigned_usb_application, uint64_t program_id) {
        return valid || (allow_unsigned_usb_application &&
            program_id >= UINT64_C(0x0100000000010000) && program_id <= UINT64_C(0x01FFFFFFFFFFFFFF));
    }
}
