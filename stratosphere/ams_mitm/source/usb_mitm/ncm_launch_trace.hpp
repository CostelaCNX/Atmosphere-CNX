/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <stratosphere.hpp>

/* Private wire layout shared with NCM recovery commands 65000/65001.
 * This stays inside the recovery build and does not change a public NCM ABI. */
namespace ams::mitm::usb_storage::ncm_trace {

    inline constexpr u32 TraceMagic = 0x3944434e; /* NCD9 */
    inline constexpr u32 TraceVersion = 1;
    inline constexpr u32 BeginTraceCommand = 65000;
    inline constexpr u32 EndTraceCommand = 65001;
    inline constexpr size_t TraceCapacity = 64;

    enum class LookupOperation : u32 {
        MetadataContentInfo = 1,
        MetadataLatestKey   = 2,
        OverlayHas          = 3,
        OverlayPath         = 4,
        NativeHas           = 5,
        NativePath          = 6,
        OpenStorage         = 7,
        OpenMetadata        = 8,
        OverlaySize         = 9,
        OverlayRead         = 10,
        OverlayRights       = 11,
        OverlayProgramId    = 12,
        OverlayCount        = 13,
        OverlayList         = 14,
    };

    struct LookupRecord {
        u64 sequence;
        u64 title_id;
        u32 operation;
        u32 result;
        u32 metadata_version;
        u8 storage_id;
        u8 content_type;
        u8 id_offset;
        u8 flags;
        u8 content_id[16];
        char detail[48];
    };
    static_assert(sizeof(LookupRecord) == 96);
    static_assert(offsetof(LookupRecord, content_id) == 32);

    struct LookupSnapshot {
        u32 magic;
        u32 version;
        u32 record_size;
        u32 count;
        u32 dropped;
        u32 flags;
        u64 session;
        u64 title_id;
        LookupRecord records[TraceCapacity];
    };
    static_assert(offsetof(LookupSnapshot, records) == 40);
    static_assert(sizeof(LookupSnapshot) == 6184);
    static_assert(std::is_trivially_copyable_v<LookupSnapshot>);

    constexpr bool HasValidShape(const LookupSnapshot &snapshot) {
        return snapshot.magic == TraceMagic
            && snapshot.version == TraceVersion
            && snapshot.record_size == sizeof(LookupRecord)
            && snapshot.count <= TraceCapacity;
    }

}
