#include <stratosphere/kvdb/kvdb_memory_key_value_store.hpp>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace ams;
namespace {
size_t checks = 0;
size_t array_limit = SIZE_MAX;
size_t array_requests = 0;
size_t largest_array_request = 0;
void Check(bool condition, const char *what) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL check %zu: %s\n", checks, what); std::exit(1); }
}
constexpr Result IoFailure = 0x1234;
using Bytes = std::vector<u8>;
std::map<std::string, Bytes> files;
std::map<std::string, size_t> calls;
std::string fail_op;
size_t fail_nth = 0;
size_t open_handles = 0;
size_t max_read = 0, max_write = 0;
bool negative_size = false;
bool Fault(const char *op) {
    const auto nth = ++calls[op];
    return fail_op == op && nth == fail_nth;
}
void ResetFault(const char *op = "", size_t nth = 0) {
    calls.clear(); fail_op = op; fail_nth = nth; max_read = max_write = 0; negative_size = false;
}
struct Handle { std::string path; int mode; };

struct Resource : MemoryResource {
    struct Block { void *ptr; size_t size; bool live; };
    std::vector<Block> blocks;
    size_t used = 0, peak = 0, limit = SIZE_MAX, attempts = 0, fail_attempt = 0;
    void *Allocate(size_t size) override {
        ++attempts;
        if ((fail_attempt && attempts == fail_attempt) || size > limit - used) return nullptr;
        void *ptr = std::malloc(std::max(size, size_t{1}));
        Check(ptr != nullptr, "host allocation");
        blocks.push_back({ptr, size, true}); used += size; peak = std::max(peak, used);
        return ptr;
    }
    void Deallocate(void *ptr, size_t size) override {
        auto it = std::find_if(blocks.begin(), blocks.end(), [ptr](const auto &b) { return b.ptr == ptr; });
        Check(it != blocks.end() && it->live, "no invalid/double free");
        Check(size == it->size, "correct resource allocation size on free");
        it->live = false; used -= size; std::memset(ptr, 0xDD, size);
    }
    ~Resource() {
        Check(used == 0, "all resource blocks released");
        for (const auto &block : blocks) std::free(block.ptr);
    }
};
using Store = kvdb::MemoryKeyValueStore<u32>;
using Model = std::map<u32, Bytes>;
void Verify(const Store &store, const Model &model) {
    Check(store.GetCount() == model.size(), "store count");
    auto it = store.begin();
    for (const auto &[key, bytes] : model) {
        Check(it != store.end(), "entry exists");
        Check(it->GetKey() == key && it->GetValueSize() == bytes.size(), "key/order/value size");
        Check(bytes.empty() || std::memcmp(it->GetValuePointer(), bytes.data(), bytes.size()) == 0, "value bytes preserved");
        ++it;
    }
    Check(it == store.end(), "no extra entries");
}
Bytes Pattern(size_t size, u32 seed) {
    Bytes result(size);
    for (size_t i = 0; i < size; ++i) result[i] = static_cast<u8>((i * 37 + seed * 13) & 255);
    return result;
}
void Fill(Store &store, const Model &model) {
    for (const auto &[key, value] : model) Check(store.Set(key, value.data(), value.size()) == 0, "insert fixture");
}
void Put32(Bytes &bytes, size_t offset, u32 value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); }

