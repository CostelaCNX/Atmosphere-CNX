// SPDX-License-Identifier: GPL-2.0-or-later
// Verbatim production Service power methods, native state/USB/FS fixtures.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using u8=std::uint8_t;using u32=std::uint32_t;using u64=std::uint64_t;using s32=std::int32_t;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
namespace ams {
struct Result{u32 value{};constexpr Result(u32 v=0):value(v){}constexpr u32 GetValue()const{return value;}};
constexpr Result ResultSuccess(){return {};}
struct TimeSpan{u64 ns;static TimeSpan FromMilliSeconds(u64 n){return {n*1000000};}static TimeSpan FromNanoSeconds(u64 n){return {n};}};
namespace os{inline u64 now=1000000;void SleepThread(TimeSpan t){now+=t.ns;}}
namespace svc{struct ResultSessionClosed:Result{ResultSessionClosed():Result(98){}static bool Includes(Result r){return r.value==98;}};
struct ResultTimedOut:Result{ResultTimedOut():Result(99){}static bool Includes(Result r){return r.value==99;}};}
namespace sm {
struct ServiceName { static int Encode(const char*){return 0;} };
inline unsigned missing;
Result HasService(bool *present,int){*present=missing==0;if(missing)--missing;return {};}
}
namespace usb{
#define ERROR(n,v) struct n:Result{n():Result(v){}static bool Includes(Result r){return r.value==v;}};
ERROR(ResultNotInitialized,1) ERROR(ResultResourceBusy,2) ERROR(ResultMediaChanged,3) ERROR(ResultInternalStateError,4)
ERROR(ResultOperationDenied,5) ERROR(ResultUnsupportedFilesystem,6) ERROR(ResultAlreadyInitialized,7)
ERROR(ResultInterrupted,8) ERROR(ResultTransactionError,9) ERROR(ResultScsiCommandFailed,10) ERROR(ResultFileNotFound,11)
ERROR(ResultInvalidParameter,12)
ERROR(ResultMemAllocFailure,13)
#undef ERROR
namespace impl{u64 UsbNowNs(){return os::now;}}
}}
#define R_SUCCEEDED(r) ((r).GetValue()==0)
#define R_FAILED(r) (!R_SUCCEEDED(r))
#define R_RETURN(r) return (r)
#define R_THROW(r) return (r)
#define R_SUCCEED() return ams::ResultSuccess()
#define R_UNLESS(c,r) do{if(!(c))return (r);}while(0)
#define R_TRY(e) do{const ams::Result rc_=(e);if(R_FAILED(rc_))return rc_;}while(0)
#define AMS_ASSERT(c) CHECK(c)
#define AMS_UNUSED(v) static_cast<void>(v)
template<class F>struct Exit{F f;~Exit(){f();}};
struct ExitBuilder{};template<class F>Exit<F>operator+(ExitBuilder,F f){return {f};}
#define JOIN_(a,b) a##b
#define JOIN(a,b) JOIN_(a,b)
#define ON_SCOPE_EXIT auto JOIN(exit_,__LINE__)=ExitBuilder{}+[&]()
#include <stratosphere/usb/usb_sleep_core.hpp>
#include <stratosphere/usb/usb_resume_marker.hpp>
#include <stratosphere/usb/usb_sleep_diagnostics.hpp>
void randomGet(void *out,size_t size){static u8 challenge;std::memset(out,++challenge,size);}
using namespace ams;using namespace ams::usb;
#include "actual_io_fault_policy.hpp"
namespace runtime_diagnostics{std::atomic<u32> disconnect_reason{};}
namespace io_diagnostics{std::atomic<u32> reconnect_requests{};}
namespace host_write{void Invalidate(){}}
namespace write_transfer{void Invalidate(){}}
namespace completion_probe{void Invalidate(){}}
enum {NexusStartupIo_LockWait, NexusStartupIo_Metadata};
namespace startup_io_diagnostics {
struct PhaseScope { explicit PhaseScope(int){} void Finish(){} };
}
namespace pause_diagnostics {
enum class Stage {VolumeWriteWait};
struct Scope {Scope(Stage,size_t){} void Finish(Result){}};
}
constexpr unsigned MaxInterfaceCount=8;
namespace host {
struct Interface {
    struct{int ID=10,deviceID_2=21;
        struct{u8 bInterfaceClass=8,bInterfaceSubClass=6,bInterfaceProtocol=0x50,bInterfaceNumber=0,bAlternateSetting=0;}interface_desc;
        u8 input_endpoint_descs[8]{2};u8 output_endpoint_descs[8]{3};
        u8 input_ss_endpoint_companion_descs[4]{},output_ss_endpoint_companion_descs[4]{};}inf;
    int deviceID=20,busID=2;u64 timestamp=30;u8 config_desc[4]{};
    struct Descriptor{std::uint16_t idVendor=0x1234,idProduct=0x7654,bcdUSB=0x300;u8 iSerialNumber{},bMaxPacketSize0=9;}device_desc;
};
struct InterfaceFilter{u32 Flags{};u8 bInterfaceClass{};};
}
namespace mass_storage {constexpr u8 InterfaceClass=8;}
namespace volume{
struct Partition{u64 first_lba=64,sector_count=1000;u8 style{},type{};bool bootable{};char name[37]{};};
struct FileInfo{u64 size{};bool directory{};};constexpr size_t MaxPartitionCount=16;
}
struct NativeCondition {
    std::function<void()> on_wait,on_timed;unsigned broadcasts{},waits{},timed_waits{};
    void Broadcast(){++broadcasts;}
    void Wait(std::mutex&m){++waits;CHECK(bool(on_wait));m.unlock();on_wait();m.lock();}
    void TimedWait(std::mutex&m,TimeSpan span){++timed_waits;m.unlock();if(on_timed)on_timed();else os::now+=span.ns;m.lock();}
};
struct FakeDevice{
    bool initialized=true,dirty{},quarantined{};int interface_id=10;u64 deadline{};
    u32 sync_result{},fingerprint_result{};unsigned syncs{},fingerprints{},quarantines{};u8 fingerprint[64]{};
    std::vector<bool> clean_reads;std::string*order{};
    bool provisional{};unsigned inits{},transient_initializations{};u32 init_result{},serial_result{};
    u32 fingerprint_failure_once{};u8 serial[32]{};std::function<void()> during_init;
    struct Geometry{u32 block_size=512;u64 block_count=4096;} geometry;
    volume::Partition layout{};
    const Geometry &GetGeometry() const{return geometry;}
    void SetWakeReadOnly(bool v){provisional=v;}
    Result Finalize(){initialized=false;return {};}
    template<class Client>Result InitializeForWake(Client*,host::Interface *i,u8 lun,u64 limit){
        CHECK(lun==0&&limit>os::now&&!initialized);++inits;
        if(transient_initializations){--transient_initializations;return Result(2);}
        if(init_result)return Result(init_result);
        provisional=true;initialized=true;quarantined=false;interface_id=i->inf.ID;
        if(during_init)during_init();
        return {};
    }
    Result CaptureWakeSerial(u8 *out,u8 idx,u64 limit){CHECK(idx&&limit>os::now);
        if(serial_result)return Result(serial_result);
        std::memcpy(out,serial,32);return {};}
    bool IsInitialized()const{return initialized;}
    bool IsQuarantined()const{return quarantined;}
    int GetInterfaceId()const{return interface_id;}
    bool NeedsSystemSleepSync()const{return dirty;}
    void SetSystemPowerDeadline(u64 n){deadline=n;}
    Result SynchronizeCacheForEject(u64 n){CHECK(n==deadline&&n>os::now);++syncs;*order+='S';if(!sync_result)dirty=false;return Result(sync_result);}
    Result CaptureSleepFingerprint(u8*out,u64 lba,u64 limit,bool clean){CHECK(lba==64&&limit>os::now);++fingerprints;*order+='F';clean_reads.push_back(clean);
        if(fingerprint_failure_once){auto rc=fingerprint_failure_once;fingerprint_failure_once=0;return Result(rc);}
        if(fingerprint_result){return Result(fingerprint_result);}std::memcpy(out,fingerprint,64);return {};}
    void Quarantine(){quarantined=true;++quarantines;*order+='Q';}
};
namespace volume {
Result DiscoverPartitions(FakeDevice *device,Partition *out,size_t,size_t *count){
    CHECK(device->provisional&&!device->quarantined);*count=1;out[0]=device->layout;return {};
}
}
struct FakeVolume{
    bool initialized=true,write_during_flush{};u32 flush_result{};unsigned flushes{};FakeDevice*device{};std::string*order{};
    bool IsInitialized()const{return initialized;}
    bool allow_marker{};sleep::VolumeMarker *disk{};unsigned mounts{},marker_checks{},marker_writes{};
    u32 marker_error{},mount_error{},marker_read_error_once{};size_t disk_size=sizeof(sleep::VolumeMarker);
    Result WriteSleepMarker(const sleep::VolumeMarker &m,bool *armed){
        CHECK(!device->provisional);*armed=allow_marker;
        if(allow_marker){++marker_writes;*disk=m;device->dirty=true;*order+='M';}
        return Result(marker_error);
    }
    Result Initialize(FakeDevice *d,const volume::Partition&){
        CHECK(d->provisional&&!d->quarantined&&!initialized);++mounts;
        initialized=mount_error==0;return Result(mount_error);}
    Result Finalize(){CHECK(device->quarantined);initialized=false;return {};}
    Result GetFileInfo(const char *,volume::FileInfo*f){CHECK(initialized);f->size=disk_size;f->directory=false;return {};}
    Result ReadFile(const char *,u64,void *b,size_t size,size_t *n){
        CHECK(initialized&&size==sizeof(*disk));++marker_checks;*order+='R';
        if(marker_read_error_once){const auto error=marker_read_error_once;marker_read_error_once=0;return Result(error);}
        std::memcpy(b,disk,size);*n=size;return {};}
    Result Flush(){++flushes;*order+='L';if(write_during_flush)device->dirty=true;return Result(flush_result);}
    // A small write can stay entirely inside FatFs. The device dirty flag
    // deliberately remains false to test revocation at the filesystem boundary.
    std::function<void()> during_mutation;
    Result Mutate(){if(during_mutation)during_mutation();return {};}
    Result CreateFile(const char*,u64){return Mutate();}
    Result ResizeFile(const char*,u64){return Mutate();}
    Result DeleteFile(const char*){return Mutate();}
    Result RenameFile(const char*,const char*){return Mutate();}
    Result WriteFile(const char*,u64,const void*,size_t size,size_t *n){*n=size;return Mutate();}
    Result CreateDirectory(const char*){return Mutate();}
};
struct FakeClient{
    bool initialized=true;u32 result{};s32 count=1;host::Interface interface{};unsigned queries{},absent_before_ready{};
    bool IsInitialized()const{return initialized;}
    unsigned transient_inits{},transient_queries{};s32 available_count=1;u32 init_result{},available_result{};
    host::Interface available{};
    Result Initialize(){if(transient_inits){--transient_inits;return Result(1);}
        initialized=init_result==0;return Result(init_result);}
    Result Finalize(){initialized=false;return {};}
    Result QueryAvailableInterfaces(host::InterfaceFilter *filter,host::Interface*out,size_t bytes,s32*n){
        CHECK(initialized&&filter->bInterfaceClass==8&&bytes==sizeof(*out)*MaxInterfaceCount);
        if(transient_queries){--transient_queries;return Result(98);}
        *n=available_count;
        for(s32 i=0;i<std::min(available_count,s32(MaxInterfaceCount));++i){out[i]=available;out[i].inf.ID+=i;}
        return Result(available_result);
    }
    Result QueryAcquiredInterfaces(host::Interface*out,std::size_t bytes,s32*n){CHECK(bytes==sizeof(host::Interface)*MaxInterfaceCount);++queries;
        *n=absent_before_ready&&queries<=absent_before_ready?0:count;if(*n>0&&*n<=s32(MaxInterfaceCount))out[0]=interface;return Result(result);}
};
enum class State{Stopped,Ready,Error};enum class FileSystemType{Unknown,ExFat};
struct Status{State state=State::Ready;u64 partition_first_lba=64,media_generation=7,block_count=4096;
    u32 block_size=512;u8 lun{},partition_index{},partition_count=1;s32 interface_id=10;
    u32 last_result{},last_io_result{};bool writable=true;};
