/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere/usb/usb_content_commands.hpp>

namespace ams::usb::content {
    constexpr unsigned int HeaderQuery_Rights = 1;
    constexpr unsigned int HeaderQuery_Program = 2;
    constexpr unsigned int Trace_NcmHeaderBegin = 200;
    constexpr unsigned int Trace_NcmHeaderResult = 201;

    struct ContentHeaderInfo {
        u64 program_id;
        u8 rights_id[16];
        u8 key_generation;
        u8 reserved[7];
    };
    static_assert(sizeof(ContentHeaderInfo) == 0x20);

    /* An exact NCM SD content path, not an arbitrary filesystem redirect.
     * Accept flat and single-bucket layouts and the common double root slash.
     * Both peers validate this before any USB header IO. */
    inline const char *GetSdContentNcaSuffix(const char *path) {
        constexpr char Prefix[] = "@SdCardContent:";
        if (path == nullptr) { return nullptr; }
        for (unsigned i = 0; i < sizeof(Prefix) - 1; ++i) {
            if (path[i] != Prefix[i]) { return nullptr; }
        }
        const char *p = path + sizeof(Prefix) - 1;
        if (*p != '/') { return nullptr; }
        while (*p == '/') { ++p; }
        const char *suffix = p;
        auto consume = [](const char *value, const char *prefix) -> const char * {
            while (*prefix) { if (*value++ != *prefix++) { return nullptr; } }
            return value;
        };
        p = consume(suffix, "registered/");
        if (p == nullptr) { p = consume(suffix, "placehld/"); }
        if (p == nullptr) { return nullptr; }
        auto hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
        unsigned n = 0;
        while (hex(p[n])) { ++n; }
        if (n == 8 && p[n] == '/') {
            p += n + 1;
            n = 0;
            while (hex(p[n])) { ++n; }
        }
        if (n != 32) { return nullptr; }
        p = consume(p + n, ".nca");
        return p != nullptr && *p == '\0' ? suffix : nullptr;
    }
}
