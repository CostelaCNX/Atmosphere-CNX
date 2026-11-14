/* Copyright (c) Atmosphere-NX. GPL-2.0-or-later. */
#pragma once
#include <stratosphere/usb/usb_slot_preferences.hpp>

namespace ams::mitm::usb_storage::slot_preference_sd {
    namespace prefs=ams::usb::slot_preferences;
    constexpr const char *Path="/config/usb_slot_preferences.bin";
    constexpr const char *Previous="/config/usb_slot_preferences.previous.bin";
    constexpr const char *Temp="/config/usb_slot_preferences.tmp";
    inline u32 loaded=0, load_result=0, save_result=0, saved_revision=0;
    inline u64 next_attempt_ns=0;

    inline Result Read(const char *path,prefs::Table &out) {
        FsFile file{};
        R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file),path,ams::fs::OpenMode_Read));
        ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };
        s64 size=0;R_TRY(fsFileGetSize(std::addressof(file),std::addressof(size)));
        R_UNLESS(size==static_cast<s64>(prefs::FileSize),usb::ResultInvalidParameter());
        u8 bytes[prefs::FileSize]{};u64 read=0;
        R_TRY(fsFileRead(std::addressof(file),0,bytes,sizeof(bytes),FsReadOption_None,std::addressof(read)));
        R_UNLESS(read==sizeof(bytes)&&prefs::Decode(out,bytes,sizeof(bytes)),usb::ResultInvalidParameter());
        R_SUCCEED();
    }
    inline void Load() {
        prefs::Table data{};
        auto rc=Read(Path,data);
        if(R_FAILED(rc))rc=Read(Previous,data);
        load_result=rc.GetValue();
        if(R_SUCCEEDED(rc)){prefs::Load(data);loaded=1;}
        // Missing/damaged cache is optional: never prevent detection or boot.
    }
    inline Result Save(const prefs::Table &data) {
        u8 bytes[prefs::FileSize]{};prefs::Encode(bytes,data);
        static_cast<void>(mitm::fs::CreateAtmosphereSdDirectory("/config"));
        const auto del=mitm::fs::DeleteAtmosphereSdFile(Temp);
        R_UNLESS(R_SUCCEEDED(del)||ams::fs::ResultPathNotFound::Includes(del),del);
        R_TRY(mitm::fs::CreateAtmosphereSdFile(Temp,0,ams::fs::CreateOption_None));
        {
            FsFile file{};
            R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file),Temp,ams::fs::OpenMode_ReadWrite));
            ON_SCOPE_EXIT { static_cast<void>(fsFileClose(std::addressof(file))); };
            R_TRY(fsFileSetSize(std::addressof(file),sizeof(bytes)));
            R_TRY(fsFileWrite(std::addressof(file),0,bytes,sizeof(bytes),FsWriteOption_Flush));
        }
        // Read back the closed temp before rotating complete snapshots.
        prefs::Table checked{};R_TRY(Read(Temp,checked));
        u8 verify[prefs::FileSize]{};prefs::Encode(verify,checked);
        R_UNLESS(std::memcmp(bytes,verify,sizeof(bytes))==0,usb::ResultTransactionError());
        char source[ams::fs::EntryNameLengthMax+1]{},target[ams::fs::EntryNameLengthMax+1]{},backup[ams::fs::EntryNameLengthMax+1]{};
        util::SNPrintf(source,sizeof(source),"%s:/atmosphere%s",ams::fs::impl::SdCardFileSystemMountName,Temp);
        util::SNPrintf(target,sizeof(target),"%s:/atmosphere%s",ams::fs::impl::SdCardFileSystemMountName,Path);
        util::SNPrintf(backup,sizeof(backup),"%s:/atmosphere%s",ams::fs::impl::SdCardFileSystemMountName,Previous);
        // When primary is absent, preserve a potentially sole good backup.
        prefs::Table prior{};const auto primary=Read(Path,prior);bool backed_up=false;
        if(R_SUCCEEDED(primary)){
            const auto removed=ams::fs::DeleteFile(backup);
            R_UNLESS(R_SUCCEEDED(removed)||ams::fs::ResultPathNotFound::Includes(removed),removed);
            R_TRY(ams::fs::RenameFile(target,backup));backed_up=true;
        } else if(!ams::fs::ResultPathNotFound::Includes(primary)) {
            // Only replace malformed data, never a file with an I/O/access error.
            R_UNLESS(usb::ResultInvalidParameter::Includes(primary),primary);
            R_TRY(ams::fs::DeleteFile(target));
        }
        const auto rc=ams::fs::RenameFile(source,target);
        if(R_FAILED(rc)&&backed_up)static_cast<void>(ams::fs::RenameFile(backup,target));
        R_RETURN(rc);
    }
    inline void Poll() {
        const u64 now=os::GetSystemTick().ToTimeSpan().GetNanoSeconds();
        if(now<next_attempt_ns)return;
        prefs::Table data{};u32 revision=0;
        if(!prefs::Snapshot(data,revision)||!revision||revision==saved_revision)return;
        // No cache/volume lock or USB operation is held during SD IPC.
        const auto rc=Save(data);save_result=rc.GetValue();
        if(R_SUCCEEDED(rc))saved_revision=revision;
        next_attempt_ns=now+30'000'000'000ULL;
    }
    inline int Format(char *out,size_t size) {
        Poll();
        return util::SNPrintf(out,size,
            "usb_slot_preference=C6\nusb_slot_preference_scope=descriptor_model_hint\n"
            "usb_slot_preference_loaded=%u\nusb_slot_preference_load_result=0x%08X\n"
            "usb_slot_preference_saved_revision=%u\nusb_slot_preference_save_result=0x%08X\n"
            "usb_slot_preference_hits=%u\nusb_slot_preference_misses=%u\n"
            "usb_slot_preference_fallbacks=%u\nusb_slot_preference_learned=%u\n"
            "usb_slot_preference_last_lun=%u\nusb_slot_preference_last_result=0x%08X\n",
            loaded,load_result,saved_revision,save_result,prefs::hits.load(),prefs::misses.load(),
            prefs::fallbacks.load(),prefs::learned.load(),prefs::last_lun.load(),prefs::last_result.load());
    }
}
