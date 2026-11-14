#include "sd_io_hooks.hpp"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_romfs_mods.hpp"
#include "usb_mod_index.hpp"
#include "usbmitm_module.hpp"
#include "usb_launch_trace.hpp"
#include "../amsmitm_fs_utils.hpp"
#include <cstdlib>
namespace ams::mitm::usb_storage {
namespace fs = ::ams::fs;
namespace {
void TraceUsbModAccess(u64 title, const char *phase, Result result, s64 offset, size_t size, u64 generation) {
    char detail[128]{};
    util::SNPrintf(detail, sizeof(detail), "M2 mods %s off=%lld bytes=%zu gen=%llu", phase,
        static_cast<long long>(offset), size, static_cast<unsigned long long>(generation));
    RecordLaunchBoundaryTrace(FsUsbStorageFirstRead, ncm::ProgramId{title}, 0, result, detail);
}
struct Free {void operator()(u8 *p)const{std::free(p);}};
using Bytes=std::unique_ptr<u8,Free>;
class IndexedRomfs final:public fs::IStorage {
    Bytes m_bytes;nexus_mod::View m_index;u64 m_generation;
    std::atomic_bool m_read_traced{false},m_failure_traced{false};
public:
    IndexedRomfs(Bytes bytes,nexus_mod::View view,u64 generation):m_bytes(std::move(bytes)),m_index(view),m_generation(generation){}
    Result Read(s64 offset,void *buffer,size_t size) override {
        R_UNLESS(offset>=0 && static_cast<u64>(offset)<=m_index.total && size<=m_index.total-static_cast<u64>(offset),fs::ResultInvalidOffset());
        R_UNLESS(buffer || !size,fs::ResultNullptrArgument());
        auto dst=static_cast<u8*>(buffer);u64 at=offset;
        while(size){
            // Metadata is an immutable validated launch snapshot, deliberately
            // independent of USB. Atmosphere's builder reads it on its worker.
            if(at<m_index.meta_size){size_t n=std::min<size_t>(size,m_index.meta_size-at);std::memcpy(dst,m_index.meta+at,n);at+=n;dst+=n;size-=n;continue;}
            size_t lo=0,hi=m_index.count;
            while(lo<hi){size_t mid=(lo+hi)/2;if(nexus_mod::U64(m_index.Record(mid))<=at)lo=mid+1;else hi=mid;}
            const u8 *r=lo?m_index.Record(lo-1):nullptr;
            u64 start=r?nexus_mod::U64(r):0,end=r?start+nexus_mod::U64(r+8):0;
            if(!r || at>=end){
                u64 next=lo<m_index.count?nexus_mod::U64(m_index.Record(lo)):m_index.total;
                R_UNLESS(next>at,fs::ResultInvalidOffset());
                size_t n=std::min<u64>(size,next-at);std::memset(dst,0,n);at+=n;dst+=n;size-=n;continue;
            }
            char path[fs::EntryNameLengthMax+1];
            util::SNPrintf(path,sizeof(path),"%s/%s",m_index.root,m_index.Name(lo-1));
            size_t n=std::min<size_t>({size,static_cast<size_t>(end-at),1024*1024}),got=0;
            auto service=GetUsbContentStorageService();R_UNLESS(service,fs::ResultSdCardNotPresent());
            const Result rc=service->ReadFileAtGeneration(path,at-start,dst,n,&got,m_generation);
            const Result checked=R_SUCCEEDED(rc) && got!=n ? fs::ResultUnexpectedInLocalFileSystemA() : rc;
            if (!m_read_traced.exchange(true, std::memory_order_relaxed))
                TraceUsbModAccess(nexus_mod::U64(m_index.bytes+16),"first-data-read",checked,at,n,m_generation);
            if (R_FAILED(checked) && !m_failure_traced.exchange(true, std::memory_order_relaxed))
                TraceUsbModAccess(nexus_mod::U64(m_index.bytes+16),"data-read-failed",checked,at,n,m_generation);
            R_UNLESS(R_SUCCEEDED(rc),fs::ResultSdCardNotPresent());
            R_UNLESS(got==n,fs::ResultUnexpectedInLocalFileSystemA());
            at+=n;dst+=n;size-=n;
        }
        R_SUCCEED();
    }
    Result GetSize(s64 *out) override{*out=m_index.total;R_SUCCEED();}
    Result Flush() override{R_SUCCEED();}
    Result Write(s64,const void*,size_t) override{R_THROW(fs::ResultUnsupportedOperation());}
    Result SetSize(s64) override{R_THROW(fs::ResultUnsupportedOperation());}
    Result OperateRange(void*,size_t,fs::OperationId,s64,s64,const void*,size_t) override{R_THROW(fs::ResultUnsupportedOperation());}
};
// Atmosphere's existing RomFS builder assumes table reads cannot fail. Read
// the base tables before starting that worker, returning transport errors to
// the caller; never ask the builder to fetch removable-media metadata.
class TableSnapshot final:public fs::IStorage {
    std::shared_ptr<fs::IStorage> m_base;
    struct Span {u64 offset,size;Bytes data;};
    Span m_parts[3]{};
    s64 m_size=0;
public:
    explicit TableSnapshot(std::shared_ptr<fs::IStorage> base):m_base(std::move(base)){}
    Result Initialize() {
        R_TRY(m_base->GetSize(&m_size));R_UNLESS(m_size>=80,fs::ResultInvalidSize());
        m_parts[0]={0,80,Bytes(static_cast<u8*>(std::malloc(80)))};
        R_UNLESS(m_parts[0].data,fs::ResultAllocationMemoryFailed());
        R_TRY(m_base->Read(0,m_parts[0].data.get(),80));
        const u8 *h=m_parts[0].data.get();R_UNLESS(nexus_mod::U64(h)==80,fs::ResultInvalidArgument());
        R_UNLESS(nexus_mod::U64(h+72)>=80 && nexus_mod::U64(h+72)<=static_cast<u64>(m_size),fs::ResultInvalidSize());
        size_t budget=2*1024*1024;
        for(size_t i=1;i<=2;i++) {
            const size_t at=i==1?24:56;const u64 offset=nexus_mod::U64(h+at),size=nexus_mod::U64(h+at+8);
            R_UNLESS(offset>=80 && offset<=static_cast<u64>(m_size) && size<=static_cast<u64>(m_size)-offset && size<=budget,fs::ResultInvalidSize());
            budget-=size;
            m_parts[i]={offset,size,Bytes(static_cast<u8*>(std::malloc(std::max<u64>(size,1))))};
            R_UNLESS(m_parts[i].data,fs::ResultAllocationMemoryFailed());
            for(u64 at=0;at<size;) {
                const size_t n=std::min<u64>(size-at,1024*1024);
                R_TRY(m_base->Read(offset+at,m_parts[i].data.get()+at,n));at+=n;
            }
        }
        R_SUCCEED();
    }
    Result Read(s64 offset,void *buffer,size_t size) override {
        R_UNLESS(offset>=0 && offset<=m_size && size<=static_cast<u64>(m_size-offset),fs::ResultInvalidOffset());
        R_UNLESS(buffer || !size,fs::ResultNullptrArgument());R_SUCCEED_IF(!size);
        for(const auto &p:m_parts) {
            if(static_cast<u64>(offset)>=p.offset && static_cast<u64>(offset)-p.offset<=p.size && size<=p.size-(offset-p.offset)) {
                std::memcpy(buffer,p.data.get()+offset-p.offset,size);R_SUCCEED();
            }
        }
        R_RETURN(m_base->Read(offset,buffer,size));
    }
    Result GetSize(s64 *out) override {*out=m_size;R_SUCCEED();}
    Result Flush() override {R_SUCCEED();}
    Result Write(s64,const void*,size_t) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result SetSize(s64) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result OperateRange(void*,size_t,fs::OperationId,s64,s64,const void*,size_t) override {R_THROW(fs::ResultUnsupportedOperation());}
};
}
Result SnapshotUsbModBase(std::shared_ptr<fs::IStorage> *base) {
    auto snapshot=std::make_shared<TableSnapshot>(*base);R_UNLESS(snapshot,fs::ResultAllocationMemoryFailed());
    R_TRY(snapshot->Initialize());*base=std::move(snapshot);R_SUCCEED();
}
Result OpenUsbModRomfs(std::shared_ptr<fs::IStorage> *out,ncm::ProgramId title){
    nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbSupport);
    out->reset();R_SUCCEED_IF(ncm::IsSystemProgramId(title) || !nexus_mod::GameId(title.value));
    FsFile file{};
    const Result opened=mitm::fs::OpenAtmosphereSdFile(&file,title,"usb_romfs.idx",fs::OpenMode_Read);
    R_SUCCEED_IF(fs::ResultPathNotFound::Includes(opened));R_TRY(opened);
    ON_SCOPE_EXIT{fsFileClose(&file);};
    s64 size=0;R_TRY(fsFileGetSize(&file,&size));
    R_UNLESS(size>=static_cast<s64>(nexus_mod::HeaderSize) && size<=static_cast<s64>(nexus_mod::MaxIndex),fs::ResultInvalidSize());
    Bytes bytes(static_cast<u8*>(std::malloc(size)));R_UNLESS(bytes,fs::ResultAllocationMemoryFailedMakeUnique());
    u64 got=0;R_TRY(fsFileRead(&file,0,bytes.get(),size,FsReadOption_None,&got));R_UNLESS(got==static_cast<u64>(size),fs::ResultInvalidSize());
    nexus_mod::View index;R_UNLESS(nexus_mod::Parse(&index,bytes.get(),size,title.value),fs::ResultInvalidArgument());
    u64 generation=0;
    R_UNLESS(IsUsbContentFileSystemRouteReady(&generation,false),fs::ResultSdCardNotPresent());
    char marker[fs::EntryNameLengthMax+1];util::SNPrintf(marker,sizeof(marker),"%s/.nexus-mod-id",index.root);
    u8 token[16]{};size_t read=0;auto service=GetUsbContentStorageService();
    R_UNLESS(service,fs::ResultSdCardNotPresent());
    R_TRY(service->ReadFileAtGeneration(marker,0,token,sizeof(token),&read,generation));
    R_UNLESS(read==sizeof(token) && std::memcmp(token,index.bytes+48,sizeof(token))==0,fs::ResultInvalidArgument());
    TraceUsbModAccess(title.value,"index-ready",ResultSuccess(),0,size,generation);
    *out=std::make_shared<IndexedRomfs>(std::move(bytes),index,generation);
    R_UNLESS(*out,fs::ResultAllocationMemoryFailed());
    R_SUCCEED();
}
}