struct Service{
    bool m_checkpoint_pending{},m_checkpoint_valid{},m_read_only{},m_skip_checkpoint_on_next_mount{};
    struct {void NewGeneration(u64){}}m_eject;
    mutable std::mutex m_power_mutex,m_volume_mutex,m_mutex;NativeCondition m_power_cv;sleep::Gate m_power_gate;
    std::string order;FakeDevice m_device;FakeVolume m_writable_volume,m_fat32_volume,m_exfat_volume;FakeClient m_client;
    volume::Partition m_partitions[volume::MaxPartitionCount]{};
    sleep::VolumeMarker m_sleep_marker{},disk_marker{};u8 m_sleep_serial[32]{};
    bool m_sleep_has_serial{},m_sleep_marker_armed{},m_resume_in_progress{},m_stop_requested{},m_running=true;
    host::Interface m_available_interfaces[MaxInterfaceCount]{};
    host::Interface m_acquired_interfaces[MaxInterfaceCount]{},m_sleep_interface{};
    bool m_sleep_had_media{},m_reconnect_requested{},m_writable_ready=true;u64 m_sleep_generation{},m_sleep_partition_lba{},generation=7;
    u8 m_sleep_fingerprint[64]{};FileSystemType m_filesystem_type=FileSystemType::ExFat;Status status;
    u32 generation_result{},status_result{},last_io{};unsigned generation_changes{},reconnects{};
    Status &m_status=status;u64 &m_media_generation=generation;
    Service(){m_device.order=&order;
        for(auto *v:{&m_writable_volume,&m_fat32_volume,&m_exfat_volume}){v->device=&m_device;v->order=&order;v->disk=&disk_marker;}
        m_fat32_volume.initialized=false;}
    bool ShouldStop()const{return m_stop_requested;}
    Result StartGuards(){
#include "actual_start_guards.hpp"
        return {};
    }
    void StopGuards(){
#include "actual_stop_guards.hpp"
    }
    Result GetMediaGeneration(u64*out){*out=generation;return Result(generation_result);}
    Result GetStatus(Status*out){*out=status;return Result(status_result);}
    Result CheckMediaGenerationLocked(u64 expected){return expected==generation?ResultSuccess():Result(ResultMediaChanged());}
    void CompleteIoLocked(Result result){
#include "actual_service_CompleteIoLocked.hpp"
    }
    void OnMountSelected(){
#include "actual_mount_checkpoint_state.hpp"
    }
    Result CreateFileAtGeneration(const char *path,u64 size,u64 media_generation){
#include "actual_service_CreateFileAtGeneration.hpp"
    }
    Result ResizeFileAtGeneration(const char *path,u64 size,u64 media_generation){
#include "actual_service_ResizeFileAtGeneration.hpp"
    }
    Result DeleteFileAtGeneration(const char *path,u64 media_generation){
#include "actual_service_DeleteFileAtGeneration.hpp"
    }
    Result RenameFileAtGeneration(const char *old_path,const char *new_path,u64 media_generation){
#include "actual_service_RenameFileAtGeneration.hpp"
    }
    Result WriteFileAtGeneration(const char *path,u64 offset,const void *buffer,size_t size,size_t *out_written,u64 media_generation){
#include "actual_service_WriteFileAtGeneration.hpp"
    }
    Result CreateDirectoryAtGeneration(const char *path,u64 media_generation){
#include "actual_service_CreateDirectoryAtGeneration.hpp"
    }
    void AdvanceMediaGeneration(){++generation;++generation_changes;order+='G';}
    void SetState(State s,Result r){status.state=s;last_io=r.value;}
    void SetLastIoResult(Result r){last_io=r.value;}
    void RequestReconnect(){m_reconnect_requested=true;++reconnects;}
    Result BeginSystemOperation(){
#include "actual_service_BeginSystemOperation.hpp"
    }
    void EndSystemOperation(){
#include "actual_service_EndSystemOperation.hpp"
    }
    bool IsSystemPowerPaused()const{
#include "actual_service_IsSystemPowerPaused.hpp"
    }
    bool IsSystemPowerQuiescent()const{
#include "actual_service_IsSystemPowerQuiescent.hpp"
    }
    void FailSystemPower(Result result){
#include "actual_service_FailSystemPower.hpp"
    }
    Result CaptureRetainedInterface(host::Interface*out,u64 deadline=0){
#include "actual_service_CaptureRetainedInterface.hpp"
    }
    Result CaptureSleepWitness(const Status &status,bool preparing){
#include "actual_service_CaptureSleepWitness.hpp"
    }
    void TryArmSleepCheckpoint(){
#include "actual_service_TryArmSleepCheckpoint.hpp"
    }
    Result PrepareSystemSleep(){
#include "actual_service_PrepareSystemSleep.hpp"
    }
    Result VerifySleepMarker(){
#include "actual_service_VerifySleepMarker.hpp"
    }
    Result RebindAfterSleep(const Status &saved,u64 deadline){
#include "actual_service_RebindAfterSleep.hpp"
    }
    Result ReserveSystemWake(){
#include "actual_service_ReserveSystemWake.hpp"
    }
    void InvalidateFailedWake(Result result){
#include "actual_service_InvalidateFailedWake.hpp"
    }
    Result ResumeSystemSleep(){
#include "actual_service_ResumeSystemSleep.hpp"
    }
};
static void CheckFailedWake(Service&s,u32 expected){CHECK(s.ResumeSystemSleep().value==expected);CHECK(s.IsSystemPowerPaused()&&s.IsSystemPowerQuiescent());
    CHECK(s.m_power_gate.GetPhase()==sleep::Phase::Failed&&s.m_device.quarantined&&s.generation_changes==1&&s.reconnects==1);
    CHECK(s.status.state==State::Error&&!s.m_writable_ready&&s.m_filesystem_type==FileSystemType::Unknown&&s.last_io==expected);
    CHECK(s.BeginSystemOperation().value==1);}
