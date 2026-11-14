/* Copyright (c) Atmosphere-NX contributors. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <cstdint>
#include <cstddef>

namespace ams::mitm::usb_storage::hang_capture {
    constexpr std::uint64_t TargetTitle = 0x0100554023408000ull;
    constexpr std::uint64_t StallMs = 20000, SelectionWindowMs = 120000, CaptureBudgetMs = 750;
    constexpr std::uint64_t StartDelayMs = 20000;
    constexpr std::uint64_t SelectionDelayMs = 20000;
    constexpr std::uint32_t UnknownResult = 0xFFFFFFFFu;
    enum Role : std::uint32_t { Monitor, Publisher, Applet, Fs0, Fs1, Fs2, Ns, UsbService, RoleCount };
    enum Phase : std::uint32_t { Idle, Suspending, Reading, Resuming, Complete };
    enum SupervisorPhase : std::uint32_t { NotRunning, WaitingSelection, Countdown, Sampling, Fatal, Cancelled };

    struct Progress {
        std::uint32_t state{}, setup_result{UnknownResult}, phase{};
        std::uint64_t cycles{}, beat_ms{}, selected_ms{}, started_ms{};
    };

    constexpr bool OldEnough(std::uint64_t now, std::uint64_t stamp, std::uint64_t age) {
        return stamp != 0 && now >= stamp && now - stamp >= age;
    }
    /* Old trigger policies are retained solely for regression tests. */
    constexpr bool ShouldCapture(std::uint64_t now, std::uint64_t selected,
                                 std::uint64_t monitor, std::uint64_t publisher) {
        return OldEnough(now, selected, StallMs) && now - selected < SelectionWindowMs
            && OldEnough(now, monitor, StallMs) && OldEnough(now, publisher, StallMs);
    }

    constexpr bool ShouldCaptureAfterStart(std::uint64_t now, std::uint64_t started) {
        return OldEnough(now, started, StartDelayMs);
    }

    /* R124 starts at the already verified mapped-title selection. Neither a
     * Start reply nor logger progress is required. Late wakes still capture. */
    constexpr bool ShouldCaptureAfterSelection(std::uint64_t now, std::uint64_t selected) {
        return OldEnough(now, selected, SelectionDelayMs);
    }

    struct ThreadRecord {
        std::uint32_t role{}, suspend_result{UnknownResult}, context_result{UnknownResult}, resume_result{UnknownResult};
        std::uint64_t thread_id{}, pc{}, lr{}, sp{}, fp{}, caller{}, arg0{};
    };
    static_assert(sizeof(ThreadRecord) == 72);

    struct Packet {
        std::uint32_t magic{0x35324855}, version{4}, size{704}, completed_mask{}; // UH25: no worker contexts
        std::uint64_t title{TargetTitle}, now_ms{}, selection_ms{}, monitor_ms{}, publisher_ms{}, module_base{};
        std::uint32_t pending_role{RoleCount}, pending_phase{}, monitor_phase{}, supervisor_phase{};
        std::uint64_t anchor{}, registered_mask{}, started_ms{}, supervisor_ms{}, supervisor_cycles{}, deadline_ms{};
        ThreadRecord threads[RoleCount]{};
    };
    static_assert(offsetof(Packet, threads) == 128 && sizeof(Packet) == 704);

    /* Legacy sampler retained for host regression tests only; R125 does not
     * instantiate this template or suspend workers. A
     * failed context read still resumes a successfully suspended thread.
     * The independent supervisor, not this potentially blocking routine,
     * enforces the overall capture budget. No exceptions are used. */
    template<class Backend>
    void SampleOne(ThreadRecord &out, Backend &backend) {
        out.thread_id = backend.ThreadId();
        backend.SetPhase(Suspending);
        out.suspend_result = backend.Suspend();
        if (out.suspend_result != 0) return;
        backend.SetPhase(Reading);
        out.context_result = backend.Read(out);
        backend.SetPhase(Resuming);
        out.resume_result = backend.Resume();
        backend.SetPhase(Complete);
    }
}
