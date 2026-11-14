/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#include "usb_hang_capture.hpp"
#include "../amsmitm_fs_utils.hpp"
#include "../bpc_mitm/bpc_ams_power_utils.hpp"

extern "C" void _start();

namespace ams::mitm::usb_storage::hang_capture {
    namespace {
        constexpr char ArmToken[] = "R125-ONE-SHOT-0100554023408000\n";
        constexpr char ArmPath[] = "/usb_content/hang_capture_r125.arm";
        std::atomic<os::ThreadType *> g_threads[RoleCount]{};
        std::atomic<u32> g_fs_index{Fs0}, g_state{}, g_setup_result{UnknownResult};
        std::atomic<u32> g_monitor_phase{};
        std::atomic<u64> g_selected{}, g_monitor_beat{}, g_publisher_beat{};
        std::atomic<u64> g_started{}, g_supervisor_beat{}, g_supervisor_cycles{};
        std::atomic<u32> g_supervisor_phase{NotRunning};
        std::atomic<bool> g_cancelled{};
        os::ThreadType g_supervisor{};
        alignas(os::ThreadStackAlignment) u8 g_supervisor_stack[0x4000];

        u64 Now() { return static_cast<u64>(os::GetSystemTick().ToTimeSpan().GetMilliSeconds()); }

        Result ConsumeToken() {
            FsFile file{};
            R_TRY(mitm::fs::OpenAtmosphereSdFile(std::addressof(file), ArmPath, ams::fs::OpenMode_ReadWrite));
            ON_SCOPE_EXIT { fsFileClose(std::addressof(file)); };
            s64 size{};
            R_TRY(fsFileGetSize(std::addressof(file), std::addressof(size)));
            R_UNLESS(size == sizeof(ArmToken) - 1, ams::fs::ResultInvalidArgument());
            char token[sizeof(ArmToken) - 1]{};
            u64 read{};
            R_TRY(fsFileRead(std::addressof(file), 0, token, sizeof(token), FsReadOption_None, std::addressof(read)));
            R_UNLESS(read == sizeof(token) && std::memcmp(token, ArmToken, sizeof(token)) == 0, ams::fs::ResultInvalidArgument());
            /* Persist consumption BEFORE starting the diagnostic supervisor.
             * A zeroed token is never accepted on a subsequent boot. Keep it
             * for inspection; the installer recognizes this exact used form. */
            std::memset(token, 0, sizeof(token));
            R_TRY(fsFileWrite(std::addressof(file), 0, token, sizeof(token), FsWriteOption_Flush));
            R_SUCCEED();
        }

        /* R125 is a fatal-path control, not a worker snapshot. No thread is
         * paused, read or resumed. Keep the same timer and fatal helper to
         * isolate the R124 sampler without adding a new launch dependency. */
        void EmitFatalSnapshot(u64 now) {
            Packet packet{};
            packet.now_ms = now;
            packet.selection_ms = g_selected.load(std::memory_order_acquire);
            packet.monitor_ms = g_monitor_beat.load(std::memory_order_acquire);
            packet.publisher_ms = g_publisher_beat.load(std::memory_order_acquire);
            packet.started_ms = g_started.load(std::memory_order_acquire);
            packet.deadline_ms = packet.selection_ms ? packet.selection_ms + SelectionDelayMs : 0;
            packet.supervisor_ms = g_supervisor_beat.load(std::memory_order_acquire);
            packet.supervisor_cycles = g_supervisor_cycles.load(std::memory_order_acquire);
            packet.supervisor_phase = g_supervisor_phase.load(std::memory_order_acquire);
            packet.module_base = reinterpret_cast<uintptr_t>(&_start);
            packet.anchor = reinterpret_cast<uintptr_t>(&EmitFatalSnapshot);
            packet.monitor_phase = g_monitor_phase.load(std::memory_order_acquire);
            for (u32 role = 0; role < RoleCount; ++role) {
                packet.threads[role].role = role;
                if (auto *thread = g_threads[role].load(std::memory_order_acquire); thread != nullptr) {
                    packet.registered_mask |= 1ull << role;
                    packet.threads[role].thread_id = thread->thread_id;
                }
            }
            FatalErrorContext context{};
            context.magic = FatalErrorContext::Magic;
            context.error_desc = 0xD125; // Intentional diagnostic, NOT a real CPU exception.
            context.program_id = 0x010041544D530000ull;
            context.pc = packet.anchor;
            context.module_base = packet.module_base;
            context.gprs[0] = packet.magic;
            context.gprs[1] = TargetTitle;
            context.report_identifier = svc::GetSystemTick();
            /* Counts stay zero: these bytes are our versioned packet, not a
             * fabricated stack trace. The offline decoder validates UH25. */
            constexpr size_t offset = offsetof(FatalErrorContext, stack_trace);
            static_assert(offset == 0x150 && sizeof(FatalErrorContext) == 0x450);
            static_assert(sizeof(Packet) <= sizeof(FatalErrorContext) - offset);
            std::memcpy(reinterpret_cast<u8 *>(std::addressof(context)) + offset, std::addressof(packet), sizeof(packet));
            /* No Horizon FS, NCM, diagnostic mutex, thread join or allocation
             * on this path. Existing Erista/Mariko fatal machinery persists
             * the report; a kernel/hardware stall can still prevent this. */
            bpc::RebootForFatalError(std::addressof(context));
            while (true) os::SleepThread(TimeSpan::FromSeconds(1));
        }