static void NominalAndNoMedia(){
    for(bool dirty:{false,true}){Service s;s.m_device.dirty=dirty;CHECK(R_SUCCEEDED(s.PrepareSystemSleep()));
        CHECK(s.order==(dirty?"LSF":"LF")&&s.m_device.clean_reads==std::vector<bool>{false});
        CHECK(s.IsSystemPowerPaused()&&s.IsSystemPowerQuiescent()&&!s.m_device.deadline&&s.generation==7);
        CHECK(R_SUCCEEDED(s.PrepareSystemSleep())&&s.m_writable_volume.flushes==1);s.order.clear();
        CHECK(R_SUCCEEDED(s.ResumeSystemSleep()));CHECK(s.order=="FL"&&s.m_device.clean_reads.back());
        CHECK(!s.IsSystemPowerPaused()&&!s.IsSystemPowerQuiescent()&&s.generation==7&&!s.generation_changes&&!s.reconnects&&!s.m_device.quarantined);
        CHECK(R_SUCCEEDED(s.BeginSystemOperation()));s.EndSystemOperation();CHECK(R_SUCCEEDED(s.ResumeSystemSleep())&&s.m_device.fingerprints==2);
    }
    Service flush_write;flush_write.m_writable_volume.write_during_flush=true;CHECK(R_SUCCEEDED(flush_write.PrepareSystemSleep()));CHECK(flush_write.order=="LSF");
    Service no_media;no_media.m_device.initialized=false;no_media.m_client.initialized=false;no_media.m_writable_volume.initialized=false;no_media.status.state=State::Stopped;
    CHECK(R_SUCCEEDED(no_media.PrepareSystemSleep())&&no_media.order.empty()&&!no_media.m_client.queries);CHECK(R_SUCCEEDED(no_media.ResumeSystemSleep())&&no_media.generation==7);
    Service cycles;for(unsigned i=0;i<100;++i){CHECK(R_SUCCEEDED(cycles.PrepareSystemSleep())&&R_SUCCEEDED(cycles.ResumeSystemSleep()));CHECK(cycles.generation==7&&!cycles.m_device.quarantined);}
}
static void DrainAndWaiters(){
    Service s;CHECK(R_SUCCEEDED(s.BeginSystemOperation())&&R_SUCCEEDED(s.BeginSystemOperation()));
    s.m_power_cv.on_timed=[&]{CHECK(s.order.empty()&&s.m_power_gate.Active()>0);s.EndSystemOperation();};
    CHECK(R_SUCCEEDED(s.PrepareSystemSleep()));CHECK(s.m_power_cv.timed_waits==2&&s.IsSystemPowerQuiescent());
    s.m_power_cv.on_wait=[&]{CHECK(s.IsSystemPowerPaused());CHECK(R_SUCCEEDED(s.ResumeSystemSleep()));};
    CHECK(R_SUCCEEDED(s.BeginSystemOperation())&&s.m_power_cv.waits==1&&s.m_power_gate.Active()==1);s.EndSystemOperation();
    Service busy;CHECK(R_SUCCEEDED(busy.BeginSystemOperation()));const auto started=os::now;
    CHECK(busy.PrepareSystemSleep().value==2&&os::now-started==sleep::DrainBudgetNs&&!busy.IsSystemPowerQuiescent()&&busy.order.empty());
    busy.EndSystemOperation();CHECK(R_SUCCEEDED(busy.PrepareSystemSleep()));
    for(bool stop:{false,true}){Service waiter;CHECK(R_SUCCEEDED(waiter.PrepareSystemSleep()));waiter.m_power_cv.on_wait=[&]{
            if(stop){std::scoped_lock lock(waiter.m_power_mutex);waiter.m_power_gate.Stop();waiter.m_power_cv.Broadcast();}
            else waiter.FailSystemPower(Result(77));};
        CHECK(waiter.BeginSystemOperation().value==1&&waiter.m_power_cv.waits==1&&waiter.IsSystemPowerQuiescent());}
    Service failed_active;CHECK(R_SUCCEEDED(failed_active.BeginSystemOperation()));failed_active.FailSystemPower(Result(77));CHECK(!failed_active.IsSystemPowerQuiescent());
    CHECK(failed_active.BeginSystemOperation().value==1);failed_active.EndSystemOperation();CHECK(failed_active.IsSystemPowerQuiescent());
}
static void Faults(){
    for(unsigned fault=0;fault<8;++fault){Service s;if(fault==0)s.generation_result=70;if(fault==1)s.status_result=71;if(fault==2)s.status.state=State::Error;
        if(fault==3){s.m_reconnect_requested=true;}if(fault==4){s.m_client.result=98;}if(fault==5){s.m_writable_volume.flush_result=75;}
        if(fault==6){s.m_device.dirty=true;s.m_device.sync_result=76;}if(fault==7)s.m_device.fingerprint_result=77;
        CHECK(R_FAILED(s.PrepareSystemSleep()));CHECK(s.m_power_gate.GetPhase()==sleep::Phase::Failed&&s.IsSystemPowerQuiescent());
        CHECK(s.generation==7&&!s.generation_changes&&!s.m_device.deadline);CHECK(s.BeginSystemOperation().value==1);
        CHECK(s.m_device.fingerprints==(fault==7?1u:0u));}
    for(unsigned field=0;field<9;++field){Service s;CHECK(R_SUCCEEDED(s.PrepareSystemSleep()));const auto reads=s.m_device.fingerprints;
        switch(field){case 0:++s.m_client.interface.inf.ID;break;case 1:++s.m_client.interface.deviceID;break;case 2:++s.m_client.interface.busID;break;
            case 3:++s.m_client.interface.timestamp;break;case 4:++s.m_client.interface.inf.deviceID_2;break;
            case 5:++s.m_client.interface.device_desc.bMaxPacketSize0;break;case 6:++s.m_client.interface.inf.interface_desc.bAlternateSetting;break;
            case 7:++s.m_client.interface.inf.input_endpoint_descs[7];break;case 8:++s.m_client.interface.inf.output_endpoint_descs[7];break;}
        CheckFailedWake(s,3);CHECK(s.m_device.fingerprints==reads);}
    for(unsigned fault=0;fault<7;++fault){Service s;CHECK(R_SUCCEEDED(s.PrepareSystemSleep()));
        if(fault==0){++s.generation;}if(fault==1){s.m_device.initialized=false;}if(fault==2){s.m_client.initialized=false;}
        if(fault==3){s.m_client.result=98;}if(fault==4){s.m_device.fingerprint_result=78;}if(fault==5){++s.m_device.fingerprint[63];}if(fault==6){s.m_client.count=0;}
        CheckFailedWake(s,fault==3?98u:fault==4?78u:3u);}
    Service restored;CHECK(R_SUCCEEDED(restored.PrepareSystemSleep()));restored.m_client.absent_before_ready=3;
    CHECK(R_SUCCEEDED(restored.ResumeSystemSleep())&&restored.generation==7&&restored.m_client.queries==4);
    for(s32 count:{-1,9}){Service s;CHECK(R_SUCCEEDED(s.PrepareSystemSleep()));s.m_client.count=count;CheckFailedWake(s,4);CHECK(s.m_device.fingerprints==1);}
}
static void PrepareDetachable(Service &s, bool serial = true) {
    s.m_writable_volume.allow_marker = true;
    s.m_client.interface.device_desc.iSerialNumber = serial ? 1 : 0;
    CHECK(R_SUCCEEDED(s.PrepareSystemSleep()));
    CHECK(s.m_sleep_marker_armed && s.m_sleep_has_serial == serial);
    CHECK(s.order == "LMSRF" && s.m_writable_volume.marker_writes == 1);
    s.m_client.count = 0;
    s.m_client.available = s.m_client.interface;
    s.m_client.available.inf.ID = 82;
    s.m_client.available.deviceID = 36;
    ++s.m_client.available.timestamp;
    s.order.clear();
}

