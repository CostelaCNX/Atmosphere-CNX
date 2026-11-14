/* SPDX-License-Identifier: GPL-2.0-only */
#include "fssystem_patch_witness.hpp"
#include <openssl/sha.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
struct Result { u32 rc{}; constexpr Result(u32 r=0) : rc(r) {} u32 GetValue() const {return rc;} };
#define R_UNLESS(c,err) do { if (!(c)) return (err); } while (0)
#define R_FAILED(r) ((r).GetValue() != 0)
#define R_SUCCEEDED(r) (!R_FAILED(r))
#define R_RETURN(r) return (r)
struct GuardMaker {};
template<class F> struct Guard { F f; ~Guard(){f();} };
template<class F> Guard<F> operator+(GuardMaker,F f){return {std::move(f)};}
#define ON_SCOPE_EXIT auto guard = GuardMaker{} + [&]()
namespace crypto {
    void GenerateSha256(void *out, size_t n, const void *data, size_t size) {
        assert(n == 32); SHA256(static_cast<const u8 *>(data), size, static_cast<u8 *>(out));
    }
    bool IsSameBytes(const void *a, const void *b, size_t n) {return !std::memcmp(a,b,n);}
}
namespace os {
    using SdkMutex=std::mutex;
    struct ThreadType {};
    ThreadType *GetCurrentThread() {static thread_local ThreadType t; return &t;}
}
namespace fs {
    constexpr Result ResultAllocationMemoryFailedAllocateShared(){return 13;}
    constexpr Result ResultInvalidNcaMetaDataHashDataHash(){return 0x1ff002;}
    struct IStorage {virtual ~IStorage() = default; virtual Result Read(s64,void*,size_t)=0;};
    struct SubStorage : IStorage {
        std::shared_ptr<IStorage> backing; s64 start, length;
        SubStorage(std::shared_ptr<IStorage> b,s64 begin,s64 n) : backing(std::move(b)),start(begin),length(n) {}
        Result Read(s64 at,void *buf,size_t n) override {
            assert(at>=0 && static_cast<s64>(at+n)<=length); return backing->Read(start+at,buf,n);
        }
    };
}
namespace ams::fs { using IStorage=::fs::IStorage; }
namespace ams::fssystem {
    namespace fs=::fs;
    struct NcaMetaDataHashData {u8 data[232];};
    using PatchMetadataInput = patch_witness::Input<sizeof(NcaMetaDataHashData)>;
    #include "observer.inc"
    bool fail_observer_allocation{};
    template<class T, class... Args> auto AllocateShared(Args&&... args) {
        if constexpr(std::is_same_v<T,ObservedPatchStorage>) if(fail_observer_allocation) return std::shared_ptr<T>{};
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
    struct NcaFsHeader {enum class FsType { RomFs };};
}
namespace us {
    namespace fssystem=ams::fssystem;
    namespace game_wake {std::atomic<u32> wake_epoch{};}
    namespace wake_compare {std::atomic<u32> ready_epoch{};}
    namespace system_power {bool paused{}, failed{}; bool IsBackgroundPaused(){return paused;} bool HasFailed(){return failed;}}
    namespace ncm {
        constexpr u64 InvalidContentId=0;
        struct String {char data[33]{};};
        String GetContentIdString(u64 id){String r;std::snprintf(r.data,sizeof(r.data),"%032llx",static_cast<unsigned long long>(id));return r;}
    }
    namespace util {constexpr auto SNPrintf=std::snprintf;}
    struct Snapshot { u64 generation; struct {u64 program_id,base,patch;} content; };
    using LaunchPin=std::shared_ptr<const Snapshot>;
    os::SdkMutex g_comparison_mutex, g_process_content_mutex;
    std::atomic<u32> g_comparison_revision{};
    #include "witness_state.inc"

