// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>

using Result = int;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using LBA_t = u64;
#define R_TRY(expr) do { Result rc_ = (expr); if (rc_) return rc_; } while (false)
#define R_UNLESS(cond, rc) do { if (!(cond)) return (rc); } while (false)
#define R_RETURN(rc) return (rc)
#define R_THROW(rc) return (rc)
#define R_SUCCEED() return 0
#define R_SUCCEED_IF(cond) do { if (cond) return 0; } while (false)
constexpr Result ResultSuccess() { return 0; }
struct OnExit {
    template<class F> struct Guard { F f; ~Guard() { f(); } };
    template<class F> Guard<F> operator+(F f) { return {f}; }
};
#define ON_SCOPE_EXIT auto guard = OnExit{} + [&]
namespace usb { constexpr int ResultNotInitialized() {return 9;} }
namespace fs {
enum class OperationId { Invalidate, QueryRange, Unknown };
constexpr Result ResultOutOfRange() {return 1;}
constexpr Result ResultInvalidSize() {return 2;}
constexpr Result ResultNullptrArgument() {return 3;}
constexpr Result ResultNotImplemented() {return 4;}
struct QueryRangeInfo {
    std::array<unsigned,16> flags;
    void Clear() {flags.fill(0);}
    bool IsFilled(unsigned v) const {return std::all_of(flags.begin(), flags.end(), [v](unsigned n){return n == v;});}
};
}
namespace fatfs {
struct CleanSectors {bool hit = true; void Clear() {hit = false;}} g_clean_sectors;
struct Context {u64 sector_size = 512;} g_context;
namespace write_cache_diagnostics {u64 last_sector_size; void Clear(u64 size) {last_sector_size = size;}}
void DiscardCleanSectors()
#include "discard_sectors.inc"
}
struct FileHandle {
    bool open = true;
    int cached_data = 1;
    int close_error = 0;
};
Result CloseFile(FileHandle* file, Result previous) {
    if (!file->close_error) file->open = false;
    return previous ? previous : file->close_error;
}
struct WritableVolume {
    struct Reader {FileHandle handle;};
    struct Impl {
        struct {bool wflag = false; LBA_t winsect = 1;} filesystem;
        std::array<Reader,2> readers;
        FileHandle writer;
        bool metadata_cached = true;
        void ClearMetadataCache() {metadata_cached = false;}
        Result CloseReaders()
#include "close_readers.inc"
    } impl;
    Impl* m_impl = &impl;
    bool IsInitialized() const {return m_impl != nullptr;}
    Result InvalidateReadCache()
#include "volume_invalidate.inc"
};
struct Storage {
    WritableVolume m_writable_volume;
    bool m_writable_ready = true;
    bool ready = true;
    u64 generation = 7;
    std::mutex m_volume_mutex;
    int admission_error = 0;
    int active = 0;
    int last_io = 0;
    bool changed_on_wake = false;
    Result BeginSystemOperation() {
        if (admission_error) return admission_error;
        if (changed_on_wake) ++generation;
        ++active;
        return 0;
    }
    void EndSystemOperation() {--active;}
    Result CheckMediaGenerationLocked(u64 expected) const {return expected == generation && ready ? 0 : 19;}
    void CompleteIoLocked(Result result) {last_io = result;}
    Result InvalidateReadCacheAtGeneration(u64 media_generation)
#include "service_invalidate.inc"
};
struct Facade {
    Storage* m_storage;
    bool m_mounted = true;
    std::mutex m_mutex;
    Result InvalidateReadCacheAtGeneration(u64 media_generation)
#include "facade_invalidate.inc"
};
struct ReadOnlyFile {
    Facade *m_service;
    u64 m_media_generation = 7;
    Result DoGetSize(s64 *out) {
        R_TRY(m_service->m_storage->CheckMediaGenerationLocked(m_media_generation));
        *out = 128;
        return 0;
    }
    Result Operate(void *dst, size_t dst_size, fs::OperationId op_id, s64 offset, s64 size)
#include "file_op.inc"
};

static void QueryRawRange() {
    Storage storage;
    Facade facade{&storage,true,{}};
    ReadOnlyFile file{&facade};
    fs::QueryRangeInfo info;
    auto query = [&](s64 start, s64 length) {
        return file.Operate(&info, sizeof(info), fs::OperationId::QueryRange, start, length);
    };
    info.flags.fill(0xff);
    assert(!query(0,128) && info.IsFilled(0));
    assert(!query(128,0));
    info.flags.fill(14);
    assert(query(-1,8) == fs::ResultOutOfRange());
    assert(query(129,0) == fs::ResultOutOfRange());
    assert(query(128,1) == fs::ResultInvalidSize());
    assert(query(1,INT64_MAX) == fs::ResultInvalidSize());
    assert(query(8,-1) == fs::ResultInvalidSize());
    assert(file.Operate(nullptr,sizeof(info),fs::OperationId::QueryRange,0,1) == fs::ResultNullptrArgument());
    assert(file.Operate(&info,sizeof(info)-1,fs::OperationId::QueryRange,0,1) == fs::ResultInvalidSize());
    ++storage.generation;
    assert(query(0,1) == 19 && info.IsFilled(14));
    assert(file.Operate(nullptr,0,fs::OperationId::Unknown,0,1) == fs::ResultNotImplemented());
    std::puts("PASS range metadata, error buffers, EOF, overflow and revoked generation");
}
static void InvalidateAfterWake() {
    Storage storage;
    Facade facade{&storage,true,{}};
    ReadOnlyFile file{&facade};
    auto invalidate = [&]{return file.Operate(nullptr,0,fs::OperationId::Invalidate,0,INT64_MAX);};
    auto &volume = storage.m_writable_volume.impl;
    // Two aliases can both have a FIL cursor. Neither can serve its old byte after invalidation.
    auto read_alias = [&](unsigned n){auto &h=volume.readers[n].handle; if(!h.open){h.cached_data=2;h.open=true;}return h.cached_data;};
    assert(read_alias(0)==1 && read_alias(1)==1);
    assert(!invalidate() && !volume.metadata_cached && !fatfs::g_clean_sectors.hit);
    assert(fatfs::write_cache_diagnostics::last_sector_size==512);
    assert(read_alias(0)==2 && read_alias(1)==2 && volume.filesystem.winsect==UINT64_MAX);
    volume.filesystem.wflag=true;
    volume.filesystem.winsect=77;
    volume.writer.cached_data=42;
    volume.readers[0].handle.close_error=91;
    assert(invalidate()==91 && storage.active==0 && storage.last_io==91);
    assert(volume.writer.open && volume.writer.cached_data==42);
    assert(volume.filesystem.winsect==77 && volume.filesystem.wflag);
    volume.readers[0].handle.close_error=0;
    storage.m_writable_ready=false;
    assert(!invalidate() && storage.active==0 && volume.readers[0].handle.open);
    storage.m_writable_ready=true;
    volume.metadata_cached=true;
    storage.admission_error=82;
    assert(invalidate()==82 && volume.metadata_cached);
    storage.admission_error=0;
    storage.changed_on_wake=true;
    assert(invalidate()==19 && storage.active==0 && volume.metadata_cached && volume.readers[0].handle.open);
    facade.m_mounted=false;
    assert(invalidate()==usb::ResultNotInitialized());
    std::puts("PASS invalidate drops clean reader aliases, preserves pending writes and rejects stale handles after admission");
}
int main() { QueryRawRange(); InvalidateAfterWake(); }