static void ArmCheckpoint(Service &s) {
    s.m_writable_volume.allow_marker = true;
    s.OnMountSelected();
    s.TryArmSleepCheckpoint();
    CHECK(s.m_checkpoint_valid && !s.m_checkpoint_pending && !s.m_device.dirty);
    CHECK(s.m_writable_volume.marker_writes == 1 && s.m_exfat_volume.marker_checks == 1);
    CHECK(s.m_device.syncs == 1 && s.m_device.fingerprints == 1);
    s.TryArmSleepCheckpoint(); // Healthy polls do not rewrite the marker.
    CHECK(s.m_writable_volume.marker_writes == 1);
}
static void CleanCheckpointAfterPortLoss() {
    Service s;
    ArmCheckpoint(s);
    const auto previous = s.disk_marker;
    s.m_client.count = 0; // Host had already dropped the acquired interface before prepare.
    s.m_client.available = s.m_client.interface;
    s.m_client.available.inf.ID = 81;
    const auto queries = s.m_client.queries;
    s.order.clear();
    CHECK(R_SUCCEEDED(s.PrepareSystemSleep()));
    CHECK(s.order.empty() && s.m_client.queries == queries && !s.m_checkpoint_valid);
    CHECK(sleep_diagnostics::checkpoint_used == 1 && sleep_diagnostics::prepare_stage == sleep_diagnostics::Parked);
    CHECK(s.m_sleep_marker_armed && s.m_power_gate.GetPhase() == sleep::Phase::Suspended);
    CHECK(R_SUCCEEDED(s.ResumeSystemSleep()));
    CHECK(s.generation == 7 && s.generation_changes == 0);
    CHECK(s.m_writable_ready && s.status.interface_id == 81 && s.m_device.inits == 1);
    // On the fresh session the next healthy poll captures a new challenge.
    s.m_client.count = 1;
    s.m_client.interface = s.m_client.available;
    s.TryArmSleepCheckpoint();
    CHECK(s.m_checkpoint_valid && s.m_writable_volume.marker_writes == 2);
    CHECK(std::memcmp(&previous, &s.disk_marker, sizeof(previous)) != 0);
    CHECK(R_SUCCEEDED(s.PrepareSystemSleep()));
    s.disk_marker = previous;
    CheckFailedWake(s, 3);
}
static void UnusableCheckpoints() {
    Service occupied;
    occupied.m_writable_volume.allow_marker = true;
    occupied.OnMountSelected();
    {
        std::scoped_lock lock(occupied.m_volume_mutex);
        // A game call already owns the volume. A worker poll must skip
        // prearming instead of waiting for an unbounded game read.
        std::thread poll([&]{occupied.TryArmSleepCheckpoint();});
        poll.join();
    }
    CHECK(occupied.m_checkpoint_pending && !occupied.m_checkpoint_valid);
    occupied.TryArmSleepCheckpoint();
    CHECK(occupied.m_checkpoint_valid && !occupied.m_checkpoint_pending);
    Service early_write;
    early_write.m_writable_volume.allow_marker = true;
    early_write.m_checkpoint_pending = true;
    CHECK(R_SUCCEEDED(early_write.CreateFileAtGeneration("/installed", 0, 7)));
    early_write.TryArmSleepCheckpoint();
    CHECK(!early_write.m_checkpoint_valid && !early_write.m_writable_volume.marker_writes);
    // The original live prepare path still flushes, syncs and verifies the
    // correct volume if the port is available after a filesystem mutation.
    CHECK(R_SUCCEEDED(early_write.PrepareSystemSleep()));
    CHECK(sleep_diagnostics::checkpoint_used == 0 && early_write.m_writable_volume.marker_writes == 1);
    CHECK(R_SUCCEEDED(early_write.ResumeSystemSleep()) && early_write.generation == 7);
    for(unsigned operation=0;operation<6;++operation){
        Service s;
        ArmCheckpoint(s);
        unsigned calls{};
        s.m_writable_volume.during_mutation = [&]{
            ++calls;
            CHECK(!s.m_checkpoint_valid && !s.m_checkpoint_pending);
        };
        u8 byte{};
        size_t written{};
        Result rc;
        switch(operation){
            case 0: rc=s.CreateFileAtGeneration("/a",1,7); break;
            case 1: rc=s.ResizeFileAtGeneration("/a",2,7); break;
            case 2: rc=s.DeleteFileAtGeneration("/a",7); break;
            case 3: rc=s.RenameFileAtGeneration("/a","/b",7); break;
            case 4: rc=s.WriteFileAtGeneration("/a",0,&byte,1,&written,7); break;
            default: rc=s.CreateDirectoryAtGeneration("/d",7); break;
        }
        CHECK(R_SUCCEEDED(rc) && calls==1 && !s.m_device.dirty);
        s.m_client.count=0;
        CHECK(s.PrepareSystemSleep().value==3 && !s.m_checkpoint_valid);
        CHECK(sleep_diagnostics::prepare_stage==sleep_diagnostics::CaptureInterface && !sleep_diagnostics::checkpoint_used);
        CHECK(R_FAILED(s.ReserveSystemWake()) && !s.m_device.inits);
    }
    for(unsigned condition=0;condition<5;++condition){
        Service s;
        s.OnMountSelected();
        s.m_writable_volume.allow_marker=condition!=0;
        if(condition==1)s.m_device.sync_result=ResultScsiCommandFailed().GetValue();
        if(condition==2)s.m_device.fingerprint_result=77;
        if(condition==3)s.m_client.count=0;
        if(condition==4){s.m_device.sync_result=ResultScsiCommandFailed().GetValue();s.m_device.quarantined=true;}
        s.TryArmSleepCheckpoint();
        CHECK(!s.m_checkpoint_valid && !s.m_checkpoint_pending);
        if(condition<2){
            // Foreign marker or unsupported command preserves ordinary access.
            CHECK(s.generation==7 && s.status.state==State::Ready && !s.m_device.quarantined);
            CHECK(!s.m_reconnect_requested && !s.m_skip_checkpoint_on_next_mount);
            s.m_client.count=0;
            CHECK(R_FAILED(s.PrepareSystemSleep()) && !s.m_device.inits);
        }else{
            CHECK(s.generation==8 && s.status.state==State::Error && s.m_device.quarantined);
            CHECK(s.m_reconnect_requested && s.m_skip_checkpoint_on_next_mount);
            // Worker acquires and mounts media again. Ordinary use can now
            // continue even with a consistently failing optional probe.
            s.m_writable_ready=true;
            s.OnMountSelected();
            CHECK(!s.m_checkpoint_pending && !s.m_skip_checkpoint_on_next_mount);
            s.OnMountSelected();
            CHECK(s.m_checkpoint_pending); // Skip exactly the immediate retry.
        }
    }
    for(unsigned condition=0;condition<2;++condition){
        Service s; ArmCheckpoint(s);
        if(condition==0)s.m_device.dirty=true;
        else{++s.generation;s.status.media_generation=s.generation;}
        s.m_client.count=0;
        CHECK(R_FAILED(s.PrepareSystemSleep()) && R_FAILED(s.ReserveSystemWake()));
    }
}