void IndexTests() {
    {
        Resource resource;
        Store store;
        resource.fail_attempt = 1;
        Check(store.Initialize(8, &resource) == 0x814, "index allocation error");
    }
    for (u32 key : {u32{1}, u32{15}, u32{40}}) {
        Resource resource;
        Store store;
        Check(store.Initialize(8, &resource) == 0, "initialize insertion test");
        Model model{{10, Pattern(8, 10)}, {20, Pattern(8, 20)}, {30, Pattern(8, 30)}};
        Fill(store, model);
        const auto used = resource.used;
        resource.fail_attempt = resource.attempts + 1;
        const auto value = Pattern(16, key);
        Check(store.Set(key, value.data(), value.size()) == 0x814, "failed insertion result");
        Check(resource.used == used, "failed insertion allocation unchanged");
        Verify(store, model);
        resource.fail_attempt = 0;
        Check(store.Set(key, value.data(), value.size()) == 0, "retry insertion");
        model[key] = value;
        Verify(store, model);
    }
    {
        Resource resource;
        Store store;
        Check(store.Initialize(2, &resource) == 0, "initialize replacements");
        Model model{{1, Pattern(32, 1)}, {2, Pattern(16, 2)}};
        Fill(store, model);
        for (size_t size : {size_t{8}, size_t{64}}) {
            const auto bytes = Pattern(size, 3);
            resource.fail_attempt = resource.attempts + 1;
            Check(store.Set(1, bytes.data(), size) == 0x814, "resize failure result");
            Verify(store, model);
        }
        resource.limit = resource.used;
        const auto attempts = resource.attempts;
        const auto replacement = Pattern(32, 4);
        Check(store.Set(1, replacement.data(), replacement.size()) == 0, "same-sized update without free space");
        Check(resource.attempts == attempts, "same-sized update does not allocate");
        model[1] = replacement;
        Verify(store, model);
        const auto *self = store.find(1)->GetValuePointer();
        Check(store.Set(1, self, 32) == 0, "self-alias same-size update");
        Verify(store, model);
        resource.limit = SIZE_MAX; resource.fail_attempt = 0;
        const auto *slice = static_cast<const u8 *>(store.find(1)->GetValuePointer()) + 4;
        Bytes expected(slice, slice + 8);
        Check(store.Set(1, slice, 8) == 0, "alias resized update copies before free");
        model[1] = expected;
        Verify(store, model);
        const auto next_attempt = resource.attempts;
        Check(store.Set(3, expected.data(), expected.size()) == 0x214, "full index result");
        Check(resource.attempts == next_attempt, "full index does not allocate");
    }
    {
        Resource resource;
        Store store;
        Check(store.Initialize(32, &resource) == 0, "initialize randomized test");
        Model model;
        std::mt19937 rng(0x814);
        for (unsigned n = 0; n < 1000; ++n) {
            const u32 key = rng() % 32;
            if (rng() % 5 == 0) {
                const bool exists = model.count(key) != 0;
                Check(store.Remove(key) == (exists ? 0 : 0x414), "random remove");
                model.erase(key);
            } else {
                const auto value = Pattern(rng() % 96, rng());
                const bool fault = (rng() % 3 == 0);
                resource.fail_attempt = fault ? resource.attempts + 1 : 0;
                const bool reuse = model.count(key) && model[key].size() == value.size();
                const auto rc = store.Set(key, value.data(), value.size());
                Check(rc == ((fault && !reuse) ? 0x814 : 0), "random allocation failure result");
                if (rc == 0) model[key] = value;
            }
            resource.fail_attempt = 0;
            Verify(store, model);
        }
    }
}

