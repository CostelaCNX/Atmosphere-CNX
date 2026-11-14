/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "usb_startup_io_protocol.h"
#include <stratosphere/os/os_tick.hpp>

namespace ams::usb::startup_io_diagnostics {

    // The startup wrapper owns this stack sample. Synchronous service/backend
    // work stays on the same thread; no global recorder, allocation or I/O.
    inline thread_local NexusStartupIoSample *current_sample = nullptr;
    inline thread_local NexusStartupIoSample *primary_owner = nullptr;
    inline thread_local uint32_t primary_stage = UINT32_MAX;

    inline uint32_t CurrentPrimaryStage() {
        return current_sample && primary_owner == current_sample ? primary_stage : UINT32_MAX;
    }

    inline uint64_t NowNs() {
        return static_cast<uint64_t>(os::GetSystemTick().ToTimeSpan().GetNanoSeconds());
    }

    inline uint64_t AddSaturated(uint64_t a, uint64_t b) {
        return b > UINT64_MAX - a ? UINT64_MAX : a + b;
    }

    inline void AddTo(NexusStartupIoSample *sample, NexusStartupIoStage stage, uint64_t start, uint64_t end) {
        const auto index = static_cast<uint32_t>(stage);
        if (!sample || index >= NEXUS_STARTUP_IO_STAGES || end < start) return;
        sample->calls[index] = AddSaturated(sample->calls[index], 1);
        sample->duration_ns[index] = AddSaturated(sample->duration_ns[index], end - start);
    }

    // Reuse the completion diagnostic's existing timestamps for nested host
    // stages. Those spans must not be summed with the primary filesystem spans.
    inline void Add(NexusStartupIoStage stage, uint64_t start, uint64_t end) {
        AddTo(current_sample, stage, start, end);
    }

    class Scope {
        NexusStartupIoSample *previous;
        bool finished{};
        public:
            explicit Scope(NexusStartupIoSample *sample) : previous(current_sample) { current_sample = sample; }
            Scope(const Scope &) = delete;
            Scope &operator=(const Scope &) = delete;
            void Finish() {
                if (finished) return;
                finished = true;
                current_sample = previous;
            }
            ~Scope() { Finish(); }
    };

    class PhaseScope {
        NexusStartupIoSample *sample;
        NexusStartupIoStage stage;
        uint64_t start{};
        NexusStartupIoSample *previous_owner{};
        uint32_t previous_stage{};
        bool tracks_primary{};
        public:
            explicit PhaseScope(NexusStartupIoStage value) : sample(current_sample), stage(value) {
                if (static_cast<uint32_t>(stage) >= NEXUS_STARTUP_IO_STAGES) sample = nullptr;
                if (sample) {
                    start = NowNs();
                    if (static_cast<uint32_t>(stage) < NexusStartupIo_HostSubmit) {
                        previous_owner = primary_owner;
                        previous_stage = primary_stage;
                        primary_owner = sample;
                        primary_stage = static_cast<uint32_t>(stage);
                        tracks_primary = true;
                    }
                }
            }
            PhaseScope(const PhaseScope &) = delete;
            PhaseScope &operator=(const PhaseScope &) = delete;
            void Finish() {
                if (!sample) return;
                AddTo(sample, stage, start, NowNs());
                if (tracks_primary) {
                    primary_owner = previous_owner;
                    primary_stage = previous_stage;
                    tracks_primary = false;
                }
                sample = nullptr;
            }
            ~PhaseScope() { Finish(); }
    };

}