static void Reenumeration() {
    for (bool serial : {false, true}) {
        Service s; PrepareDetachable(s, serial);
        s.m_client.transient_inits = 2;
        s.m_client.transient_queries = 1;
        s.m_device.transient_initializations = 2;
        sm::missing = 2;
        CHECK(R_SUCCEEDED(s.ReserveSystemWake()) && s.m_device.inits == 0);
        CHECK(s.IsSystemPowerPaused() && s.m_power_gate.GetPhase() == sleep::Phase::Resuming);
        s.m_device.during_init = [&] {
            CHECK(s.m_resume_in_progress && !s.IsSystemPowerQuiescent());
            CHECK(s.status.state == State::Ready && s.status.interface_id == 10 && s.generation == 7);
            CHECK(!s.m_writable_volume.initialized && s.m_device.provisional);
        };
        s.m_power_cv.on_wait = [&] { CHECK(R_SUCCEEDED(s.ResumeSystemSleep())); };
        CHECK(R_SUCCEEDED(s.BeginSystemOperation())); s.EndSystemOperation();
        CHECK(s.status.interface_id == 82 && s.generation == 7 && !s.generation_changes);
        CHECK(s.m_writable_ready && !s.m_device.provisional && s.m_device.inits == 3);
        CHECK(s.m_exfat_volume.mounts == 1 && s.m_exfat_volume.marker_checks == 2);
        CHECK(s.m_writable_volume.mounts == 1 && s.m_writable_volume.marker_writes == 1);
    }
    for (int fault = 0; fault < 11; ++fault) {
        Service s; PrepareDetachable(s);
        if (fault == 0) ++s.disk_marker.challenge[31];             // wrong card, same reader/boot sector
        if (fault == 1) ++s.m_device.serial[31];                   // another device
        if (fault == 2) ++s.m_device.geometry.block_count;
        if (fault == 3) ++s.m_device.fingerprint[63];
        if (fault == 4) ++s.m_device.layout.sector_count;
        if (fault == 5) ++s.m_exfat_volume.disk_size;
        if (fault == 6) s.m_client.available_count = 2;            // indistinguishable candidates
        if (fault == 7) s.m_client.available_count = MaxInterfaceCount;
        if (fault == 8) s.m_client.available_count = 0;            // times out, no open
        if (fault == 9) s.m_writable_volume.mount_error = 74;      // must not clear write guard
        if (fault == 10) {s.m_client.count=1; ++s.disk_marker.challenge[0];} // card swapped behind retained bridge
        CHECK(R_FAILED(s.ResumeSystemSleep()));
        CHECK(s.m_power_gate.GetPhase() == sleep::Phase::Failed && s.generation_changes == 1);
        CHECK(s.m_device.quarantined && !s.m_writable_ready && s.m_writable_volume.marker_writes == 1);
        if (fault != 9) CHECK(s.m_writable_volume.mounts == 0);
        if (fault >= 6 && fault <= 8) CHECK(s.m_device.inits == 0);
    }
    Service stale; PrepareDetachable(stale); stale.m_client.count = 1;
    stale.m_device.fingerprint_failure_once = 98;
    CHECK(R_SUCCEEDED(stale.ResumeSystemSleep()) && stale.generation == 7 && stale.m_device.inits == 1);
    Service changed_link; PrepareDetachable(changed_link);
    changed_link.m_client.available.device_desc.bcdUSB = 0x200;
    changed_link.m_client.available.device_desc.bMaxPacketSize0 = 64;
    ++changed_link.m_client.available.inf.interface_desc.bAlternateSetting;
    ++changed_link.m_client.available.inf.input_endpoint_descs[3];
    ++changed_link.m_client.available.inf.output_ss_endpoint_companion_descs[0];
    CHECK(R_SUCCEEDED(changed_link.ResumeSystemSleep()) && changed_link.generation == 7);
    CHECK(sleep_diagnostics::rebind_visible_max == 1 && sleep_diagnostics::rebind_candidates == 1 &&
          sleep_diagnostics::rebind_exact_max == 0 && changed_link.m_exfat_volume.marker_checks == 2);
    for(unsigned mismatch = 0; mismatch < 6; ++mismatch){
        Service other; PrepareDetachable(other);
        auto &candidate = other.m_client.available;
        if(mismatch == 0) ++candidate.device_desc.idVendor;
        if(mismatch == 1) ++candidate.device_desc.idProduct;
        if(mismatch == 2) ++candidate.inf.interface_desc.bInterfaceNumber;
        if(mismatch == 3) ++candidate.inf.interface_desc.bInterfaceSubClass;
        if(mismatch == 4) candidate.inf.interface_desc.bInterfaceProtocol = 1;
        if(mismatch == 5) candidate.inf.ID = -1;
        CheckFailedWake(other, 3);
        CHECK(other.m_device.inits == 0 && other.m_writable_volume.mounts == 0);
        CHECK(sleep_diagnostics::rebind_visible_max == 1 && sleep_diagnostics::rebind_candidates == 0 &&
              sleep_diagnostics::rebind_queries > 1 && sleep_diagnostics::rebind_query_result == 0);
    }
    Service ambig; PrepareDetachable(ambig); ambig.m_client.available_count = 2;
    ++ambig.m_client.available.inf.output_endpoint_descs[3];
    CheckFailedWake(ambig, 5);
    CHECK(!ambig.m_device.inits && sleep_diagnostics::rebind_candidates == 2 && sleep_diagnostics::rebind_exact_max == 0);
    Service different_marker; PrepareDetachable(different_marker);
    ++different_marker.m_client.available.device_desc.bcdUSB; ++different_marker.disk_marker.challenge[0];
    CheckFailedWake(different_marker, 3);
    CHECK(different_marker.m_device.inits == 1 && !different_marker.m_writable_volume.mounts && different_marker.m_device.provisional);
    for(u32 error : {98u,99u,8u,9u,10u}){
        Service torn_read; PrepareDetachable(torn_read);
        torn_read.m_client.count = 1; torn_read.m_exfat_volume.marker_read_error_once = error;
        CHECK(R_SUCCEEDED(torn_read.ResumeSystemSleep()) && torn_read.generation == 7);
        CHECK(torn_read.m_device.inits == 1 && torn_read.m_exfat_volume.marker_checks == 3 && torn_read.m_writable_volume.mounts == 1);
    }
    Service missing_marker; PrepareDetachable(missing_marker);
    missing_marker.m_client.count = 1; missing_marker.m_exfat_volume.marker_read_error_once = 11;
    CheckFailedWake(missing_marker, 11);
    CHECK(!missing_marker.m_device.inits && !missing_marker.m_writable_volume.mounts);
    Service bad_query; PrepareDetachable(bad_query); bad_query.m_client.available_result = 74;
    CheckFailedWake(bad_query, 74);
    CHECK(sleep_diagnostics::rebind_queries == 1 && sleep_diagnostics::rebind_query_result == 74);
    Service cancelled; PrepareDetachable(cancelled);
    cancelled.m_device.during_init = [&] {
        std::scoped_lock lock(cancelled.m_power_mutex);
        cancelled.m_stop_requested = true; cancelled.m_power_gate.Fail();
    };
    CHECK(R_FAILED(cancelled.ResumeSystemSleep()) && cancelled.m_device.provisional);
    CHECK(cancelled.status.interface_id == 10 && cancelled.m_writable_volume.marker_writes == 1);
    Service dirty; dirty.m_writable_volume.allow_marker = true; dirty.m_device.sync_result = 77;
    CHECK(R_FAILED(dirty.PrepareSystemSleep()) && R_FAILED(dirty.ReserveSystemWake()));
    CHECK(R_FAILED(dirty.ResumeSystemSleep()) && dirty.m_device.inits == 0);
    Service stop_fail; stop_fail.FailSystemPower(Result(77));stop_fail.StopGuards();stop_fail.m_running=false;
    CHECK(R_FAILED(stop_fail.StartGuards()) && stop_fail.m_power_gate.GetPhase()==sleep::Phase::Failed);
    Service stopped;stopped.m_running=false;stopped.m_device.initialized=false;stopped.m_writable_volume.initialized=false;
    stopped.StopGuards();CHECK(stopped.m_power_gate.GetPhase()==sleep::Phase::Stopped);
    CHECK(R_SUCCEEDED(stopped.PrepareSystemSleep()) && stopped.IsSystemPowerQuiescent());
    CHECK(R_FAILED(stopped.StartGuards()));
    CHECK(R_SUCCEEDED(stopped.ReserveSystemWake())&&R_SUCCEEDED(stopped.ResumeSystemSleep())&&R_SUCCEEDED(stopped.StartGuards()));
    for(unsigned window=0;window<4;++window){
        Service interrupted;
        CHECK(interrupted.m_power_gate.BeginSleep());
        if(window>=1)CHECK(interrupted.m_power_gate.Park());
        if(window>=2)CHECK(R_SUCCEEDED(interrupted.ReserveSystemWake()));
        if(window==3){
            interrupted.m_resume_in_progress=true;
            interrupted.m_power_cv.on_wait=[&]{
                CHECK(interrupted.m_power_gate.GetPhase()==sleep::Phase::Failed);
                interrupted.m_resume_in_progress=false;
            };
        }
        interrupted.StopGuards();interrupted.m_running=false;
        CHECK(interrupted.m_power_cv.waits==(window==3?1u:0u));
        CHECK(interrupted.m_power_gate.GetPhase()==sleep::Phase::Failed);
        CHECK(R_FAILED(interrupted.StartGuards())&&R_FAILED(interrupted.ResumeSystemSleep()));
    }
}

int main(){NominalAndNoMedia();DrainAndWaiters();Faults();Reenumeration();CleanCheckpointAfterPortLoss();UnusableCheckpoints();std::printf("PASS %u actual storage sleep-service lifecycle and fault checks\n",checks);return 0;}
