// SPDX-License-Identifier: GPL-2.0-or-later
// Actual production ExecuteCommand body; native transfers and clock are fixtures.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
using u8=std::uint8_t; using u16=std::uint16_t; using u32=std::uint32_t; using u64=std::uint64_t;
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);} } while(0)
namespace ams {
struct Result {u32 value{};constexpr Result(u32 v=0):value(v){} constexpr u32 GetValue()const{return value;}};
constexpr Result ResultSuccess(){return {};}
struct TimeSpan {u64 ns;static TimeSpan FromMilliSeconds(u64 n){return {n*1000000};}};
namespace os {inline u64 now=1000000;void SleepThread(TimeSpan t){now+=t.ns;}}
namespace usb {
#define ERROR(n,v) struct n:Result{constexpr n():Result(v){}static bool Includes(Result r){return r.value==v;}};
ERROR(ResultNotInitialized,1) ERROR(ResultInvalidParameter,2) ERROR(ResultAlignmentError,3)
ERROR(ResultScsiCommandFailed,4) ERROR(ResultTransactionError,5) ERROR(ResultOperationDenied,6)
ERROR(ResultInternalStateError,7)
ERROR(ResultResourceBusy,8) ERROR(ResultMediaChanged,9)
#undef ERROR
constexpr bool IsDmaAligned(u64 p){return p%4096==0;}
namespace impl {u64 UsbNowNs(){return os::now;}constexpr u64 SenseBudgetNs=3000000000ULL;}
namespace pause_diagnostics {enum class Stage{Command};struct Scope{template<class...T>Scope(T...){}void Finish(Result){}};}
namespace completion_diagnostics {struct CommandScope{template<class...T>CommandScope(T...){}void Finish(u32){}};}
namespace io_diagnostics {inline std::atomic<u64> last_command_ns{};}
namespace runtime_diagnostics {inline std::atomic<u64> ready_recovered{},ready_retries{};inline std::atomic<u32> last_sense_action{};}
namespace sleep_diagnostics {inline std::atomic<u64> reset_attentions{},ready_retries{};}
}}
#define R_SUCCEEDED(r) ((r).GetValue()==0)
#define R_FAILED(r) (!R_SUCCEEDED(r))
#define R_RETURN(r) return (r)
#define R_THROW(r) return (r)
#define R_SUCCEED() return ams::ResultSuccess()
#define R_UNLESS(c,r) do{if(!(c))return (r);}while(0)
#define R_TRY(e) do{const ams::Result rc_=(e);if(R_FAILED(rc_))return rc_;}while(0)
#include "usb_scsi_recovery.hpp"
#include <stratosphere/usb/usb_sleep_core.hpp>
#include <stratosphere/usb/usb_wake_identity.hpp>
using namespace ams;using namespace ams::usb;
constexpr u8 ScsiRequestSense=3;
constexpr u8 ScsiReadCapacity10=0x25,ScsiServiceActionIn=0x9e,ScsiReadCapacity16=0x10,ScsiRead10=0x28,ScsiRead16=0x88;
constexpr std::size_t MaxCommandBlockSize=16;
constexpr u32 MaxSenseDataSize=18;
constexpr u32 NEXUS_USB_COMPLETION_SOURCE_COMMAND=0;
constexpr u32 NEXUS_USB_COMPLETION_SOURCE_SYNC=2;
enum class Transport{Bot,Uas};enum class DataDirection{None,In,Out};
static bool IsValidBlockSize(u32 n){return n>=512&&n<=4096&&(n&(n-1))==0;}
static u32 LoadBigEndian32(const u8*p){return (u32(p[0])<<24)|(u32(p[1])<<16)|(u32(p[2])<<8)|p[3];}
static u64 LoadBigEndian64(const u8*p){return (u64(LoadBigEndian32(p))<<32)|LoadBigEndian32(p+4);}
static void StoreBigEndian32(u8*p,u32 n){for(unsigned i=0;i<4;++i)p[i]=u8(n>>(24-i*8));}
static void StoreBigEndian64(u8*p,u64 n){StoreBigEndian32(p,u32(n>>32));StoreBigEndian32(p+4,u32(n));}
static void StoreBigEndian16(u8*p,u32 n){p[0]=u8(n>>8);p[1]=u8(n);}
namespace crypto {
// Native SHA is outside this fixture. Byte-sensitive stand-in checks exactly
// what data is fingerprinted, without duplicating or testing the SHA algorithm.
inline unsigned calls;
void GenerateSha256(void*out,std::size_t n,const void*input,std::size_t bytes){CHECK(n==32);++calls;auto*p=static_cast<u8*>(out);std::memset(p,0,n);
    const auto*b=static_cast<const u8*>(input);for(std::size_t i=0;i<bytes;++i)p[i%32]=u8((p[i%32]*33)^b[i]^u8(i));}
}
struct Step {u8 opcode;u32 result;u8 key{},asc{},ascq{};u8 format=0x70;u32 size=18;u32 actual=UINT32_MAX;u8 fill{};std::vector<u8> payload{};};
struct Device {
    bool m_is_initialized=true,m_keep_alive_failed=false,m_unsynchronized_writes=false,m_wake_read_only=false;
    u64 m_system_power_deadline{};
    Transport m_transport=Transport::Bot;
    struct Geometry {u64 block_count=1024;u32 block_size=512;}m_geometry;
    alignas(4096)u8 m_data_buffer[4096]{};
    alignas(4096)u8 m_status_buffer[4096]{};
    struct DescriptorReply {u32 result; std::vector<u8> bytes;};
    struct ControlInterface {
        bool live = true;
        std::vector<DescriptorReply> script;
        std::vector<u16> requests, languages;
        bool IsInitialized() const { return live; }
        Result ControlTransfer(u8 type, u8 request, u16 value, u16 language, u16 length,
                               u8 *buffer, u32 *received, u64 deadline) {
            CHECK(type==0x80 && request==6 && length==255 && os::now < deadline);
            CHECK(requests.size()<script.size());
            const auto &reply=script[requests.size()]; requests.push_back(value); languages.push_back(language);
            os::now+=1'000'000; *received=static_cast<u32>(reply.bytes.size());
            CHECK(*received<=length);
            if (*received) std::memcpy(buffer,reply.bytes.data(),*received);
            return reply.result;
        }
    } m_interface;
    std::vector<Step> script;std::size_t cursor{};unsigned resets{},quarantines{},records{};
    u32 reset_result{},last_result{};std::vector<u64> deadlines;std::vector<u8> commands;std::vector<std::vector<u8>> cdbs;
    void Quarantine(){m_keep_alive_failed=true;++quarantines;}
    Result ResetTransport(){++resets;return Result(reset_result);}
    void RecordCommandResult(u8,Result r){++records;last_result=r.value;}
    Result NativeReply(const u8*command,std::size_t command_size,DataDirection direction,void*data,u32 bytes,u32*out,u64 deadline){
        CHECK(cursor<script.size());auto s=script[cursor++];CHECK(s.opcode==command[0]);commands.push_back(command[0]);deadlines.push_back(deadline);
        cdbs.emplace_back(command,command+command_size);
        os::now+=1000000;*out=bytes;
        if(s.opcode==3){CHECK(direction==DataDirection::In&&bytes==18);auto*p=static_cast<u8*>(data);std::memset(p,0,18);p[0]=s.format;
            if((s.format&0x7f)==0x70){p[2]=s.key;p[7]=10;p[12]=s.asc;p[13]=s.ascq;}else{p[1]=s.key;p[2]=s.asc;p[3]=s.ascq;}
            *out=s.size;
        }else if(direction==DataDirection::In){std::memset(data,s.fill,bytes);if(!s.payload.empty()){CHECK(s.payload.size()<=bytes);std::memcpy(data,s.payload.data(),s.payload.size());}}
        if(s.actual!=UINT32_MAX)*out=s.actual;
        return Result(s.result);
    }
    Result ExecuteCommandOnce(const u8*command,std::size_t command_size,DataDirection direction,void*b,u32 z,u32*out,u64 deadline_ns=0){
#include "actual_bot_submit_gate.hpp"
        return NativeReply(command,command_size,direction,b,z,out,deadline_ns);
    }
    Result ExecuteCommandUasOnce(const u8*command,std::size_t command_size,DataDirection direction,void*b,u32 z,u32*out,u64 deadline_ns=0,u32 *wake_sense_size=nullptr){
#include "actual_uas_submit_gate.hpp"
        const auto step=script.at(cursor);
        auto rc=NativeReply(command,command_size,direction,b,z,out,deadline_ns);
        if (wake_sense_size && rc.GetValue()==4 && !step.payload.empty()) {
            *wake_sense_size=static_cast<u32>(step.payload.size());
            std::memcpy(m_status_buffer+16,step.payload.data(),step.payload.size());
        }
        return rc;
    }
    Result ExecuteCommand(const u8*command,std::size_t command_size,DataDirection direction,void*data,u32 data_size,u32*out_transferred_size){
#include "actual_execute_command.hpp"
    }
    Result ExecuteSleepRead(const u8*command,std::size_t command_size,u32 data_size,u64 deadline,sleep::ReadRetry&retry){
#include "actual_execute_sleep_read.hpp"
    }
    Result CaptureSleepFingerprint(u8*out_hashes,u64 partition_lba,u64 deadline,bool clean_resume){
#include "actual_capture_sleep_fingerprint.hpp"
    }
    Result ReadWakeCapacity(Geometry *out,u64 deadline,sleep::ReadRetry &retry){
#include "actual_read_wake_capacity.hpp"
    }
    Result CaptureWakeSerial(u8 out_digest[32],u8 descriptor_index,u64 deadline){
#include "actual_capture_wake_serial.hpp"
    }
    Result SynchronizeCacheForEject(u64 deadline_ns=0){
#include "actual_sync_cache.hpp"
    }
};
alignas(4096) static u8 payload[4096];
static Result Run(Device&d,u8 opcode=0x28,DataDirection direction=DataDirection::In){u8 c[16]{};c[0]=opcode;u32 n{};return d.ExecuteCommand(c,10,direction,payload,sizeof(payload),&n);}
static void OrdinaryPolicies(){
    for(u8 opcode:{u8(0x2a),u8(0x8a),u8(0x1b)})for(u32 error:{77u,78u}){
        Device d;d.script={{opcode,error}};CHECK(Run(d,opcode,opcode==0x1b?DataDirection::None:DataDirection::Out).value==error);
        CHECK(d.cursor==1&&d.resets==0&&d.m_keep_alive_failed&&d.records==1);
    }
    for(u8 opcode:{u8(0x2a),u8(0x8a)})for(auto sense:std::vector<Step>{{3,0,2,4,1},{3,0,6,0x29,0},{3,0,6,0x28,0}}){
        Device d;d.script={{opcode,4},sense};CHECK(Run(d,opcode,DataDirection::Out).value==5);CHECK(d.cursor==2&&!d.resets&&d.m_keep_alive_failed);
    }
    for(auto sense:std::vector<Step>{{3,0,6,0x29,0},{3,0,6,0x28,0},{3,0,3,0x11,0},{3,0,2,4,1,0x71}}){
        Device d;d.script={{0x28,4},sense};CHECK(Run(d).value==5);CHECK(d.cursor==2&&!d.resets&&d.m_keep_alive_failed);
    }
    for(u8 key:{u8(5),u8(7)}){Device d;d.script={{0x28,4},{3,0,key,0x20,0}};CHECK(Run(d).value==6);CHECK(!d.m_keep_alive_failed&&!d.resets&&d.cursor==2);}
    Device recover;recover.script={{0x28,77},{0x28,0}};CHECK(R_SUCCEEDED(Run(recover)));CHECK(recover.cursor==2&&recover.resets==1&&!recover.m_keep_alive_failed);
    Device twice;twice.script={{0x28,77},{0x28,78}};CHECK(Run(twice).value==78);CHECK(twice.cursor==2&&twice.resets==1&&twice.m_keep_alive_failed);
    Device reset_fail;reset_fail.reset_result=99;reset_fail.script={{0x28,77}};CHECK(Run(reset_fail).value==77);CHECK(reset_fail.resets==1&&reset_fail.m_keep_alive_failed);
    Device draining;draining.m_system_power_deadline=os::now+5000000000ULL;draining.script={{0x28,77}};
    CHECK(Run(draining).value==77);CHECK(!draining.resets&&draining.cursor==1&&draining.m_keep_alive_failed);
    Device probe;probe.m_geometry.block_count=0;probe.script={{0x28,4},{3,0,6,0x29,0},{0x28,0}};CHECK(R_SUCCEEDED(Run(probe)));CHECK(probe.cursor==3&&!probe.resets&&!probe.m_keep_alive_failed);
    Device sense_fail;sense_fail.script={{0x28,4},{3,88}};CHECK(Run(sense_fail).value==5);CHECK(sense_fail.m_keep_alive_failed&&sense_fail.cursor==2);
    Device short_sense;short_sense.script={{0x28,4},{3,0,2,4,1,0x70,13}};CHECK(Run(short_sense).value==5);CHECK(short_sense.m_keep_alive_failed);
    Device ready;ready.script={{0x28,4},{3,0,2,4,1},{0x28,0}};CHECK(R_SUCCEEDED(Run(ready)));CHECK(ready.cursor==3&&ready.deadlines.back()>0&&!ready.resets);
    Device not_ready;for(unsigned i=0;i<21;++i){not_ready.script.push_back({0x28,4});not_ready.script.push_back({3,0,2,4,1});}
    const auto begin=os::now;CHECK(Run(not_ready).value==5);CHECK(not_ready.m_keep_alive_failed&&!not_ready.resets&&not_ready.cursor<=42);CHECK(os::now-begin<=5100000000ULL);
    for(auto direction:{DataDirection::In,DataDirection::Out}){Device uas;uas.m_transport=Transport::Uas;const u8 opcode=direction==DataDirection::In?0x28:0x2a;uas.script={{opcode,77}};
        CHECK(Run(uas,opcode,direction).value==77);CHECK(uas.quarantines==1&&!uas.resets&&uas.cursor==1);}
    Device poisoned;poisoned.m_keep_alive_failed=true;CHECK(Run(poisoned).value==1);CHECK(!poisoned.cursor&&!poisoned.records);
}
static Step Capacity(u64 count=1024,u32 size=512,bool wide=false){Step step{wide?u8(0x9e):u8(0x25),0};step.payload.resize(wide?32:8);
    if(wide){StoreBigEndian64(step.payload.data(),count-1);StoreBigEndian32(step.payload.data()+8,size);}
    else {StoreBigEndian32(step.payload.data(),count>UINT32_MAX?UINT32_MAX:u32(count-1));StoreBigEndian32(step.payload.data()+4,size);}return step;}
