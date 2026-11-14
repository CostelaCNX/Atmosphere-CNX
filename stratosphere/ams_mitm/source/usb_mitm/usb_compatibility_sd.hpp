/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere/usb/usb_compatibility.hpp>

namespace ams::mitm::usb_storage::compatibility_sd {
    namespace policy = ams::usb::compatibility;
    constexpr const char *Temp = "/logs/usb_compatibility.txt.tmp";
    inline uint32_t saved_map_revision{}, saved_selection_revision{}, save_result{};
    inline uint64_t next_attempt_ns{};

    // Monitor thread only, outside USB/volume/cache locks. Fixed-size output,
    // closed + flushed temporary file, one previous complete report retained.
    inline Result Save(const char *data, size_t length) {
        R_UNLESS(data != nullptr && length && length < policy::ReportBytes, usb::ResultInvalidParameter());
        static_cast<void>(mitm::fs::CreateAtmosphereSdDirectory("/logs"));
        const auto del = mitm::fs::DeleteAtmosphereSdFile(Temp);
        R_UNLESS(R_SUCCEEDED(del) || ams::fs::ResultPathNotFound::Includes(del), del);
        R_TRY(mitm::fs::CreateAtmosphereSdFile(Temp, 0, ams::fs::CreateOption_None));
        {
            FsFile file{};
            R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), Temp, ams::fs::OpenMode_ReadWrite));
            ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };
            R_TRY(fsFileSetSize(std::addressof(file), length));
            R_TRY(fsFileWrite(std::addressof(file), 0, data, length, FsWriteOption_Flush));
        }
        char source[ams::fs::EntryNameLengthMax+1]{}, target[ams::fs::EntryNameLengthMax+1]{}, previous[ams::fs::EntryNameLengthMax+1]{};
        util::SNPrintf(source, sizeof(source), "%s:/atmosphere%s", ams::fs::impl::SdCardFileSystemMountName, Temp);
        util::SNPrintf(target, sizeof(target), "%s:/atmosphere/logs/usb_compatibility.txt", ams::fs::impl::SdCardFileSystemMountName);
        util::SNPrintf(previous, sizeof(previous), "%s:/atmosphere/logs/usb_compatibility.previous.txt", ams::fs::impl::SdCardFileSystemMountName);
        ams::fs::DirectoryEntryType type{};
        const auto present = ams::fs::GetEntryType(std::addressof(type), target);
        bool rotated = false;
        if (R_SUCCEEDED(present)) {
            R_UNLESS(type == ams::fs::DirectoryEntryType_File, usb::ResultInvalidParameter());
            const auto removed = ams::fs::DeleteFile(previous);
            R_UNLESS(R_SUCCEEDED(removed) || ams::fs::ResultPathNotFound::Includes(removed), removed);
            R_TRY(ams::fs::RenameFile(target, previous)); rotated = true;
        } else R_UNLESS(ams::fs::ResultPathNotFound::Includes(present), present);
        // With no primary, keep the previous report (including across reboot).
        const auto published = ams::fs::RenameFile(source, target);
        if (R_FAILED(published) && rotated) static_cast<void>(ams::fs::RenameFile(previous, target));
        R_RETURN(published);
    }
    inline void Poll(const usb::mapping::SnapshotData &map) {
        const uint64_t now = os::GetSystemTick().ToTimeSpan().GetNanoSeconds();
        if (now < next_attempt_ns) return;
        policy::State state{};
        if (!policy::Snapshot(state) || (!map.revision && !state.revision)) return;
        if (saved_map_revision == map.revision && saved_selection_revision == state.revision) return;
        static char report[policy::ReportBytes];
        const int length = policy::Format(report, sizeof(report), map, state);
        const auto rc = length > 0 && static_cast<size_t>(length) < sizeof(report)
            ? Save(report, static_cast<size_t>(length)) : static_cast<Result>(usb::ResultInvalidParameter());
        save_result = rc.GetValue();
        if (R_SUCCEEDED(rc)) { saved_map_revision = map.revision; saved_selection_revision = state.revision; }
        next_attempt_ns = now + (R_SUCCEEDED(rc) ? 10'000'000'000ULL : 30'000'000'000ULL);
    }
    inline int Format(char *out, size_t size, const usb::mapping::SnapshotData &map) {
        Poll(map);
        return util::SNPrintf(out, size,
            "usb_compatibility=C12\nusb_compatibility_log=atmosphere/logs/usb_compatibility.txt\n"
            "usb_compatibility_save_result=0x%08X\nusb_compatibility_saved_map_revision=%u\n"
            "usb_compatibility_saved_selection_revision=%u\nusb_compatibility_usb3=host_settings_unchanged\n",
            save_result, saved_map_revision, saved_selection_revision);
    }
}
