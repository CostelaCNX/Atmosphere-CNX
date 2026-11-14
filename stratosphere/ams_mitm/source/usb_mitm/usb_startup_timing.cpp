/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "usb_startup_timing.hpp"
#include "usb_homebrew_service.hpp"
#include "usbmitm_module.hpp"
#include "sd_io_hooks.hpp"
#include "../fs_mitm/fs_shim.h"
#include <stratosphere/usb/usb_startup_timing_core.hpp>
#include <stratosphere/usb/usb_startup_io_core.hpp>
#include <stratosphere/usb/usb_startup_io_diagnostics.hpp>
#include <stratosphere/usb/usb_startup_command_diagnostics.hpp>
#include <stratosphere/usb/usb_write_cache_diagnostics.hpp>
#include <stratosphere/usb/usb_startup_report.hpp>
#include <stratosphere/usb/usb_startup_publish.hpp>
#include <stratosphere/fs/impl/fs_newable.hpp>
namespace ams::mitm::usb_storage {
namespace {
os::SdkMutex g_startup_lock{};
/* Save and Submit share this gate so a new 60-second capture cannot start
 * during report I/O. Ordinary filesystem calls never acquire it. The short
 * recorder lock is always released before any FS/SM IPC. */
os::SdkMutex g_startup_save_gate{};
nexus_startup::Recorder g_startup;
nexus_startup::IoRecorder g_startup_io;
u64 Now(){return os::GetSystemTick().ToTimeSpan().GetNanoSeconds();}
struct SaveStatus {
    u64 saved_sequence{},attempt_sequence{},attempts{},failures{},saved_count{};
    u32 last_result{};
    char last_path[192]{};
} g_save_status;

/* Private, non-domain native-SD sessions: never use the virtual filesystem
 * or ams_mitm's shared single-slot libnx FS session pool for report writing. */
class StartupReportFileSystem {
    FsFileSystem sd{};
    Result Initialize() {
        R_SUCCEED_IF(serviceIsActive(&sd.s));
        os::NativeHandle handle{};
        R_TRY(sm::GetServiceHandle(&handle,sm::ServiceName::Encode("fsp-srv")));
        Service root{};serviceCreate(&root,handle);
        ON_SCOPE_EXIT { serviceClose(&root); };
        const u64 placeholder=0;
        R_TRY(serviceDispatchIn(&root,1,placeholder,.in_send_pid=true));
        // Identical raw command18, routed through the existing linker hook
        // so all file handles remain attributed to diagnostic physical SD.
        R_RETURN(fsOpenSdCardFileSystemFwd(&root,&sd));
    }
    Result ExistingMatches(bool *found,const char *path,const char *data,size_t size) {
        *found=false;
        FsDirEntryType type{};
        const Result check=fsFsGetEntryType(&sd,path,&type);
        R_SUCCEED_IF(fs::ResultPathNotFound::Includes(check));
        R_TRY(check);
        R_UNLESS(type==FsDirEntryType_File,fs::ResultPathAlreadyExists());
        FsFile file{};R_TRY(fsFsOpenFile(&sd,path,FsOpenMode_Read,&file));
        ON_SCOPE_EXIT { fsFileClose(&file); };
        s64 length{};R_TRY(fsFileGetSize(&file,&length));
        R_UNLESS(length==static_cast<s64>(size),fs::ResultPathAlreadyExists());
        char chunk[1024];
        for(size_t offset=0;offset<size;) {
            const size_t count=std::min(sizeof(chunk),size-offset);u64 actual{};
            R_TRY(fsFileRead(&file,offset,chunk,count,FsReadOption_None,&actual));
            R_UNLESS(actual==count && std::memcmp(chunk,data+offset,count)==0,fs::ResultPathAlreadyExists());
            offset+=count;
        }
        *found=true;R_SUCCEED();
    }
public:
    void Reset() { if(serviceIsActive(&sd.s))fsFsClose(&sd);sd={}; }
    ~StartupReportFileSystem(){Reset();}
    u32 CreateExclusive(const char *path,size_t size) { return fsFsCreateFile(&sd,path,static_cast<s64>(size),0); }
    u32 WriteFlushed(const char *path,const char *data,size_t size) {
        FsFile file{};const auto opened=fsFsOpenFile(&sd,path,FsOpenMode_Write,&file);
        if(R_FAILED(opened))return opened;
        ON_SCOPE_EXIT { fsFileClose(&file); };
        return fsFileWrite(&file,0,data,size,FsWriteOption_Flush);
    }
    u32 Rename(const char *temporary,const char *path) { return fsFsRenameFile(&sd,temporary,path); }
    u32 Commit() {
        const Result rc=fsFsCommit(&sd);
        // Save-data-style Commit is optional on native SD. The report file
        // was already flushed and closed before the atomic rename.
        if(fs::ResultUnsupportedOperation::Includes(rc)||fs::ResultNotImplemented::Includes(rc))return 0;
        return rc.GetValue();
    }
    void DeleteTemporary(const char *path) { static_cast<void>(fsFsDeleteFile(&sd,path)); }
    Result Publish(const char *path,const char *temporary,const char *data,size_t size) {
        R_TRY(Initialize());
        for(const char *directory:{"/switch","/switch/nexus-usb-bench","/switch/nexus-usb-bench/reports"}) {
            const Result rc=fsFsCreateDirectory(&sd,directory);
            R_UNLESS(R_SUCCEEDED(rc)||fs::ResultPathAlreadyExists::Includes(rc),rc);
        }
        bool existing=false;R_TRY(ExistingMatches(&existing,path,data,size));
        if(existing)R_RETURN(static_cast<Result>(Commit())); // Retry after an uncertain final commit.
        // CreateFile is exclusive. Neither an existing final report nor an
        // unrelated partial file is removed to force this write to succeed.
        R_RETURN(static_cast<Result>(nexus_startup::PublishReport(*this,temporary,path,data,size)));
    }
};
struct Call {
    u64 sequence{},start{},offset{},requested{};
    u32 operation{};
    const char *path{};
    NexusStartupIoSample sample{};
    NexusStartupIoSample *previous{};
    NexusStartupCommandSample command_sample{};
    NexusStartupCommandSample *previous_command{};
    bool context_active{false};
    Call(u64 id,u32 op,const char *p,u64 off=0,u64 bytes=0):offset(off),requested(bytes),operation(op),path(p) {
        std::scoped_lock lk(g_startup_lock);
        const u64 now=Now();
        if(g_startup.BeginCall(id,now)){sequence=id;start=now;}
        previous=usb::startup_io_diagnostics::current_sample;
        usb::startup_io_diagnostics::current_sample=
            sequence && g_startup_io.Supported(sequence)?&sample:nullptr;
        previous_command=usb::startup_command_diagnostics::current_sample;
        usb::startup_command_diagnostics::current_sample=
            usb::startup_io_diagnostics::current_sample?&command_sample:nullptr;
        context_active=true;
    }
    Call(const Call &)=delete;
    Call &operator=(const Call &)=delete;
    ~Call(){if(sequence)static_cast<void>(Finish(usb::ResultInternalStateError()));else Restore();}
    void Restore(){if(context_active){usb::startup_io_diagnostics::current_sample=previous;usb::startup_command_diagnostics::current_sample=previous_command;context_active=false;}}
    Result Finish(Result rc,u64 completed=0) {
        Restore();
        if(sequence){const u64 end=Now();std::scoped_lock lk(g_startup_lock);
            g_startup_io.Record(sequence,start,end,operation,path,offset,requested,completed,rc.GetValue(),sample,&command_sample);
            g_startup.EndCall(sequence,start,end,operation,path,offset,requested,completed,rc.GetValue());sequence=0;}
        return rc;
    }
};
struct ObjectPath {
    u64 sequence;
    char path[256]{};
    ObjectPath(u64 id,const fs::Path &p):sequence(id){util::Strlcpy(path,p.IsEmpty()?"/":p.GetString(),sizeof(path));}
};
class File final:public fs::fsa::IFile,public fs::impl::Newable {
    std::unique_ptr<fs::fsa::IFile> base;
    ObjectPath object;
public:
    File(std::unique_ptr<fs::fsa::IFile> value,u64 id,const fs::Path &path):base(std::move(value)),object(id,path){}
    sf::cmif::DomainObjectId GetDomainObjectId() const override {return base->GetDomainObjectId();}
private:
    Result DoRead(size_t *out,s64 offset,void *buffer,size_t size,const fs::ReadOption &option) override {
        Call c(object.sequence,NexusStartup_Read,object.path,offset,size);
        const auto rc=base->Read(out,offset,buffer,size,option);
        return c.Finish(rc,R_SUCCEEDED(rc)?*out:0);
    }
    Result DoWrite(s64 offset,const void *buffer,size_t size,const fs::WriteOption &option) override {
        Call c(object.sequence,NexusStartup_Write,object.path,offset,size);
        const auto rc=base->Write(offset,buffer,size,option);return c.Finish(rc,R_SUCCEEDED(rc)?size:0);
    }
    Result DoGetSize(s64 *out) override {Call c(object.sequence,NexusStartup_GetSize,object.path);return c.Finish(base->GetSize(out));}
    Result DoFlush() override {Call c(object.sequence,NexusStartup_Flush,object.path);return c.Finish(base->Flush());}
    Result DoSetSize(s64 size) override {Call c(object.sequence,NexusStartup_Metadata,object.path);return c.Finish(base->SetSize(size));}
    Result DoOperateRange(void *dst,size_t dst_size,fs::OperationId op,s64 offset,s64 size,const void *src,size_t src_size) override {
        Call c(object.sequence,NexusStartup_Metadata,object.path);return c.Finish(base->OperateRange(dst,dst_size,op,offset,size,src,src_size));
    }
};
class Directory final:public fs::fsa::IDirectory,public fs::impl::Newable {
    std::unique_ptr<fs::fsa::IDirectory> base;
    ObjectPath object;
public:
    Directory(std::unique_ptr<fs::fsa::IDirectory> value,u64 id,const fs::Path &path):base(std::move(value)),object(id,path){}
    sf::cmif::DomainObjectId GetDomainObjectId() const override {return base->GetDomainObjectId();}
private:
    Result DoRead(s64 *out,fs::DirectoryEntry *entries,s64 max) override {
        Call c(object.sequence,NexusStartup_ReadDirectory,object.path);return c.Finish(base->Read(out,entries,max));
    }
    Result DoGetEntryCount(s64 *out) override {Call c(object.sequence,NexusStartup_Metadata,object.path);return c.Finish(base->GetEntryCount(out));}
};
/* Transparent forwarding adapter. Only externally requested operations are
 * measured: nested size/metadata reads inside the USB filesystem are included
 * in their parent operation and are not counted a second time. */
class FileSystem final:public fs::fsa::IFileSystem,public fs::impl::Newable {
    std::shared_ptr<fs::fsa::IFileSystem> base;
    u64 sequence;
public:
    FileSystem(std::shared_ptr<fs::fsa::IFileSystem> value,u64 id):base(std::move(value)),sequence(id){}
private:
    Result DoOpenFile(std::unique_ptr<fs::fsa::IFile> *out,const fs::Path &path,fs::OpenMode mode) override {
        Call c(sequence,NexusStartup_OpenFile,path.GetString());
        std::unique_ptr<fs::fsa::IFile> file;const auto rc=base->OpenFile(&file,path,mode);
        R_TRY(c.Finish(rc));
        *out=std::make_unique<File>(std::move(file),sequence,path);
        R_UNLESS(*out,fs::ResultAllocationMemoryFailedMakeUnique());R_SUCCEED();
    }
    Result DoOpenDirectory(std::unique_ptr<fs::fsa::IDirectory> *out,const fs::Path &path,fs::OpenDirectoryMode mode) override {
        Call c(sequence,NexusStartup_OpenDirectory,path.GetString());
        std::unique_ptr<fs::fsa::IDirectory> dir;const auto rc=base->OpenDirectory(&dir,path,mode);
        R_TRY(c.Finish(rc));
        *out=std::make_unique<Directory>(std::move(dir),sequence,path);
        R_UNLESS(*out,fs::ResultAllocationMemoryFailedMakeUnique());R_SUCCEED();
    }
#define TIMED_PATH(OP,EXPR) Call c(sequence,OP,path.GetString());return c.Finish(EXPR)
#define TIMED_ROOT(OP,EXPR) Call c(sequence,OP,"/");return c.Finish(EXPR)
    Result DoCreateFile(const fs::Path &path,s64 size,int flags) override {TIMED_PATH(NexusStartup_Metadata,base->CreateFile(path,size,flags));}
    Result DoDeleteFile(const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->DeleteFile(path));}
    Result DoCreateDirectory(const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->CreateDirectory(path));}
    Result DoDeleteDirectory(const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->DeleteDirectory(path));}
    Result DoDeleteDirectoryRecursively(const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->DeleteDirectoryRecursively(path));}
    Result DoCleanDirectoryRecursively(const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->CleanDirectoryRecursively(path));}
    Result DoRenameFile(const fs::Path &path,const fs::Path &next) override {TIMED_PATH(NexusStartup_Metadata,base->RenameFile(path,next));}
    Result DoRenameDirectory(const fs::Path &path,const fs::Path &next) override {TIMED_PATH(NexusStartup_Metadata,base->RenameDirectory(path,next));}
    Result DoGetEntryType(fs::DirectoryEntryType *out,const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->GetEntryType(out,path));}
    Result DoGetFreeSpaceSize(s64 *out,const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->GetFreeSpaceSize(out,path));}
    Result DoGetTotalSpaceSize(s64 *out,const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->GetTotalSpaceSize(out,path));}
    Result DoGetFileTimeStampRaw(fs::FileTimeStampRaw *out,const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->GetFileTimeStampRaw(out,path));}
    Result DoQueryEntry(char *dst,size_t dst_size,const char *src,size_t src_size,fs::fsa::QueryId query,const fs::Path &path) override {TIMED_PATH(NexusStartup_Metadata,base->QueryEntry(dst,dst_size,src,src_size,query,path));}
    Result DoCommit() override {TIMED_ROOT(NexusStartup_Flush,base->Commit());}
    Result DoFlush() override {TIMED_ROOT(NexusStartup_Flush,base->Flush());}
    Result DoCommitProvisionally(s64 counter) override {TIMED_ROOT(NexusStartup_Flush,base->CommitProvisionally(counter));}
    Result DoRollback() override {TIMED_ROOT(NexusStartup_Metadata,base->Rollback());}
