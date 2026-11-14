/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "usb_startup_command_protocol.h"
#include "usb_completion_core.hpp"
#include "usb_startup_io_diagnostics.hpp"

namespace ams::usb::startup_command_diagnostics {
    // The outer startup wrapper owns and restores this stack sample. Nothing
    // here allocates, performs I/O, or depends on the boot-global ring retaining
    // an event. A missing context adds no new clock reads.
    inline thread_local NexusStartupCommandSample *current_sample = nullptr;
    class LogicalCommandScope;
    inline thread_local LogicalCommandScope *current_command = nullptr;

    struct Attempt {
        LogicalCommandScope *owner{};
        uint64_t lba{};
        uint32_t opcode{NEXUS_STARTUP_COMMAND_UNKNOWN};
        uint32_t tag{}, flags{}, transport{}, lun{}, sector_size{};
    };
    inline thread_local Attempt current_attempt{};
    inline thread_local LogicalCommandScope *segment_owner = nullptr;
    inline thread_local uint32_t current_segment = NexusStartupCommand_Unknown;

    class Scope {
        NexusStartupCommandSample *previous;
        bool finished{};
        public:
            explicit Scope(NexusStartupCommandSample *sample) : previous(current_sample) { current_sample = sample; }
            Scope(const Scope &) = delete;
            Scope &operator=(const Scope &) = delete;
            void Finish() { if (!finished) { finished = true; current_sample = previous; } }
            ~Scope() { Finish(); }
    };

    class LogicalCommandScope {
        NexusStartupCommandSample *sample;
        LogicalCommandScope *previous;
        NexusStartupUsbCommand event;
        bool finished{};
        public:
            LogicalCommandScope() : sample(current_sample), previous(current_command) {
                if (sample) {
                    event = {};
                    event.filesystem_stage = startup_io_diagnostics::CurrentPrimaryStage();
                    event.max_wait_segment = NexusStartupCommand_Unknown;
                    event.max_wait_opcode = NEXUS_STARTUP_COMMAND_UNKNOWN;
                }
                current_command = sample ? this : nullptr;
            }
            LogicalCommandScope(const LogicalCommandScope &) = delete;
            LogicalCommandScope &operator=(const LogicalCommandScope &) = delete;
            bool Active() const { return sample && sample == current_sample && !finished; }
            void Segment(uint32_t stage, uint64_t start, uint64_t end) {
                if (stage >= NEXUS_STARTUP_COMMAND_SEGMENTS || end < start) return;
                event.segment_calls[stage] = completion_diagnostics::Add(event.segment_calls[stage], 1);
                event.segment_ns[stage] = completion_diagnostics::Add(event.segment_ns[stage], end - start);
            }
            void Wait(const NexusUsbCompletionEvent &legacy, uint64_t elapsed) {
                // Called before the old accumulator, with exactly its first /
                // strictly-longer selection rule. Equal waits keep the first.
                if (!Active() || (legacy.wait_calls && elapsed <= legacy.max_wait_ns)) return;
                event.max_wait_segment = segment_owner == this ? current_segment : static_cast<uint32_t>(NexusStartupCommand_Unknown);
                const Attempt attempt = current_attempt.owner == this ? current_attempt : Attempt{};
                event.max_wait_lba = attempt.lba;
                event.max_wait_opcode = attempt.opcode;
                event.max_wait_tag = attempt.tag;
                event.max_wait_flags = attempt.flags;
                event.max_wait_transport = attempt.transport;
                event.max_wait_lun = attempt.lun;
                event.max_wait_sector_size = attempt.sector_size;
            }
            void Finish(const NexusUsbCompletionEvent &legacy) {
                if (finished) return;
                finished = true;
                current_command = previous;
                if (!sample) return;
                event.command = legacy;
                event.command.sequence = 0; // Never expose a stale/global ring identifier.
                sample->command_count = completion_diagnostics::Add(sample->command_count, 1);
                if (sample->state == NexusStartupCommand_None ||
                    (sample->state == NexusStartupCommand_Captured &&
                     (event.command.duration_ns > sample->slowest.command.duration_ns ||
                      (event.command.duration_ns == sample->slowest.command.duration_ns &&
                       event.command.start_ns < sample->slowest.command.start_ns)))) {
                    sample->slowest = event;
                    sample->state = NexusStartupCommand_Captured;
                }
            }
            ~LogicalCommandScope() { if (!finished) current_command = previous; }
    };

    inline LogicalCommandScope *ActiveCommand() {
        return current_command && current_command->Active() ? current_command : nullptr;
    }

    class AttemptScope {
        Attempt previous;
        public:
            // A default scope describes recovery/control work, not a CDB.
            AttemptScope() : previous(current_attempt) { current_attempt = {}; }
            AttemptScope(const uint8_t *command, size_t size, uint32_t tag,
                         uint32_t lun, uint32_t sector_size, uint32_t transport) : AttemptScope() {
                auto *owner = ActiveCommand();
                if (!owner || !command || !size) return;
                current_attempt.owner = owner;
                current_attempt.opcode = command[0];
                current_attempt.tag = tag;
                current_attempt.lun = lun;
                current_attempt.sector_size = sector_size;
                current_attempt.transport = transport;
                current_attempt.flags = NEXUS_STARTUP_COMMAND_ATTEMPT_VALID;
                NexusUsbCompletionEvent parsed{};
                completion_diagnostics::SetLba(parsed, command, size);
                if (parsed.flags & NEXUS_USB_COMPLETION_LBA_VALID) {
                    current_attempt.lba = parsed.lba;
                    current_attempt.flags |= NEXUS_STARTUP_COMMAND_LBA_VALID;
                }
            }
            AttemptScope(const AttemptScope &) = delete;
            AttemptScope &operator=(const AttemptScope &) = delete;
            ~AttemptScope() { current_attempt = previous; }
    };

    class SegmentScope {
        LogicalCommandScope *owner;
        LogicalCommandScope *previous_owner;
        uint32_t stage, previous_stage;
        uint64_t start{};
        bool finished{};
        public:
            explicit SegmentScope(NexusStartupCommandSegment value)
                : owner(ActiveCommand()), previous_owner(segment_owner), stage(static_cast<uint32_t>(value)),
                  previous_stage(current_segment) {
                if (stage >= NEXUS_STARTUP_COMMAND_SEGMENTS) owner = nullptr;
                segment_owner = owner;
                current_segment = stage;
                if (owner) start = startup_io_diagnostics::NowNs();
            }
            SegmentScope(const SegmentScope &) = delete;
            SegmentScope &operator=(const SegmentScope &) = delete;
            void Finish() {
                if (finished) return;
                finished = true;
                if (owner) owner->Segment(stage, start, startup_io_diagnostics::NowNs());
                segment_owner = previous_owner;
                current_segment = previous_stage;
            }
            ~SegmentScope() { Finish(); }
    };

    inline void ObserveHost(completion_diagnostics::HostStage stage,
                            const NexusUsbCompletionEvent &legacy, uint64_t elapsed) {
        if (stage == completion_diagnostics::HostStage::Wait) {
            if (auto *active = ActiveCommand()) active->Wait(legacy, elapsed);
        }
    }
}
