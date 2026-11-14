/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace ams::mitm::usb_storage::verification {
    /* Deliberately not a general path translator. Only the registered archive
     * itself is accepted: no subfiles, placeholder paths, or normalization of
     * traversal. The mounted file is still opened read-only at its generation. */
    inline bool Translate(char *out, std::size_t capacity, const char *path, std::size_t bound) {
        if (!out || !path || capacity == 0) return false;
        std::size_t length = 0;
        while (length < bound && path[length]) ++length;
        if (length == bound) return false;
        const char *suffix = nullptr;
        for (const char *prefix : {"@Sdcard:/Nintendo/Contents/", "@SdCardContent:/", "usbcontent:/"}) {
            const auto n = std::strlen(prefix);
            if (length >= n && std::memcmp(path, prefix, n) == 0) { suffix = path + n; break; }
        }
        if (!suffix) return false;
        if (*suffix == '/') ++suffix; // Stock may use //registered.
        constexpr char registered[] = "registered/";
        if (std::strncmp(suffix, registered, sizeof(registered)-1) != 0) return false;
        const char *dir = suffix + sizeof(registered)-1;
        if (std::strlen(dir) != 8 + 1 + 32 + 4 || dir[8] != '/' || std::strcmp(dir+41, ".nca") != 0) return false;
        auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
        bool nonzero = false;
        for (unsigned i = 0; i < 41; ++i) {
            if (i == 8) continue;
            if (!hex(dir[i])) return false;
            if (i > 8 && dir[i] != '0') nonzero = true;
        }
        if (!nonzero || std::memcmp(dir, "FFFFFFFF", 8) == 0 || std::memcmp(dir, "ffffffff", 8) == 0) return false;
        constexpr char mount[] = "usbcontent:/";
        const auto size = sizeof(mount)-1 + std::strlen(suffix) + 1;
        if (size > capacity) return false;
        std::memcpy(out, mount, sizeof(mount)-1);
        std::memcpy(out+sizeof(mount)-1, suffix, std::strlen(suffix)+1);
        return true;
    }

    inline bool Identity(std::uint64_t header, std::uint64_t requested, std::uint64_t title) {
        /* id=0 is the native unspecified-id form, not permission to open an
         * unrelated archive. Limit this candidate to this application's base
         * or patch. Add-on identities require separate evidence/handling. */
        return title != 0 && (header == title || (title <= UINT64_MAX-0x800 && header == title+0x800))
            && (requested == 0 || requested == header);
    }

    struct Section { int index = -1; bool partition = false; };
    inline Section Select(std::uint32_t type, std::uint8_t content) {
        // Native FS types and NCA content types are distinct enums.
        switch (type) {
            case 2: if (content == 0) return {2, true}; break;  // Program logo, not ExeFS.
            case 3: if (content == 2) return {0, false}; break; // Control.
            case 4: if (content == 3) return {0, false}; break; // Manual.
            case 5: if (content == 1) return {0, true}; break;  // Meta PFS.
            case 6:
                if (content == 0) return {1, false};           // Program data.
                if (content == 4 || content == 5) return {0, false};
                break;
        }
        return {};
    }
    inline bool ShouldFallback(std::uint32_t native, std::uint8_t attributes, bool ready) {
        return native == 0x202 && attributes == 0 && ready;
    }
}
