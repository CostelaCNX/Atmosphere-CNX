/* SPDX-License-Identifier: GPL-2.0-only */
#include "usb_wake_compare.hpp"
#include "usb_game_wake_trace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <utility>

namespace wc = ams::mitm::usb_storage::wake_compare;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s64 = std::int64_t;
using u8 = std::uint8_t;
struct Result {
    std::uint32_t value{};
    constexpr Result(std::uint32_t rc = 0) : value(rc) {}
    constexpr std::uint32_t GetValue() const { return value; }
};
#define R_RETURN(rc) return (rc)
#define R_SUCCEEDED(rc) (!(rc).GetValue())
#define R_FAILED(rc) (!R_SUCCEEDED(rc))
constexpr Result ResultSuccess() { return Result{}; }
#define R_TRY(rc) do { auto _result = (rc); if (R_FAILED(_result)) return _result; } while (false)
#define R_UNLESS(ok, rc) do { if (!(ok)) return (rc); } while (false)
#define R_SUCCEED() return ResultSuccess()
constexpr std::uint32_t ReportedError = 0x0023f802;
constexpr std::uint32_t OtherError = 0x123402;
constexpr std::int64_t ReportedOffset = 1970355808;
constexpr std::size_t ReportedSize = 32;

struct ReadCapture {
    void *destination{};
    s64 offset = -1;
    std::size_t size{};
};
struct TestStorage {
    ReadCapture *capture;
    u32 result;
    u8 fill;
    Result Read(s64 offset, void *buffer, std::size_t size) {
        *capture = {buffer, offset, size};
        std::memset(buffer, fill, size);
        return result;
    }
};
namespace ams::fs {
    using IStorage = TestStorage;
    struct ResultIntegrityVerificationStorageCorrupted {
        static bool Includes(Result r) { return r.GetValue() == ReportedError; }
    };
    constexpr Result ResultPathNotFound() { return Result{0x202}; }
}

namespace fssystem::NcaFsHeader { enum class FsType { RomFs }; }
namespace ncm {
    struct ProgramId { u64 value; };
    constexpr u64 InvalidContentId = 0;
    struct IdString { char data[33]; };
    IdString GetContentIdString(u64 id) {
        IdString out{};
        std::snprintf(out.data, sizeof(out.data), "%032llx", static_cast<unsigned long long>(id));
        return out;
    }
}
namespace util { constexpr auto SNPrintf = std::snprintf; }
namespace system_power {
    std::atomic_bool paused{}, failed{};
    bool IsBackgroundPaused() { return paused.load(); }
    bool HasFailed() { return failed.load(); }
}
namespace os { using SdkMutex = std::mutex; }
namespace ams::mitm::usb_storage {
    struct UsbLaunchSnapshot {
        u64 generation;
        struct Content { u64 program_id, base, patch; } content;
    };
    using LaunchPin = std::shared_ptr<const UsbLaunchSnapshot>;
    struct Comparison
    #include "comparison.inc"
    ;
    os::SdkMutex g_comparison_mutex;
    Comparison g_comparison;
    std::atomic<u32> g_comparison_revision{};
    LaunchPin live_binding;
    std::atomic<unsigned> opens{};
    std::atomic<u64> generation{1};
    std::atomic_bool hold_open{}, open_started{};
    u32 fresh_result{}, open_result{};
    ReadCapture original_capture, probe_capture;
    std::vector<const UsbLaunchSnapshot *> opened_snapshots;