        void Supervisor(void *) {
            const u64 started = Now();
            while (!g_cancelled.load(std::memory_order_acquire)) {
                const u64 now = Now();
                g_supervisor_beat.store(now, std::memory_order_release);
                g_supervisor_cycles.fetch_add(1, std::memory_order_release);
                const u64 selected = g_selected.load(std::memory_order_acquire);
                g_supervisor_phase.store(selected ? Countdown : WaitingSelection, std::memory_order_release);
                if (selected == 0 && OldEnough(now, started, 180000)) break;
                /* Explicitly authorized timed snapshot, not a hang detector.
                 * Selection has already verified the exact USB title mapping.
                 * No Start reply, heartbeat, trace lock or FS status dependency.
                 * A late scheduler wake still captures after the deadline. */
                if (ShouldCaptureAfterSelection(now, selected)) {
                    if (g_cancelled.load(std::memory_order_acquire)) break;
                    g_state.store(3, std::memory_order_release);
                    g_supervisor_phase.store(Fatal, std::memory_order_release);
                    g_supervisor_beat.store(Now(), std::memory_order_release);
                    EmitFatalSnapshot(Now());
                    return;
                }
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
            }
            g_cancelled.store(true, std::memory_order_release);
            g_supervisor_phase.store(Cancelled, std::memory_order_release);
            g_supervisor_beat.store(Now(), std::memory_order_release);
            g_state.store(4, std::memory_order_release);
        }

        Result StartImpl() {
            R_TRY(ConsumeToken());
            R_TRY(os::CreateThread(std::addressof(g_supervisor), Supervisor, nullptr,
                g_supervisor_stack, sizeof(g_supervisor_stack), os::GetThreadCurrentPriority(os::GetCurrentThread()) - 1));
            g_state.store(1, std::memory_order_release);
            os::StartThread(std::addressof(g_supervisor));
            R_SUCCEED();
        }
    }

    void RegisterCurrentThread(Role role) {
        if (role >= RoleCount) return;
        os::ThreadType *expected = nullptr;
        g_threads[role].compare_exchange_strong(expected, os::GetCurrentThread(), std::memory_order_release);
    }
    void RegisterFsThread() {
        const u32 role = g_fs_index.fetch_add(1, std::memory_order_relaxed);
        if (role <= Fs2) RegisterCurrentThread(static_cast<Role>(role));
    }
    void StartOneShot() { g_setup_result.store(StartImpl().GetValue(), std::memory_order_release); }
    void NotifySelection(ncm::ProgramId client, ncm::ProgramId title) {
        if ((client != ncm::SystemAppletId::Qlaunch && client != ncm::SystemProgramId::Am)
                || title.value != TargetTitle || g_state.load(std::memory_order_acquire) != 1) return;
        u64 expected = 0;
        if (g_selected.compare_exchange_strong(expected, Now(), std::memory_order_release)) {
            u32 armed = 1;
            g_state.compare_exchange_strong(armed, 2, std::memory_order_release);
        }
    }
    void NotifyStart(ncm::ProgramId client, ncm::ProgramId title, Result start_result) {
        if ((client != ncm::SystemAppletId::Qlaunch && client != ncm::SystemProgramId::Am)
                || title.value != TargetTitle || R_FAILED(start_result)
                || g_state.load(std::memory_order_acquire) != 2 || g_cancelled.load(std::memory_order_acquire)) return;
        /* Observability only: selection already fixed the deadline. No Start
         * reply is required, and repeated calls cannot postpone capture. */
        u64 expected = 0;
        if (g_started.compare_exchange_strong(expected, Now(), std::memory_order_release)) {
            u32 selected = 2;
            g_state.compare_exchange_strong(selected, 5, std::memory_order_release);
        }
    }
    void Cancel(ncm::ProgramId title) {
        if (title.value == TargetTitle && g_selected.load(std::memory_order_acquire) != 0) g_cancelled.store(true, std::memory_order_release);
    }
    void BeatMonitor(u32 phase) {
        g_monitor_phase.store(phase, std::memory_order_release);
        g_monitor_beat.store(Now(), std::memory_order_release);
    }
    void BeatPublisher() { g_publisher_beat.store(Now(), std::memory_order_release); }
    u32 GetState() { return g_state.load(std::memory_order_acquire); }
    u32 GetSetupResult() { return g_setup_result.load(std::memory_order_acquire); }
    Progress GetProgress() {
        return {GetState(), GetSetupResult(), g_supervisor_phase.load(std::memory_order_acquire),
            g_supervisor_cycles.load(std::memory_order_acquire), g_supervisor_beat.load(std::memory_order_acquire),
            g_selected.load(std::memory_order_acquire), g_started.load(std::memory_order_acquire)};
    }
}
