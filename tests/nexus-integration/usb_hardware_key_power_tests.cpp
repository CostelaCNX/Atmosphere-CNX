/* SPDX-License-Identifier: GPL-2.0-only */
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using u8 = std::uint8_t;
using s32 = std::int32_t;
using Result = int;
#define NON_COPYABLE(T) T(const T &) = delete; T &operator=(const T &) = delete
#define NON_MOVEABLE(T) T(T &&) = delete; T &operator=(T &&) = delete
#define AMS_ASSERT(expr) assert(expr)
#define AMS_ABORT_UNLESS(expr) assert(expr)
#define AMS_UNUSED(...) static_cast<void>(std::make_tuple(__VA_ARGS__))
#define R_RETURN(expr) return (expr)
#define R_THROW(expr) return (expr)
#define R_SUCCEED() return 0
#define R_UNLESS(expr, rc) do { if (!(expr)) return (rc); } while (false)
#define R_ABORT_UNLESS(rc) assert((rc) == 0)
#define R_TRY_CATCH(rc) if (const auto caught = (rc); caught)
#define R_CATCH(type) if (caught == type())
#define R_END_TRY_CATCH_WITH_ABORT_UNLESS

namespace ams {
namespace crypto { struct AesDecryptor128 { static constexpr size_t KeySize = 16; }; }
namespace os { using SdkMutex = std::mutex; }
namespace fs {
    namespace impl { struct Newable {}; }
    constexpr Result ResultTargetNotFound() { return 1; }
    constexpr Result ResultAllocationMemoryFailed() { return 2; }
}
namespace util {
    using std::unique_lock;
    template<class T> struct IntrusiveListBaseNode {};
    // A host stand-in for the small intrusive MRU lists. The production cache
    // controls item order, mutex ownership, selection, hits and invalidation.
    template<class T> struct IntrusiveListBaseTraits {
        struct ListType {
            std::array<T*, 3> items{};
            size_t count{};
            struct iterator {
                T **ptr;
                T &operator*() const { return **ptr; }
                T *operator->() const { return *ptr; }
                iterator &operator++() { ++ptr; return *this; }
                bool operator==(const iterator &) const = default;
            };
            iterator begin() { return {items.data()}; }
            iterator end() { return {items.data() + count}; }
            iterator rbegin() { assert(count); return {items.data() + count - 1}; }
            bool empty() const { return count == 0; }
            void pop_back() { assert(count); --count; }
            void erase(iterator it) {
                std::move(it.ptr + 1, end().ptr, it.ptr);
                --count;
            }
            void push_front(T &entry) {
                assert(count < items.size());
                std::move_backward(items.begin(), items.begin() + count, items.begin() + count + 1);
                items[0] = &entry;
                ++count;
            }
        };
    };
}
}
#include "fssystem_key_slot_cache.hpp"
#include "fssystem_hardware_key_power.hpp"

namespace ams::spl {
    struct AccessKey { u8 bytes[16]; };
    std::array<int,3> physical{};
    int internal_loads{}, prepared_loads{};
    Result LoadAesKey(int slot, int kek, const void *source, size_t n) {
        assert(n == 16); physical.at(slot) = *static_cast<const u8*>(source) + kek;
        ++internal_loads; return 0;
    }
    Result LoadPreparedAesKey(int slot, const AccessKey &access) {
        physical.at(slot) = access.bytes[0]; ++prepared_loads; return 0;
    }
}
namespace ams::fssystem {
    KeySlotCache g_key_slot_cache;
    enum class KeyType : int { NcaExternalKey=1000 };
    int GetKeyTypeValue(u8 key_index, u8 key_generation) { return key_index + 3 * key_generation; }
    int GetNcaKekAccessKey(int key_type) { return key_type; }
    std::atomic_bool pause_compute{}, inside_compute{};
    void ComputeCtr(void *dst, size_t dst_size, int slot, const void *src, size_t src_size, const void*, size_t) {
        assert(dst_size == src_size && dst_size == 1);
        inside_compute.store(true);
        while (pause_compute.load()) std::this_thread::yield();
        *static_cast<u8*>(dst) = *static_cast<const u8*>(src) ^ spl::physical.at(slot);
        inside_compute.store(false);
    }
    #include "crypto_functions.inc"
}
int main() {
    using namespace ams;
    using namespace ams::fssystem;
    KeySlotCacheEntry a(0), b(1), c(2);
    for (auto *entry : {&a,&b,&c}) g_key_slot_cache.AddEntry(entry);
    // Two readers, one using a prepared access key and one an internal key.
    const std::array<u8,16> fixture{{17}};
    const u8 input = 71;
    const auto read = [&](bool external) {
        u8 output{};
        auto fn = external ? DecryptAesCtrForPreparedKey : DecryptAesCtr;
        fn(&output, 1, 1, 1, fixture.data(), fixture.size(), nullptr, 0, &input, 1);
        assert(output == (input ^ (external ? 17 : 21)));
    };
    assert(!EndHardwareKeySleep());
    for (int n=0;n<3;++n) { read(true); read(false); }
    assert(spl::prepared_loads == 1 && spl::internal_loads == 1);

    // The suspend callback cannot report completion while an accessor is still
    // computing under its old slot binding.
    pause_compute.store(true);
    auto inflight = std::async(std::launch::async,[&]{ read(true); });
    while (!inside_compute.load()) std::this_thread::yield();
    std::promise<void> calling;
    auto prepare = std::async(std::launch::async,[&]{ calling.set_value(); BeginHardwareKeySleep(); });
    calling.get_future().wait();
    assert(prepare.wait_for(std::chrono::milliseconds(25)) == std::future_status::timeout);
    pause_compute.store(false);
    inflight.get(); prepare.get();
    BeginHardwareKeySleep();
    // LP0 loses the physical slot contents. These two reads already had
    // ciphertext in memory and run before the USB recovery worker is started.
    spl::physical.fill(0);
    read(true); read(false); read(true); read(false);
    assert(spl::prepared_loads == 3 && spl::internal_loads == 3);
    // Invalidate every binding, including those populated during transition.
    assert(EndHardwareKeySleep());
    assert(!EndHardwareKeySleep());
    spl::physical.fill(0);
    read(true); read(false); read(true); read(false);
    assert(spl::prepared_loads == 4 && spl::internal_loads == 4);
    for (int i=0;i<32;++i) {
        BeginHardwareKeySleep();
        spl::physical.fill(0);
        assert(EndHardwareKeySleep());
        read(true); read(false);
    }
    assert(spl::prepared_loads == 36 && spl::internal_loads == 36);
    std::puts("PASS prepared and internal hardware reload, early ciphertext, stationary cache hits, active lease drain, repeated wake and retry");
}
