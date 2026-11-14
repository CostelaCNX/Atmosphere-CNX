/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <cstddef>
#include <cstdint>

namespace ams::mitm::usb_storage::verification {
    constexpr std::size_t AddOnPrefixSize = 0x38;
    constexpr std::size_t AddOnMaxSize = 64 * 1024;

    /* Parse an integrity-checked PackagedContentMetaHeader and AddOn extended
     * header. No ID-mask/family guess grants access. Add-on files contain no
     * extended-data region: header, extended header, content/meta records and
     * final 32-byte digest. The caller streams the entire verified file. */
    inline bool AddOnAssociation(const std::uint8_t *data, std::size_t prefix_size,
        std::uint64_t file_size, std::uint64_t header_id, std::uint64_t title) {
        if (!data || prefix_size < AddOnPrefixSize || file_size < AddOnPrefixSize ||
            file_size > AddOnMaxSize || !header_id || !title) return false;
        auto le = [data](std::size_t pos, unsigned size) {
            std::uint64_t value = 0;
            for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(data[pos+i]) << (i*8);
            return value;
        };
        const auto ext = le(14, 2);
        if (data[12] != 0x82 || data[13] != 0 || (ext != 16 && ext != 24) ||
            le(0, 8) != header_id || le(32, 8) != title) return false;
        const auto bytes = 32 + ext + le(16, 2)*56 + le(18, 2)*16 + 32;
        return bytes == file_size;
    }
}