    LaunchPin FindProcessContent(u64 pid, ncm::ProgramId program) {
        return pid == 51 && live_binding && live_binding->content.program_id == program.value ? live_binding : nullptr;
    }
    Result CheckLaunchGeneration(const UsbLaunchSnapshot &snapshot) {
        return snapshot.generation == generation.load() ? ResultSuccess() : Result{OtherError};
    }
    Result OpenLaunchProgramStorage(std::shared_ptr<TestStorage> *out, void *,
                                    const UsbLaunchSnapshot &pin, fssystem::NcaFsHeader::FsType type,
                                    const LaunchPin &witness_pin = {}) {
        assert(type == fssystem::NcaFsHeader::FsType::RomFs);
        assert(!witness_pin || witness_pin.get() == &pin);
        const bool original = opens.fetch_add(1) == 0;
        opened_snapshots.push_back(&pin);
        if (!original) {
            open_started.store(true);
            while (hold_open.load()) std::this_thread::yield();
            if (open_result) return open_result;
        }
        *out = std::make_shared<TestStorage>(TestStorage{original ? &original_capture : &probe_capture,
                    original ? ReportedError : fresh_result, static_cast<u8>(original ? 0x14 : 0xf6)});
        return 0;
    }
    Result OpenPinnedProcessRomFs(std::shared_ptr<ams::fs::IStorage> *out, LaunchPin *pin,
                                  u64 process_id, ncm::ProgramId program_id)
    #include "original_open.inc"
    void QueueWakeComparison(const LaunchPin &pin, u32 open_epoch, u32 read_epoch, s64 offset,
                             size_t size, Result original)
    #include "queue.inc"
    void RunWakeComparison()
    #include "consume.inc"
    // The dedicated patch metadata fixture exercises the appended report.
    [[maybe_unused]] int AppendPatchWitness(char *, size_t, int used) { return used; }
    int wake_compare::Format(char *out, size_t capacity)
    #include "format.inc"
}
namespace ams::mitm::usb_storage {
    constexpr int FsUsbStorageFirstRead = 1;
    void RecordLaunchBoundaryTrace(int, u64, int, Result, const char *) {}
}

namespace ams::mitm::fs {
    namespace afs = ams::fs;
    struct DomainFixture {
        struct Object { unsigned GetId() const { return 19; } };
        Object object{};
        Object *m_object = &object;
        std::atomic_bool m_read_traced{false};
        std::shared_ptr<TestStorage> m_base;
        u64 m_program_id{};
        usb_storage::LaunchPin m_launch_pin;
        u32 m_open_epoch = usb_storage::game_wake::wake_epoch.load();
        template<class Fn> Result Trace(usb_storage::game_wake::Op, u32, s64, s64, Fn fn) {
            return fn();
        }
        Result Read(s64 offset, void *buffer, size_t size)
        #include "domain_read.inc"
    };
}

namespace ams::usb::sleep_diagnostics {
    std::atomic<u64> resumes{}, generation_before{}, generation_after{};
    std::atomic_bool marker_armed{};
}
namespace ams::mitm::usb_storage::power_fixture {
    namespace sd = ::ams::usb::sleep_diagnostics;
    enum class JobState { Queued, Running, Finished };
    JobState g_job{};
    bool g_quit{};
    std::mutex g_recovery_mutex;
    struct Condition { void Wait(std::mutex &) { assert(false); } } g_recovery_cv;
    u32 g_job_result{};
    std::atomic<u32> g_report_revision{};
    bool restored_before_resume{};
    void RestoreHardwareKeyCache() { restored_before_resume = true; }
    struct PowerService {
        u32 result{};
        bool actual_resume{};
        Result ResumeSystemSleep() const {
            assert(restored_before_resume);
            if (actual_resume) ++sd::resumes;
            g_quit = true;  // Execute exactly one iteration of the real recovery worker.
            return result;
        }
    } power;
    PowerService *GetUsbContentStorageService() { return &power; }
    void Recover(void *)
    #include "recover.inc"

    void CheckPowerProof() {
        for (int bad = 0; bad < 6; ++bad) {
            const auto epoch = ++game_wake::wake_epoch;
            wc::ready_epoch = 0;
            g_job = JobState::Queued;
            g_quit = false;
            restored_before_resume = false;
            power = {bad == 1 ? ReportedError : 0, bad != 2};
            sd::marker_armed = bad != 3;
            sd::generation_before = bad == 4 ? 0 : 1;
            sd::generation_after = bad == 5 ? 2 : sd::generation_before.load();
            Recover(nullptr);
            assert(g_job == JobState::Finished && g_job_result == power.result);
            assert(wc::ready_epoch == (bad == 0 ? epoch : 0U));
        }
        std::puts("PASS recovery worker requires a completed real resume, marker and same nonzero media generation before comparison");
    }
}

