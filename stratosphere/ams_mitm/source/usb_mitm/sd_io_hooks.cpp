// SPDX-License-Identifier: GPL-2.0-or-later
// Read-only instrumentation: no buffers, arguments, results or routing changed.
#ifdef NEXUS_SD_IO_HOST_TEST
#include "sd_io_host.hpp"
#else
#include <stratosphere.hpp>
#endif
#include "sd_io_hooks.hpp"
#include <mutex>
namespace nexus_sd_io {
thread_local Origin current_origin=Origin::Other;
namespace {
ams::os::SdkMutex mutex;
Registry<512> registry;
NexusSdIoSnapshot state{};
uint64_t now() {
#ifdef NEXUS_SD_IO_HOST_TEST
    return host_now();
#else
    return ams::os::GetSystemTick().ToTimeSpan().GetNanoSeconds();
#endif
}
void initialize(){if(!state.epoch_ns)state.epoch_ns=now();}
Key key(const Service& s){return {s.session,s.object_id};}
void track(const Service& s,Kind kind,Tag tag) {
    std::scoped_lock lock(mutex);
    // A fresh successful open replaces any stale identity, including an
    // untracked native object reusing a closed SD handle/object ID.
    registry.erase(key(s),kind);
    if(tag.known)registry.add(key(s),kind,tag.origin);
}
void untrack(const Service& s,Kind kind){std::scoped_lock lock(mutex);registry.erase(key(s),kind);}
class Call {
    Op op;
    uint64_t start{};
public:
    Tag tag;
    Call(Tag value,Op operation):op(operation),tag(value) {
        tag.origin=classify(tag.origin,current_origin);
        {std::scoped_lock lock(mutex);initialize();if(tag.known)begin(state.buckets[unsigned(tag.origin)],op);}
        start=now();
    }
    Call(const Service& s,Kind kind,Op operation,const char* path=nullptr):op(operation) {
        {std::scoped_lock lock(mutex);initialize();tag=registry.find(key(s),kind);
         tag.origin=classify(tag.origin,current_origin,path);
         if(tag.known)begin(state.buckets[unsigned(tag.origin)],op);
         else if(op==Op::Read)++state.unknown_read_calls;
         else if(op==Op::Write)++state.unknown_write_calls;}
        start=now();
    }
    void done(uint32_t result,uint64_t bytes=0) {
        const auto elapsed=now()-start;
        if(tag.known){std::scoped_lock lock(mutex);finish(state.buckets[unsigned(tag.origin)],op,elapsed,result,bytes);}
    }
};
}
void EnterRequest(){std::scoped_lock lock(mutex);initialize();++state.usb_requests;++state.usb_requests_active;}
void LeaveRequest(){std::scoped_lock lock(mutex);--state.usb_requests_active;}
void Snapshot(NexusSdIoSnapshot* out,uint64_t generation) {
    std::scoped_lock lock(mutex);initialize();
    state.magic=NEXUS_SD_IO_MAGIC;state.version=NEXUS_SD_IO_VERSION;
    state.size=sizeof(state);state.bucket_count=NEXUS_SD_IO_BUCKETS;state.coverage=31;
    state.sampled_ns=now();++state.sequence;state.registry_drops=registry.drops;
    state.media_generation=generation;*out=state;
}
}
using namespace nexus_sd_io;
extern "C" Result __real_fsOpenSdCardFileSystem(FsFileSystem* out);
extern "C" Result __wrap_fsOpenSdCardFileSystem(FsFileSystem* out) {
    Call call(Tag{true,Origin::Other},Op::Metadata);
    const auto rc=__real_fsOpenSdCardFileSystem(out);call.done(rc);
    if(!rc && true)track(out->s,Kind::FileSystem,call.tag);
    return rc;
}

extern "C" Result __real_fsOpenSdCardFileSystemFwd(Service* s, FsFileSystem* out);
extern "C" Result __wrap_fsOpenSdCardFileSystemFwd(Service* s, FsFileSystem* out) {
    Call call(Tag{true,Origin::Other},Op::Metadata);
    const auto rc=__real_fsOpenSdCardFileSystemFwd(s,out);call.done(rc);
    if(!rc && true)track(out->s,Kind::FileSystem,call.tag);
    return rc;
}

extern "C" Result __real_fsOpenContentStorageFileSystem(FsFileSystem* out, FsContentStorageId id);
extern "C" Result __wrap_fsOpenContentStorageFileSystem(FsFileSystem* out, FsContentStorageId id) {
    Call call(Tag{id==FsContentStorageId_SdCard,Origin::NativeContent},Op::Metadata);
    const auto rc=__real_fsOpenContentStorageFileSystem(out,id);call.done(rc);
    if(!rc)track(out->s,Kind::FileSystem,call.tag);
    return rc;
}

