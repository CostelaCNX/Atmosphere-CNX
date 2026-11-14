// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_virtual_sd.hpp"
#include "usb_homebrew_service.hpp"
#include "usbmitm_module.hpp"
#include "usb_startup_timing.hpp"
namespace ams::mitm::usb_storage {
namespace {
struct Slot {
    u64 pid{};
    sf::CopyHandle process{};
    nexus_vsd::Session session{};
};
os::SdkMutex g_vsd_lock{};
Slot g_vsd_slots[8]{};
VirtualSdDiagnostics g_vsd_diagnostics{};
std::atomic_bool g_reaper_ready{false};
std::atomic<u64> g_reaper_closed{0};
std::atomic<u32> g_reaper_init_result{0};
// Atmosphere API priority (not raw Horizon priority): 24 -> 52.
constexpr s32 ReaperPriority = 24;
static_assert(ReaperPriority >= os::HighestThreadPriority && ReaperPriority <= os::LowestThreadPriority);
alignas(os::ThreadStackAlignment) u8 g_reaper_stack[0x4000]{};
os::ThreadType g_reaper_thread;
u64 Now(){return os::GetSystemTick().ToTimeSpan().GetMilliSeconds();}
void ReapVirtualSdProcesses() {
    sf::CopyHandle retired[8]{};
    size_t count=0;
    {
        std::unique_lock lk(g_vsd_lock,std::try_to_lock);
        if(!lk.owns_lock())return;
        ++g_vsd_diagnostics.polls;
        for(auto &s:g_vsd_slots)if(s.pid) {
            s64 state{};
            const Result rc=svc::GetProcessInfo(&state,s.process.GetOsHandle(),svc::ProcessInfoType_ProcessState);
            if(R_FAILED(rc)) {
                ++g_vsd_diagnostics.query_errors;
                g_vsd_diagnostics.last_query_error=rc.GetValue();
                continue; // An error is NOT proof that a process exited.
            }
            // Process handles can signal for non-exit state changes too.
            // Never reset the signal shared with PM or reap a live/debugged app.
            if(state!=svc::ProcessState_Terminated)continue;
            g_vsd_diagnostics.last_pid=s.pid;
            g_vsd_diagnostics.last_ms=Now();
            ++g_vsd_diagnostics.detached;
            s.session.Clear();
            retired[count++]=std::move(s.process);
            s.pid=0;
        }
    }
    // The last close may finalize kernel memory. Never do it while holding
    // the session lock, a filesystem/USB lock, or a diagnostics lock.
    for(size_t i=0;i<count;++i) {
        retired[i].Reset();
        g_reaper_closed.fetch_add(1,std::memory_order_relaxed);
    }
}
void VirtualSdReaperThread(void *) {
    while(true) {
        ReapVirtualSdProcesses();
        os::SleepThread(TimeSpan::FromMilliSeconds(250));
    }
}
Slot *Find(u64 pid){for(auto &s:g_vsd_slots)if(s.pid==pid)return &s;return nullptr;}
NexusVsdStatus Status(const Slot *s){
    NexusVsdStatus out{NEXUS_VSD_MAGIC,NEXUS_VSD_VERSION,0,
        g_reaper_ready.load(std::memory_order_acquire)?NEXUS_VSD_CAP_USB_ROOT:0u,0,0};
    if(s){out.flags=NEXUS_VSD_LOADER_SEEN;
        if(s->session.phase==nexus_vsd::Phase::Armed)out.flags|=NEXUS_VSD_ARMED;
        if(s->session.phase==nexus_vsd::Phase::Active)out.flags|=NEXUS_VSD_ACTIVE;
        out.serial=s->session.serial;out.generation=s->session.generation;}
    return out;
}
Result BufferPath(char *out,const sf::InBuffer &in) {
    R_UNLESS(in.GetSize()>1 && in.GetSize()<=nexus_vsd::PathCapacity,usb::ResultInvalidParameter());
    const auto p=reinterpret_cast<const char *>(in.GetPointer());
    R_UNLESS(p && p[in.GetSize()-1]==0 && std::strlen(p)==in.GetSize()-1,usb::ResultInvalidParameter());
    std::memcpy(out,p,in.GetSize());R_SUCCEED();
}
}
void InitializeVirtualSdReaper() {
    // Single boot-time caller, before publishing the service. Creation failure
    // leaves SD-only loading usable without retaining any process handles.
    const Result rc=os::CreateThread(&g_reaper_thread,VirtualSdReaperThread,nullptr,
        g_reaper_stack,sizeof(g_reaper_stack),ReaperPriority);
    g_reaper_init_result.store(rc.GetValue(),std::memory_order_relaxed);
    if(R_FAILED(rc))return;
    os::SetThreadNamePointer(&g_reaper_thread,"NexusVsdReaper");
    os::StartThread(&g_reaper_thread);
    g_reaper_ready.store(true,std::memory_order_release);
}
bool GetVirtualSdDiagnostics(VirtualSdDiagnostics *out) {
    std::unique_lock lk(g_vsd_lock,std::try_to_lock);
    if(!lk.owns_lock())return false;
    *out=g_vsd_diagnostics;
    for(const auto &s:g_vsd_slots)if(s.pid)++out->watched;
    out->closed=g_reaper_closed.load(std::memory_order_relaxed);
    out->running=g_reaper_ready.load(std::memory_order_acquire);
    out->init_result=g_reaper_init_result.load(std::memory_order_relaxed);
    return true;
}
bool GetVirtualSdLease(VirtualSdLease *out,u64 pid){
    std::scoped_lock lk(g_vsd_lock);
    auto s=Find(pid);
    if(!s || s->session.phase!=nexus_vsd::Phase::Active)return false;
    *out={s->session.profile,pid,s->session.serial,s->session.generation};return true;
}
Result CheckVirtualSdLease(const VirtualSdLease &lease){
    {std::scoped_lock lk(g_vsd_lock);auto s=Find(lease.pid);
        R_UNLESS(s && s->session.Valid(lease.serial,lease.generation),fs::ResultSdCardNotPresent());}
    usb::storage::Status status{};
    R_TRY(GetUsbContentStorageService()->GetStatus(&status));
    R_UNLESS(status.state==usb::storage::State::Ready && status.running &&
        status.media_generation==lease.generation,fs::ResultSdCardNotPresent());
    R_SUCCEED();
}
Result HomebrewService::QueryVirtualSd(sf::Out<NexusVsdStatus> out,const sf::ClientProcessId &pid){
    std::scoped_lock lk(g_vsd_lock);out.SetValue(Status(Find(pid.GetValue().value)));R_SUCCEED();
}
Result HomebrewService::CancelVirtualSd(const sf::ClientProcessId &pid){
    // Keep the authenticated loader introduction for a later Arm in this same
    // process. The independent watcher owns its bounded lifetime, even Idle.
    {std::scoped_lock lk(g_vsd_lock);if(auto s=Find(pid.GetValue().value))s->session.Clear();}
    RetireStartupTimingCapture(pid.GetValue().value);R_SUCCEED();
}
Result HomebrewService::BeginVirtualSdLoad(sf::Out<NexusVsdStatus> out,const sf::ClientProcessId &pid,
    sf::CopyHandle &&process,const sf::InBuffer &path){
    char value[nexus_vsd::PathCapacity]{};
    R_TRY(BufferPath(value,path));
    u64 handle_pid{};R_TRY(svc::GetProcessId(&handle_pid,process.GetOsHandle()));
    R_UNLESS(process.IsManaged() && handle_pid && handle_pid==pid.GetValue().value,usb::ResultInvalidParameter());
    if(!g_reaper_ready.load(std::memory_order_acquire)) {
        out.SetValue(Status(nullptr));R_SUCCEED(); // No mapping and no retained handle.
    }
    s64 state{};
    R_TRY(svc::GetProcessInfo(&state,process.GetOsHandle(),svc::ProcessInfoType_ProcessState));
    R_UNLESS(state!=svc::ProcessState_Terminated,usb::ResultInvalidParameter());
    // A subsequent loader handoff (including an untimed SD chainload) ends
    // the previous game's observation before its filesystem view changes.
    RetireStartupTimingCapture(handle_pid);
    std::scoped_lock lk(g_vsd_lock);
    auto s=Find(handle_pid);
    if(!s){for(auto &candidate:g_vsd_slots)if(!candidate.pid){s=&candidate;break;}}
    R_UNLESS(s,usb::ResultResourceBusy());
    if(!s->pid) {
        s->pid=handle_pid;s->process=std::move(process);
        ++g_vsd_diagnostics.retained;
    } // Existing PID already has a validated watch; RAII closes the extra copy.
    const bool armed=s->session.phase==nexus_vsd::Phase::Armed;
    const bool active=s->session.Begin(value,Now());
    out.SetValue(Status(s));
    // Expired or mismatched arm must not silently load an SD copy.
    R_UNLESS(!armed || active,usb::ResultInvalidParameter());
    R_SUCCEED();
}
Result HomebrewService::ArmVirtualSd(const sf::ClientProcessId &pid,u64 generation,const sf::InBuffer &path){
    R_UNLESS(g_reaper_ready.load(std::memory_order_acquire),usb::ResultNotInitialized());
    char value[nexus_vsd::PathCapacity]{};R_TRY(BufferPath(value,path));
    nexus_vsd::Profile profile{};
    R_UNLESS(nexus_vsd::Parse(value,profile),usb::ResultInvalidParameter());
    auto service=GetUsbContentStorageService();
    usb::volume::FileInfo info{};R_TRY(service->GetFileInfoAtGeneration(value,&info,generation));
    R_UNLESS(!info.directory && info.size>=0x80,usb::ResultInvalidParameter());
    u8 header[0x80]{};size_t count{};
    R_TRY(service->ReadFileAtGeneration(value,0,header,sizeof(header),&count,generation));
    R_UNLESS(count==sizeof(header) && std::memcmp(header+0x10,"NRO0",4)==0,usb::ResultInvalidParameter());
    u32 size{};std::memcpy(&size,header+0x18,sizeof(size));
    R_UNLESS(size>=0x80 && size<=info.size,usb::ResultInvalidParameter());
    std::scoped_lock lk(g_vsd_lock);
    auto s=Find(pid.GetValue().value);
    R_UNLESS(s,usb::ResultNotInitialized()); // custom hbloader must have introduced this PID
    R_UNLESS(s->session.Arm(value,generation,Now()),usb::ResultInvalidParameter());
    R_SUCCEED();
}
}
