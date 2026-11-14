/* Nexus S2: isolated read-only USB diagnostic. GPL-2.0-or-later. */
#include <stratosphere.hpp>
#include <stratosphere/usb/usb_device_map.hpp>
#include <stratosphere/usb/usb_compatibility.hpp>
#include <stratosphere/usb/usb_reader_fixed_sense.hpp>
#include <stratosphere/usb/usb_slot_preferences.hpp>
#include "../amsmitm_initialization.hpp"
#include "../amsmitm_fs_utils.hpp"
#include "usb_probe_module.hpp"
#include "protocol.h"

#define NX_S2_INTERFACE(C,H) \
    AMS_SF_METHOD_INFO(C,H,0,Result,GetStatus,(const sf::OutBuffer &out),(out)) \
    AMS_SF_METHOD_INFO(C,H,1,Result,List,(sf::Out<u32> count,const sf::InBuffer &in,const sf::OutBuffer &out),(count,in,out)) \
    AMS_SF_METHOD_INFO(C,H,2,Result,Read,(sf::Out<u32> count,const sf::InBuffer &in,const sf::OutBuffer &out),(count,in,out))
AMS_SF_DEFINE_INTERFACE(ams::mitm::usb_probe::impl, IProbe, NX_S2_INTERFACE, 0x5fca267e);

namespace ams::mitm::usb_probe {
namespace {
    usb::storage::Service g_storage(true);
    usb::volume::DirectoryEntryInfo g_entries[NX_S2_ENTRIES]{};
    NxS2Entry g_wire_entries[NX_S2_ENTRIES]{};
    alignas(0x1000) u8 g_read[NX_S2_READ_MAX]{};
    usb::mapping::SnapshotData g_map{};
    usb::compatibility::State g_profile{};
    char g_log[usb::mapping::ReportBytes]{};
    char g_status_log[2048]{};
    os::ThreadType g_logger{};
    alignas(os::ThreadStackAlignment) u8 g_logger_stack[0x10000]{};
    std::atomic<u32> g_log_result{}, g_request_result{}, g_request_kind{};
    std::atomic<u64> g_request_count{}, g_read_bytes{};
    u32 g_cache_result{};

    Result LoadSlotCache(const char *path) {
        namespace prefs=usb::slot_preferences;
        FsFile file{};
        R_TRY(mitm::fs::OpenAtmosphereSdFile(&file,path,ams::fs::OpenMode_Read));
        ON_SCOPE_EXIT { fsFileClose(&file); };
        s64 size=0; R_TRY(fsFileGetSize(&file,&size));
        R_UNLESS(size==static_cast<s64>(prefs::FileSize),usb::ResultInvalidParameter());
        u8 bytes[prefs::FileSize]{}; u64 read=0; prefs::Table table{};
        R_TRY(fsFileRead(&file,0,bytes,sizeof(bytes),FsReadOption_None,&read));
        R_UNLESS(read==sizeof(bytes) && prefs::Decode(table,bytes,sizeof(bytes)),usb::ResultInvalidParameter());
        prefs::Load(table); R_SUCCEED();
    }

