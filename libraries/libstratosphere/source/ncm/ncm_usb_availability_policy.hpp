/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstring>

namespace ams::ncm::usb_availability {
    // Exact registered NCA leaves only. Never classify metadata, placeholders,
    // directories, other mounts or arbitrary missing SD paths as USB content.
    inline const char *GetRegisteredId(const char *path) {
        if (!path) return nullptr;
        constexpr char common[] = "@SdCardContent:";
        if (std::strncmp(path, common, sizeof(common) - 1) == 0) path += sizeof(common) - 1;
        if (*path != '/') return nullptr;
        while (*path == '/') ++path;
        constexpr char root[] = "registered/";
        if (std::strncmp(path, root, sizeof(root) - 1) != 0) return nullptr;
        path += sizeof(root) - 1;
        auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
        unsigned n = 0;
        while (hex(path[n])) ++n;
        if (n == 8 && path[n] == '/') {
            path += n + 1;
            n = 0;
            while (hex(path[n])) ++n;
        }
        return n == 32 && std::strcmp(path + n, ".nca") == 0 ? path : nullptr;
    }
}