static Result Capture(Device&d,bool clean=true,u64 partition=64,u64 budget=5000000000ULL){u8 hashes[64]{};return d.CaptureSleepFingerprint(hashes,partition,sleep::Deadline(os::now,budget),clean);}
static void ExplicitResume(){
    for(bool clean:{false,true}){Device d;d.script={{0x25,4},{3,0,6,0x29,0},Capacity(),{0x28,0},{0x28,0}};
        CHECK(Capture(d,clean).value==(clean?0u:4u));CHECK(d.cursor==(clean?5u:2u)&&!d.resets);}
    for(auto failure:std::vector<Step>{{3,0,6,0x28,0},{3,0,6,0x29,5},{3,0,6,0x29,0,0x71},{3,0,3,0x11,0},{3,0,6,0x29,0,0x70,13}}){
        Device d;d.script={{0x25,4},failure};CHECK(Capture(d).value==4&&d.cursor==2&&!d.resets);}
    Device second_reset;second_reset.script={{0x25,4},{3,0,6,0x29,0},Capacity(),{0x28,4},{3,0,6,0x29,2}};
    CHECK(Capture(second_reset).value==4&&second_reset.cursor==5&&!second_reset.resets);
    Device ready;ready.script={{0x25,4},{3,0,2,4,1},Capacity(),{0x28,0},{0x28,0}};CHECK(R_SUCCEEDED(Capture(ready)));CHECK(ready.cursor==5&&!ready.resets);
    Device timeout;for(unsigned i=0;i<21;++i){timeout.script.push_back({0x25,4});timeout.script.push_back({3,0,2,4,1});}
    const auto begin=os::now;CHECK(R_FAILED(Capture(timeout)));CHECK(timeout.cursor<=42&&!timeout.resets&&os::now-begin<5000000000ULL);
    Device expired;CHECK(Capture(expired,true,64,0).value==8&&!expired.cursor);
    for(unsigned position=0;position<3;++position){Device d;d.script={Capacity(),{0x28,0},{0x28,0}};d.script[position].result=77;
        CHECK(Capture(d).value==77&&d.cursor==position+1&&!d.resets);}
    for(unsigned position=0;position<3;++position){Device d;d.script={Capacity(),{0x28,0},{0x28,0}};d.script[position].actual=position?511:7;
        CHECK(Capture(d).value==5&&d.cursor==position+1&&!d.resets);}
    for(auto geometry:std::vector<Step>{Capacity(1025),Capacity(1024,4096)}){Device d;d.script={geometry};CHECK(Capture(d).value==9&&d.cursor==1);}
    Device wide;wide.m_geometry.block_count=UINT64_C(0x100001001);wide.script={Capacity(wide.m_geometry.block_count),Capacity(wide.m_geometry.block_count,512,true),{0x28,0},{0x88,0}};
    CHECK(R_SUCCEEDED(Capture(wide,true,UINT64_C(0x100000010))));CHECK(wide.cursor==4&&wide.cdbs[1][1]==0x10&&wide.cdbs[1][13]==32);
    CHECK(wide.cdbs[2].size()==10&&LoadBigEndian32(wide.cdbs[2].data()+2)==0&&wide.cdbs[2][8]==1);
    CHECK(wide.cdbs[3].size()==16&&LoadBigEndian64(wide.cdbs[3].data()+2)==UINT64_C(0x100000010)&&LoadBigEndian32(wide.cdbs[3].data()+10)==1);
    for(auto&deadline:wide.deadlines)CHECK(deadline==wide.deadlines.front());
    for(auto transport:{Transport::Bot,Transport::Uas}){Device d;d.m_transport=transport;d.script={Capacity(),{0x28,0},{0x28,0}};
        u8 first[64]{},second[64]{};CHECK(R_SUCCEEDED(d.CaptureSleepFingerprint(first,0,os::now+5000000000ULL,true)));CHECK(d.cursor==3);
        d.script.insert(d.script.end(),{Capacity(),{0x28,0},{0x28,0}});d.script.back().payload.resize(512);d.script.back().payload[317]=1;
        CHECK(R_SUCCEEDED(d.CaptureSleepFingerprint(second,0,os::now+5000000000ULL,true)));CHECK(std::memcmp(first,second,32)==0);CHECK(std::memcmp(first+32,second+32,32)!=0);}
    Device uas;uas.m_transport=Transport::Uas;uas.script={{0x25,4}};CHECK(Capture(uas).value==4&&uas.cursor==1&&!uas.resets);
    Device bad;bad.m_geometry.block_size=513;CHECK(Capture(bad).value==2&&!bad.cursor);
    Device partition;CHECK(Capture(partition,true,1024).value==2&&!partition.cursor);
    Device poisoned;poisoned.m_keep_alive_failed=true;CHECK(Capture(poisoned).value==1&&!poisoned.cursor);
    Device no_write;sleep::ReadRetry retry{true};u8 command[10]{0x2a};CHECK(no_write.ExecuteSleepRead(command,10,512,os::now+5000,retry).value==2&&!no_write.cursor);
}
static void DirtyWriteTracking(){
    for(auto transport:{Transport::Bot,Transport::Uas}){
        for(u32 failure:{0u,77u}){Device d;d.m_transport=transport;d.script={{0x2a,failure}};
            CHECK(Run(d,0x2a,DataDirection::Out).value==failure&&d.m_unsynchronized_writes&&d.cursor==1);}
        Device expired;expired.m_transport=transport;expired.m_system_power_deadline=os::now;
        CHECK(Run(expired,0x2a,DataDirection::Out).value==8&&!expired.cursor&&!expired.m_unsynchronized_writes);
        for(unsigned failure=0;failure<3;++failure){Device d;d.m_transport=transport;d.m_unsynchronized_writes=true;
            d.script={{0x35,failure==1?77u:0u}};if(failure==2)d.script[0].actual=1;
            const auto deadline=os::now+3000000000ULL;CHECK(d.SynchronizeCacheForEject(deadline).value==(failure==1?77u:failure==2?5u:0u));
            CHECK(d.m_unsynchronized_writes==(failure!=0)&&d.cursor==1&&!d.resets&&d.deadlines[0]==deadline);
        }
        Device eject_clean;eject_clean.m_transport=transport;eject_clean.script={{0x35,0}};
        CHECK(R_SUCCEEDED(eject_clean.SynchronizeCacheForEject())&&eject_clean.cursor==1&&!eject_clean.m_unsynchronized_writes);
        Device sync_expired;sync_expired.m_transport=transport;sync_expired.m_unsynchronized_writes=true;
        CHECK(sync_expired.SynchronizeCacheForEject(os::now).value==8&&sync_expired.m_unsynchronized_writes&&!sync_expired.cursor);
    }
}
static void FreshSessionChecks() {
    const u8 languages[]{4,3,9,4};
    const u8 serial[]{12,3,'N',0,'x',0,'2',0,'4',0,'3',0};
    const u8 filler[]{10,3,'0',0,'0',0,'0',0,'0',0};
    CHECK(sleep::SerialLanguage(languages, sizeof(languages)) == 0x409);
    CHECK(sleep::HasUsefulSerial(serial, sizeof(serial)));
    CHECK(!sleep::HasUsefulSerial(filler, sizeof(filler)));
    for (std::size_t n=0; n<sizeof(serial); ++n) CHECK(!sleep::HasUsefulSerial(serial,n));
    Device serial_device;
    serial_device.m_interface.script={{0,{4,3,9,4}},{0,std::vector<u8>(serial,serial+sizeof(serial))}};
    u8 observed_digest[32]{}, expected_digest[32]{};
    crypto::GenerateSha256(expected_digest,32,serial+2,sizeof(serial)-2);
    CHECK(R_SUCCEEDED(serial_device.CaptureWakeSerial(observed_digest,7,os::now+5'000'000'000ULL)));
    CHECK(std::memcmp(observed_digest,expected_digest,32)==0);
    CHECK((serial_device.m_interface.requests==std::vector<u16>{0x300,0x307}));
    CHECK((serial_device.m_interface.languages==std::vector<u16>{0,0x409}));
    Device stall; stall.m_interface.script={{0x3248c,{}}};
    CHECK(stall.CaptureWakeSerial(observed_digest,7,os::now+5'000'000'000ULL).value==6);
    Device malformed; malformed.m_interface.script={{0,{12,3,9,4}}};
    CHECK(malformed.CaptureWakeSerial(observed_digest,7,os::now+5'000'000'000ULL).value==6);
    Device unknown; unknown.m_interface.script={{99,{}}};
    CHECK(unknown.CaptureWakeSerial(observed_digest,7,os::now+5'000'000'000ULL).value==99);
    Device expires; expires.m_interface.script={{0,{4,3,9,4}}};
    CHECK(expires.CaptureWakeSerial(observed_digest,7,os::now+500'000).value==8 && expires.m_interface.requests.size()==1);
    CHECK(sleep::WakeSenseSize(2,18,34,256)==18);
    CHECK(sleep::WakeSenseSize(2,18,33,256)==0 && sleep::WakeSenseSize(2,257,273,256)==0);
    CHECK(sleep::WakeSenseSize(2,7,23,256)==0 && sleep::WakeSenseSize(8,18,34,256)==0);
    for (auto transport : {Transport::Bot,Transport::Uas}) {
        for (u8 opcode : {u8(0x2a),u8(0x8a),u8(0x35),u8(0x1b)}) {
            Device d; d.m_wake_read_only=true; d.m_transport=transport;
            u8 command[16]{opcode}; u32 transferred=0;
            const auto direction = (opcode == 0x2a || opcode == 0x8a) ? DataDirection::Out : DataDirection::None;
            const auto rc=transport==Transport::Bot ? d.ExecuteCommandOnce(command,10,direction,payload,0,&transferred)
                : d.ExecuteCommandUasOnce(command,10,direction,payload,0,&transferred);
            CHECK(rc.value==6 && d.cursor==0 && !d.m_unsynchronized_writes);
        }
    }
    Device ready; ready.script={{0x25,4},{3,0,6,0x28,0},Capacity(8888,4096)};
    sleep::ReadRetry fresh{true}; fresh.fresh_session=true;
    Device::Geometry geometry{};
    CHECK(R_SUCCEEDED(ready.ReadWakeCapacity(&geometry,os::now+5000000000ULL,fresh)));
    CHECK(geometry.block_count==8888 && geometry.block_size==4096 && ready.resets==0);
    Device repeated; repeated.script={{0x25,4},{3,0,6,0x28,0},{0x25,4},{3,0,6,0x28,0}};
    sleep::ReadRetry twice{true}; twice.fresh_session=true;
    CHECK(R_FAILED(repeated.ReadWakeCapacity(&geometry,os::now+5000000000ULL,twice)) && !repeated.resets);
    Device uas; uas.m_transport=Transport::Uas;
    Step unit{0x25,4}; unit.payload={0x72,6,0x29,0,0,0,0,0};
    uas.script={unit,Capacity(999)};
    sleep::ReadRetry fresh_uas{true};fresh_uas.fresh_session=true;
    CHECK(R_SUCCEEDED(uas.ReadWakeCapacity(&geometry,os::now+5000000000ULL,fresh_uas)));
    CHECK(geometry.block_count==999 && uas.cursor==2 && uas.resets==0);
}
int main(){OrdinaryPolicies();ExplicitResume();DirtyWriteTracking();FreshSessionChecks();std::printf("PASS %u ordinary transport and actual resume-read checks\n",checks);return 0;}