#undef TIMED_PATH
#undef TIMED_ROOT
};
}
u64 GetStartupTimingCapture(u64 pid,u32 source){
    const u64 now=Now();std::scoped_lock lk(g_startup_lock);return g_startup.Active(pid,source,now);
}
void RetireStartupTimingCapture(u64 pid){
    std::scoped_lock lk(g_startup_lock);g_startup.Supersede(pid,Now());
}
void PollStartupTimingAutosave(){
    /* Sole caller is the existing one-second USB monitor in both modes.
     * Fixed buffers live here, not on its stack. Failed immutable snapshots
     * stay pending; the core pins unsaved slots until MarkSaved succeeds. */
    static StartupReportFileSystem filesystem;
    static NexusStartupSnapshot pending{};
    static NexusStartupIoDetails pending_io{};
    static NexusStartupCommandDetails pending_commands{};
    static char encoded[nexus_startup::ReportWriter::BufferSize];
    static size_t encoded_size{};
    static u64 next_attempt_ns{},consecutive_failures{};
    static u64 boot_id[2]{};
    static bool boot_id_ready{};
    static u32 collision_suffix{};
    std::unique_lock save(g_startup_save_gate,std::try_to_lock);
    if(!save.owns_lock()||Now()<next_attempt_ns)return;
    {
        std::scoped_lock lk(g_startup_lock);
        if(g_startup.HasActiveOrInflight(Now()))return;
        if(!pending.sequence) {
            if(!g_startup.NextUnsaved(&pending,Now()))return;
            if(!g_startup_io.Snapshot(&pending_io,pending.sequence,pending.sampled_ns) ||
               !g_startup_io.CommandSnapshot(&pending_commands,pending.sequence,pending.sampled_ns)) {
                pending={};return;
            }
        }
    }
    if(!encoded_size) {
        // This boot-cumulative snapshot is sampled after the capture window,
        // separately from its atomic per-operation timings. It never starts USB.
        usb::storage::Status storage_status{};
        NexusUsbWriteCacheSnapshot cache{};
        const bool cache_valid=R_SUCCEEDED(GetUsbContentStorageService()->GetStatus(&storage_status)) &&
            usb::write_cache_diagnostics::Snapshot(&cache,storage_status.media_generation) &&
            R_SUCCEEDED(GetUsbContentStorageService()->GetStatus(&storage_status)) &&
            storage_status.media_generation==cache.media_generation;
        nexus_startup::ReportWriter writer(encoded,sizeof(encoded));
        encoded_size=writer.Format(pending,&pending_io,&pending_commands,cache_valid?&cache:nullptr);
    }
    if(!boot_id_ready){os::GenerateRandomBytes(boot_id,sizeof(boot_id));boot_id_ready=true;}
    char path[192]{},temporary[224]{};
    util::SNPrintf(path,sizeof(path),"/switch/nexus-usb-bench/reports/startup-auto-%016llx%016llx-%016llx-%02x.json",
        static_cast<unsigned long long>(boot_id[0]),static_cast<unsigned long long>(boot_id[1]),
        static_cast<unsigned long long>(pending.sequence),collision_suffix);
    u64 attempt{};
    {std::scoped_lock lk(g_startup_lock);attempt=++g_save_status.attempts;g_save_status.attempt_sequence=pending.sequence;}
    util::SNPrintf(temporary,sizeof(temporary),"%s.%016llx.tmp",path,static_cast<unsigned long long>(attempt));
    const Result result=[&]() -> Result {
        R_UNLESS(encoded_size,usb::ResultInvalidParameter());
        const nexus_sd_io::Scope diagnostic(nexus_sd_io::Origin::Diagnostic);
        R_RETURN(filesystem.Publish(path,temporary,encoded,encoded_size));
    }();
    if(R_FAILED(result))filesystem.Reset(); // Next bounded retry opens fresh native sessions.
    {
        std::scoped_lock lk(g_startup_lock);
        g_save_status.last_result=result.GetValue();
        if(R_SUCCEEDED(result) && g_startup.MarkSaved(pending.sequence)) {
            g_save_status.saved_sequence=pending.sequence;++g_save_status.saved_count;
            util::Strlcpy(g_save_status.last_path,path,sizeof(g_save_status.last_path));
            pending={};pending_io={};pending_commands={};encoded_size=0;consecutive_failures=0;next_attempt_ns=0;collision_suffix=0;
        } else {
            ++g_save_status.failures;
            if(R_SUCCEEDED(result))g_save_status.last_result=usb::ResultInternalStateError().GetValue();
            if(fs::ResultPathAlreadyExists::Includes(result)&&collision_suffix<16)++collision_suffix;
            consecutive_failures=std::min<u64>(consecutive_failures+1,7);
            const u64 seconds=std::min<u64>(UINT64_C(1)<<(consecutive_failures-1),60);
            next_attempt_ns=Now()+seconds*UINT64_C(1000000000);
        }
    }
}
int FormatStartupTimingAutosaveStatus(char *out,size_t capacity){
    SaveStatus status{};{std::scoped_lock lk(g_startup_lock);status=g_save_status;}
    return util::SNPrintf(out,capacity,
        "startup_autosave=1\nstartup_io_timings=SD10\nstartup_io_query=86\nstartup_usb_commands=SD11\nstartup_command_query=87\nusb_write_cache=SD12\nusb_write_cache_query=88\nstartup_report_directory=/switch/nexus-usb-bench/reports\n"
        "startup_autosave_saved_count=%llu\nstartup_autosave_saved_sequence=%llu\n"
        "startup_autosave_attempt_sequence=%llu\nstartup_autosave_attempts=%llu\n"
        "startup_autosave_failures=%llu\nstartup_autosave_last_result=0x%08X\nstartup_autosave_last_path=%s\n",
        static_cast<unsigned long long>(status.saved_count),static_cast<unsigned long long>(status.saved_sequence),
        static_cast<unsigned long long>(status.attempt_sequence),static_cast<unsigned long long>(status.attempts),
        static_cast<unsigned long long>(status.failures),status.last_result,status.last_path);
}
std::unique_ptr<fs::fsa::IFileSystem> WrapStartupTimingFileSystem(std::shared_ptr<fs::fsa::IFileSystem> base,u64 capture){
    return std::make_unique<FileSystem>(std::move(base),capture);
}
Result HomebrewService::SubmitStartupTiming(sf::Out<u64> out,const sf::ClientProcessId &pid,const sf::InBuffer &in){
    R_UNLESS(in.GetSize()==sizeof(NexusStartupLoader) && in.GetPointer(),usb::ResultInvalidParameter());
    NexusStartupLoader loader{};std::memcpy(&loader,in.GetPointer(),sizeof(loader));
    R_UNLESS(nexus_startup::Recorder::Valid(loader),usb::ResultInvalidParameter());
    // A concurrent save can briefly delay admission, never reject it. Start
    // the observation clock only after that save has finished.
    std::scoped_lock save(g_startup_save_gate);
    std::scoped_lock lk(g_startup_lock);
    const auto sequence=g_startup.Submit(pid.GetValue().value,Now(),loader);
    R_UNLESS(sequence,usb::ResultResourceBusy());
    NexusStartupSnapshot snapshot{};
    for(u32 index=0;index<NEXUS_STARTUP_SLOT_COUNT;++index) {
        if(g_startup.Snapshot(&snapshot,index,Now()) && snapshot.sequence==sequence) {
            g_startup_io.Reset(index,snapshot);break;
        }
    }
    out.SetValue(sequence);R_SUCCEED();
}
Result HomebrewService::QueryStartupTiming(const sf::OutBuffer &out,u32 index){
    R_UNLESS(out.GetSize()==sizeof(NexusStartupSnapshot) && out.GetPointer() && index<NEXUS_STARTUP_SLOT_COUNT,usb::ResultInvalidParameter());
    NexusStartupSnapshot snapshot{};
    // Fixed-size copy only: no filesystem/USB calls, allocations or logs while
    // holding this lock. Do not assume alignment of a caller's IPC buffer.
    {std::scoped_lock lk(g_startup_lock);g_startup.Snapshot(&snapshot,index,Now());}
    std::memcpy(out.GetPointer(),&snapshot,sizeof(snapshot));
    R_SUCCEED();
}
Result HomebrewService::QueryStartupIoTiming(const sf::OutBuffer &out,u32 index){
    R_UNLESS(out.GetSize()==sizeof(NexusStartupIoSnapshot) && out.GetPointer() && index<NEXUS_STARTUP_SLOT_COUNT,usb::ResultInvalidParameter());
    NexusStartupIoSnapshot snapshot{};
    {
        std::scoped_lock lk(g_startup_lock);
        const u64 now=Now();
        R_UNLESS(g_startup.Snapshot(&snapshot.startup,index,now),usb::ResultInvalidParameter());
        R_UNLESS(g_startup_io.Snapshot(&snapshot.io,snapshot.startup.sequence,now),usb::ResultInternalStateError());
    }
    std::memcpy(out.GetPointer(),&snapshot,sizeof(snapshot));R_SUCCEED();
}
Result HomebrewService::QueryStartupCommandTiming(const sf::OutBuffer &out,u32 index){
    R_UNLESS(out.GetSize()==sizeof(NexusStartupCommandSnapshot) && out.GetPointer() && index<NEXUS_STARTUP_SLOT_COUNT,usb::ResultInvalidParameter());
    NexusStartupCommandSnapshot snapshot{};
    {
        std::scoped_lock lk(g_startup_lock);const u64 now=Now();
        R_UNLESS(g_startup.Snapshot(&snapshot.io.startup,index,now),usb::ResultInvalidParameter());
        R_UNLESS(g_startup_io.Snapshot(&snapshot.io.io,snapshot.io.startup.sequence,now),usb::ResultInternalStateError());
        R_UNLESS(g_startup_io.CommandSnapshot(&snapshot.commands,snapshot.io.startup.sequence,now),usb::ResultInternalStateError());
    }
    std::memcpy(out.GetPointer(),&snapshot,sizeof(snapshot));R_SUCCEED();
}
}
