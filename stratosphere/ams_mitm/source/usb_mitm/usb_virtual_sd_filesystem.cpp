// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_virtual_sd.hpp"
#include "usbmitm_module.hpp"
#include <stratosphere/fs/impl/fs_newable.hpp>
#include <stratosphere/usb/usb_io_diagnostics.hpp>
namespace ams::mitm::usb_storage {
namespace {
Result Convert(Result rc){
    if(usb::ResultFileNotFound::Includes(rc))R_THROW(fs::ResultPathNotFound());
    if(usb::ResultAlreadyInitialized::Includes(rc))R_THROW(fs::ResultPathAlreadyExists());
    if(usb::ResultInvalidParameter::Includes(rc))R_THROW(fs::ResultInvalidArgument());
    if(usb::ResultOperationDenied::Includes(rc)||usb::ResultResourceBusy::Includes(rc))R_THROW(fs::ResultTargetLocked());
    if(usb::ResultMediaChanged::Includes(rc)||usb::ResultNotInitialized::Includes(rc)||usb::ResultInterrupted::Includes(rc))
        R_THROW(fs::ResultSdCardNotPresent());
    R_RETURN(rc);
}
struct BoundPath {
    VirtualSdLease lease;
    char path[fs::EntryNameLengthMax+1]{};
    explicit BoundPath(const VirtualSdLease &value):lease(value){}
    Result Bind(const fs::Path &value){
        const char *p=value.IsEmpty()?"/":value.GetString();
        R_UNLESS(nexus_vsd::Resolve(path,sizeof(path),lease.profile,p),fs::ResultInvalidArgument());
        R_RETURN(CheckVirtualSdLease(lease));
    }
    Result Check() const {R_RETURN(CheckVirtualSdLease(lease));}
    auto Service() const {return GetUsbContentStorageService();}
    Result Info(usb::volume::FileInfo *out) const {
        R_TRY(Check());R_RETURN(Convert(Service()->GetFileInfoAtGeneration(path,out,lease.generation)));
    }
};
class File final:public fs::fsa::IFile,public fs::impl::Newable {
    usb::storage::EjectLease m_eject_lease;
    BoundPath m_path;fs::OpenMode m_mode;
public:
    Result AcquireLease() {R_RETURN(m_eject_lease.Acquire(m_path.Service(),m_path.lease.generation));}
    File(const BoundPath &path,fs::OpenMode mode):m_path(path),m_mode(mode){}
private:
    Result DoRead(size_t *out,s64 offset,void *data,size_t size,const fs::ReadOption &option) override {
        R_TRY(m_path.Check());
        size_t request{};R_TRY(DryRead(&request,offset,size,option,m_mode));
        *out=0;R_SUCCEED_IF(request==0);
        R_RETURN(Convert(m_path.Service()->ReadFileAtGeneration(m_path.path,offset,data,request,out,m_path.lease.generation)));
    }
    Result DoWrite(s64 offset,const void *data,size_t size,const fs::WriteOption &option) override {
        bool append{};R_TRY(DryWrite(&append,offset,size,option,m_mode));R_TRY(m_path.Check());
        // DryWrite already enforces AllowAppend. The generation-checked
        // backend grows through f_lseek/f_write and synchronizes this request.
        // A preliminary resize needlessly closes every cached file and adds
        // a second metadata sync before writing any of the caller's data.
        size_t written{};
        R_TRY(Convert(m_path.Service()->WriteFileAtGeneration(m_path.path,offset,data,size,&written,m_path.lease.generation)));
        R_UNLESS(written==size,fs::ResultUnexpectedInLocalFileSystemA());
        if(option.HasFlushFlag()) { R_TRY(DoFlush()); }
        if(append)usb::io_diagnostics::virtual_sd_append_writes.fetch_add(1,std::memory_order_relaxed);
        R_SUCCEED();
    }
    Result DoFlush() override {R_TRY(m_path.Check());R_RETURN(Convert(m_path.Service()->FlushAtGeneration(m_path.lease.generation)));}
    Result DoGetSize(s64 *out) override {
        usb::volume::FileInfo info{};R_TRY(m_path.Info(&info));
        R_UNLESS(!info.directory && info.size<=static_cast<u64>(INT64_MAX),fs::ResultOutOfRange());
        *out=info.size;R_SUCCEED();
    }
    Result DoSetSize(s64 size) override {
        R_TRY(DrySetSize(size,m_mode));R_TRY(m_path.Check());
        R_RETURN(Convert(m_path.Service()->ResizeFileAtGeneration(m_path.path,size,m_path.lease.generation)));
    }
    Result DoOperateRange(void *,size_t,fs::OperationId,s64,s64,const void *,size_t) override {R_THROW(fs::ResultNotImplemented());}
public:
    sf::cmif::DomainObjectId GetDomainObjectId() const override {AMS_ABORT();}
};
class Directory final:public fs::fsa::IDirectory,public fs::impl::Newable {
    usb::storage::EjectLease m_eject_lease;
    BoundPath m_path;fs::OpenDirectoryMode m_mode;size_t m_offset{};
    Result ReadBatch(usb::volume::DirectoryEntryInfo *out,size_t max,size_t offset,size_t *count){
        R_TRY(m_path.Check());
        R_RETURN(Convert(m_path.Service()->ReadDirectoryAtGeneration(m_path.path,out,max,offset,
            (m_mode&fs::OpenDirectoryMode_Directory)!=0,(m_mode&fs::OpenDirectoryMode_File)!=0,count,m_path.lease.generation)));
    }
public:
    Result AcquireLease() {R_RETURN(m_eject_lease.Acquire(m_path.Service(),m_path.lease.generation));}
    Directory(const BoundPath &path,fs::OpenDirectoryMode mode):m_path(path),m_mode(mode){}
    sf::cmif::DomainObjectId GetDomainObjectId() const override {AMS_ABORT();}
private:
    Result DoRead(s64 *count,fs::DirectoryEntry *out,s64 max) override {
        *count=0;R_UNLESS(max>=0,fs::ResultInvalidArgument());R_TRY(m_path.Check());
        while(*count<max){
            usb::volume::DirectoryEntryInfo entries[8]{};size_t got{};
            R_TRY(ReadBatch(entries,std::min<size_t>(8,max-*count),m_offset,&got));
            if(!got)break;
            for(size_t i=0;i<got;++i){
                R_UNLESS(entries[i].size<=static_cast<u64>(INT64_MAX),fs::ResultOutOfRange());
                auto &e=out[(*count)++];e={};util::Strlcpy(e.name,entries[i].name,sizeof(e.name));
                e.type=entries[i].directory?fs::DirectoryEntryType_Directory:fs::DirectoryEntryType_File;
                e.file_size=entries[i].directory?0:entries[i].size;
            }
            m_offset+=got;
        }
        R_SUCCEED();
    }
    Result DoGetEntryCount(s64 *count) override {
        *count=0;
        for(;;){
            usb::volume::DirectoryEntryInfo entries[8]{};size_t got{};R_TRY(ReadBatch(entries,8,*count,&got));
            R_UNLESS(got<=static_cast<u64>(INT64_MAX-*count),fs::ResultOutOfRange());
            *count+=got;if(!got)break;
        }
        R_SUCCEED();
    }
};
class FileSystem final:public fs::fsa::IFileSystem,public fs::impl::Newable {
    VirtualSdLease m_lease;
    // Keep the forwarded domain object alive, but never expose its real SD data.
    std::shared_ptr<fs::fsa::IFileSystem> m_anchor;
    Result Delete(const fs::Path &path,bool directory){
        R_UNLESS(path!="/",fs::ResultInvalidArgument());
        BoundPath p(m_lease);R_TRY(p.Bind(path));usb::volume::FileInfo info{};R_TRY(p.Info(&info));
        R_UNLESS(info.directory==directory,fs::ResultPathNotFound());
        R_RETURN(Convert(p.Service()->DeleteFileAtGeneration(p.path,m_lease.generation)));
    }
    Result Rename(const fs::Path &old_path,const fs::Path &new_path,bool directory){
        R_UNLESS(old_path!="/" && new_path!="/",fs::ResultInvalidArgument());
        BoundPath old(m_lease),next(m_lease);R_TRY(old.Bind(old_path));R_TRY(next.Bind(new_path));
        usb::volume::FileInfo info{};R_TRY(old.Info(&info));R_UNLESS(info.directory==directory,fs::ResultPathNotFound());
        R_RETURN(Convert(old.Service()->RenameFileAtGeneration(old.path,next.path,m_lease.generation)));
    }
public:
    FileSystem(const VirtualSdLease &lease,std::shared_ptr<fs::fsa::IFileSystem> anchor):m_lease(lease),m_anchor(std::move(anchor)){}
private:
    Result DoCreateFile(const fs::Path &path,s64 size,int flags) override {
        R_UNLESS(size>=0 && flags==0,fs::ResultInvalidArgument());
        BoundPath p(m_lease);R_TRY(p.Bind(path));
        R_RETURN(Convert(p.Service()->CreateFileAtGeneration(p.path,size,m_lease.generation)));
    }
    Result DoCreateDirectory(const fs::Path &path) override {
        BoundPath p(m_lease);R_TRY(p.Bind(path));R_RETURN(Convert(p.Service()->CreateDirectoryAtGeneration(p.path,m_lease.generation)));
    }
    Result DoDeleteFile(const fs::Path &path) override {R_RETURN(Delete(path,false));}
    Result DoDeleteDirectory(const fs::Path &path) override {R_RETURN(Delete(path,true));}
    Result DoDeleteDirectoryRecursively(const fs::Path &) override {R_THROW(fs::ResultNotImplemented());}
    Result DoCleanDirectoryRecursively(const fs::Path &) override {R_THROW(fs::ResultNotImplemented());}
    Result DoRenameFile(const fs::Path &a,const fs::Path &b) override {R_RETURN(Rename(a,b,false));}
    Result DoRenameDirectory(const fs::Path &a,const fs::Path &b) override {R_RETURN(Rename(a,b,true));}
    Result DoGetEntryType(fs::DirectoryEntryType *out,const fs::Path &path) override {
        BoundPath p(m_lease);R_TRY(p.Bind(path));usb::volume::FileInfo info{};R_TRY(p.Info(&info));
        *out=info.directory?fs::DirectoryEntryType_Directory:fs::DirectoryEntryType_File;R_SUCCEED();
    }
    Result DoOpenFile(std::unique_ptr<fs::fsa::IFile> *out,const fs::Path &path,fs::OpenMode mode) override {
        BoundPath p(m_lease);R_TRY(p.Bind(path));usb::volume::FileInfo info{};R_TRY(p.Info(&info));
        R_UNLESS(!info.directory,fs::ResultPathNotFound());
        auto file=std::make_unique<File>(p,mode);R_UNLESS(file,fs::ResultAllocationMemoryFailedMakeUnique());
        R_TRY(file->AcquireLease());
        *out=std::move(file);R_SUCCEED();
    }
    Result DoOpenDirectory(std::unique_ptr<fs::fsa::IDirectory> *out,const fs::Path &path,fs::OpenDirectoryMode mode) override {
        BoundPath p(m_lease);R_TRY(p.Bind(path));usb::volume::FileInfo info{};R_TRY(p.Info(&info));
        R_UNLESS(info.directory,fs::ResultPathNotFound());
        auto dir=std::make_unique<Directory>(p,mode);R_UNLESS(dir,fs::ResultAllocationMemoryFailedMakeUnique());
        R_TRY(dir->AcquireLease());
        *out=std::move(dir);R_SUCCEED();
    }
    Result DoCommit() override {R_TRY(CheckVirtualSdLease(m_lease));R_RETURN(Convert(GetUsbContentStorageService()->FlushAtGeneration(m_lease.generation)));}
    Result DoFlush() override {R_RETURN(DoCommit());}
    Result Space(s64 *out,const fs::Path &path,bool total){
        BoundPath p(m_lease);R_TRY(p.Bind(path));
        u64 free{},size{};R_TRY(Convert(p.Service()->GetSpaceSize(&free,&size)));R_TRY(p.Check());
        const u64 value=total?size:free;R_UNLESS(value<=static_cast<u64>(INT64_MAX),fs::ResultOutOfRange());
        *out=value;R_SUCCEED();
    }
    Result DoGetFreeSpaceSize(s64 *out,const fs::Path &path) override {R_RETURN(Space(out,path,false));}
    Result DoGetTotalSpaceSize(s64 *out,const fs::Path &path) override {R_RETURN(Space(out,path,true));}
    Result DoGetFileTimeStampRaw(fs::FileTimeStampRaw *out,const fs::Path &path) override {
        BoundPath p(m_lease);R_TRY(p.Bind(path));usb::volume::FileInfo info{};R_TRY(p.Info(&info));
        // The shared-volume API currently has no timestamp fields.
        *out={};R_SUCCEED();
    }
};
}
Result CreateVirtualSdFileSystem(std::unique_ptr<fs::fsa::IFileSystem> *out,
    const VirtualSdLease &lease,std::shared_ptr<fs::fsa::IFileSystem> anchor){
    R_TRY(CheckVirtualSdLease(lease));
    auto fs=std::make_unique<FileSystem>(lease,std::move(anchor));
    R_UNLESS(fs,ams::fs::ResultAllocationMemoryFailedMakeUnique());
    *out=std::move(fs);R_SUCCEED();
}
}