void CheckFreshVerification() {
    unsigned opens = 0;
    unsigned reads = 0;
    bool generation_valid = true;
    wc::Results detail{};
    auto valid = [&] { return generation_valid; };
    auto open = [&] { ++opens; return 0U; };
    auto read = [&] { ++reads; return 0U; };
    assert(wc::Compare(detail, valid, open, read) == wc::Stage::Verified);
    assert(opens == 1 && reads == 1 && detail.open == 0 && detail.read == 0);
    detail = {};
    auto bad_open = [&] { ++opens; return OtherError; };
    assert(wc::Compare(detail, valid, bad_open, read) == wc::Stage::OpenFailed);
    assert(opens == 2 && reads == 1 && detail.open == OtherError && detail.read == wc::NotAttempted);
    detail = {};
    auto bad_read = [&] { ++reads; return ReportedError; };
    assert(wc::Compare(detail, valid, open, bad_read) == wc::Stage::ReadFailed);
    assert(opens == 3 && reads == 2 && detail.open == 0 && detail.read == ReportedError);

    // A new sleep or a changed medium may invalidate the snapshot at any step.
    generation_valid = false;
    detail = {};
    assert(wc::Compare(detail, valid, open, read) == wc::Stage::Stale);
    assert(opens == 3 && reads == 2 && detail.open == wc::NotAttempted && detail.read == wc::NotAttempted);
    generation_valid = true;
    auto change_during_open = [&] { ++opens; generation_valid = false; return 0U; };
    assert(wc::Compare(detail, valid, change_during_open, read) == wc::Stage::Stale);
    assert(opens == 4 && reads == 2 && detail.open == 0 && detail.read == wc::NotAttempted);
    generation_valid = true;
    auto change_during_read = [&] { ++reads; generation_valid = false; return 0U; };
    assert(wc::Compare(detail, valid, open, change_during_read) == wc::Stage::Stale);
    assert(opens == 5 && reads == 3 && detail.open == 0 && detail.read == 0);
    std::puts("PASS independent verification, open/read error distinction and media/wake changes at each I/O boundary");
}

void CheckAdmission() {
    assert(wc::Eligible(0, 1, 1, ReportedOffset, ReportedSize));
    assert(wc::Eligible(0, 2, 2, INT64_MAX - 1, 1));
    assert(!wc::Eligible(0, 0, 0, 0, 1));
    assert(!wc::Eligible(1, 1, 1, 0, 1));
    assert(!wc::Eligible(2, 1, 1, 0, 1));
    assert(!wc::Eligible(0, 2, 1, 0, 1));
    assert(!wc::Eligible(0, 1, 2, 0, 1));
    assert(!wc::Eligible(0, 1, 1, -1, 1));
    assert(!wc::Eligible(0, 1, 1, 0, 0));
    assert(!wc::Eligible(0, 1, 1, INT64_MAX, 1));
    assert(!wc::Eligible(0, 1, 1, 1, SIZE_MAX));
    std::puts("PASS retained handle, current wake, signed range overflow and empty request admission");
}

namespace us = ams::mitm::usb_storage;
void CheckRetainedHandle() {
    using namespace us;
    constexpr u64 title = 0x0100fd8022daa000;
    auto original_pin = std::make_shared<UsbLaunchSnapshot>(UsbLaunchSnapshot{1, {title, 23, 32}});
    live_binding = original_pin;
    ams::mitm::fs::DomainFixture handle;
    assert(!OpenPinnedProcessRomFs(&handle.m_base, &handle.m_launch_pin, 51, {title}).GetValue());
    assert(handle.m_launch_pin == original_pin && handle.m_open_epoch == 0);
    // Modify the dispatch mapping to a different update of the *same* title.
    // The diagnostic must retain the actual original base and patch snapshot.
    live_binding = std::make_shared<UsbLaunchSnapshot>(UsbLaunchSnapshot{1, {title, 23, 99}});
    std::array<u8, ReportedSize + 2> bytes{};
    auto data = bytes.data() + 1;
    auto read = [&] { return handle.Read(ReportedOffset, data, ReportedSize).GetValue(); };
    assert(read() == ReportedError && g_comparison.stage == wc::Stage::Idle);
    game_wake::BeginWake();
    handle.m_base->result = 0;
    assert(read() == 0 && g_comparison.stage == wc::Stage::Idle);
    handle.m_base->result = OtherError;
    assert(read() == OtherError && g_comparison.stage == wc::Stage::Idle);
    handle.m_base->result = ReportedError;
    handle.m_launch_pin.reset();  // Unpinned layered/native storage.
    assert(read() == ReportedError && g_comparison.stage == wc::Stage::Idle);
    handle.m_launch_pin = original_pin;
    assert(read() == ReportedError && g_comparison.stage == wc::Stage::Queued && opens == 1);
    wc::ready_epoch = game_wake::wake_epoch.load();
    const auto caller_bytes = bytes;
    assert(bytes.front() == 0 && bytes.back() == 0 && bytes[1] == 0x14);
    RunWakeComparison();
    assert(opens == 2 && opened_snapshots[1] == opened_snapshots[0] && opened_snapshots[1] == original_pin.get());
    assert(g_comparison.stage == wc::Stage::Verified && handle.m_base->result == ReportedError);
    assert(original_capture.offset == probe_capture.offset && probe_capture.offset == ReportedOffset);
    assert(original_capture.size == probe_capture.size && probe_capture.size == ReportedSize);
    assert(original_capture.destination == data && probe_capture.destination != data && bytes == caller_bytes);
    // The game can retry and still receives the retained handle and its original error.
    assert(read() == ReportedError);
    RunWakeComparison();
    assert(opens == 2 && bytes == caller_bytes);
    std::array<char, 512> report{};
    assert(wc::Format(report.data(), report.size()) > 0);
    assert(std::strstr(report.data(), "usb_romfs_compare=verified\n"));
    assert(std::strstr(report.data(), "usb_romfs_compare_base=00000000000000000000000000000017\n"));
    assert(std::strstr(report.data(), "usb_romfs_compare_patch=00000000000000000000000000000020\n"));
    assert(std::strstr(report.data(), "usb_romfs_compare_offset=1970355808\n"));
    assert(std::strstr(report.data(), "usb_romfs_compare_size=32\n"));
    assert(std::strstr(report.data(), "usb_romfs_compare_original=0x0023F802\n"));
    assert(std::strstr(report.data(), "usb_romfs_compare_read=0x00000000\n"));
    std::puts("PASS two independent handles from same immutable base+patch; real SL5 offset and size; caller's bytes and return code preserved");
}