void StreamingTests() {
    const std::string archive = "/db/imkvdb.arc", temporary = "/db/imkvdb.tmp";
    const Model model{{1, Pattern(7, 1)}, {3, Pattern(12, 3)}, {7, Pattern(31, 7)}};
    Bytes fixture;
    {
        Resource resource;
        Store store;
        Check(store.Initialize("/db", 8, &resource) == 0, "initialize file store");
        Fill(store, model);
        Check(store.Save() == 0, "legacy archive fixture save");
        fixture = files[archive];
        const auto arrays_before = array_requests;
        ResetFault();
        Check(store.SaveStreaming() == 0, "streaming archive save");
        Check(array_requests == arrays_before, "stream save has no array allocation");
        Check(files[archive] == fixture, "stream archive byte-identical to legacy codec");
        Check(open_handles == 0, "save closes file");
        const auto write_calls = calls["write"];
        for (const char *op : {"create", "open", "write", "flush"}) {
            const size_t count = std::strcmp(op, "write") == 0 ? write_calls : 1;
            for (size_t n = 1; n <= count; ++n) {
                files[archive] = fixture;
                ResetFault(op, n);
                Check(store.SaveStreaming() == IoFailure, "stream save propagates injected I/O failure");
                Check(files[archive] == fixture, "old archive preserved before successful temporary flush");
                Check(open_handles == 0, "failed save closes file");
                Verify(store, model);
            }
        }
        ResetFault("rename", 1);
        Check(store.SaveStreaming() == IoFailure, "rename failure never reported as success");
        Check(files[temporary] == fixture, "complete temp retained on rename failure");
        ResetFault(); files[archive] = fixture;
        Check(store.SaveStreaming(true) == 0, "explicit destructive save supported");
        Check(files[archive] == fixture, "destructive save same bytes");
    }
    {
        Resource resource;
        Store store;
        Check(store.Initialize("/db", 8, &resource) == 0, "initialize streaming reader");
        const auto index_bytes = resource.used;
        ResetFault(); files[archive] = fixture;
        auto arrays_before = array_requests;
        Check(store.LoadStreaming() == 0, "streaming load");
        Check(array_requests == arrays_before, "stream load has no array allocation");
        Verify(store, model);
        const auto read_calls = calls["read"];
        for (const char *op : {"open", "size", "read", "short_read"}) {
            const size_t count = (std::strcmp(op, "read") == 0 || std::strcmp(op, "short_read") == 0) ? read_calls : 1;
            for (size_t n = 1; n <= count; ++n) {
                files[archive] = fixture; ResetFault(op, n);
                const auto expected = std::strcmp(op, "short_read") == 0 ? 0x614 : IoFailure;
                Check(store.LoadStreaming() == expected, "read failure/error propagation");
                Check(store.GetCount() == 0 && resource.used == index_bytes, "failed load releases partial values");
                Check(open_handles == 0, "failed load closes file");
            }
        }
        for (size_t n = 1; n <= model.size(); ++n) {
            ResetFault(); files[archive] = fixture;
            resource.fail_attempt = resource.attempts + n;
            Check(store.LoadStreaming() == 0x814, "load value allocation failure");
            Check(store.GetCount() == 0 && resource.used == index_bytes, "allocation failure clears partial load");
            resource.fail_attempt = 0;
        }
        for (size_t length = 0; length < fixture.size(); ++length) {
            ResetFault(); files[archive] = Bytes(fixture.begin(), fixture.begin() + length);
            Check(store.LoadStreaming() != 0, "every truncated archive rejected");
            Check(store.GetCount() == 0 && resource.used == index_bytes, "truncation leaves no entries/leak");
            Check(open_handles == 0, "truncation closes file");
        }
        for (unsigned which = 0; which < 8; ++which) {
            Bytes malformed = fixture;
            if (which == 0) malformed[0] ^= 1;
            if (which == 1) malformed[12] ^= 1;
            if (which == 2) Put32(malformed, 16, 8);
            if (which == 3) Put32(malformed, 20, UINT32_MAX);
            if (which == 4) Put32(malformed, 8, UINT32_MAX);
            if (which == 5) Put32(malformed, 12 + 12 + 4 + 7 + 12, 1); // duplicate second key
            if (which == 6) Put32(malformed, 12 + 12 + 4 + 7 + 12, 0); // descending second key
            files[archive] = malformed; ResetFault();
            if (which == 7) negative_size = true;
            Check(store.LoadStreaming() != 0, "corrupt archive rejected");
            Check(store.GetCount() == 0 && resource.used == index_bytes, "corrupt load releases partial entries");
        }
        files[archive] = fixture; files[archive].insert(files[archive].end(), 64, 0); ResetFault();
        Check(store.LoadStreaming() == 0, "legacy trailing padding accepted"); Verify(store, model);
        files.erase(archive); ResetFault();
        Check(store.LoadStreaming() == 0 && store.GetCount() == 0, "missing archive is empty database");
        Check(open_handles == 0, "missing archive opens no handles");
        Check(store.SaveStreaming() == 0, "empty archive save");
        Check(files[archive].size() == 12, "empty archive wire size");
        Check(store.LoadStreaming() == 0 && store.GetCount() == 0, "empty archive load");
    }
    {
        Resource resource;
        Store store;
        Check(store.Initialize("/db", 1300, &resource) == 0, "large database initialize");
        Model large;
        for (u32 key = 0; key < 1200; ++key) large[key] = Pattern(key == 0 ? 192 * 1024 : 1024, key);
        Fill(store, large);
        ResetFault();
        Check(store.Save() == 0, "large legacy fixture save");
        const auto large_fixture = files[archive];
        Check(large_fixture.size() > 1024 * 1024, "fixture exceeds old NCM general heap");
        const auto resident_used = resource.used;
        Check(resident_used < 2 * 1024 * 1024 + 512 * 1024, "fixture fits unchanged resident database heap");
        array_limit = 1024 * 1024;
        Check(store.Save() == 0x814, "old buffered commit fails at 1 MiB scratch cap");
        Check(store.Load() == 0x814, "old buffered load fails at 1 MiB scratch cap");
        const auto attempts_before = array_requests;
        ResetFault();
        Check(store.LoadStreaming() == 0, "stream load succeeds with old scratch allocation cap");
        Verify(store, large);
        Check(max_read <= 64 * 1024 && max_read == 64 * 1024, "bounded direct read chunks");
        ResetFault();
        Check(store.SaveStreaming() == 0, "stream commit succeeds with old scratch allocation cap");
        Check(max_write <= 64 * 1024 && max_write == 64 * 1024, "bounded direct write chunks");
        Check(array_requests == attempts_before, "large stream load/save allocate no archive scratch");
        Check(files[archive] == large_fixture, "large archive matches legacy bytes");
        Check(resource.used == resident_used, "streaming leaves resident memory unchanged");
        Check(open_handles == 0, "large load/save close handles");
        array_limit = SIZE_MAX;
    }
}
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept {
    ++array_requests; largest_array_request = std::max(largest_array_request, size);
    if (size > array_limit) return nullptr;
    return std::malloc(std::max(size, size_t{1}));
}
void operator delete[](void *ptr) noexcept { std::free(ptr); }
void operator delete[](void *ptr, size_t) noexcept { std::free(ptr); }