    NxS2Status Status() {
        usb::storage::Status s{};
        R_DISCARD(g_storage.GetStatus(&s));
        NxS2Status w{};
        w.version=NX_S2_VERSION; w.state=static_cast<u32>(s.state);
        w.result=s.last_result; w.io_result=s.last_io_result; w.generation=s.media_generation;
        w.block_count=s.block_count; w.block_size=s.block_size;
        w.filesystem=static_cast<u32>(s.filesystem_type); w.lun=s.lun;
        w.probe_stage=s.probe_stage; w.probe_result=s.probe_result; w.interfaces=s.available_interface_count;
        std::memcpy(w.vendor,s.vendor,sizeof(s.vendor)); std::memcpy(w.product,s.product,sizeof(s.product));
        return w;
    }
    void Save(const char *path,const char *data,size_t size) {
        if (size>sizeof(g_log)) return;
        R_DISCARD(mitm::fs::CreateAtmosphereSdFile(path,0,ams::fs::CreateOption_None));
        FsFile file{};
        Result rc=mitm::fs::OpenAtmosphereSdFile(&file,path,ams::fs::OpenMode_ReadWrite|ams::fs::OpenMode_AllowAppend);
        if (R_SUCCEEDED(rc)) {
            ON_SCOPE_EXIT { fsFileClose(&file); };
            rc=fsFileSetSize(&file,size);
            if (R_SUCCEEDED(rc)) rc=fsFileWrite(&file,0,data,size,FsWriteOption_Flush);
        }
        g_log_result.store(rc.GetValue());
    }
    void LogThread(void *) {
        R_DISCARD(mitm::fs::CreateAtmosphereSdDirectory("/logs"));
        for (;;) {
            // Status mutex is short-lived; released before any SD operation.
            const auto s=Status();
            const int n=util::SNPrintf(g_status_log,sizeof(g_status_log),
                "Nexus S2 / read-only transport / no content routing\n"
                "state=%u result=%08X io=%08X generation=%llu\n"
                "blocks=%llu block_size=%u filesystem=%u lun=%u\n"
                "probe_stage=%u probe_result=%08X interfaces=%u\n"
                "previous_log_result=%08X requests=%llu last_command=%u last_result=%08X bytes_read=%llu\n"
                "slot_cache_load=%08X cache_hits=%u cache_fallbacks=%u cache_saving=disabled\n"
                "USB3 settings unchanged. FAT32/exFAT only. No USB writes.\n",
                s.state,s.result,s.io_result,(unsigned long long)s.generation,
                (unsigned long long)s.block_count,s.block_size,s.filesystem,s.lun,
                s.probe_stage,s.probe_result,s.interfaces,g_log_result.load(),
                (unsigned long long)g_request_count.load(),g_request_kind.load(),g_request_result.load(),
                (unsigned long long)g_read_bytes.load(),g_cache_result,
                usb::slot_preferences::hits.load(),usb::slot_preferences::fallbacks.load());
            if (n>0 && size_t(n)<sizeof(g_status_log)) Save("/logs/nexus_usb_s2_status.txt",g_status_log,n);
            if (usb::mapping::Snapshot(g_map)) {
                int len=usb::mapping::Format(g_log,sizeof(g_log),g_map);
                if (len>0) Save("/logs/nexus_usb_s2_devices.txt",g_log,len);
                if (usb::compatibility::Snapshot(g_profile)) {
                    len=usb::compatibility::Format(g_log,sizeof(g_log),g_map,g_profile);
                    if (len>0) Save("/logs/nexus_usb_s2_compatibility.txt",g_log,len);
                }
            }
            int len=usb::reader_fixed_sense::Format(g_log,sizeof(g_log));
            if (len>0) Save("/logs/nexus_usb_s2_startup.txt",g_log,len);
            os::SleepThread(TimeSpan::FromSeconds(10));
        }
    }
    Result Request(NxS2Request *r,const sf::InBuffer &in) {
        R_UNLESS(in.GetSize()==sizeof(*r),usb::ResultInvalidParameter());
        std::memcpy(r,in.GetPointer(),sizeof(*r));
        R_UNLESS(!r->reserved && r->generation && nxS2ValidPath(r->path,sizeof(r->path)),usb::ResultInvalidParameter());
        R_SUCCEED();
    }
    class Probe {
    public:
        Result GetStatus(const sf::OutBuffer &out) {
            R_UNLESS(out.GetSize()==sizeof(NxS2Status),usb::ResultInvalidParameter());
            const auto s=Status(); std::memcpy(out.GetPointer(),&s,sizeof(s)); R_SUCCEED();
        }
        Result List(sf::Out<u32> count,const sf::InBuffer &in,const sf::OutBuffer &out) {
            count.SetValue(0); NxS2Request r{}; R_TRY(Request(&r,in));
            R_UNLESS(r.offset<=1000000 && r.count==NX_S2_ENTRIES && out.GetSize()==sizeof(g_wire_entries),usb::ResultInvalidParameter());
            size_t found=0; std::memset(g_entries,0,sizeof(g_entries)); std::memset(g_wire_entries,0,sizeof(g_wire_entries));
            const Result rc=g_storage.ReadDirectoryAtGeneration(r.path,g_entries,NX_S2_ENTRIES,r.offset,true,true,&found,r.generation);
            g_request_kind.store(1); g_request_result.store(rc.GetValue()); g_request_count.fetch_add(1);
            R_TRY(rc); R_UNLESS(found<=NX_S2_ENTRIES,usb::ResultInvalidParameter());
            for (size_t i=0;i<found;++i) {
                std::memcpy(g_wire_entries[i].name,g_entries[i].name,255);
                g_wire_entries[i].size=g_entries[i].size; g_wire_entries[i].directory=g_entries[i].directory;
            }
            std::memcpy(out.GetPointer(),g_wire_entries,sizeof(g_wire_entries)); count.SetValue(found); R_SUCCEED();
        }
        Result Read(sf::Out<u32> count,const sf::InBuffer &in,const sf::OutBuffer &out) {
            count.SetValue(0); NxS2Request r{}; R_TRY(Request(&r,in));
            R_UNLESS(r.count && r.count<=NX_S2_READ_MAX && out.GetSize()==r.count && r.offset<=UINT64_MAX-r.count,usb::ResultInvalidParameter());
            size_t read=0; std::memset(g_read,0,r.count);
            const Result rc=g_storage.ReadFileAtGeneration(r.path,r.offset,g_read,r.count,&read,r.generation);
            g_request_kind.store(2); g_request_result.store(rc.GetValue()); g_request_count.fetch_add(1);
            R_TRY(rc); R_UNLESS(read<=r.count,usb::ResultInvalidParameter());
            std::memcpy(out.GetPointer(),g_read,r.count); count.SetValue(read); g_read_bytes.fetch_add(read); R_SUCCEED();
        }
    };
    static_assert(impl::IsIProbe<Probe>);
    sf::hipc::ServerManager<1,sf::hipc::DefaultServerManagerOptions,2> g_server;
    sf::UnmanagedServiceObject<impl::IProbe,Probe> g_object;
}
void MitmModule::ThreadFunction(void *) {
    mitm::WaitInitialized();
    os::SleepThread(TimeSpan::FromSeconds(10));
    auto cache=LoadSlotCache("/config/usb_slot_preferences.bin");
    if (R_FAILED(cache)) cache=LoadSlotCache("/config/usb_slot_preferences.previous.bin");
    g_cache_result=cache.GetValue();
    // No app process handles, content routing, title metadata or launch waits.
    R_DISCARD(g_storage.Start());
    if (R_SUCCEEDED(os::CreateThread(&g_logger,LogThread,nullptr,g_logger_stack,sizeof(g_logger_stack),30))) os::StartThread(&g_logger);
    // Failure leaves stock services alive. It must never abort ams_mitm.
    if (R_SUCCEEDED(g_server.RegisterObjectForServer(g_object.GetShared(),sm::ServiceName::Encode("nx:usb2"),2))) g_server.LoopProcess();
}
}