    struct Backing : fs::IStorage {
        std::array<u8,1024> data{}; unsigned reads{}; bool fail{};
        Result Read(s64 at,void *buf,size_t n) override {
            ++reads;
            if(fail) return 17;
            assert(at>=0 && at+n<=data.size());
            std::memcpy(buf,data.data()+at,n);return 0;
        }
    };
    std::shared_ptr<Backing> disk=std::make_shared<Backing>();
    std::shared_ptr<fs::IStorage> retained;
    using fssystem::NcaMetaDataHashData;
    using fssystem::ObservedPatchStorage;
    using fssystem::PatchMetadataInput;
    namespace patch_witness = fssystem::patch_witness;
    int decrypt_mask=0x6c;
    int section=1;
    struct MetadataInfo {
        s64 offset=64; struct {u8 value[32]{};} hash;
    } meta_data_hash_data_info;
    std::shared_ptr<int> m_reader=std::make_shared<int>(1);
    u64 generation=3;
    unsigned opens{};
    Result CheckLaunchGeneration(const Snapshot &pin){return pin.generation==generation ? 0 : 18;}
    struct Bound {u64 process_id; LaunchPin snapshot;};
    Bound g_process_contents[2]{};

    Result Verify(std::shared_ptr<fs::IStorage> base_storage) {
        const auto meta_data_hash_data_offset = meta_data_hash_data_info.offset;
        constexpr auto size = sizeof(NcaMetaDataHashData);
        const s64 meta_data_hash_data_size=(size+15)/16*16;
        const struct {s64 indirect_offset;} patch_info{16};
        #include "raw_capture.inc"
        retained=enc_storage;
        std::array<u8,240> tmp{};
        const s64 start=meta_data_hash_data_offset-patch_info.indirect_offset;
        // The hash metadata request is split and padded by the simulated alignment layer.
        Result rc=enc_storage->Read(start+160,tmp.data()+160,80);
        if(R_FAILED(rc)) return rc;
        rc=enc_storage->Read(start,tmp.data(),160); if(R_FAILED(rc)) return rc;
        NcaMetaDataHashData meta_data_hash_data;
        for(size_t i=0;i<size;++i) meta_data_hash_data.data[i]=tmp[i]^decrypt_mask;
        u8 meta_data_hash_data_hash[32]{};
        crypto::GenerateSha256(meta_data_hash_data_hash,32,&meta_data_hash_data,size);
        #include "normal_hash_check.inc"
        return 0;
    }

