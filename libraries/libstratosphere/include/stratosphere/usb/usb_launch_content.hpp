/* USB launch snapshot. Metadata and entitlement remain owned by Horizon.
 * GPL-2.0-only, consistent with the surrounding Atmosphere code. */
#pragma once
#include <vapours.hpp>
#include <stratosphere/ncm/ncm_content_id.hpp>

namespace ams::usb::content {
    /* process_id=0 releases only this Loader pin; no persistent map mutation. */
    struct ProcessContentBinding {
        u64 program_id;
        u64 process_id;
        u64 pin_id;
    };
    static_assert(sizeof(ProcessContentBinding) == 24);

    constexpr u32 LaunchContentVersion = 1;
    constexpr size_t MaxLaunchAddOns = 128;
    struct LaunchAddOn {
        u64 data_id;
        ncm::ContentId content_id;
        u32 version;
        u32 storage_id;
    };
    struct LaunchContent {
        u32 version;
        u32 size;
        u64 program_id;
        u64 application_id;
        u64 patch_id;
        u32 patch_version;
        u32 add_on_count;
        ncm::ContentId base;
        ncm::ContentId patch;
        LaunchAddOn add_ons[MaxLaunchAddOns];
    };
    static_assert(sizeof(LaunchAddOn) == 32);
    static_assert(sizeof(LaunchContent) == 4168);

    inline bool IsValidLaunchContent(const LaunchContent &v) {
        if (v.version != LaunchContentVersion || v.size != sizeof(v)
            || v.program_id == 0 || v.application_id == 0
            || v.program_id < v.application_id || v.program_id - v.application_id > 255
            || v.base == ncm::InvalidContentId || v.add_on_count > MaxLaunchAddOns
            || ((v.patch_id == 0) != (v.patch == ncm::InvalidContentId))
            || (v.patch_id == 0 && v.patch_version != 0)) return false;
        for (u32 i = 0; i < v.add_on_count; ++i) {
            const auto &a = v.add_ons[i];
            if (a.data_id == 0 || a.content_id == ncm::InvalidContentId || a.storage_id != 5) return false;
            for (u32 j = 0; j < i; ++j) if (v.add_ons[j].data_id == a.data_id) return false;
        }
        return true;
    }
}
