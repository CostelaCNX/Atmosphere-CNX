// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_system_power.hpp"
#include "usb_game_wake_trace.hpp"
#include "../../../../libraries/libstratosphere/source/fssystem/fssystem_hardware_key_power.hpp"
#include "usb_wake_compare.hpp"
#include "usb_system_power_core.hpp"
#include "usbmitm_module.hpp"
#include <stratosphere/usb/usb_sleep_diagnostics.hpp>

namespace ams::mitm::usb_storage::system_power {
namespace {
// Custom IDs have no global allocator. A collision fails closed; never borrow
// a stock ID or hijack another module. Reference/policy notes are in SL1 tests.
constexpr u32 ModuleId = 0x4e58;
constexpr u64 RegistrationBudgetNs = 10'000'000'000ULL;
constexpr u64 BackgroundDrainBudgetNs = 1'000'000'000ULL;
constexpr size_t StackSize = 0x4000;
alignas(os::ThreadStackAlignment) u8 g_stack[StackSize]{};
os::ThreadType g_thread;
alignas(os::ThreadStackAlignment) u8 g_recovery_stack[0x8000]{};
os::ThreadType g_recovery_thread;
os::SdkMutex g_recovery_mutex;
os::SdkConditionVariable g_recovery_cv;
enum class JobState { Idle, Queued, Running, Finished };
JobState g_job{JobState::Idle};
bool g_quit{};
u32 g_job_result{};
std::atomic<u32> g_report_revision{};
std::atomic<u32> g_crypto_refreshes{};
::PscPmModule g_module{};
ActivityGate g_background(true);
Coordinator g_coordinator;
std::atomic<u32> g_phase{}, g_registered{}, g_init_result{}, g_query_result{}, g_ack_result{};
std::atomic<u32> g_last_state{6}, g_last_flags{}, g_failure_result{};
std::atomic<u64> g_recent_states{UINT64_MAX};
std::atomic<u32> g_last_deferred_state{6}, g_prepare_state{6};
std::atomic<u64> g_requests{}, g_queries{}, g_acks{}, g_failures{}, g_background_skips{};
std::atomic<u64> g_registration_attempts{}, g_background_drain_ns{};
std::atomic<u64> g_deferred_acks{};
std::atomic<u32> g_transition_result{};

u64 Now() { return os::GetSystemTick().ToTimeSpan().GetNanoSeconds(); }

void RestoreHardwareKeyCache() {
    if (fssystem::EndHardwareKeySleep()) g_crypto_refreshes.fetch_add(1, std::memory_order_relaxed);
}

void Recover(void *) {
    for (;;) {
        {
            std::unique_lock lk(g_recovery_mutex);
            while (g_job != JobState::Queued && !g_quit) g_recovery_cv.Wait(g_recovery_mutex);
            if (g_quit) return;
            g_job = JobState::Running;
        }
        namespace sd = ::ams::usb::sleep_diagnostics;
        const auto prior_resumes = sd::resumes.load();
        RestoreHardwareKeyCache();
        const Result result = GetUsbContentStorageService()->ResumeSystemSleep();
        const auto generation = sd::generation_before.load();
        if (R_SUCCEEDED(result) && sd::resumes.load() != prior_resumes && sd::marker_armed.load() &&
            generation && sd::generation_after.load() == generation) {
            // Both the retained endpoint and a newly bound one must validate
            // the marker before reopening this media generation.
            wake_compare::ready_epoch.store(game_wake::wake_epoch.load(std::memory_order_acquire), std::memory_order_release);
        }
        std::scoped_lock lk(g_recovery_mutex);
        g_job_result = result.GetValue();
        g_job = JobState::Finished;
        g_report_revision.fetch_add(1, std::memory_order_release);
    }
}

void StopRecoveryThread() {
    {
        std::scoped_lock lk(g_recovery_mutex);
        g_quit = true;
        g_recovery_cv.Broadcast();
    }
    os::WaitThread(&g_recovery_thread);
    os::DestroyThread(&g_recovery_thread);
}

struct Backend {
    void CloseBackground() { g_background.Close(); }
    u32 DrainBackground() {
        const u64 started = Now();
        while (g_background.Active()) {
            if (Now() - started >= BackgroundDrainBudgetNs) {
                g_background_drain_ns.store(Now() - started);
                return svc::ResultTimedOut().GetValue();
            }
            os::SleepThread(TimeSpan::FromMilliSeconds(1));
        }
        g_background_drain_ns.store(Now() - started);
        return 0;
    }
    u32 PrepareStorage() {
        g_prepare_state.store(g_last_state.load());
        const Result result = GetUsbContentStorageService()->PrepareSystemSleep();
        if (R_SUCCEEDED(result)) fssystem::BeginHardwareKeySleep();
        return result.GetValue();
    }
    u32 ReserveWake() { return GetUsbContentStorageService()->ReserveSystemWake().GetValue(); }
    u32 BusyResult() const { return usb::ResultResourceBusy().GetValue(); }
    void StartRecovery() {
        std::scoped_lock lk(g_recovery_mutex);
        AMS_ASSERT(g_job == JobState::Idle);
        game_wake::BeginWake();
        g_job = JobState::Queued;
        g_recovery_cv.Broadcast();
    }
    bool TakeWakeResult(u32 &result) {
        std::scoped_lock lk(g_recovery_mutex);
        if (g_job != JobState::Finished) return false;
        result = g_job_result;
        g_job = JobState::Idle;
        return true;
    }
    bool IsBackgroundQuiescent() const { return g_background.Active() == 0; }
    bool IsStorageQuiescent() const {
        {
            std::scoped_lock lk(g_recovery_mutex);
            if (g_job == JobState::Queued || g_job == JobState::Running) return false;
        }
        return GetUsbContentStorageService()->IsSystemPowerQuiescent();
    }
    void OpenBackground() {
        // A failed ReserveWake can skip the worker. FullAwake may still
        // release the diagnostic path; normal crypto caching can resume here.
        if (g_failure_result.load(std::memory_order_acquire)) RestoreHardwareKeyCache();
        g_background.Open();
    }
    void FailStorage(u32 result) {
        g_failure_result.store(result);
        g_failures.fetch_add(1);
        GetUsbContentStorageService()->FailSystemPower(Result(result));
        g_report_revision.fetch_add(1, std::memory_order_release);
    }
    u32 InvalidStateResult() const { return usb::ResultInvalidParameter().GetValue(); }
    u32 Acknowledge(u32 state) {
        const Result rc = ::pscPmModuleAcknowledge(&g_module, static_cast<::PscPmState>(state));
        g_ack_result.store(rc.GetValue());
        g_acks.fetch_add(1);
        return rc.GetValue();
    }
};

void Fail(Result result) {
    Backend backend;
    g_coordinator.Fail(result.GetValue(), backend);
    g_phase.store(static_cast<u32>(g_coordinator.GetPhase()), std::memory_order_release);
}

void Watcher(void *) {
    Backend backend;
    while (true) {
        g_coordinator.PollWake(backend);
        g_phase.store(static_cast<u32>(g_coordinator.GetPhase()), std::memory_order_release);
        const Result waited = ::eventWait(&g_module.event, 250'000'000ULL);
        if (svc::ResultTimedOut::Includes(waited)) continue;
        if (R_FAILED(waited)) { g_query_result.store(waited.GetValue()); Fail(waited); break; }
        ::PscPmState state{};
        u32 flags{};
        const Result query = ::pscPmModuleGetRequest(&g_module, &state, &flags);
        g_queries.fetch_add(1);
        g_query_result.store(query.GetValue());
        if (R_FAILED(query)) { Fail(query); break; }
        g_last_state.store(static_cast<u32>(state));
        const u32 nibble = static_cast<u32>(state) < 15 ? static_cast<u32>(state) : 15;
        g_recent_states.store((g_recent_states.load(std::memory_order_relaxed) << 4) | nibble, std::memory_order_relaxed);
        g_last_flags.store(flags);
        g_requests.fetch_add(1);
        for (;;) {
            const auto outcome = g_coordinator.Handle(static_cast<u32>(state), backend);
            g_transition_result.store(outcome.result);
            g_phase.store(static_cast<u32>(g_coordinator.GetPhase()), std::memory_order_release);
            if (outcome.ack_attempted) break;
            // A timeout is diagnostic, not permission to ACK active I/O.
            // Keep this exact request pending. PSC may cancel/abort a stuck
            // transition; no SD write is attempted to announce the problem.
            g_deferred_acks.fetch_add(1);
            g_last_deferred_state.store(static_cast<u32>(state), std::memory_order_relaxed);
            os::SleepThread(TimeSpan::FromMilliSeconds(10));
        }
        if (g_ack_result.load()) break;
    }
    // A failed event/query/ACK must not leave an unserviced PSC registration.
    // USB remains quarantined. No SD logging or device teardown in this path.
    while (!backend.IsBackgroundQuiescent() || !backend.IsStorageQuiescent()) {
        os::SleepThread(TimeSpan::FromMilliSeconds(10));
    }
    StopRecoveryThread();
    ::pscPmModuleFinalize(&g_module);
    ::pscPmModuleClose(&g_module);
    ::eventClose(&g_module.event);
    g_registered.store(0);
    ::pscmExit();
}
}

Result Initialize() {
    // HasService avoids indefinitely blocking smGetService while PSC starts.
    // This bounded retry runs solely on the optional USB monitor thread.
    const u64 started = Now();
    Result rc = svc::ResultTimedOut();
    for (;;) {
        bool present = false;
        g_registration_attempts.fetch_add(1);
        rc = sm::HasService(&present, sm::ServiceName::Encode("psc:m"));
        if (R_FAILED(rc)) break;
        if (present) { rc = ::pscmInitialize(); break; }
        if (Now() - started >= RegistrationBudgetNs) { rc = svc::ResultTimedOut(); break; }
        os::SleepThread(TimeSpan::FromMilliSeconds(100));
    }
    if (R_FAILED(rc)) { g_init_result.store(rc.GetValue()); Fail(rc); R_RETURN(rc); }

    const u32 dependencies[] = { static_cast<u32>(psc::PmModuleId_Usb), static_cast<u32>(psc::PmModuleId_Fs) };
    rc = ::pscmGetPmModule(&g_module, static_cast<::PscPmModuleId>(ModuleId),
        dependencies, util::size(dependencies), true);
    if (R_FAILED(rc)) {
        ::pscmExit();
        g_init_result.store(rc.GetValue()); Fail(rc); R_RETURN(rc);
    }
    g_registered.store(1);
    rc = os::CreateThread(&g_recovery_thread, Recover, nullptr, g_recovery_stack,
        sizeof(g_recovery_stack), os::GetThreadCurrentPriority(os::GetCurrentThread()));
    if (R_FAILED(rc)) {
        ::pscPmModuleFinalize(&g_module); ::pscPmModuleClose(&g_module); ::eventClose(&g_module.event);
        g_registered.store(0); g_init_result.store(rc.GetValue()); Fail(rc); ::pscmExit(); R_RETURN(rc);
    }
    os::SetThreadNamePointer(&g_recovery_thread, "NexusUsbWake");
    os::StartThread(&g_recovery_thread);
    rc = os::CreateThread(&g_thread, Watcher, nullptr, g_stack, sizeof(g_stack),
        os::GetThreadCurrentPriority(os::GetCurrentThread()));
    if (R_FAILED(rc)) {
        StopRecoveryThread();
        ::pscPmModuleFinalize(&g_module); ::pscPmModuleClose(&g_module);
        ::eventClose(&g_module.event); ::pscmExit();
        g_registered.store(0); g_init_result.store(rc.GetValue()); Fail(rc); R_RETURN(rc);
    }
    os::SetThreadNamePointer(&g_thread, "NexusUsbPower");
    g_background.Open();
    os::StartThread(&g_thread);
    R_SUCCEED();
}

bool IsBackgroundPaused() { return g_background.Closed(); }
bool HasFailed() { return g_failure_result.load(std::memory_order_acquire) != 0; }
u32 ReportRevision() { return g_report_revision.load(std::memory_order_acquire); }
bool TryEnterBackground() {
    const bool entered = g_background.TryEnter();
    if (!entered) g_background_skips.fetch_add(1, std::memory_order_relaxed);
    return entered;
}
void LeaveBackground() { g_background.Leave(); }

int Format(char *out, size_t capacity) {
    namespace sd = ams::usb::sleep_diagnostics;
    const int power_size = util::SNPrintf(out, capacity,
        "usb_sleep_build=SL7\nusb_sleep_module_id=0x%08X\n"
        "usb_sleep_crypto_refreshes=%u\nusb_sleep_registered=%u\nusb_sleep_init_result=0x%08X\n"
        "usb_sleep_registration_attempts=%llu\nusb_sleep_phase=%u\n"
        "usb_sleep_state=%u\nusb_sleep_flags=0x%08X\nusb_sleep_requests=%llu\n"
        "usb_sleep_queries=%llu\nusb_sleep_query_result=0x%08X\n"
        "usb_sleep_acks=%llu\nusb_sleep_ack_result=0x%08X\n"
        "usb_sleep_failures=%llu\nusb_sleep_failure_result=0x%08X\n"
        "usb_sleep_transition_result=0x%08X\nusb_sleep_deferred_acks=%llu\n"
        "usb_sleep_recent_states=0x%016llX\nusb_sleep_last_deferred_state=%u\n"
        "usb_sleep_background_closed=%u\nusb_sleep_background_active=%u\n"
        "usb_sleep_background_skips=%llu\nusb_sleep_background_drain_ns=%llu\n"
        "usb_sleep_storage_phase=%u\nusb_sleep_prepares=%llu\nusb_sleep_resumes=%llu\n"
        "usb_sleep_storage_failures=%llu\nusb_sleep_storage_result=0x%08X\n"
        "usb_sleep_generation_before=%llu\nusb_sleep_generation_after=%llu\n"
        "usb_sleep_prepare_ns=%llu\nusb_sleep_resume_ns=%llu\n"
        "usb_sleep_reset_attentions=%llu\nusb_sleep_ready_retries=%llu\nusb_sleep_identity_failures=%llu\n"
        "usb_sleep_marker_armed=%u\nusb_sleep_rebind_attempts=%u\nusb_sleep_rebind_successes=%u\n"
        "usb_sleep_rebind_stage=%u\nusb_sleep_rebind_result=0x%08X\nusb_sleep_rebind_candidates=%u\n"
        "usb_sleep_rebind_queries=%u\nusb_sleep_rebind_query_result=0x%08X\n"
        "usb_sleep_rebind_visible_max=%u\nusb_sleep_rebind_exact_max=%u\n"
        "usb_sleep_prepare_state=%u\nusb_sleep_prepare_stage=%u\n"
        "usb_sleep_checkpoint_result=0x%08X\nusb_sleep_checkpoint_used=%u\n",
        ModuleId, g_crypto_refreshes.load(), g_registered.load(), g_init_result.load(),
        static_cast<unsigned long long>(g_registration_attempts.load()), g_phase.load(),
        g_last_state.load(), g_last_flags.load(), static_cast<unsigned long long>(g_requests.load()),
        static_cast<unsigned long long>(g_queries.load()), g_query_result.load(),
        static_cast<unsigned long long>(g_acks.load()), g_ack_result.load(),
        static_cast<unsigned long long>(g_failures.load()), g_failure_result.load(),
        g_transition_result.load(), static_cast<unsigned long long>(g_deferred_acks.load()),
        static_cast<unsigned long long>(g_recent_states.load()), g_last_deferred_state.load(),
        g_background.Closed() ? 1u : 0u, g_background.Active(),
        static_cast<unsigned long long>(g_background_skips.load()),
        static_cast<unsigned long long>(g_background_drain_ns.load()),
        sd::phase.load(), static_cast<unsigned long long>(sd::prepares.load()),
        static_cast<unsigned long long>(sd::resumes.load()), static_cast<unsigned long long>(sd::failures.load()),
        sd::last_result.load(), static_cast<unsigned long long>(sd::generation_before.load()),
        static_cast<unsigned long long>(sd::generation_after.load()),
        static_cast<unsigned long long>(sd::last_prepare_ns.load()),
        static_cast<unsigned long long>(sd::last_resume_ns.load()),
        static_cast<unsigned long long>(sd::reset_attentions.load()),
        static_cast<unsigned long long>(sd::ready_retries.load()),
        static_cast<unsigned long long>(sd::identity_failures.load()),
        static_cast<u32>(sd::marker_armed.load()), static_cast<u32>(sd::rebind_attempts.load()),
        static_cast<u32>(sd::rebind_successes.load()), static_cast<u32>(sd::rebind_stage.load()),
        static_cast<u32>(sd::rebind_result.load()), static_cast<u32>(sd::rebind_candidates.load()),
        sd::rebind_queries.load(), sd::rebind_query_result.load(),
        sd::rebind_visible_max.load(), sd::rebind_exact_max.load(), g_prepare_state.load(),
        sd::prepare_stage.load(), sd::checkpoint_result.load(), sd::checkpoint_used.load());
    if (power_size <= 0 || static_cast<size_t>(power_size) >= capacity) return -1;
    const size_t remaining = capacity - static_cast<size_t>(power_size);
    const int game_size = game_wake::Format(out + power_size, remaining);
    if (game_size <= 0 || static_cast<size_t>(game_size) >= remaining) return -1;
    const auto room = remaining - static_cast<size_t>(game_size);
    const int comparison = wake_compare::Format(out + power_size + game_size, room);
    if (comparison <= 0 || static_cast<size_t>(comparison) >= room) return -1;
    return power_size + game_size + comparison;
}

}