extern "C" Result __real_fsOpenContentStorageFileSystemFwd(Service* s, FsFileSystem* out, FsContentStorageId id);
extern "C" Result __wrap_fsOpenContentStorageFileSystemFwd(Service* s, FsFileSystem* out, FsContentStorageId id) {
    Call call(Tag{id==FsContentStorageId_SdCard,Origin::NativeContent},Op::Metadata);
    const auto rc=__real_fsOpenContentStorageFileSystemFwd(s,out,id);call.done(rc);
    if(!rc)track(out->s,Kind::FileSystem,call.tag);
    return rc;
}

extern "C" Result __real_fsFsOpenFile(FsFileSystem* fs, const char* path, u32 mode, FsFile* out);
extern "C" Result __wrap_fsFsOpenFile(FsFileSystem* fs, const char* path, u32 mode, FsFile* out) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsOpenFile(fs,path,mode,out);call.done(rc);
    if(!rc)track(out->s,Kind::File,call.tag);
    return rc;
}

extern "C" Result __real_fsFsOpenDirectory(FsFileSystem* fs, const char* path, u32 mode, FsDir* out);
extern "C" Result __wrap_fsFsOpenDirectory(FsFileSystem* fs, const char* path, u32 mode, FsDir* out) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsOpenDirectory(fs,path,mode,out);call.done(rc);
    if(!rc)track(out->s,Kind::Directory,call.tag);
    return rc;
}

extern "C" void __real_fsFsClose(FsFileSystem* p);
extern "C" void __wrap_fsFsClose(FsFileSystem* p) {
    untrack(p->s,Kind::FileSystem);
    __real_fsFsClose(p);
}

extern "C" void __real_fsFileClose(FsFile* p);
extern "C" void __wrap_fsFileClose(FsFile* p) {
    untrack(p->s,Kind::File);
    __real_fsFileClose(p);
}

extern "C" void __real_fsDirClose(FsDir* p);
extern "C" void __wrap_fsDirClose(FsDir* p) {
    untrack(p->s,Kind::Directory);
    __real_fsDirClose(p);
}

extern "C" Result __real_fsFileRead(FsFile* f, s64 off, void* buf, u64 size, u32 option, u64* actual);
extern "C" Result __wrap_fsFileRead(FsFile* f, s64 off, void* buf, u64 size, u32 option, u64* actual) {
    Call call(f->s,Kind::File,Op::Read);
    const auto rc=__real_fsFileRead(f,off,buf,size,option,actual);
    call.done(rc,(!rc && actual)?*actual:0);return rc;
}

extern "C" Result __real_fsFileWrite(FsFile* f, s64 off, const void* buf, u64 size, u32 option);
extern "C" Result __wrap_fsFileWrite(FsFile* f, s64 off, const void* buf, u64 size, u32 option) {
    Call call(f->s,Kind::File,Op::Write);
    const auto rc=__real_fsFileWrite(f,off,buf,size,option);
    call.done(rc,rc?0:size);return rc;
}

extern "C" Result __real_fsFileFlush(FsFile* f);
extern "C" Result __wrap_fsFileFlush(FsFile* f) {
    Call call(f->s,Kind::File,Op::Flush);
    const auto rc=__real_fsFileFlush(f);call.done(rc);return rc;
}

extern "C" Result __real_fsFileGetSize(FsFile* f, s64* out);
extern "C" Result __wrap_fsFileGetSize(FsFile* f, s64* out) {
    Call call(f->s,Kind::File,Op::Metadata);
    const auto rc=__real_fsFileGetSize(f,out);call.done(rc);return rc;
}

extern "C" Result __real_fsFileSetSize(FsFile* f, s64 size);
extern "C" Result __wrap_fsFileSetSize(FsFile* f, s64 size) {
    Call call(f->s,Kind::File,Op::Metadata);
    const auto rc=__real_fsFileSetSize(f,size);call.done(rc);return rc;
}

extern "C" Result __real_fsFsCommit(FsFileSystem* fs);
extern "C" Result __wrap_fsFsCommit(FsFileSystem* fs) {
    Call call(fs->s,Kind::FileSystem,Op::Flush);
    const auto rc=__real_fsFsCommit(fs);call.done(rc);return rc;
}

extern "C" Result __real_fsDirRead(FsDir* d, s64* count, size_t max_entries, FsDirectoryEntry* entries);
extern "C" Result __wrap_fsDirRead(FsDir* d, s64* count, size_t max_entries, FsDirectoryEntry* entries) {
    Call call(d->s,Kind::Directory,Op::Metadata);
    const auto rc=__real_fsDirRead(d,count,max_entries,entries);call.done(rc);return rc;
}

extern "C" Result __real_fsDirGetEntryCount(FsDir* d, s64* count);
extern "C" Result __wrap_fsDirGetEntryCount(FsDir* d, s64* count) {
    Call call(d->s,Kind::Directory,Op::Metadata);
    const auto rc=__real_fsDirGetEntryCount(d,count);call.done(rc);return rc;
}

