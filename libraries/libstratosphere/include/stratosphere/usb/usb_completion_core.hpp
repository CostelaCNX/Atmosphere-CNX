/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <limits>
#include "usb_completion_protocol.h"

namespace ams::usb::completion_diagnostics {
    enum class HostStage : uint32_t { Submit, Wait, Report };
    constexpr uint64_t Add(uint64_t a, uint64_t b) {
        return b > UINT64_MAX - a ? UINT64_MAX : a + b;
    }
    inline void Increment(uint32_t &n) { if (n != UINT32_MAX) ++n; }
    constexpr bool IsWrite(uint32_t opcode) { return opcode == 0x2a || opcode == 0x8a; }
    inline void SetLba(NexusUsbCompletionEvent &e, const uint8_t *command, size_t size) {
        e.flags &= ~NEXUS_USB_COMPLETION_LBA_VALID; e.lba = 0;
        if (!command || !size) return;
        unsigned count = 0;
        if ((command[0] == 0x28 || command[0] == 0x2a) && size >= 10) count = 4;
        if ((command[0] == 0x88 || command[0] == 0x8a) && size >= 16) count = 8;
        if (!count) return;
        uint64_t lba = 0;
        for (unsigned i = 0; i < count; ++i) lba = (lba << 8) | command[2 + i];
        e.lba = lba; e.flags |= NEXUS_USB_COMPLETION_LBA_VALID;
    }
    inline NexusUsbCompletionEvent MakeEvent(uint64_t start, uint64_t prior_end,
            uint64_t bytes, uint32_t opcode, uint32_t source) {
        NexusUsbCompletionEvent e{};
        e.start_ns = start; e.bytes = bytes; e.opcode = opcode; e.source = source;
        e.result = NEXUS_USB_COMPLETION_UNKNOWN_RESULT;
        e.max_wait_result = NEXUS_USB_COMPLETION_UNKNOWN_RESULT;
        if (prior_end != 0 && start >= prior_end) {
            e.flags = NEXUS_USB_COMPLETION_IDLE_VALID;
            e.idle_ns = start - prior_end;
        }
        return e;
    }
    inline void AccumulateHost(NexusUsbCompletionEvent &e, HostStage stage,
            uint64_t start, uint64_t duration, uint32_t bytes, uint32_t endpoint,
            uint32_t xfer_id, uint32_t result) {
        switch (stage) {
            case HostStage::Submit: Increment(e.host_calls); e.submit_ns = Add(e.submit_ns, duration); break;
            case HostStage::Wait:
                Increment(e.wait_calls); e.wait_ns = Add(e.wait_ns, duration);
                if (e.wait_calls == 1 || duration > e.max_wait_ns) {
                    e.max_wait_ns = duration; e.max_wait_start_ns = start;
                    e.max_wait_bytes = bytes; e.max_wait_endpoint = endpoint;
                    e.max_wait_xfer_id = xfer_id; e.max_wait_result = result;
                }
                break;
            case HostStage::Report: Increment(e.report_calls); e.report_ns = Add(e.report_ns, duration); break;
        }
    }
    class Recorder {
        private:
            #if defined(NEXUS_USB_COMPLETION_HOST_TEST)
            friend struct RecorderTestAccess;
            #endif
            std::atomic_flag busy = ATOMIC_FLAG_INIT;
            std::atomic<uint64_t> dropped{0}, unattributed{0};
            uint64_t sequence{}, commands{}, keepalives{}, host_calls{}, last_end{}, retained{};
            uint64_t write_bytes{}, last_write_end{};
            uint32_t previous_opcode{}, previous_bytes{};
            bool have_previous{};
            NexusUsbCompletionEvent events[NEXUS_USB_COMPLETION_RING]{};
            uint32_t next{}, count{};
            struct Unlock { std::atomic_flag &flag; ~Unlock() { flag.clear(std::memory_order_release); } };
        public:
            Recorder() = default;
            Recorder(const Recorder &) = delete;
            Recorder &operator=(const Recorder &) = delete;
            void CountUnattributedHost() { unattributed.fetch_add(1, std::memory_order_relaxed); }
            bool CaptureContext(NexusUsbCompletionEvent &event) {
                event.flags &= ~(NEXUS_USB_COMPLETION_CONTEXT_VALID | NEXUS_USB_COMPLETION_PREVIOUS_VALID | NEXUS_USB_COMPLETION_WRITE_HISTORY_VALID);
                event.write_bytes_before = 0; event.prior_write_end_ns = 0;
                event.previous_opcode = 0; event.previous_bytes = 0;
                if (busy.test_and_set(std::memory_order_acquire)) {
                    dropped.fetch_add(1, std::memory_order_relaxed); return false;
                }
                const Unlock unlock{busy};
                event.flags |= NEXUS_USB_COMPLETION_CONTEXT_VALID;
                event.write_bytes_before = write_bytes;
                if (have_previous) {
                    event.flags |= NEXUS_USB_COMPLETION_PREVIOUS_VALID;
                    event.previous_opcode = previous_opcode; event.previous_bytes = previous_bytes;
                }
                if (last_write_end != 0 && event.start_ns >= last_write_end) {
                    event.flags |= NEXUS_USB_COMPLETION_WRITE_HISTORY_VALID;
                    event.prior_write_end_ns = last_write_end;
                }
                return true;
            }
            bool Record(NexusUsbCompletionEvent event) {
                if (busy.test_and_set(std::memory_order_acquire)) {
                    dropped.fetch_add(1, std::memory_order_relaxed); return false;
                }
                const Unlock unlock{busy};
                sequence = Add(sequence, 1); commands = Add(commands, 1);
                if (event.source == NEXUS_USB_COMPLETION_SOURCE_KEEP_ALIVE) keepalives = Add(keepalives, 1);
                host_calls = Add(host_calls, event.host_calls);
                const auto end = Add(event.start_ns, event.duration_ns);
                if (end > last_end) last_end = end;
                previous_opcode = event.opcode;
                previous_bytes = event.bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(event.bytes);
                have_previous = true;
                if (IsWrite(event.opcode) && event.result == 0) {
                    write_bytes = Add(write_bytes, event.bytes);
                    if (end > last_write_end) last_write_end = end;
                }
                if (event.duration_ns >= NEXUS_USB_COMPLETION_SLOW_NS ||
                    ((event.flags & NEXUS_USB_COMPLETION_IDLE_VALID) && event.idle_ns >= NEXUS_USB_COMPLETION_IDLE_NS) ||
                    event.source == NEXUS_USB_COMPLETION_SOURCE_KEEP_ALIVE) {
                    event.sequence = sequence; event.reserved = 0;
                    events[next] = event; next = (next + 1) % NEXUS_USB_COMPLETION_RING;
                    if (count < NEXUS_USB_COMPLETION_RING) ++count;
                    retained = Add(retained, 1);
                }
                return true;
            }
            bool Snapshot(NexusUsbCompletionSnapshot *out, uint64_t sampled_ns, uint64_t generation) {
                if (!out || busy.test_and_set(std::memory_order_acquire)) return false;
                const Unlock unlock{busy};
                *out = {};
                out->magic = NEXUS_USB_COMPLETION_MAGIC; out->version = NEXUS_USB_COMPLETION_VERSION;
                out->size = sizeof(*out); out->event_count = count;
                out->sampled_ns = sampled_ns; out->media_generation = generation;
                out->sequence = sequence; out->dropped = dropped.load(std::memory_order_relaxed);
                out->command_count = commands; out->keep_alive_count = keepalives;
                out->host_transfer_count = host_calls;
                out->host_unattributed = unattributed.load(std::memory_order_relaxed);
                out->last_command_end_ns = last_end;
                out->slow_threshold_ns = NEXUS_USB_COMPLETION_SLOW_NS;
                out->idle_threshold_ns = NEXUS_USB_COMPLETION_IDLE_NS;
                out->retained_total = retained;
                const auto first = (next + NEXUS_USB_COMPLETION_RING - count) % NEXUS_USB_COMPLETION_RING;
                for (uint32_t i = 0; i < count; ++i) out->events[i] = events[(first + i) % NEXUS_USB_COMPLETION_RING];
                return true;
            }
    };
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
}