namespace ams::fs {
Result GetEntryType(DirectoryEntryType *out, const char *path) {
    if (std::string(path) == "/db") { *out = DirectoryEntryType_Directory; return 0; }
    if (!files.count(path)) return ResultPathNotFound();
    *out = DirectoryEntryType_File; return 0;
}
Result OpenFile(FileHandle *out, const char *path, int mode) {
    if (Fault("open")) return IoFailure;
    if (!files.count(path)) return ResultPathNotFound();
    out->handle = new Handle{path, mode}; ++open_handles; return 0;
}
void CloseFile(FileHandle file) { Check(open_handles != 0, "valid file close"); --open_handles; delete static_cast<Handle *>(file.handle); }
Result GetFileSize(s64 *out, FileHandle file) {
    if (Fault("size")) return IoFailure;
    *out = negative_size ? -1 : static_cast<s64>(files.at(static_cast<Handle *>(file.handle)->path).size()); return 0;
}
Result ReadFile(size_t *out, FileHandle file, s64 offset, void *dst, size_t size) {
    max_read = std::max(max_read, size);
    if (Fault("read")) return IoFailure;
    auto &bytes = files.at(static_cast<Handle *>(file.handle)->path);
    if (offset < 0 || static_cast<u64>(offset) > bytes.size()) return IoFailure;
    auto actual = std::min(size, bytes.size() - static_cast<size_t>(offset));
    if (Fault("short_read") && actual) --actual;
    std::memcpy(dst, bytes.data() + offset, actual); *out = actual; return 0;
}
Result ReadFile(FileHandle file, s64 offset, void *dst, size_t size) { size_t actual; const auto rc = ReadFile(&actual, file, offset, dst, size); return rc ? rc : (actual == size ? 0 : IoFailure); }
Result WriteFile(FileHandle file, s64 offset, const void *src, size_t size, const WriteOption &) {
    max_write = std::max(max_write, size);
    const bool fail = Fault("write");
    auto &bytes = files.at(static_cast<Handle *>(file.handle)->path);
    if (offset < 0 || static_cast<u64>(offset) > bytes.size() || size > bytes.size() - static_cast<size_t>(offset)) return IoFailure;
    std::memcpy(bytes.data() + offset, src, fail ? size / 2 : size);
    return fail ? IoFailure : 0;
}
Result FlushFile(FileHandle) { return Fault("flush") ? IoFailure : 0; }
Result DeleteFile(const char *path) { if (Fault("delete")) return IoFailure; return files.erase(path) ? 0 : Result(ResultPathNotFound()); }
Result CreateFile(const char *path, s64 size) { if (Fault("create")) return IoFailure; if (size < 0 || files.count(path)) return IoFailure; files[path] = Bytes(static_cast<size_t>(size)); return 0; }
Result RenameFile(const char *from, const char *to) { if (Fault("rename")) return IoFailure; if (!files.count(from) || files.count(to)) return IoFailure; files[to] = std::move(files[from]); files.erase(from); return 0; }
}

int main() {
    IndexTests(); StreamingTests();
    Check(open_handles == 0, "no leaked filesystem handles");
    std::printf("PASS %zu checks: production KVDB failure atomicity, allocation-free equal-size updates, legacy-compatible streaming archive, malformed/short I/O, partial-load cleanup and >1 MiB allocation regression.\n", checks);
    std::printf("Largest legacy archive allocation attempted: %zu bytes; streaming archive array allocations: zero.\n", largest_array_request);
    return 0;
}