extern "C" Result __real_fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 option);
extern "C" Result __wrap_fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 option) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsCreateFile(fs,path,size,option);call.done(rc);return rc;
}

extern "C" Result __real_fsFsGetEntryType(FsFileSystem* fs, const char* path, FsDirEntryType* out);
extern "C" Result __wrap_fsFsGetEntryType(FsFileSystem* fs, const char* path, FsDirEntryType* out) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsGetEntryType(fs,path,out);call.done(rc);return rc;
}

extern "C" Result __real_fsFsGetFileTimeStampRaw(FsFileSystem* fs, const char* path, FsTimeStampRaw* out);
extern "C" Result __wrap_fsFsGetFileTimeStampRaw(FsFileSystem* fs, const char* path, FsTimeStampRaw* out) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsGetFileTimeStampRaw(fs,path,out);call.done(rc);return rc;
}

extern "C" Result __real_fsFileOperateRange(FsFile* f, FsOperationId op, s64 off, s64 len, FsRangeInfo* out);
extern "C" Result __wrap_fsFileOperateRange(FsFile* f, FsOperationId op, s64 off, s64 len, FsRangeInfo* out) {
    Call call(f->s,Kind::File,Op::Metadata);
    const auto rc=__real_fsFileOperateRange(f,op,off,len,out);call.done(rc);return rc;
}

extern "C" Result __real_fsFsQueryEntry(FsFileSystem* fs, void* out, size_t out_size, const void* in, size_t in_size, const char* path, FsFileSystemQueryId id);
extern "C" Result __wrap_fsFsQueryEntry(FsFileSystem* fs, void* out, size_t out_size, const void* in, size_t in_size, const char* path, FsFileSystemQueryId id) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsQueryEntry(fs,out,out_size,in,in_size,path,id);call.done(rc);return rc;
}

extern "C" Result __real_fsFsDeleteFile(FsFileSystem* fs, const char* path);
extern "C" Result __wrap_fsFsDeleteFile(FsFileSystem* fs, const char* path) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsDeleteFile(fs,path);call.done(rc);return rc;
}

extern "C" Result __real_fsFsCreateDirectory(FsFileSystem* fs, const char* path);
extern "C" Result __wrap_fsFsCreateDirectory(FsFileSystem* fs, const char* path) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsCreateDirectory(fs,path);call.done(rc);return rc;
}

extern "C" Result __real_fsFsDeleteDirectory(FsFileSystem* fs, const char* path);
extern "C" Result __wrap_fsFsDeleteDirectory(FsFileSystem* fs, const char* path) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsDeleteDirectory(fs,path);call.done(rc);return rc;
}

extern "C" Result __real_fsFsDeleteDirectoryRecursively(FsFileSystem* fs, const char* path);
extern "C" Result __wrap_fsFsDeleteDirectoryRecursively(FsFileSystem* fs, const char* path) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsDeleteDirectoryRecursively(fs,path);call.done(rc);return rc;
}

extern "C" Result __real_fsFsCleanDirectoryRecursively(FsFileSystem* fs, const char* path);
extern "C" Result __wrap_fsFsCleanDirectoryRecursively(FsFileSystem* fs, const char* path) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsCleanDirectoryRecursively(fs,path);call.done(rc);return rc;
}

extern "C" Result __real_fsFsGetFreeSpace(FsFileSystem* fs, const char* path, s64* out);
extern "C" Result __wrap_fsFsGetFreeSpace(FsFileSystem* fs, const char* path, s64* out) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsGetFreeSpace(fs,path,out);call.done(rc);return rc;
}

extern "C" Result __real_fsFsGetTotalSpace(FsFileSystem* fs, const char* path, s64* out);
extern "C" Result __wrap_fsFsGetTotalSpace(FsFileSystem* fs, const char* path, s64* out) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,path);
    const auto rc=__real_fsFsGetTotalSpace(fs,path,out);call.done(rc);return rc;
}

extern "C" Result __real_fsFsRenameFile(FsFileSystem* fs, const char* old_path, const char* new_path);
extern "C" Result __wrap_fsFsRenameFile(FsFileSystem* fs, const char* old_path, const char* new_path) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,old_path);
    const auto rc=__real_fsFsRenameFile(fs,old_path,new_path);call.done(rc);return rc;
}

extern "C" Result __real_fsFsRenameDirectory(FsFileSystem* fs, const char* old_path, const char* new_path);
extern "C" Result __wrap_fsFsRenameDirectory(FsFileSystem* fs, const char* old_path, const char* new_path) {
    Call call(fs->s,Kind::FileSystem,Op::Metadata,old_path);
    const auto rc=__real_fsFsRenameDirectory(fs,old_path,new_path);call.done(rc);return rc;
}
