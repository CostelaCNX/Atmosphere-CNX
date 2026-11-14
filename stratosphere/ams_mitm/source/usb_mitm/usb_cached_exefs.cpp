#include "sd_io_hooks.hpp"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_romfs_mods.hpp"
#include "usb_mod_index.hpp"
#include "../amsmitm_fs_utils.hpp"
namespace ams::mitm::usb_storage {
namespace fs = ::ams::fs;
namespace {
// Only an explicitly registered title receives this wrapper. USB remains the
// base code provider; optional replacements are read from the physical SD.
class CachedExefs final:public fs::fsa::IFileSystem {
    std::shared_ptr<fs::fsa::IFileSystem> m_sd,m_base;
    ncm::ProgramId m_title;
    Result CachedPath(fs::Path *out,const fs::Path &p,const char *suffix="") {
        const char *name=p.GetString();
        R_UNLESS(name && name[0]=='/' && nexus_mod::Path(name+1,std::strlen(name+1),false)
            && std::strchr(name+1,'/')==nullptr,fs::ResultInvalidPath());
        R_RETURN(out->InitializeWithFormat("/atmosphere/contents/%016" PRIx64 "/exefs%s%s",m_title.value,name,suffix));
    }
    Result CheckStub(const fs::Path &path) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbSupport);
        fs::Path stub;R_TRY(CachedPath(&stub,path,".stub"));
        fs::DirectoryEntryType type;const auto rc=m_sd->GetEntryType(&type,stub);
        if(R_SUCCEEDED(rc))R_THROW(fs::ResultPathNotFound());
        if(fs::ResultPathNotFound::Includes(rc))R_SUCCEED();
        R_RETURN(rc);
    }
public:
    CachedExefs(std::shared_ptr<fs::fsa::IFileSystem> sd,std::shared_ptr<fs::fsa::IFileSystem> base,ncm::ProgramId title)
        :m_sd(std::move(sd)),m_base(std::move(base)),m_title(title){}
private:
    Result DoGetEntryType(fs::DirectoryEntryType *out,const fs::Path &path) override {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbSupport);
        if(std::strcmp(path.GetString(),"/")==0){*out=fs::DirectoryEntryType_Directory;R_SUCCEED();}
        fs::Path cached;R_TRY(CachedPath(&cached,path));
        const auto rc=m_sd->GetEntryType(out,cached);
        if(!fs::ResultPathNotFound::Includes(rc))R_RETURN(rc);
        R_TRY(CheckStub(path));R_RETURN(m_base->GetEntryType(out,path));
    }
    Result DoOpenFile(std::unique_ptr<fs::fsa::IFile> *out,const fs::Path &path,fs::OpenMode mode) override {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbSupport);
        R_UNLESS(mode==fs::OpenMode_Read,fs::ResultInvalidOpenMode());
        fs::Path cached;R_TRY(CachedPath(&cached,path));
        const auto rc=m_sd->OpenFile(out,cached,mode);
        if(!fs::ResultPathNotFound::Includes(rc))R_RETURN(rc);
        R_TRY(CheckStub(path));R_RETURN(m_base->OpenFile(out,path,mode));
    }
    Result DoOpenDirectory(std::unique_ptr<fs::fsa::IDirectory> *,const fs::Path &,fs::OpenDirectoryMode) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result DoCreateFile(const fs::Path &,s64,int) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result DoDeleteFile(const fs::Path &) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result DoCreateDirectory(const fs::Path &) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result DoDeleteDirectory(const fs::Path &) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result DoDeleteDirectoryRecursively(const fs::Path &) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result DoCleanDirectoryRecursively(const fs::Path &) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result DoRenameFile(const fs::Path &,const fs::Path &) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result DoRenameDirectory(const fs::Path &,const fs::Path &) override {R_THROW(fs::ResultUnsupportedOperation());}
    Result DoCommit() override {R_SUCCEED();}
};
}
Result ApplyUsbCachedExefs(std::shared_ptr<fs::fsa::IFileSystem> *code,ncm::ProgramId title) {
        nexus_sd_io::Scope sd_scope(nexus_sd_io::Origin::UsbSupport);
    R_SUCCEED_IF(ncm::IsSystemProgramId(title) || !nexus_mod::GameId(title.value));
    FsFile enabled{};const auto rc=mitm::fs::OpenAtmosphereSdFile(&enabled,title,"usb_exefs.enabled",fs::OpenMode_Read);
    R_SUCCEED_IF(fs::ResultPathNotFound::Includes(rc));R_TRY(rc);
    ON_SCOPE_EXIT{fsFileClose(&enabled);};
    char marker[8]{};u64 read=0;s64 size=0;R_TRY(fsFileGetSize(&enabled,&size));
    R_UNLESS(size==8,fs::ResultInvalidSize());
    R_TRY(fsFileRead(&enabled,0,marker,8,FsReadOption_None,&read));
    R_UNLESS(read==8 && std::memcmp(marker,"NXUEXE1\n",8)==0,fs::ResultInvalidArgument());
    FsFileSystem sd{};R_TRY(fsOpenSdCardFileSystem(&sd));
    auto native=std::make_shared<fs::RemoteFileSystem>(sd);
    if(!native){fsFsClose(&sd);R_THROW(fs::ResultAllocationMemoryFailed());}
    *code=std::make_shared<CachedExefs>(std::move(native),std::move(*code),title);
    R_UNLESS(*code,fs::ResultAllocationMemoryFailed());R_SUCCEED();
}
}