    Result OpenLaunchProgramStorage(std::shared_ptr<ams::fs::IStorage> *out, void *, const Snapshot &snapshot,
                                    fssystem::NcaFsHeader::FsType, const LaunchPin &pin) {
        assert(pin.get()==&snapshot);
        ++opens; pw::Sample s;
        assert(BeginPatchWitness(pin,m_reader.get(),s));
        const auto rc=Verify(disk);
        FinishPatchWitness(pin,section,game_wake::wake_epoch.load(),s,rc,R_SUCCEEDED(CheckLaunchGeneration(snapshot)));
        if(R_SUCCEEDED(rc)) *out=retained;
        return rc;
    }
    #include "monitor_report.inc"
    std::string Report() {
        char buf[1024]{};
        const int n=AppendPatchWitness(buf,sizeof(buf),0);
        assert(n>0 && n==int(std::strlen(buf)));
        return buf;
    }
}

int main() {
    using namespace us;
    constexpr size_t n=sizeof(NcaMetaDataHashData);
    std::array<u8,n> plain{};
    for(size_t i=0;i<disk->data.size();++i) disk->data[i]=static_cast<u8>(i*7+3);
    for(size_t i=0;i<n;++i) plain[i]=disk->data[meta_data_hash_data_info.offset+i]^decrypt_mask;
    crypto::GenerateSha256(meta_data_hash_data_info.hash.value,32,plain.data(),n);
    auto pin=std::make_shared<Snapshot>(Snapshot{3,{45,31,32}});
    g_process_contents[0]={151,pin};
    std::shared_ptr<fs::IStorage> storage;
    auto open=[&]{return OpenLaunchProgramStorage(&storage,nullptr,*pin,fssystem::NcaFsHeader::FsType::RomFs,pin);};
    auto wake=[] {wake_compare::ready_epoch=++game_wake::wake_epoch;};
    auto check=[&](const char *text){assert(Report().find(text)!=std::string::npos);};
    auto probe=[&]{auto r=disk->reads;RunPatchWitness();assert(disk->reads==r+2);};
    assert(R_SUCCEEDED(open()) && disk->reads==2);
    check("usb_patch_witness_baseline=1\n");
    u8 after{};
    assert(R_SUCCEEDED(storage->Read(5,&after,1)) && after==disk->data[21]); // Released capture stack.
    wake(); probe();
    check("usb_patch_witness_same_cipher=1\n"); check("usb_patch_witness_same_plain=1\n"); check("usb_patch_witness_wake_verified=1\n");
    auto count=opens;RunPatchWitness();assert(opens==count);
    auto raw=g_patch_witness.baseline.cipher;
    pw::Digest expected_raw{};
    crypto::GenerateSha256(expected_raw.data(),32,disk->data.data()+meta_data_hash_data_info.offset,n);
    assert(raw==expected_raw);
    // Distinguish changed ciphertext from unchanged bytes with different decryption.
    wake(); ++decrypt_mask; probe();
    check("usb_patch_witness_same_cipher=1\n"); check("usb_patch_witness_same_plain=0\n"); check("usb_patch_witness_wake_verified=0\n");
    assert(g_patch_witness.open_result==0x1ff002);
    --decrypt_mask; wake(); disk->data[meta_data_hash_data_info.offset+7]^=0xf0; probe();
    check("usb_patch_witness_same_cipher=0\n"); check("usb_patch_witness_same_plain=0\n");
    disk->data[meta_data_hash_data_info.offset+7]^=0xf0;
    wake(); meta_data_hash_data_info.hash.value[0]^=1; probe();
    check("usb_patch_witness_same_header_hash=0\n"); check("usb_patch_witness_same_cipher=-1\n");
    meta_data_hash_data_info.hash.value[0]^=1;
    wake(); ++section; probe(); check("usb_patch_witness_same_extent=0\n"); --section;
    wake(); fssystem::fail_observer_allocation=true; probe(); check("usb_patch_witness_same_cipher=-1\n");
    check("usb_patch_witness_wake_verified=1\n"); fssystem::fail_observer_allocation=false;
    wake(); disk->fail=true; RunPatchWitness(); assert(g_patch_witness.open_result==17);
    check("usb_patch_witness_same_cipher=-1\n");check("usb_patch_witness_wake_verified=-1\n"); disk->fail=false;
    std::puts("PASS cold and wake exact ciphertext ranges from the same real read calls; differing decryption/backing bytes/header/section, fallback allocation and I/O failure");
    wake(); g_process_contents[0].process_id=0;count=opens;RunPatchWitness();assert(opens==count);
    check("usb_patch_witness_open=0xFFFFFFFF\n"); check("usb_patch_witness_sample_epoch=0\n");
    check("usb_patch_witness_same_cipher=-1\n"); check("usb_patch_witness_same_plain=-1\n");
    check("usb_patch_witness_wake_verified=-1\n");
    g_process_contents[0].process_id=151;
    wake();++generation;RunPatchWitness();assert(opens==count);--generation;
    wake();system_power::paused=true;RunPatchWitness();assert(opens==count);system_power::paused=false;
    ++game_wake::wake_epoch;RunPatchWitness();assert(opens==count);
    pw::Sample slot;assert(BeginPatchWitness(pin,m_reader.get(),slot));assert(pw::Find(m_reader.get())==&slot);
    int foreign{};assert(!pw::Find(&foreign));
    std::thread other([&]{pw::Sample local;assert(!pw::Find(m_reader.get()));assert(!BeginPatchWitness(pin,m_reader.get(),local));});
    other.join();FinishPatchWitness(pin,section,game_wake::wake_epoch.load(),slot,0,false);assert(!pw::Find(m_reader.get()));
    char tiny[4]{};assert(AppendPatchWitness(tiny,sizeof(tiny),0)<0);
    std::puts("PASS one probe per proven live launch and wake; pin/generation/paused gating, reader and thread isolation, truncation safety");
}