void CheckCapacityAndConcurrency() {
    using namespace us;
    const auto pin = g_comparison.pin;
    auto wake = [&] { game_wake::BeginWake(); wc::ready_epoch = game_wake::wake_epoch.load(); };
    auto queue = [&](s64 offset, size_t size) {
        QueueWakeComparison(pin, 0, wc::ready_epoch.load(), offset, size, ReportedError);
    };
    wake();
    queue(0, wc::MaxRead + 1);
    RunWakeComparison();
    assert(g_comparison.stage == wc::Stage::TooLarge && opens == 2);
    constexpr size_t Threads = 16;
    std::barrier gate(Threads);
    std::vector<std::thread> workers;
    for (size_t i = 0; i < Threads; ++i) workers.emplace_back([&, i] {
        gate.arrive_and_wait();
        queue(i, wc::MaxRead);
        RunWakeComparison();
    });
    for (auto &t : workers) t.join();
    assert(opens == 3 && g_comparison.stage == wc::Stage::Verified && probe_capture.size == wc::MaxRead);

    wake();
    open_started = false;
    hold_open = true;
    queue(1024, 16);
    std::thread stalled(RunWakeComparison);
    while (!open_started.load()) std::this_thread::yield();
    char report[512]{};
    assert(wc::Format(report, sizeof(report)) > 0 && std::strstr(report, "usb_romfs_compare=running\n"));
    wake();
    queue(2048, 64);
    hold_open = false;
    stalled.join();
    assert(g_comparison.stage == wc::Stage::Queued && g_comparison.offset == 2048);
    fresh_result = ReportedError;
    RunWakeComparison();
    assert(g_comparison.stage == wc::Stage::ReadFailed && g_comparison.fresh.read == ReportedError);
    assert(probe_capture.offset == 2048 && probe_capture.size == 64);
    wake();
    queue(17, 32);
    ++generation;
    RunWakeComparison();
    assert(g_comparison.stage == wc::Stage::Stale && g_comparison.fresh.open == wc::NotAttempted);
    generation = pin->generation;
    for (auto *flag : {&system_power::paused, &system_power::failed}) {
        wake();
        queue(42, 1);
        *flag = true;
        RunWakeComparison();
        *flag = false;
        assert(g_comparison.stage == wc::Stage::Stale && g_comparison.fresh.read == wc::NotAttempted);
    }
    std::puts("PASS bounded large reads, 16 contending producers/workers, observable running state, next-wake supersession and stale media");
}

int main() {
    CheckAdmission();
    CheckFreshVerification();
    CheckRetainedHandle();
    CheckCapacityAndConcurrency();
    us::power_fixture::CheckPowerProof();
}
