/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stratosphere.hpp>
#include "usb_completion_core.hpp"
#include "usb_pause_diagnostics.hpp"
#include "usb_io_diagnostics.hpp"
#include "usb_startup_io_diagnostics.hpp"
#include "usb_startup_command_diagnostics.hpp"

namespace ams::usb::completion_diagnostics {
    inline Recorder recorder;
    inline thread_local NexusUsbCompletionEvent *current_command = nullptr;

    class CommandScope {
        private:
            NexusUsbCompletionEvent event;
            NexusUsbCompletionEvent *previous;
            startup_command_diagnostics::LogicalCommandScope startup_command;
            bool finished{false};
        public:
            CommandScope(uint64_t bytes, uint32_t opcode, uint32_t source,
                         const uint8_t *command = nullptr, size_t command_size = 0)
                : event(MakeEvent(pause_diagnostics::NowNs(),
                    io_diagnostics::last_command_ns.load(std::memory_order_relaxed), bytes, opcode, source)),
                  previous(current_command) {
                SetLba(event, command, command_size);
                recorder.CaptureContext(event);
                current_command = &event;
            }
            CommandScope(const CommandScope &) = delete;
            CommandScope &operator=(const CommandScope &) = delete;
            void Finish(uint32_t result) {
                if (finished) return;
                finished = true;
                const auto end = pause_diagnostics::NowNs();
                event.duration_ns = end >= event.start_ns ? end - event.start_ns : 0;
                event.result = result;
                current_command = previous;
                startup_command.Finish(event);
                recorder.Record(event);
            }
            ~CommandScope() { Finish(NEXUS_USB_COMPLETION_UNKNOWN_RESULT); }
    };

    template<typename Operation>
    Result MeasureHost(HostStage stage, uint32_t bytes, uint32_t endpoint, uint32_t xfer_id, Operation operation) {
        const auto start = pause_diagnostics::NowNs();
        const Result result = operation();
        const auto end = pause_diagnostics::NowNs();
        const auto elapsed = end >= start ? end - start : 0;
        const auto startup_stage = stage == HostStage::Submit ? NexusStartupIo_HostSubmit :
            stage == HostStage::Wait ? NexusStartupIo_HostWait : NexusStartupIo_HostReport;
        startup_io_diagnostics::Add(startup_stage, start, end);
        const auto pause_stage = stage == HostStage::Submit ? pause_diagnostics::Stage::HostSubmit :
            stage == HostStage::Wait ? pause_diagnostics::Stage::HostWait : pause_diagnostics::Stage::HostReport;
        pause_diagnostics::recorder.Record(pause_stage, start, elapsed, bytes, 0, result.GetValue());
        if (current_command) {
            startup_command_diagnostics::ObserveHost(stage, *current_command, elapsed);
            AccumulateHost(*current_command, stage, start, elapsed, bytes, endpoint, xfer_id, result.GetValue());
        }
        else if (stage == HostStage::Submit) recorder.CountUnattributedHost();
        return result;
    }
    inline bool Snapshot(NexusUsbCompletionSnapshot *out, uint64_t generation) {
        if (!recorder.Snapshot(out, 0, generation)) return false;
        out->sampled_ns = pause_diagnostics::NowNs();
        return true;
    }
}
